/* courier-types.h — data types shared across the app */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

/* ---------- Account ---------- */

typedef enum {
  COURIER_SECURITY_SSL,      /* implicit TLS (993 / 465) */
  COURIER_SECURITY_STARTTLS, /* upgrade (143 / 587)      */
  COURIER_SECURITY_NONE,
} CourierSecurity;

typedef struct {
  char *id;
  char *name;
  char *email;
  char *username;
  char *password;            /* not persisted in the key file */
  char *imap_host;
  guint imap_port;
  CourierSecurity imap_security;
  char *smtp_host;
  guint smtp_port;
  CourierSecurity smtp_security;
} CourierAccount;

CourierAccount *courier_account_new  (void);
CourierAccount *courier_account_copy (const CourierAccount *account);
void            courier_account_free (CourierAccount *account);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (CourierAccount, courier_account_free)

/* ---------- Folder ---------- */

typedef struct {
  char *path;          /* server name, decoded UTF-8 */
  char *display_name;
  char *role;          /* "inbox", "sent", "trash", ... or NULL */
  int   unread;
} CourierFolder;

CourierFolder *courier_folder_new   (const char *path, const char *display_name, const char *role);
CourierFolder *courier_folder_copy  (const CourierFolder *folder);
void           courier_folder_free  (CourierFolder *folder);
const char    *courier_folder_get_icon (const CourierFolder *folder);
int            courier_folder_compare  (gconstpointer a, gconstpointer b); /* for GPtrArray sort */

/* ---------- Message summary (list row) ---------- */

#define COURIER_TYPE_MESSAGE (courier_message_get_type ())
G_DECLARE_FINAL_TYPE (CourierMessage, courier_message, COURIER, MESSAGE, GObject)

struct _CourierMessage {
  GObject    parent_instance;
  char      *uid;
  char      *subject;
  char      *sender_name;
  char      *sender_addr;
  char      *recipient;   /* first To: name or address, shown in Sent/Drafts */
  char      *to;          /* full To: list */
  GDateTime *date;
  gboolean   seen;
  gboolean   flagged;
  gboolean   has_attachment;
};

CourierMessage *courier_message_new_from_headers (const char *uid, GBytes *headers,
                                                  gboolean seen, gboolean flagged);
const char *courier_message_get_sender (CourierMessage *self);
void        courier_message_set_seen    (CourierMessage *self, gboolean seen);
void        courier_message_set_flagged (CourierMessage *self, gboolean flagged);

/* ---------- Full parsed message ---------- */

typedef struct {
  char   *filename;
  char   *content_type;
  char   *content_id;
  GBytes *data;
  gboolean is_inline;
} CourierAttachment;

typedef struct {
  char      *uid;
  char      *subject;
  char      *from_name;
  char      *from_addr;
  char      *to;          /* display strings */
  char      *cc;
  char      *reply_to;    /* raw address list, may be NULL */
  GDateTime *date;
  char      *message_id;
  char      *references;
  char      *html;
  char      *text;
  GPtrArray *attachments; /* CourierAttachment*, real attachments            */
  GPtrArray *inline_parts;/* CourierAttachment*, cid:-referenced inline parts */
} CourierMessageDetail;

CourierMessageDetail *courier_message_detail_parse (const char *uid, GBytes *raw, GError **error);
void                  courier_message_detail_free  (CourierMessageDetail *detail);
void                  courier_attachment_free      (CourierAttachment *attachment);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (CourierMessageDetail, courier_message_detail_free)

G_END_DECLS
