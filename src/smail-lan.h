/* smail-lan.h — chat with other SMail users on the same local network.
 *
 * Discovery: every few seconds a small JSON "hello" is broadcast over UDP
 * (default port 47820). Messages: one JSON line per TCP connection to the
 * peer's message port (default 47821), answered with an acknowledgement.
 * Peers can also be added by IP address when broadcasts are blocked.
 * Traffic is not encrypted: use it only on networks you trust. */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

#define SMAIL_LAN_DISCOVERY_PORT 47820
#define SMAIL_LAN_MESSAGE_PORT   47821
#define SMAIL_LAN_MAX_TEXT       (16 * 1024)

typedef struct {
  char    *id;
  char    *name;
  char    *host;
  guint16  port;
  gint64   last_seen;   /* monotonic seconds */
  gboolean manual;      /* added by address */
} SMailLanPeer;

void            smail_lan_peer_free   (SMailLanPeer *peer);
gboolean        smail_lan_peer_online (const SMailLanPeer *peer);

#define SMAIL_TYPE_LAN (smail_lan_get_type ())
G_DECLARE_FINAL_TYPE (SMailLan, smail_lan, SMAIL, LAN, GObject)

/* Signals:
 *   "peers-changed"    ()
 *   "message-received" (const char *peer_id, const char *peer_name, const char *text, gint64 unix_time) */

SMailLan *smail_lan_new (const char *id, const char *name);
/* Must be called before start. tcp_port 0 picks a free port. */
void        smail_lan_set_ports             (SMailLan *self, guint16 discovery_port, guint16 tcp_port);
void        smail_lan_set_broadcast_address (SMailLan *self, const char *address);
gboolean    smail_lan_start   (SMailLan *self, GError **error);
void        smail_lan_stop    (SMailLan *self);
gboolean    smail_lan_is_running (SMailLan *self);
guint16     smail_lan_get_port   (SMailLan *self);
const char *smail_lan_get_id     (SMailLan *self);
const char *smail_lan_get_name   (SMailLan *self);
void        smail_lan_set_name   (SMailLan *self, const char *name);

GPtrArray            *smail_lan_list_peers (SMailLan *self);   /* SMailLanPeer* copies, sorted */
const SMailLanPeer *smail_lan_find_peer  (SMailLan *self, const char *id);

/* Add a peer by address: asks it who it is. Result: the peer id (char*, transfer full). */
void   smail_lan_add_peer_async  (SMailLan *self, const char *host, guint16 port,
                                    GCancellable *c, GAsyncReadyCallback cb, gpointer data);
char  *smail_lan_add_peer_finish (SMailLan *self, GAsyncResult *res, GError **error);

void     smail_lan_send_async  (SMailLan *self, const char *peer_id, const char *text,
                                  GCancellable *c, GAsyncReadyCallback cb, gpointer data);
gboolean smail_lan_send_finish (SMailLan *self, GAsyncResult *res, GError **error);

G_END_DECLS
