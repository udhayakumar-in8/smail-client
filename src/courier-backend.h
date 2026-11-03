/* courier-backend.h — asynchronous mail store API (threaded, GTask based) */
#pragma once

#include "courier-types.h"

G_BEGIN_DECLS

#define COURIER_TYPE_BACKEND (courier_backend_get_type ())
G_DECLARE_DERIVABLE_TYPE (CourierBackend, courier_backend, COURIER, BACKEND, GObject)

/* All vfuncs are called from a worker thread with the backend lock held. */
struct _CourierBackendClass {
  GObjectClass parent_class;

  GPtrArray *(*list_folders)  (CourierBackend *self, GError **error);
  GPtrArray *(*list_messages) (CourierBackend *self, const char *folder, guint limit, GError **error);
  GBytes    *(*fetch_message) (CourierBackend *self, const char *folder, const char *uid, GError **error);
  gboolean   (*set_flag)      (CourierBackend *self, const char *folder, const char *uid,
                               const char *flag, gboolean add, GError **error);
  gboolean   (*move)          (CourierBackend *self, const char *folder, const char *uid,
                               const char *dest, GError **error);
  gboolean   (*remove)        (CourierBackend *self, const char *folder, const char *uid, GError **error);
  gboolean   (*append)        (CourierBackend *self, const char *folder, GBytes *message, GError **error);
  gboolean   (*send)          (CourierBackend *self, GBytes *message, const char *from,
                               const char * const *recipients, GError **error);
  /* Whether the server files sent mail on its own (Gmail, Outlook...) */
  gboolean   (*saves_sent)    (CourierBackend *self);
};

CourierBackend *courier_backend_new_imap (const CourierAccount *account);
CourierBackend *courier_backend_new_demo (void);

const CourierAccount *courier_backend_get_account (CourierBackend *self);
const CourierFolder  *courier_backend_find_role   (CourierBackend *self, const char *role);
gboolean              courier_backend_is_demo     (CourierBackend *self);

/* Async operations. Results:
 *   list_folders  -> GPtrArray<CourierFolder*>  (transfer full)
 *   list_messages -> GPtrArray<CourierMessage*> (transfer full)
 *   fetch_message -> CourierMessageDetail*      (transfer full)
 *   everything else -> gboolean                                 */
void       courier_backend_list_folders  (CourierBackend *self, GCancellable *c,
                                          GAsyncReadyCallback cb, gpointer data);
void       courier_backend_list_messages (CourierBackend *self, const char *folder, guint limit,
                                          GCancellable *c, GAsyncReadyCallback cb, gpointer data);
void       courier_backend_fetch_message (CourierBackend *self, const char *folder, const char *uid,
                                          GCancellable *c, GAsyncReadyCallback cb, gpointer data);
void       courier_backend_set_flag      (CourierBackend *self, const char *folder, const char *uid,
                                          const char *flag, gboolean add,
                                          GAsyncReadyCallback cb, gpointer data);
void       courier_backend_archive       (CourierBackend *self, const char *folder, const char *uid,
                                          GAsyncReadyCallback cb, gpointer data);
void       courier_backend_move          (CourierBackend *self, const char *folder, const char *uid,
                                          const char *dest, GAsyncReadyCallback cb, gpointer data);
void       courier_backend_trash         (CourierBackend *self, const char *folder, const char *uid,
                                          GAsyncReadyCallback cb, gpointer data);
void       courier_backend_send          (CourierBackend *self, GBytes *message, const char *from,
                                          const char * const *recipients,
                                          GAsyncReadyCallback cb, gpointer data);

gpointer   courier_backend_finish_pointer (CourierBackend *self, GAsyncResult *res, GError **error);
gboolean   courier_backend_finish_boolean (CourierBackend *self, GAsyncResult *res, GError **error);

/* Synchronous login test used by the account dialog (call from a thread). */
gboolean   courier_backend_test_account (const CourierAccount *account, GError **error);

/* SMTP (courier-smtp.c) */
gboolean   courier_smtp_send (const CourierAccount *account, GBytes *message, const char *from,
                              const char * const *recipients, GError **error);

G_END_DECLS
