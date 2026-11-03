/* courier-store.h — local SQLite database for everything that lives on this computer:
 * calendar events, tasks, reminders, mail filters, the mail log, LAN chat history
 * and small settings. Use from the main thread only. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct _CourierStore CourierStore;

typedef struct {
  gint64 id;              /* 0 = not saved yet */
  char  *title;
  char  *location;
  char  *notes;
  gint64 start;           /* unix time */
  gint64 end;
  int    remind_minutes;  /* -1 = no reminder */
} CourierEvent;

typedef struct {
  gint64   id;
  char    *title;
  char    *notes;
  gint64   due;           /* 0 = no due date */
  gboolean done;
  gboolean remind;        /* remind at the due time */
  gint64   created;
} CourierTodo;

typedef struct {
  gint64   id;
  char    *title;
  gint64   at;
  gboolean fired;
  char    *source;        /* "", "event" or "todo" */
  gint64   source_id;
} CourierReminder;

typedef struct {
  gint64   id;
  char    *name;
  gboolean enabled;
  char    *field;         /* "from", "to", "subject", "any" */
  char    *op;            /* "contains", "not-contains", "equals", "starts-with", "ends-with" */
  char    *value;
  char    *action;        /* "move", "read", "star", "trash" */
  char    *target;        /* folder for "move" */
} CourierFilter;

typedef struct {
  gint64 id;
  char  *direction;       /* "received" or "sent" */
  char  *account;         /* account e-mail address */
  char  *folder;
  char  *uid;
  char  *message_id;
  char  *sender;
  char  *recipients;
  char  *subject;
  gint64 date;
  gint64 logged;
  char  *preview;         /* first part of the body text (sent mail) */
  char  *eml_path;        /* full copy of a sent message, may be NULL */
} CourierMailLogEntry;

typedef struct {
  gint64   id;
  char    *peer_id;
  char    *peer_name;
  gboolean outgoing;
  char    *text;
  gint64   ts;
} CourierChatMessage;

void courier_event_free      (CourierEvent *e);
void courier_todo_free       (CourierTodo *t);
void courier_reminder_free   (CourierReminder *r);
void courier_filter_free     (CourierFilter *f);
void courier_mail_log_free   (CourierMailLogEntry *e);
void courier_chat_message_free (CourierChatMessage *m);
CourierFilter *courier_filter_copy (const CourierFilter *f);
CourierEvent  *courier_event_copy  (const CourierEvent *e);
CourierTodo   *courier_todo_copy   (const CourierTodo *t);

/* path NULL = ~/.local/share/courier/courier.db; ":memory:" works for tests */
CourierStore *courier_store_open  (const char *path, GError **error);
void          courier_store_close (CourierStore *store);
char         *courier_store_data_dir (void);

/* settings */
char     *courier_store_get_setting (CourierStore *s, const char *key);
void      courier_store_set_setting (CourierStore *s, const char *key, const char *value);

/* events: saving also (re)creates the linked reminder */
GPtrArray *courier_store_list_events (CourierStore *s, gint64 from, gint64 to); /* overlapping [from,to) */
gboolean   courier_store_save_event  (CourierStore *s, CourierEvent *e, GError **error);
gboolean   courier_store_delete_event (CourierStore *s, gint64 id, GError **error);

/* todos */
GPtrArray *courier_store_list_todos  (CourierStore *s, gboolean done);
gboolean   courier_store_save_todo   (CourierStore *s, CourierTodo *t, GError **error);
gboolean   courier_store_delete_todo (CourierStore *s, gint64 id, GError **error);

/* reminders */
GPtrArray *courier_store_list_reminders  (CourierStore *s, gboolean fired, int limit);
GPtrArray *courier_store_due_reminders   (CourierStore *s, gint64 now);   /* not fired, at <= now */
gboolean   courier_store_save_reminder   (CourierStore *s, CourierReminder *r, GError **error);
gboolean   courier_store_mark_fired      (CourierStore *s, gint64 id);
gboolean   courier_store_snooze_reminder (CourierStore *s, gint64 id, gint64 until);
gboolean   courier_store_delete_reminder (CourierStore *s, gint64 id, GError **error);

/* filters (in evaluation order) */
GPtrArray *courier_store_list_filters  (CourierStore *s);
gboolean   courier_store_save_filter   (CourierStore *s, CourierFilter *f, GError **error);
gboolean   courier_store_delete_filter (CourierStore *s, gint64 id, GError **error);

/* mail log; received entries are unique per (account, folder, uid) */
gboolean   courier_store_log_mail    (CourierStore *s, CourierMailLogEntry *e, GError **error);
GPtrArray *courier_store_list_mail_log (CourierStore *s, const char *direction /* NULL = all */,
                                        const char *query /* NULL = all */, int limit);

/* highest UID already seen per account+folder (0 = never synced) */
gint64     courier_store_get_last_uid (CourierStore *s, const char *account, const char *folder, gboolean *known);
void       courier_store_set_last_uid (CourierStore *s, const char *account, const char *folder, gint64 uid);

/* LAN chat history */
gboolean   courier_store_add_chat_message (CourierStore *s, CourierChatMessage *m, GError **error);
GPtrArray *courier_store_list_chat_messages (CourierStore *s, const char *peer_id, int limit);

G_END_DECLS
