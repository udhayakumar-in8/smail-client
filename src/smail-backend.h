/* smail-backend.h — asynchronous mail store API (threaded, GTask based) */
#pragma once

#include "smail-types.h"

G_BEGIN_DECLS

#define SMAIL_TYPE_BACKEND (smail_backend_get_type ())
G_DECLARE_DERIVABLE_TYPE (SMailBackend, smail_backend, SMAIL, BACKEND, GObject)

/* All vfuncs are called from a worker thread with the backend lock held. */
struct _SMailBackendClass {
  GObjectClass parent_class;

  GPtrArray *(*list_folders)  (SMailBackend *self, GError **error);
  GPtrArray *(*list_messages) (SMailBackend *self, const char *folder, guint limit, GError **error);
  GBytes    *(*fetch_message) (SMailBackend *self, const char *folder, const char *uid, GError **error);
  gboolean   (*set_flag)      (SMailBackend *self, const char *folder, const char *uid,
                               const char *flag, gboolean add, GError **error);
  gboolean   (*move)          (SMailBackend *self, const char *folder, const char *uid,
                               const char *dest, GError **error);
  gboolean   (*remove)        (SMailBackend *self, const char *folder, const char *uid, GError **error);
  gboolean   (*append)        (SMailBackend *self, const char *folder, GBytes *message,
                               const char *flags, GError **error);
  GPtrArray *(*search)        (SMailBackend *self, const char *folder, const char *query,
                               guint limit, GError **error);
  gboolean   (*create_folder) (SMailBackend *self, const char *name, GError **error);
  gboolean   (*rename_folder) (SMailBackend *self, const char *from, const char *to, GError **error);
  gboolean   (*delete_folder) (SMailBackend *self, const char *name, GError **error);
  gboolean   (*send)          (SMailBackend *self, GBytes *message, const char *from,
                               const char * const *recipients, GError **error);
  /* Whether the server files sent mail on its own (Gmail, Outlook...) */
  gboolean   (*saves_sent)    (SMailBackend *self);
};

SMailBackend *smail_backend_new_imap (const SMailAccount *account);
SMailBackend *smail_backend_new_demo (void);

const SMailAccount *smail_backend_get_account (SMailBackend *self);
const SMailFolder  *smail_backend_find_role   (SMailBackend *self, const char *role);
gboolean              smail_backend_is_demo     (SMailBackend *self);

/* Async operations. Results:
 *   list_folders  -> GPtrArray<SMailFolder*>  (transfer full)
 *   list_messages -> GPtrArray<SMailMessage*> (transfer full)
 *   fetch_message -> SMailMessageDetail*      (transfer full)
 *   everything else -> gboolean                                 */
void       smail_backend_list_folders  (SMailBackend *self, GCancellable *c,
                                          GAsyncReadyCallback cb, gpointer data);
void       smail_backend_list_messages (SMailBackend *self, const char *folder, guint limit,
                                          GCancellable *c, GAsyncReadyCallback cb, gpointer data);
void       smail_backend_fetch_message (SMailBackend *self, const char *folder, const char *uid,
                                          GCancellable *c, GAsyncReadyCallback cb, gpointer data);
void       smail_backend_set_flag      (SMailBackend *self, const char *folder, const char *uid,
                                          const char *flag, gboolean add,
                                          GAsyncReadyCallback cb, gpointer data);
void       smail_backend_archive       (SMailBackend *self, const char *folder, const char *uid,
                                          GAsyncReadyCallback cb, gpointer data);
void       smail_backend_move          (SMailBackend *self, const char *folder, const char *uid,
                                          const char *dest, GAsyncReadyCallback cb, gpointer data);
void       smail_backend_trash         (SMailBackend *self, const char *folder, const char *uid,
                                          GAsyncReadyCallback cb, gpointer data);
void       smail_backend_send          (SMailBackend *self, GBytes *message, const char *from,
                                          const char * const *recipients,
                                          GAsyncReadyCallback cb, gpointer data);

/* search -> GPtrArray<SMailMessage*>; the rest -> gboolean */
void       smail_backend_search          (SMailBackend *self, const char *folder, const char *query, guint limit,
                                          GCancellable *c, GAsyncReadyCallback cb, gpointer data);
void       smail_backend_append          (SMailBackend *self, const char *folder, GBytes *message,
                                          const char *flags, GAsyncReadyCallback cb, gpointer data);
void       smail_backend_create_folder   (SMailBackend *self, const char *name,
                                          GAsyncReadyCallback cb, gpointer data);
void       smail_backend_rename_folder   (SMailBackend *self, const char *from, const char *to,
                                          GAsyncReadyCallback cb, gpointer data);
void       smail_backend_delete_folder   (SMailBackend *self, const char *name,
                                          GAsyncReadyCallback cb, gpointer data);
/* Folder separator used by the server ("/" or "."), known after list_folders. */
const char *smail_backend_get_delimiter  (SMailBackend *self);

gpointer   smail_backend_finish_pointer (SMailBackend *self, GAsyncResult *res, GError **error);
gboolean   smail_backend_finish_boolean (SMailBackend *self, GAsyncResult *res, GError **error);

/* Synchronous login test used by the account dialog (call from a thread). */
gboolean   smail_backend_test_account (const SMailAccount *account, GError **error);

/* SMTP (smail-smtp.c) */
gboolean   smail_smtp_send (const SMailAccount *account, GBytes *message, const char *from,
                              const char * const *recipients, GError **error);

G_END_DECLS
