/* smail-imap.h — minimal synchronous IMAP4rev1 client (run it from a worker thread) */
#pragma once

#include "smail-types.h"

G_BEGIN_DECLS

#define SMAIL_IMAP_ERROR (smail_imap_error_quark ())
GQuark smail_imap_error_quark (void);

typedef enum {
  SMAIL_IMAP_ERROR_NO,       /* server answered NO  */
  SMAIL_IMAP_ERROR_BAD,      /* server answered BAD */
  SMAIL_IMAP_ERROR_PROTOCOL, /* unparsable reply    */
} SMailImapError;

typedef struct _SMailImap SMailImap;

SMailImap *smail_imap_connect      (const SMailAccount *account, GCancellable *cancellable, GError **error);
void         smail_imap_free         (SMailImap *imap);
void         smail_imap_logout       (SMailImap *imap);
gboolean     smail_imap_is_connected (SMailImap *imap);
gboolean     smail_imap_has_capability (SMailImap *imap, const char *cap);

GPtrArray   *smail_imap_list_folders  (SMailImap *imap, GError **error);   /* SMailFolder* */
GPtrArray   *smail_imap_list_messages (SMailImap *imap, const char *folder, guint limit, GError **error); /* SMailMessage* */
GBytes      *smail_imap_fetch_message (SMailImap *imap, const char *folder, const char *uid, GError **error);
gboolean     smail_imap_store_flag    (SMailImap *imap, const char *folder, const char *uid,
                                         const char *flag, gboolean add, GError **error);
gboolean     smail_imap_move          (SMailImap *imap, const char *folder, const char *uid,
                                         const char *dest, GError **error);
gboolean     smail_imap_delete        (SMailImap *imap, const char *folder, const char *uid, GError **error);
GPtrArray   *smail_imap_search         (SMailImap *imap, const char *folder, const char *query,
                                        guint limit, GError **error);           /* SMailMessage* */
gboolean     smail_imap_create_folder  (SMailImap *imap, const char *name, GError **error);
gboolean     smail_imap_rename_folder  (SMailImap *imap, const char *from, const char *to, GError **error);
gboolean     smail_imap_delete_folder  (SMailImap *imap, const char *name, GError **error);
gboolean     smail_imap_append        (SMailImap *imap, const char *folder, GBytes *message,
                                         const char *flags, GError **error);

char *smail_imap_utf7_encode (const char *utf8);
char *smail_imap_utf7_decode (const char *imap);

G_END_DECLS
