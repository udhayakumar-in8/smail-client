/* smail-types.h — data types shared across the app */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

/* ---------- Account ---------- */

typedef enum {
  SMAIL_SECURITY_SSL,      /* implicit TLS (993 / 465) */
  SMAIL_SECURITY_STARTTLS, /* upgrade (143 / 587)      */
  SMAIL_SECURITY_NONE,
} SMailSecurity;

typedef struct {
  char *id;
  char *name;
  char *email;
  char *username;
  char *password;            /* not persisted in the key file */
  char *imap_host;
  guint imap_port;
  SMailSecurity imap_security;
  char *smtp_host;
  guint smtp_port;
  SMailSecurity smtp_security;
} SMailAccount;

SMailAccount *smail_account_new  (void);
SMailAccount *smail_account_copy (const SMailAccount *account);
void            smail_account_free (SMailAccount *account);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (SMailAccount, smail_account_free)

/* ---------- Folder ---------- */

typedef struct {
  char *path;          /* server name, decoded UTF-8 */
  char *display_name;
  char *role;          /* "inbox", "sent", "trash", ... or NULL */
  int   unread;
} SMailFolder;

SMailFolder *smail_folder_new   (const char *path, const char *display_name, const char *role);
SMailFolder *smail_folder_copy  (const SMailFolder *folder);
void           smail_folder_free  (SMailFolder *folder);
const char    *smail_folder_get_icon (const SMailFolder *folder);
int            smail_folder_compare  (gconstpointer a, gconstpointer b); /* for GPtrArray sort */

/* ---------- Message summary (list row) ---------- */

#define SMAIL_TYPE_MESSAGE (smail_message_get_type ())
G_DECLARE_FINAL_TYPE (SMailMessage, smail_message, SMAIL, MESSAGE, GObject)

struct _SMailMessage {
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

SMailMessage *smail_message_new_from_headers (const char *uid, GBytes *headers,
                                                  gboolean seen, gboolean flagged);
const char *smail_message_get_sender (SMailMessage *self);
void        smail_message_set_seen    (SMailMessage *self, gboolean seen);
void        smail_message_set_flagged (SMailMessage *self, gboolean flagged);

/* ---------- Full parsed message ---------- */

typedef struct {
  char   *filename;
  char   *content_type;
  char   *content_id;
  GBytes *data;
  gboolean is_inline;
} SMailAttachment;

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
  GPtrArray *attachments; /* SMailAttachment*, real attachments            */
  GPtrArray *inline_parts;/* SMailAttachment*, cid:-referenced inline parts */
} SMailMessageDetail;

SMailMessageDetail *smail_message_detail_parse (const char *uid, GBytes *raw, GError **error);
void                  smail_message_detail_free  (SMailMessageDetail *detail);
void                  smail_attachment_free      (SMailAttachment *attachment);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (SMailMessageDetail, smail_message_detail_free)

G_END_DECLS
