/* courier-backend.c — threaded dispatch + IMAP implementation */
#include "courier-backend.h"
#include "courier-imap.h"

#include <string.h>

/* ======================= base class ======================= */

typedef struct {
  CourierAccount *account;
  GPtrArray      *folders; /* last listed folders, for role lookup */
  GMutex          lock;    /* serialises access to the connection */
} CourierBackendPrivate;

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE (CourierBackend, courier_backend, G_TYPE_OBJECT)

static void
courier_backend_finalize (GObject *object)
{
  CourierBackendPrivate *priv = courier_backend_get_instance_private (COURIER_BACKEND (object));
  courier_account_free (priv->account);
  g_clear_pointer (&priv->folders, g_ptr_array_unref);
  g_mutex_clear (&priv->lock);
  G_OBJECT_CLASS (courier_backend_parent_class)->finalize (object);
}

static gboolean
default_saves_sent (CourierBackend *self)
{
  return FALSE;
}

static void
courier_backend_class_init (CourierBackendClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = courier_backend_finalize;
  klass->saves_sent = default_saves_sent;
}

static void
courier_backend_init (CourierBackend *self)
{
  CourierBackendPrivate *priv = courier_backend_get_instance_private (self);
  g_mutex_init (&priv->lock);
}

const CourierAccount *
courier_backend_get_account (CourierBackend *self)
{
  CourierBackendPrivate *priv = courier_backend_get_instance_private (self);
  return priv->account;
}

static void
courier_backend_set_account (CourierBackend *self, const CourierAccount *account)
{
  CourierBackendPrivate *priv = courier_backend_get_instance_private (self);
  courier_account_free (priv->account);
  priv->account = courier_account_copy (account);
}

const CourierFolder *
courier_backend_find_role (CourierBackend *self, const char *role)
{
  CourierBackendPrivate *priv = courier_backend_get_instance_private (self);
  for (guint i = 0; priv->folders && i < priv->folders->len; i++) {
    CourierFolder *f = g_ptr_array_index (priv->folders, i);
    if (f->role && g_str_equal (f->role, role))
      return f;
  }
  return NULL;
}

/* ---------------- jobs ---------------- */

typedef enum {
  JOB_LIST_FOLDERS,
  JOB_LIST_MESSAGES,
  JOB_FETCH,
  JOB_SET_FLAG,
  JOB_ARCHIVE,
  JOB_TRASH,
  JOB_SEND,
} JobKind;

typedef struct {
  JobKind  kind;
  char    *folder;
  char    *uid;
  char    *flag;
  gboolean add;
  guint    limit;
  GBytes  *message;
  char    *from;
  GStrv    recipients;
} Job;

static void
job_free (Job *job)
{
  g_free (job->folder);
  g_free (job->uid);
  g_free (job->flag);
  g_clear_pointer (&job->message, g_bytes_unref);
  g_free (job->from);
  g_strfreev (job->recipients);
  g_free (job);
}

static int
compare_newest_first (gconstpointer pa, gconstpointer pb)
{
  CourierMessage *a = *(CourierMessage **) pa;
  CourierMessage *b = *(CourierMessage **) pb;
  if (a->date && b->date)
    return g_date_time_compare (b->date, a->date);
  guint64 ua = g_ascii_strtoull (a->uid, NULL, 10), ub = g_ascii_strtoull (b->uid, NULL, 10);
  return ua < ub ? 1 : ua > ub ? -1 : 0;
}

static void
run_job (GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable)
{
  CourierBackend *self = source;
  CourierBackendClass *klass = COURIER_BACKEND_GET_CLASS (self);
  CourierBackendPrivate *priv = courier_backend_get_instance_private (self);
  Job *job = task_data;
  GError *error = NULL;

  g_mutex_lock (&priv->lock);

  switch (job->kind) {
  case JOB_LIST_FOLDERS: {
    GPtrArray *folders = klass->list_folders (self, &error);
    if (folders) {
      GPtrArray *copy = g_ptr_array_new_with_free_func ((GDestroyNotify) courier_folder_free);
      for (guint i = 0; i < folders->len; i++)
        g_ptr_array_add (copy, courier_folder_copy (g_ptr_array_index (folders, i)));
      g_clear_pointer (&priv->folders, g_ptr_array_unref);
      priv->folders = copy;
      g_task_return_pointer (task, folders, (GDestroyNotify) g_ptr_array_unref);
    }
    break;
  }
  case JOB_LIST_MESSAGES: {
    GPtrArray *msgs = klass->list_messages (self, job->folder, job->limit, &error);
    if (msgs)
      g_ptr_array_sort (msgs, compare_newest_first);
    if (msgs)
      g_task_return_pointer (task, msgs, (GDestroyNotify) g_ptr_array_unref);
    break;
  }
  case JOB_FETCH: {
    g_autoptr (GBytes) raw = klass->fetch_message (self, job->folder, job->uid, &error);
    if (raw) {
      CourierMessageDetail *d = courier_message_detail_parse (job->uid, raw, &error);
      if (d)
        g_task_return_pointer (task, d, (GDestroyNotify) courier_message_detail_free);
    }
    break;
  }
  case JOB_SET_FLAG:
    if (klass->set_flag (self, job->folder, job->uid, job->flag, job->add, &error))
      g_task_return_boolean (task, TRUE);
    break;
  case JOB_ARCHIVE: {
    const CourierFolder *dest = courier_backend_find_role (self, "archive");
    if (!dest)
      dest = courier_backend_find_role (self, "all");
    if (!dest)
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "This account has no archive folder");
    else if (g_str_equal (dest->path, job->folder))
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_EXISTS, "The message is already archived");
    else if (klass->move (self, job->folder, job->uid, dest->path, &error))
      g_task_return_boolean (task, TRUE);
    break;
  }
  case JOB_TRASH: {
    const CourierFolder *trash = courier_backend_find_role (self, "trash");
    gboolean ok;
    if (!trash || g_str_equal (trash->path, job->folder))
      ok = klass->remove (self, job->folder, job->uid, &error);
    else
      ok = klass->move (self, job->folder, job->uid, trash->path, &error);
    if (ok)
      g_task_return_boolean (task, TRUE);
    break;
  }
  case JOB_SEND:
    if (klass->send (self, job->message, job->from, (const char * const *) job->recipients, &error)) {
      const CourierFolder *sent = courier_backend_find_role (self, "sent");
      if (sent && !klass->saves_sent (self)) {
        g_autoptr (GError) append_error = NULL;
        if (!klass->append (self, sent->path, job->message, &append_error))
          g_warning ("Message sent but not saved to %s: %s", sent->path, append_error->message);
      }
      g_task_return_boolean (task, TRUE);
    }
    break;
  }

  g_mutex_unlock (&priv->lock);

  if (error)
    g_task_return_error (task, error);
}

static void
start_job (CourierBackend *self, Job *job, GCancellable *c, GAsyncReadyCallback cb, gpointer data)
{
  g_autoptr (GTask) task = g_task_new (self, c, cb, data);
  g_task_set_task_data (task, job, (GDestroyNotify) job_free);
  g_task_set_return_on_cancel (task, FALSE);
  g_task_run_in_thread (task, run_job);
}

void
courier_backend_list_folders (CourierBackend *self, GCancellable *c, GAsyncReadyCallback cb, gpointer data)
{
  Job *job = g_new0 (Job, 1);
  job->kind = JOB_LIST_FOLDERS;
  start_job (self, job, c, cb, data);
}

void
courier_backend_list_messages (CourierBackend *self, const char *folder, guint limit,
                               GCancellable *c, GAsyncReadyCallback cb, gpointer data)
{
  Job *job = g_new0 (Job, 1);
  job->kind = JOB_LIST_MESSAGES;
  job->folder = g_strdup (folder);
  job->limit = limit;
  start_job (self, job, c, cb, data);
}

void
courier_backend_fetch_message (CourierBackend *self, const char *folder, const char *uid,
                               GCancellable *c, GAsyncReadyCallback cb, gpointer data)
{
  Job *job = g_new0 (Job, 1);
  job->kind = JOB_FETCH;
  job->folder = g_strdup (folder);
  job->uid = g_strdup (uid);
  start_job (self, job, c, cb, data);
}

void
courier_backend_set_flag (CourierBackend *self, const char *folder, const char *uid,
                          const char *flag, gboolean add, GAsyncReadyCallback cb, gpointer data)
{
  Job *job = g_new0 (Job, 1);
  job->kind = JOB_SET_FLAG;
  job->folder = g_strdup (folder);
  job->uid = g_strdup (uid);
  job->flag = g_strdup (flag);
  job->add = add;
  start_job (self, job, NULL, cb, data);
}

void
courier_backend_archive (CourierBackend *self, const char *folder, const char *uid,
                         GAsyncReadyCallback cb, gpointer data)
{
  Job *job = g_new0 (Job, 1);
  job->kind = JOB_ARCHIVE;
  job->folder = g_strdup (folder);
  job->uid = g_strdup (uid);
  start_job (self, job, NULL, cb, data);
}

void
courier_backend_trash (CourierBackend *self, const char *folder, const char *uid,
                       GAsyncReadyCallback cb, gpointer data)
{
  Job *job = g_new0 (Job, 1);
  job->kind = JOB_TRASH;
  job->folder = g_strdup (folder);
  job->uid = g_strdup (uid);
  start_job (self, job, NULL, cb, data);
}

void
courier_backend_send (CourierBackend *self, GBytes *message, const char *from,
                      const char * const *recipients, GAsyncReadyCallback cb, gpointer data)
{
  Job *job = g_new0 (Job, 1);
  job->kind = JOB_SEND;
  job->message = g_bytes_ref (message);
  job->from = g_strdup (from);
  job->recipients = g_strdupv ((char **) recipients);
  start_job (self, job, NULL, cb, data);
}

gpointer
courier_backend_finish_pointer (CourierBackend *self, GAsyncResult *res, GError **error)
{
  return g_task_propagate_pointer (G_TASK (res), error);
}

gboolean
courier_backend_finish_boolean (CourierBackend *self, GAsyncResult *res, GError **error)
{
  return g_task_propagate_boolean (G_TASK (res), error);
}

/* ======================= IMAP backend ======================= */

#define COURIER_TYPE_IMAP_BACKEND (courier_imap_backend_get_type ())
G_DECLARE_FINAL_TYPE (CourierImapBackend, courier_imap_backend, COURIER, IMAP_BACKEND, CourierBackend)

struct _CourierImapBackend {
  CourierBackend parent_instance;
  CourierImap   *imap;
};

G_DEFINE_FINAL_TYPE (CourierImapBackend, courier_imap_backend, COURIER_TYPE_BACKEND)

static CourierImap *
ensure_connection (CourierImapBackend *self, GError **error)
{
  if (self->imap && courier_imap_is_connected (self->imap))
    return self->imap;
  g_clear_pointer (&self->imap, courier_imap_free);
  const CourierAccount *account = courier_backend_get_account (COURIER_BACKEND (self));
  if (!account->password || !*account->password) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                 "No password saved. Open Account Settings to enter it.");
    return NULL;
  }
  self->imap = courier_imap_connect (courier_backend_get_account (COURIER_BACKEND (self)), NULL, error);
  return self->imap;
}

/* Run `expr` (which uses `imap` and `error`); if the connection dropped,
 * reconnect once and retry. Server NO/BAD answers are not retried. */
#define WITH_RETRY(result, expr)                                              \
  G_STMT_START {                                                              \
    for (int attempt = 0; attempt < 2; attempt++) {                           \
      g_autoptr (GError) local = NULL;                                        \
      CourierImap *imap = ensure_connection (self, &local);                   \
      if (imap) {                                                             \
        GError **error_ = &local;                                             \
        result = (expr);                                                      \
        (void) error_;                                                        \
      }                                                                       \
      if (!local) break;                                                      \
      if (attempt == 0 && self->imap && !courier_imap_is_connected (self->imap)) \
        continue;                                                             \
      g_propagate_error (error, g_steal_pointer (&local));                    \
      break;                                                                  \
    }                                                                         \
  } G_STMT_END

static GPtrArray *
imap_list_folders (CourierBackend *backend, GError **error)
{
  CourierImapBackend *self = COURIER_IMAP_BACKEND (backend);
  GPtrArray *res = NULL;
  WITH_RETRY (res, courier_imap_list_folders (imap, error_));
  return res;
}

static GPtrArray *
imap_list_messages (CourierBackend *backend, const char *folder, guint limit, GError **error)
{
  CourierImapBackend *self = COURIER_IMAP_BACKEND (backend);
  GPtrArray *res = NULL;
  WITH_RETRY (res, courier_imap_list_messages (imap, folder, limit, error_));
  return res;
}

static GBytes *
imap_fetch_message (CourierBackend *backend, const char *folder, const char *uid, GError **error)
{
  CourierImapBackend *self = COURIER_IMAP_BACKEND (backend);
  GBytes *res = NULL;
  WITH_RETRY (res, courier_imap_fetch_message (imap, folder, uid, error_));
  return res;
}

static gboolean
imap_set_flag (CourierBackend *backend, const char *folder, const char *uid,
               const char *flag, gboolean add, GError **error)
{
  CourierImapBackend *self = COURIER_IMAP_BACKEND (backend);
  gboolean res = FALSE;
  WITH_RETRY (res, courier_imap_store_flag (imap, folder, uid, flag, add, error_));
  return res;
}

static gboolean
imap_move (CourierBackend *backend, const char *folder, const char *uid, const char *dest, GError **error)
{
  CourierImapBackend *self = COURIER_IMAP_BACKEND (backend);
  gboolean res = FALSE;
  WITH_RETRY (res, courier_imap_move (imap, folder, uid, dest, error_));
  return res;
}

static gboolean
imap_remove (CourierBackend *backend, const char *folder, const char *uid, GError **error)
{
  CourierImapBackend *self = COURIER_IMAP_BACKEND (backend);
  gboolean res = FALSE;
  WITH_RETRY (res, courier_imap_delete (imap, folder, uid, error_));
  return res;
}

static gboolean
imap_append (CourierBackend *backend, const char *folder, GBytes *message, GError **error)
{
  CourierImapBackend *self = COURIER_IMAP_BACKEND (backend);
  gboolean res = FALSE;
  WITH_RETRY (res, courier_imap_append (imap, folder, message, "\\Seen", error_));
  return res;
}

static gboolean
imap_send (CourierBackend *backend, GBytes *message, const char *from,
           const char * const *recipients, GError **error)
{
  return courier_smtp_send (courier_backend_get_account (backend), message, from, recipients, error);
}

static gboolean
imap_saves_sent (CourierBackend *backend)
{
  const char *host = courier_backend_get_account (backend)->smtp_host;
  return host && (strstr (host, "gmail.com") || strstr (host, "googlemail.com") ||
                  strstr (host, "office365.com") || strstr (host, "outlook.com"));
}

static void
courier_imap_backend_finalize (GObject *object)
{
  CourierImapBackend *self = COURIER_IMAP_BACKEND (object);
  courier_imap_free (self->imap);
  G_OBJECT_CLASS (courier_imap_backend_parent_class)->finalize (object);
}

static void
courier_imap_backend_class_init (CourierImapBackendClass *klass)
{
  CourierBackendClass *bc = COURIER_BACKEND_CLASS (klass);
  G_OBJECT_CLASS (klass)->finalize = courier_imap_backend_finalize;
  bc->list_folders = imap_list_folders;
  bc->list_messages = imap_list_messages;
  bc->fetch_message = imap_fetch_message;
  bc->set_flag = imap_set_flag;
  bc->move = imap_move;
  bc->remove = imap_remove;
  bc->append = imap_append;
  bc->send = imap_send;
  bc->saves_sent = imap_saves_sent;
}

static void
courier_imap_backend_init (CourierImapBackend *self)
{
}

CourierBackend *
courier_backend_new_imap (const CourierAccount *account)
{
  CourierBackend *self = g_object_new (COURIER_TYPE_IMAP_BACKEND, NULL);
  courier_backend_set_account (self, account);
  return self;
}

gboolean
courier_backend_is_demo (CourierBackend *self)
{
  return !COURIER_IS_IMAP_BACKEND (self);
}

gboolean
courier_backend_test_account (const CourierAccount *account, GError **error)
{
  CourierImap *imap = courier_imap_connect (account, NULL, error);
  if (!imap)
    return FALSE;
  courier_imap_logout (imap);
  courier_imap_free (imap);
  return TRUE;
}

/* The demo backend lives in courier-demo.c and needs to set its account */
void _courier_backend_set_account (CourierBackend *self, const CourierAccount *account);
void
_courier_backend_set_account (CourierBackend *self, const CourierAccount *account)
{
  courier_backend_set_account (self, account);
}
