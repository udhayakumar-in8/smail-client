/* courier-chat-page.c — peers on the LAN and conversations with them */
#include "courier-chat-page.h"
#include "courier-ui.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <string.h>

typedef struct {
  CourierStore     *store;
  CourierLan       *lan;
  GtkWidget        *page;
  GtkWidget        *split;
  GtkWidget        *peer_list;
  GtkWidget        *conv_page;
  GtkWidget        *conv_title;
  GtkWidget        *conv_stack;
  GtkWidget        *messages;
  GtkWidget        *scroll;
  GtkWidget        *entry;
  GtkWidget        *send;
  char             *current;   /* peer id */
  GHashTable       *unread;    /* peer id -> count */
  gboolean          active;
  CourierUnreadFunc unread_func;
  gpointer          unread_data;
} ChatPage;

static ChatPage *
chat (GtkWidget *page)
{
  return g_object_get_data (G_OBJECT (page), "chat-page");
}

static void
chat_free (ChatPage *c)
{
  g_signal_handlers_disconnect_by_data (c->lan, c);
  g_object_unref (c->lan);
  g_hash_table_unref (c->unread);
  g_free (c->current);
  g_free (c);
}

static char *
local_addresses (void)
{
  struct ifaddrs *ifs = NULL;
  GString *out = g_string_new (NULL);
  if (getifaddrs (&ifs) == 0) {
    for (struct ifaddrs *i = ifs; i; i = i->ifa_next) {
      if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET)
        continue;
      char buf[INET_ADDRSTRLEN];
      inet_ntop (AF_INET, &((struct sockaddr_in *) i->ifa_addr)->sin_addr, buf, sizeof buf);
      if (g_str_has_prefix (buf, "127."))
        continue;
      g_string_append_printf (out, "%s%s", out->len ? ", " : "", buf);
    }
    freeifaddrs (ifs);
  }
  return g_string_free (out, FALSE);
}

/* ---------------- unread ---------------- */

static int
total_unread (ChatPage *c)
{
  int total = 0;
  GHashTableIter it;
  gpointer v;
  g_hash_table_iter_init (&it, c->unread);
  while (g_hash_table_iter_next (&it, NULL, &v))
    total += GPOINTER_TO_INT (v);
  return total;
}

static void
notify_unread (ChatPage *c)
{
  if (c->unread_func)
    c->unread_func (total_unread (c), c->unread_data);
}

/* ---------------- bubbles ---------------- */

static GtkWidget *
bubble (const char *text, gboolean outgoing, gint64 ts)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_halign (box, outgoing ? GTK_ALIGN_END : GTK_ALIGN_START);
  gtk_widget_add_css_class (box, "bubble-box");

  GtkWidget *label = gtk_label_new (text);
  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (label), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_selectable (GTK_LABEL (label), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (label), 48);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_widget_add_css_class (label, "bubble");
  if (outgoing)
    gtk_widget_add_css_class (label, "outgoing");
  gtk_box_append (GTK_BOX (box), label);

  g_autofree char *when = courier_format_unix (ts);
  GtkWidget *time = gtk_label_new (when);
  gtk_widget_add_css_class (time, "caption");
  gtk_widget_add_css_class (time, "dim-label");
  gtk_widget_set_halign (time, outgoing ? GTK_ALIGN_END : GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (box), time);

  GtkWidget *row = gtk_list_box_row_new ();
  gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), FALSE);
  gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);
  g_object_set_data (G_OBJECT (row), "time", time);
  return row;
}

static gboolean
scroll_to_end (gpointer data)
{
  GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (data));
  gtk_adjustment_set_value (adj, gtk_adjustment_get_upper (adj));
  return G_SOURCE_REMOVE;
}

static void
append_bubble (ChatPage *c, GtkWidget *row)
{
  gtk_list_box_append (GTK_LIST_BOX (c->messages), row);
  g_timeout_add_full (G_PRIORITY_DEFAULT, 50, scroll_to_end, g_object_ref (c->scroll), g_object_unref);
}

/* ---------------- peers ---------------- */

static void
update_conversation_header (ChatPage *c)
{
  const CourierLanPeer *p = courier_lan_find_peer (c->lan, c->current);
  if (!p)
    return;
  g_autofree char *sub = g_strdup_printf ("%s · %s", courier_lan_peer_online (p) ? "Online" : "Offline", p->host);
  adw_window_title_set_title (ADW_WINDOW_TITLE (c->conv_title), p->name);
  adw_window_title_set_subtitle (ADW_WINDOW_TITLE (c->conv_title), sub);
  adw_navigation_page_set_title (ADW_NAVIGATION_PAGE (c->conv_page), p->name);
}

static void
rebuild_peers (ChatPage *c)
{
  gtk_list_box_remove_all (GTK_LIST_BOX (c->peer_list));
  g_autoptr (GPtrArray) peers = courier_lan_list_peers (c->lan);
  for (guint i = 0; i < peers->len; i++) {
    CourierLanPeer *p = g_ptr_array_index (peers, i);
    gboolean online = courier_lan_peer_online (p);

    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *avatar = adw_avatar_new (32, p->name, TRUE);
    if (!online)
      gtk_widget_set_opacity (avatar, 0.5);
    gtk_box_append (GTK_BOX (box), avatar);
    GtkWidget *labels = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand (labels, TRUE);
    GtkWidget *name = gtk_label_new (p->name);
    gtk_label_set_xalign (GTK_LABEL (name), 0);
    gtk_label_set_ellipsize (GTK_LABEL (name), PANGO_ELLIPSIZE_END);
    g_autofree char *st = g_strdup_printf ("%s · %s", online ? "Online" : "Offline", p->host);
    GtkWidget *status = gtk_label_new (st);
    gtk_label_set_xalign (GTK_LABEL (status), 0);
    gtk_widget_add_css_class (status, "caption");
    gtk_widget_add_css_class (status, online ? "success" : "dim-label");
    gtk_box_append (GTK_BOX (labels), name);
    gtk_box_append (GTK_BOX (labels), status);
    gtk_box_append (GTK_BOX (box), labels);

    int unread = GPOINTER_TO_INT (g_hash_table_lookup (c->unread, p->id));
    if (unread) {
      g_autofree char *n = g_strdup_printf ("%d", unread);
      GtkWidget *badge = gtk_label_new (n);
      gtk_widget_add_css_class (badge, "unread-badge");
      gtk_widget_set_valign (badge, GTK_ALIGN_CENTER);
      gtk_box_append (GTK_BOX (box), badge);
    }

    GtkWidget *row = gtk_list_box_row_new ();
    gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);
    g_object_set_data_full (G_OBJECT (row), "peer-id", g_strdup (p->id), g_free);
    gtk_list_box_append (GTK_LIST_BOX (c->peer_list), row);
    if (g_strcmp0 (p->id, c->current) == 0)
      gtk_list_box_select_row (GTK_LIST_BOX (c->peer_list), GTK_LIST_BOX_ROW (row));
  }
  update_conversation_header (c);
}

static void
open_peer (ChatPage *c, const char *id)
{
  g_free (c->current);
  c->current = g_strdup (id);
  g_hash_table_remove (c->unread, id);
  notify_unread (c);

  gtk_list_box_remove_all (GTK_LIST_BOX (c->messages));
  g_autoptr (GPtrArray) history = courier_store_list_chat_messages (c->store, id, 300);
  for (guint i = 0; i < history->len; i++) {
    CourierChatMessage *m = g_ptr_array_index (history, i);
    gtk_list_box_append (GTK_LIST_BOX (c->messages), bubble (m->text, m->outgoing, m->ts));
  }
  g_timeout_add_full (G_PRIORITY_DEFAULT, 50, scroll_to_end, g_object_ref (c->scroll), g_object_unref);
  gtk_stack_set_visible_child_name (GTK_STACK (c->conv_stack), "chat");
  gtk_widget_set_sensitive (c->entry, TRUE);
  gtk_widget_set_sensitive (c->send, TRUE);
  update_conversation_header (c);
  rebuild_peers (c);
  adw_navigation_split_view_set_show_content (ADW_NAVIGATION_SPLIT_VIEW (c->split), TRUE);
  gtk_widget_grab_focus (c->entry);
}

static void
on_peer_activated (GtkListBox *box, GtkListBoxRow *row, ChatPage *c)
{
  open_peer (c, g_object_get_data (G_OBJECT (row), "peer-id"));
}

static void
on_peers_changed (CourierLan *lan, ChatPage *c)
{
  rebuild_peers (c);
}

static void
on_message_received (CourierLan *lan, const char *id, const char *name, const char *text, gint64 ts, ChatPage *c)
{
  CourierChatMessage m = { 0, (char *) id, (char *) name, FALSE, (char *) text, ts };
  courier_store_add_chat_message (c->store, &m, NULL);

  gboolean visible = c->active && g_strcmp0 (id, c->current) == 0;
  if (g_strcmp0 (id, c->current) == 0)
    append_bubble (c, bubble (text, FALSE, ts));
  if (!visible) {
    g_hash_table_insert (c->unread, g_strdup (id),
                         GINT_TO_POINTER (GPOINTER_TO_INT (g_hash_table_lookup (c->unread, id)) + 1));
    notify_unread (c);
    rebuild_peers (c);
  }

  GtkWindow *win = GTK_WINDOW (gtk_widget_get_root (c->page));
  if (!visible || !win || !gtk_window_is_active (win)) {
    g_autoptr (GNotification) n = g_notification_new (name && *name ? name : "Local chat");
    g_notification_set_body (n, text);
    g_notification_set_default_action (n, "app.activate");
    GApplication *app = g_application_get_default ();
    if (app)
      g_application_send_notification (app, "lan-chat", n);
    g_autofree char *t = g_strdup_printf ("%s: %s", name && *name ? name : "Someone", text);
    courier_toast (c->page, t);
  }
}

/* ---------------- sending ---------------- */

typedef struct {
  ChatPage  *chat;
  GtkWidget *row;
  char      *peer_id;
  char      *peer_name;
  char      *text;
  GtkWidget *page;
} SendRequest;

static void
on_sent (GObject *src, GAsyncResult *res, gpointer data)
{
  SendRequest *r = data;
  g_autoptr (GError) error = NULL;
  if (courier_lan_send_finish (COURIER_LAN (src), res, &error)) {
    CourierChatMessage m = { 0, r->peer_id, r->peer_name, TRUE, r->text, courier_unix_now () };
    courier_store_add_chat_message (r->chat->store, &m, NULL);
  } else {
    GtkWidget *time = g_object_get_data (G_OBJECT (r->row), "time");
    gtk_label_set_text (GTK_LABEL (time), "Not delivered");
    gtk_widget_add_css_class (time, "error");
    g_autofree char *msg = g_strdup_printf ("Couldn't send: %s", error->message);
    courier_toast (r->page, msg);
  }
  g_object_unref (r->row);
  g_object_unref (r->page);
  g_free (r->peer_id);
  g_free (r->peer_name);
  g_free (r->text);
  g_free (r);
}

static void
send_current (ChatPage *c)
{
  const char *raw = gtk_editable_get_text (GTK_EDITABLE (c->entry));
  g_autofree char *text = g_strstrip (g_strdup (raw));
  if (!*text || !c->current)
    return;
  const CourierLanPeer *peer = courier_lan_find_peer (c->lan, c->current);
  if (!peer) {
    courier_toast (c->page, "This contact is no longer available");
    return;
  }
  GtkWidget *row = bubble (text, TRUE, courier_unix_now ());
  append_bubble (c, row);
  gtk_editable_set_text (GTK_EDITABLE (c->entry), "");

  SendRequest *r = g_new0 (SendRequest, 1);
  r->chat = c;
  r->row = g_object_ref (row);
  r->page = g_object_ref (c->page);
  r->peer_id = g_strdup (peer->id);
  r->peer_name = g_strdup (peer->name);
  r->text = g_strdup (text);
  courier_lan_send_async (c->lan, peer->id, text, NULL, on_sent, r);
}

static void
on_send (GtkWidget *w, ChatPage *c)
{
  send_current (c);
}

/* ---------------- add by address ---------------- */

static void
on_peer_added (GObject *src, GAsyncResult *res, gpointer data)
{
  GtkWidget *page = data;
  g_autoptr (GError) error = NULL;
  g_autofree char *id = courier_lan_add_peer_finish (COURIER_LAN (src), res, &error);
  if (!id) {
    g_autofree char *msg = g_strdup_printf ("Couldn't connect: %s", error->message);
    courier_toast (page, msg);
  } else {
    open_peer (chat (page), id);
  }
  g_object_unref (page);
}

static void
on_add_response (AdwAlertDialog *dialog, const char *response, GtkWidget *page)
{
  if (!g_str_equal (response, "add"))
    return;
  GtkWidget *entry = adw_alert_dialog_get_extra_child (dialog);
  g_autofree char *text = g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (entry))));
  if (!*text)
    return;
  guint16 port = COURIER_LAN_MESSAGE_PORT;
  char *colon = strrchr (text, ':');
  if (colon && !strchr (colon + 1, ']') && strchr (text, ':') == colon) {
    *colon = '\0';
    port = (guint16) atoi (colon + 1);
  }
  courier_lan_add_peer_async (chat (page)->lan, text, port, NULL, on_peer_added, g_object_ref (page));
}

static void
on_add_peer (GtkButton *b, GtkWidget *page)
{
  AdwDialog *d = adw_alert_dialog_new ("Add by Address",
                                       "Enter the IP address of a computer on your network that has "
                                       "Courier open, for example 192.168.1.20");
  GtkWidget *entry = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (entry), "192.168.1.20");
  gtk_entry_set_activates_default (GTK_ENTRY (entry), TRUE);
  adw_alert_dialog_set_extra_child (ADW_ALERT_DIALOG (d), entry);
  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (d), "cancel", "_Cancel", "add", "_Connect", NULL);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (d), "add", ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (d), "add");
  g_signal_connect (d, "response", G_CALLBACK (on_add_response), page);
  adw_dialog_present (d, page);
  gtk_widget_grab_focus (entry);
}

/* ---------------- public ---------------- */

void
courier_chat_page_set_unread_func (GtkWidget *page, CourierUnreadFunc func, gpointer data)
{
  ChatPage *c = chat (page);
  c->unread_func = func;
  c->unread_data = data;
}

void
courier_chat_page_set_active (GtkWidget *page, gboolean active)
{
  ChatPage *c = chat (page);
  c->active = active;
  if (active && c->current) {
    g_hash_table_remove (c->unread, c->current);
    notify_unread (c);
    rebuild_peers (c);
  }
}

GtkWidget *
courier_chat_page_get_split (GtkWidget *page)
{
  return chat (page)->split;
}

GtkWidget *
courier_chat_page_new (CourierStore *store, CourierLan *lan, const char *lan_error)
{
  ChatPage *c = g_new0 (ChatPage, 1);
  c->store = store;
  c->lan = g_object_ref (lan);
  c->unread = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  /* --- peers (sidebar) --- */
  GtkWidget *side = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *banner = adw_banner_new (lan_error ? lan_error : "");
  adw_banner_set_revealed (ADW_BANNER (banner), lan_error != NULL);
  gtk_box_append (GTK_BOX (side), banner);

  g_autofree char *addrs = local_addresses ();
  g_autofree char *me = g_strdup_printf ("You appear as “%s”%s%s", courier_lan_get_name (lan),
                                         *addrs ? " on " : "", addrs);
  GtkWidget *me_label = gtk_label_new (me);
  gtk_label_set_wrap (GTK_LABEL (me_label), TRUE);
  gtk_label_set_xalign (GTK_LABEL (me_label), 0);
  gtk_widget_add_css_class (me_label, "caption");
  gtk_widget_add_css_class (me_label, "dim-label");
  gtk_widget_add_css_class (me_label, "chat-me");
  gtk_box_append (GTK_BOX (side), me_label);

  c->peer_list = gtk_list_box_new ();
  gtk_widget_add_css_class (c->peer_list, "navigation-sidebar");
  GtkWidget *empty = gtk_label_new ("Looking for people on your network…\n\n"
                                    "Others appear here when they open Courier on the same Wi-Fi or LAN.");
  gtk_label_set_wrap (GTK_LABEL (empty), TRUE);
  gtk_label_set_justify (GTK_LABEL (empty), GTK_JUSTIFY_CENTER);
  gtk_widget_add_css_class (empty, "dim-label");
  gtk_widget_add_css_class (empty, "list-placeholder");
  gtk_list_box_set_placeholder (GTK_LIST_BOX (c->peer_list), empty);
  g_signal_connect (c->peer_list, "row-activated", G_CALLBACK (on_peer_activated), c);
  GtkWidget *pscroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (pscroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (pscroll), c->peer_list);
  gtk_widget_set_vexpand (pscroll, TRUE);
  gtk_box_append (GTK_BOX (side), pscroll);

  GtkWidget *side_header;
  GtkWidget *side_toolbar = courier_page_new ("Local Chat", side, &side_header);
  GtkWidget *add = courier_icon_button ("list-add-symbolic", "Add by Address");
  adw_header_bar_pack_end (ADW_HEADER_BAR (side_header), add);

  /* --- conversation --- */
  GtkWidget *nobody = adw_status_page_new ();
  adw_status_page_set_icon_name (ADW_STATUS_PAGE (nobody), "network-workgroup-symbolic");
  adw_status_page_set_title (ADW_STATUS_PAGE (nobody), "Local Chat");
  adw_status_page_set_description (ADW_STATUS_PAGE (nobody),
    "Message people on the same network without the internet. Pick someone from the list.");

  c->messages = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (c->messages), GTK_SELECTION_NONE);
  gtk_widget_add_css_class (c->messages, "chat-messages");
  c->scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (c->scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (c->scroll), c->messages);
  gtk_widget_set_vexpand (c->scroll, TRUE);

  c->conv_stack = gtk_stack_new ();
  gtk_stack_add_named (GTK_STACK (c->conv_stack), nobody, "empty");
  gtk_stack_add_named (GTK_STACK (c->conv_stack), c->scroll, "chat");

  GtkWidget *conv_toolbar = adw_toolbar_view_new ();
  GtkWidget *conv_header = adw_header_bar_new ();
  c->conv_title = adw_window_title_new ("", NULL);
  adw_header_bar_set_title_widget (ADW_HEADER_BAR (conv_header), c->conv_title);
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (conv_toolbar), conv_header);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (conv_toolbar), c->conv_stack);

  GtkWidget *compose = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class (compose, "chat-compose");
  c->entry = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (c->entry), "Message");
  gtk_entry_set_max_length (GTK_ENTRY (c->entry), 4000);
  gtk_widget_set_hexpand (c->entry, TRUE);
  c->send = courier_icon_button ("mail-send-symbolic", "Send");
  gtk_widget_add_css_class (c->send, "suggested-action");
  gtk_widget_add_css_class (c->send, "circular");
  gtk_widget_set_sensitive (c->entry, FALSE);
  gtk_widget_set_sensitive (c->send, FALSE);
  g_signal_connect (c->entry, "activate", G_CALLBACK (on_send), c);
  g_signal_connect (c->send, "clicked", G_CALLBACK (on_send), c);
  gtk_box_append (GTK_BOX (compose), c->entry);
  gtk_box_append (GTK_BOX (compose), c->send);
  adw_toolbar_view_add_bottom_bar (ADW_TOOLBAR_VIEW (conv_toolbar), compose);

  c->split = adw_navigation_split_view_new ();
  adw_navigation_split_view_set_sidebar (ADW_NAVIGATION_SPLIT_VIEW (c->split),
                                         adw_navigation_page_new (side_toolbar, "Local Chat"));
  c->conv_page = GTK_WIDGET (adw_navigation_page_new (conv_toolbar, "Conversation"));
  adw_navigation_split_view_set_content (ADW_NAVIGATION_SPLIT_VIEW (c->split), ADW_NAVIGATION_PAGE (c->conv_page));
  adw_navigation_split_view_set_min_sidebar_width (ADW_NAVIGATION_SPLIT_VIEW (c->split), 240);

  c->page = c->split;
  g_object_set_data_full (G_OBJECT (c->page), "chat-page", c, (GDestroyNotify) chat_free);
  g_signal_connect (add, "clicked", G_CALLBACK (on_add_peer), c->page);
  g_signal_connect (lan, "peers-changed", G_CALLBACK (on_peers_changed), c);
  g_signal_connect (lan, "message-received", G_CALLBACK (on_message_received), c);
  rebuild_peers (c);
  return c->page;
}
