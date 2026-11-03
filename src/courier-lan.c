/* courier-lan.c — LAN discovery (UDP broadcast) and messaging (TCP, JSON lines) */
#include "courier-lan.h"

#include <json-glib/json-glib.h>
#include <string.h>

#define BEACON_INTERVAL 5   /* seconds */
#define PEER_TIMEOUT    20  /* seconds without a hello → offline */
#define IO_TIMEOUT      8   /* seconds */

struct _CourierLan {
  GObject          parent_instance;

  char            *id;
  char            *name;
  guint16          discovery_port;
  guint16          tcp_port;
  char            *broadcast;

  GSocketService  *service;
  GSocket         *udp;
  GSource         *udp_source;
  guint            beacon_id;
  GMainContext    *context;
  GHashTable      *peers;   /* id -> CourierLanPeer* */
};

G_DEFINE_FINAL_TYPE (CourierLan, courier_lan, G_TYPE_OBJECT)

enum { SIGNAL_PEERS_CHANGED, SIGNAL_MESSAGE, N_SIGNALS };
static guint signals[N_SIGNALS];

/* ---------------- peers ---------------- */

void
courier_lan_peer_free (CourierLanPeer *p)
{
  if (!p) return;
  g_free (p->id);
  g_free (p->name);
  g_free (p->host);
  g_free (p);
}

static CourierLanPeer *
peer_copy (const CourierLanPeer *src)
{
  CourierLanPeer *p = g_memdup2 (src, sizeof *src);
  p->id = g_strdup (src->id);
  p->name = g_strdup (src->name);
  p->host = g_strdup (src->host);
  return p;
}

static gint64
now_seconds (void)
{
  return g_get_monotonic_time () / G_USEC_PER_SEC;
}

gboolean
courier_lan_peer_online (const CourierLanPeer *p)
{
  return p->last_seen > 0 && now_seconds () - p->last_seen <= PEER_TIMEOUT;
}

/* Insert or refresh a peer (main thread). Returns TRUE if something visible changed. */
static gboolean
touch_peer (CourierLan *self, const char *id, const char *name, const char *host, guint16 port, gboolean manual)
{
  if (!id || !*id || g_str_equal (id, self->id) || !host || port == 0)
    return FALSE;
  CourierLanPeer *p = g_hash_table_lookup (self->peers, id);
  gboolean changed = FALSE;
  if (!p) {
    p = g_new0 (CourierLanPeer, 1);
    p->id = g_strdup (id);
    g_hash_table_insert (self->peers, p->id, p);
    changed = TRUE;
  }
  if (!courier_lan_peer_online (p))
    changed = TRUE;
  if (g_strcmp0 (p->name, name) != 0 || g_strcmp0 (p->host, host) != 0 || p->port != port)
    changed = TRUE;
  g_free (p->name);
  p->name = g_strdup (name && *name ? name : host);
  g_free (p->host);
  p->host = g_strdup (host);
  p->port = port;
  p->manual = p->manual || manual;
  p->last_seen = now_seconds ();
  return changed;
}

static int
compare_peers (gconstpointer a, gconstpointer b)
{
  const CourierLanPeer *pa = *(CourierLanPeer **) a, *pb = *(CourierLanPeer **) b;
  gboolean oa = courier_lan_peer_online (pa), ob = courier_lan_peer_online (pb);
  if (oa != ob)
    return oa ? -1 : 1;
  return g_utf8_collate (pa->name, pb->name);
}

GPtrArray *
courier_lan_list_peers (CourierLan *self)
{
  GPtrArray *out = g_ptr_array_new_with_free_func ((GDestroyNotify) courier_lan_peer_free);
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init (&iter, self->peers);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    g_ptr_array_add (out, peer_copy (value));
  g_ptr_array_sort (out, compare_peers);
  return out;
}

const CourierLanPeer *
courier_lan_find_peer (CourierLan *self, const char *id)
{
  return id ? g_hash_table_lookup (self->peers, id) : NULL;
}

/* ---------------- JSON ---------------- */

static char *
hello_json (CourierLan *self, const char *type)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "app");  json_builder_add_string_value (b, "courier");
  json_builder_set_member_name (b, "type"); json_builder_add_string_value (b, type);
  json_builder_set_member_name (b, "id");   json_builder_add_string_value (b, self->id);
  json_builder_set_member_name (b, "name"); json_builder_add_string_value (b, self->name);
  json_builder_set_member_name (b, "port"); json_builder_add_int_value (b, courier_lan_get_port (self));
  json_builder_end_object (b);
  g_autoptr (JsonNode) root = json_builder_get_root (b);
  return json_to_string (root, FALSE);
}

static JsonObject *
parse_object (const char *data, gssize len)
{
  g_autoptr (JsonParser) parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, data, len, NULL))
    return NULL;
  JsonNode *root = json_parser_get_root (parser);
  if (!root || !JSON_NODE_HOLDS_OBJECT (root))
    return NULL;
  JsonObject *obj = json_node_get_object (root);
  if (g_strcmp0 (json_object_get_string_member_with_default (obj, "app", ""), "courier") != 0)
    return NULL;
  return json_object_ref (obj);
}

static const char *
jstr (JsonObject *o, const char *key)
{
  return json_object_get_string_member_with_default (o, key, NULL);
}

/* ---------------- incoming TCP (worker thread) ---------------- */

typedef struct {
  CourierLan *self;
  char       *id, *name, *host, *text;
  guint16     port;
  gint64      ts;
  gboolean    is_message;
} Incoming;

static gboolean
deliver_incoming (gpointer data)
{
  Incoming *in = data;
  CourierLan *self = in->self;
  if (self->service) {
    if (touch_peer (self, in->id, in->name, in->host, in->port, FALSE))
      g_signal_emit (self, signals[SIGNAL_PEERS_CHANGED], 0);
    if (in->is_message)
      g_signal_emit (self, signals[SIGNAL_MESSAGE], 0, in->id, in->name, in->text, in->ts);
  }
  g_object_unref (in->self);
  g_free (in->id); g_free (in->name); g_free (in->host); g_free (in->text);
  g_free (in);
  return G_SOURCE_REMOVE;
}

static char *
remote_host (GSocketConnection *connection)
{
  g_autoptr (GSocketAddress) addr = g_socket_connection_get_remote_address (connection, NULL);
  if (!addr || !G_IS_INET_SOCKET_ADDRESS (addr))
    return NULL;
  return g_inet_address_to_string (g_inet_socket_address_get_address (G_INET_SOCKET_ADDRESS (addr)));
}

static gboolean
on_incoming (GThreadedSocketService *service, GSocketConnection *connection, GObject *source, gpointer data)
{
  CourierLan *self = data;
  g_socket_set_timeout (g_socket_connection_get_socket (connection), IO_TIMEOUT);

  g_autoptr (GDataInputStream) in = g_data_input_stream_new (g_io_stream_get_input_stream (G_IO_STREAM (connection)));
  GOutputStream *out = g_io_stream_get_output_stream (G_IO_STREAM (connection));
  gsize len = 0;
  g_autofree char *line = g_data_input_stream_read_line_utf8 (in, &len, NULL, NULL);
  if (!line || len > COURIER_LAN_MAX_TEXT * 2)
    return TRUE;

  g_autoptr (JsonObject) obj = parse_object (line, len);
  if (!obj)
    return TRUE;
  const char *type = jstr (obj, "type");
  const char *id = jstr (obj, "id");
  const char *text = jstr (obj, "text");
  if (!type || !id)
    return TRUE;

  g_autofree char *reply = NULL;
  gboolean is_message = g_str_equal (type, "msg") && text && *text;
  if (g_str_equal (type, "hello"))
    reply = hello_json (self, "hello");
  else if (is_message)
    reply = g_strdup ("{\"app\":\"courier\",\"type\":\"ack\"}");
  else
    return TRUE;

  g_autofree char *reply_line = g_strconcat (reply, "\n", NULL);
  g_output_stream_write_all (out, reply_line, strlen (reply_line), NULL, NULL, NULL);

  Incoming *msg = g_new0 (Incoming, 1);
  msg->self = g_object_ref (self);
  msg->id = g_strdup (id);
  msg->name = g_strdup (jstr (obj, "name"));
  msg->host = remote_host (connection);
  msg->port = (guint16) json_object_get_int_member_with_default (obj, "port", 0);
  msg->text = g_strdup (text);
  msg->ts = json_object_get_int_member_with_default (obj, "ts", g_get_real_time () / G_USEC_PER_SEC);
  msg->is_message = is_message;
  g_main_context_invoke (self->context, deliver_incoming, msg);
  return TRUE;
}

/* ---------------- UDP discovery ---------------- */

static void
send_hello_to (CourierLan *self, const char *host, guint16 port)
{
  g_autoptr (GInetAddress) addr = g_inet_address_new_from_string (host);
  if (!addr)
    return;
  g_autoptr (GSocketAddress) sa = g_inet_socket_address_new (addr, port);
  g_autofree char *hello = hello_json (self, "hello");
  g_socket_send_to (self->udp, sa, hello, strlen (hello), NULL, NULL);
}

static gboolean
beacon_tick (gpointer data)
{
  CourierLan *self = data;
  if (!self->udp)
    return G_SOURCE_REMOVE;
  send_hello_to (self, self->broadcast, self->discovery_port);

  /* manual peers learn about us even without broadcast; expire silent peers */
  gboolean changed = FALSE;
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init (&iter, self->peers);
  while (g_hash_table_iter_next (&iter, NULL, &value)) {
    CourierLanPeer *p = value;
    if (p->manual)
      send_hello_to (self, p->host, self->discovery_port);
    if (p->last_seen && now_seconds () - p->last_seen == PEER_TIMEOUT + 1)
      changed = TRUE;
    if (!p->manual && now_seconds () - p->last_seen > PEER_TIMEOUT * 6) {
      g_hash_table_iter_remove (&iter);
      changed = TRUE;
    }
  }
  /* coarse: refresh the UI whenever someone could have gone offline */
  static int ticks = 0;
  if (changed || ++ticks % 4 == 0)
    g_signal_emit (self, signals[SIGNAL_PEERS_CHANGED], 0);
  return G_SOURCE_CONTINUE;
}

static gboolean
on_udp_readable (GSocket *socket, GIOCondition cond, gpointer data)
{
  CourierLan *self = data;
  char buf[2048];
  for (;;) {
    GSocketAddress *from = NULL;
    gssize n = g_socket_receive_from (socket, &from, buf, sizeof buf - 1, NULL, NULL);
    if (n <= 0) {
      g_clear_object (&from);
      break;
    }
    buf[n] = '\0';
    g_autoptr (JsonObject) obj = parse_object (buf, n);
    if (obj && from && G_IS_INET_SOCKET_ADDRESS (from)) {
      const char *type = jstr (obj, "type");
      g_autofree char *host = g_inet_address_to_string (
        g_inet_socket_address_get_address (G_INET_SOCKET_ADDRESS (from)));
      const char *id = jstr (obj, "id");
      if (g_strcmp0 (type, "hello") == 0) {
        if (touch_peer (self, id, jstr (obj, "name"), host,
                        (guint16) json_object_get_int_member_with_default (obj, "port", 0), FALSE))
          g_signal_emit (self, signals[SIGNAL_PEERS_CHANGED], 0);
      } else if (g_strcmp0 (type, "bye") == 0 && id) {
        CourierLanPeer *p = g_hash_table_lookup (self->peers, id);
        if (p) {
          p->last_seen = 0;
          g_signal_emit (self, signals[SIGNAL_PEERS_CHANGED], 0);
        }
      }
    }
    g_clear_object (&from);
  }
  return G_SOURCE_CONTINUE;
}

/* ---------------- lifecycle ---------------- */

gboolean
courier_lan_start (CourierLan *self, GError **error)
{
  if (self->service)
    return TRUE;

  self->service = G_SOCKET_SERVICE (g_threaded_socket_service_new (4));
  if (self->tcp_port) {
    if (!g_socket_listener_add_inet_port (G_SOCKET_LISTENER (self->service), self->tcp_port, NULL, error))
      goto fail;
  } else {
    self->tcp_port = g_socket_listener_add_any_inet_port (G_SOCKET_LISTENER (self->service), NULL, error);
    if (!self->tcp_port)
      goto fail;
  }
  g_signal_connect (self->service, "run", G_CALLBACK (on_incoming), self);
  g_socket_service_start (self->service);

  /* discovery is best effort: chatting by address still works without it */
  g_autoptr (GError) udp_error = NULL;
  self->udp = g_socket_new (G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_DATAGRAM, G_SOCKET_PROTOCOL_UDP, &udp_error);
  if (self->udp) {
    g_autoptr (GInetAddress) any = g_inet_address_new_any (G_SOCKET_FAMILY_IPV4);
    g_autoptr (GSocketAddress) sa = g_inet_socket_address_new (any, self->discovery_port);
    g_socket_set_broadcast (self->udp, TRUE);
    g_socket_set_blocking (self->udp, FALSE);
    if (g_socket_bind (self->udp, sa, TRUE, &udp_error)) {
      self->udp_source = g_socket_create_source (self->udp, G_IO_IN, NULL);
      g_source_set_callback (self->udp_source, G_SOURCE_FUNC (on_udp_readable), self, NULL);
      g_source_attach (self->udp_source, self->context);
    } else {
      g_clear_object (&self->udp);
    }
  }
  if (udp_error)
    g_message ("LAN discovery disabled: %s", udp_error->message);

  if (self->udp) {
    self->beacon_id = g_timeout_add_seconds (BEACON_INTERVAL, beacon_tick, self);
    beacon_tick (self);
  }
  return TRUE;

fail:
  g_socket_service_stop (self->service);
  g_clear_object (&self->service);
  return FALSE;
}

void
courier_lan_stop (CourierLan *self)
{
  if (self->udp) {
    g_autofree char *bye = hello_json (self, "bye");
    g_autoptr (GInetAddress) addr = g_inet_address_new_from_string (self->broadcast);
    if (addr) {
      g_autoptr (GSocketAddress) sa = g_inet_socket_address_new (addr, self->discovery_port);
      g_socket_send_to (self->udp, sa, bye, strlen (bye), NULL, NULL);
    }
  }
  g_clear_handle_id (&self->beacon_id, g_source_remove);
  if (self->udp_source) {
    g_source_destroy (self->udp_source);
    g_clear_pointer (&self->udp_source, g_source_unref);
  }
  if (self->udp)
    g_socket_close (self->udp, NULL);
  g_clear_object (&self->udp);
  if (self->service) {
    g_signal_handlers_disconnect_by_data (self->service, self);
    g_socket_service_stop (self->service);
    g_socket_listener_close (G_SOCKET_LISTENER (self->service));
    g_clear_object (&self->service);
  }
}

gboolean courier_lan_is_running (CourierLan *self) { return self->service != NULL; }
guint16  courier_lan_get_port   (CourierLan *self) { return self->tcp_port; }
const char *courier_lan_get_id   (CourierLan *self) { return self->id; }
const char *courier_lan_get_name (CourierLan *self) { return self->name; }

void
courier_lan_set_name (CourierLan *self, const char *name)
{
  g_free (self->name);
  self->name = g_strdup (name && *name ? name : g_get_host_name ());
}

void
courier_lan_set_ports (CourierLan *self, guint16 discovery_port, guint16 tcp_port)
{
  self->discovery_port = discovery_port;
  self->tcp_port = tcp_port;
}

void
courier_lan_set_broadcast_address (CourierLan *self, const char *address)
{
  g_free (self->broadcast);
  self->broadcast = g_strdup (address);
}

/* ---------------- outgoing (worker thread) ---------------- */

typedef struct {
  char   *host;
  guint16 port;
  char   *line;   /* JSON request */
  char   *expect; /* expected reply type */
} Request;

static void
request_free (Request *r)
{
  g_free (r->host);
  g_free (r->line);
  g_free (r->expect);
  g_free (r);
}

static void
request_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
  Request *r = data;
  GError *error = NULL;
  g_autoptr (GSocketClient) client = g_socket_client_new ();
  g_socket_client_set_timeout (client, IO_TIMEOUT);
  g_autoptr (GSocketConnection) conn = g_socket_client_connect_to_host (client, r->host, r->port, cancellable, &error);
  if (!conn) {
    g_task_return_error (task, error);
    return;
  }
  g_autofree char *line = g_strconcat (r->line, "\n", NULL);
  if (!g_output_stream_write_all (g_io_stream_get_output_stream (G_IO_STREAM (conn)), line, strlen (line),
                                  NULL, cancellable, &error)) {
    g_task_return_error (task, error);
    return;
  }
  g_autoptr (GDataInputStream) in = g_data_input_stream_new (g_io_stream_get_input_stream (G_IO_STREAM (conn)));
  gsize len;
  g_autofree char *reply = g_data_input_stream_read_line_utf8 (in, &len, cancellable, &error);
  if (!reply) {
    if (!error)
      error = g_error_new (G_IO_ERROR, G_IO_ERROR_CONNECTION_CLOSED, "The other side closed the connection");
    g_task_return_error (task, error);
    return;
  }
  JsonObject *obj = parse_object (reply, len);
  if (!obj || g_strcmp0 (jstr (obj, "type"), r->expect) != 0) {
    if (obj)
      json_object_unref (obj);
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "That address is not a Courier chat peer");
    return;
  }
  g_task_return_pointer (task, obj, (GDestroyNotify) json_object_unref);
}

void
courier_lan_add_peer_async (CourierLan *self, const char *host, guint16 port,
                            GCancellable *c, GAsyncReadyCallback cb, gpointer data)
{
  GTask *task = g_task_new (self, c, cb, data);
  Request *r = g_new0 (Request, 1);
  r->host = g_strdup (host);
  r->port = port ? port : COURIER_LAN_MESSAGE_PORT;
  r->line = hello_json (self, "hello");
  r->expect = g_strdup ("hello");
  g_task_set_task_data (task, r, (GDestroyNotify) request_free);
  g_task_run_in_thread (task, request_thread);
  g_object_unref (task);
}

char *
courier_lan_add_peer_finish (CourierLan *self, GAsyncResult *res, GError **error)
{
  Request *r = g_task_get_task_data (G_TASK (res));
  g_autoptr (JsonObject) obj = g_task_propagate_pointer (G_TASK (res), error);
  if (!obj)
    return NULL;
  const char *id = jstr (obj, "id");
  if (!id || g_str_equal (id, self->id)) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "That's this computer");
    return NULL;
  }
  touch_peer (self, id, jstr (obj, "name"), r->host,
              (guint16) json_object_get_int_member_with_default (obj, "port", r->port), TRUE);
  g_signal_emit (self, signals[SIGNAL_PEERS_CHANGED], 0);
  return g_strdup (id);
}

void
courier_lan_send_async (CourierLan *self, const char *peer_id, const char *text,
                        GCancellable *c, GAsyncReadyCallback cb, gpointer data)
{
  GTask *task = g_task_new (self, c, cb, data);
  const CourierLanPeer *peer = courier_lan_find_peer (self, peer_id);
  if (!peer) {
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Unknown contact");
    g_object_unref (task);
    return;
  }
  if (!text || !*text || strlen (text) > COURIER_LAN_MAX_TEXT) {
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Messages can be up to 16 KB");
    g_object_unref (task);
    return;
  }

  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "app");  json_builder_add_string_value (b, "courier");
  json_builder_set_member_name (b, "type"); json_builder_add_string_value (b, "msg");
  json_builder_set_member_name (b, "id");   json_builder_add_string_value (b, self->id);
  json_builder_set_member_name (b, "name"); json_builder_add_string_value (b, self->name);
  json_builder_set_member_name (b, "port"); json_builder_add_int_value (b, self->tcp_port);
  json_builder_set_member_name (b, "text"); json_builder_add_string_value (b, text);
  json_builder_set_member_name (b, "ts");   json_builder_add_int_value (b, g_get_real_time () / G_USEC_PER_SEC);
  json_builder_end_object (b);
  g_autoptr (JsonNode) root = json_builder_get_root (b);

  Request *r = g_new0 (Request, 1);
  r->host = g_strdup (peer->host);
  r->port = peer->port;
  r->line = json_to_string (root, FALSE);
  r->expect = g_strdup ("ack");
  g_task_set_task_data (task, r, (GDestroyNotify) request_free);
  g_task_run_in_thread (task, request_thread);
  g_object_unref (task);
}

gboolean
courier_lan_send_finish (CourierLan *self, GAsyncResult *res, GError **error)
{
  g_autoptr (JsonObject) obj = g_task_propagate_pointer (G_TASK (res), error);
  return obj != NULL;
}

/* ---------------- GObject ---------------- */

static void
courier_lan_dispose (GObject *object)
{
  courier_lan_stop (COURIER_LAN (object));
  G_OBJECT_CLASS (courier_lan_parent_class)->dispose (object);
}

static void
courier_lan_finalize (GObject *object)
{
  CourierLan *self = COURIER_LAN (object);
  g_free (self->id);
  g_free (self->name);
  g_free (self->broadcast);
  g_hash_table_unref (self->peers);
  g_main_context_unref (self->context);
  G_OBJECT_CLASS (courier_lan_parent_class)->finalize (object);
}

static void
courier_lan_class_init (CourierLanClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = courier_lan_dispose;
  G_OBJECT_CLASS (klass)->finalize = courier_lan_finalize;
  signals[SIGNAL_PEERS_CHANGED] = g_signal_new ("peers-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                                                0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SIGNAL_MESSAGE] = g_signal_new ("message-received", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                                          0, NULL, NULL, NULL, G_TYPE_NONE, 4,
                                          G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT64);
}

static void
courier_lan_init (CourierLan *self)
{
  self->peers = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, (GDestroyNotify) courier_lan_peer_free);
  self->discovery_port = COURIER_LAN_DISCOVERY_PORT;
  self->tcp_port = COURIER_LAN_MESSAGE_PORT;
  self->broadcast = g_strdup ("255.255.255.255");
  self->context = g_main_context_ref_thread_default ();
}

CourierLan *
courier_lan_new (const char *id, const char *name)
{
  CourierLan *self = g_object_new (COURIER_TYPE_LAN, NULL);
  self->id = g_strdup (id);
  courier_lan_set_name (self, name);
  return self;
}
