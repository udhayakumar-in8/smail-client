/* courier-imap.h — minimal synchronous IMAP4rev1 client (run it from a worker thread) */
#pragma once

#include "courier-types.h"

G_BEGIN_DECLS

#define COURIER_IMAP_ERROR (courier_imap_error_quark ())
GQuark courier_imap_error_quark (void);

typedef enum {
  COURIER_IMAP_ERROR_NO,       /* server answered NO  */
  COURIER_IMAP_ERROR_BAD,      /* server answered BAD */
  COURIER_IMAP_ERROR_PROTOCOL, /* unparsable reply    */
} CourierImapError;

typedef struct _CourierImap CourierImap;

CourierImap *courier_imap_connect      (const CourierAccount *account, GCancellable *cancellable, GError **error);
void         courier_imap_free         (CourierImap *imap);
void         courier_imap_logout       (CourierImap *imap);
gboolean     courier_imap_is_connected (CourierImap *imap);
gboolean     courier_imap_has_capability (CourierImap *imap, const char *cap);

GPtrArray   *courier_imap_list_folders  (CourierImap *imap, GError **error);   /* CourierFolder* */
GPtrArray   *courier_imap_list_messages (CourierImap *imap, const char *folder, guint limit, GError **error); /* CourierMessage* */
GBytes      *courier_imap_fetch_message (CourierImap *imap, const char *folder, const char *uid, GError **error);
gboolean     courier_imap_store_flag    (CourierImap *imap, const char *folder, const char *uid,
                                         const char *flag, gboolean add, GError **error);
gboolean     courier_imap_move          (CourierImap *imap, const char *folder, const char *uid,
                                         const char *dest, GError **error);
gboolean     courier_imap_delete        (CourierImap *imap, const char *folder, const char *uid, GError **error);
gboolean     courier_imap_append        (CourierImap *imap, const char *folder, GBytes *message,
                                         const char *flags, GError **error);

char *courier_imap_utf7_encode (const char *utf8);
char *courier_imap_utf7_decode (const char *imap);

G_END_DECLS
