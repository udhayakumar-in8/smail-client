/* courier-lan.h — chat with other Courier users on the same local network.
 *
 * Discovery: every few seconds a small JSON "hello" is broadcast over UDP
 * (default port 47820). Messages: one JSON line per TCP connection to the
 * peer's message port (default 47821), answered with an acknowledgement.
 * Peers can also be added by IP address when broadcasts are blocked.
 * Traffic is not encrypted: use it only on networks you trust. */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

#define COURIER_LAN_DISCOVERY_PORT 47820
#define COURIER_LAN_MESSAGE_PORT   47821
#define COURIER_LAN_MAX_TEXT       (16 * 1024)

typedef struct {
  char    *id;
  char    *name;
  char    *host;
  guint16  port;
  gint64   last_seen;   /* monotonic seconds */
  gboolean manual;      /* added by address */
} CourierLanPeer;

void            courier_lan_peer_free   (CourierLanPeer *peer);
gboolean        courier_lan_peer_online (const CourierLanPeer *peer);

#define COURIER_TYPE_LAN (courier_lan_get_type ())
G_DECLARE_FINAL_TYPE (CourierLan, courier_lan, COURIER, LAN, GObject)

/* Signals:
 *   "peers-changed"    ()
 *   "message-received" (const char *peer_id, const char *peer_name, const char *text, gint64 unix_time) */

CourierLan *courier_lan_new (const char *id, const char *name);
/* Must be called before start. tcp_port 0 picks a free port. */
void        courier_lan_set_ports             (CourierLan *self, guint16 discovery_port, guint16 tcp_port);
void        courier_lan_set_broadcast_address (CourierLan *self, const char *address);
gboolean    courier_lan_start   (CourierLan *self, GError **error);
void        courier_lan_stop    (CourierLan *self);
gboolean    courier_lan_is_running (CourierLan *self);
guint16     courier_lan_get_port   (CourierLan *self);
const char *courier_lan_get_id     (CourierLan *self);
const char *courier_lan_get_name   (CourierLan *self);
void        courier_lan_set_name   (CourierLan *self, const char *name);

GPtrArray            *courier_lan_list_peers (CourierLan *self);   /* CourierLanPeer* copies, sorted */
const CourierLanPeer *courier_lan_find_peer  (CourierLan *self, const char *id);

/* Add a peer by address: asks it who it is. Result: the peer id (char*, transfer full). */
void   courier_lan_add_peer_async  (CourierLan *self, const char *host, guint16 port,
                                    GCancellable *c, GAsyncReadyCallback cb, gpointer data);
char  *courier_lan_add_peer_finish (CourierLan *self, GAsyncResult *res, GError **error);

void     courier_lan_send_async  (CourierLan *self, const char *peer_id, const char *text,
                                  GCancellable *c, GAsyncReadyCallback cb, gpointer data);
gboolean courier_lan_send_finish (CourierLan *self, GAsyncResult *res, GError **error);

G_END_DECLS
