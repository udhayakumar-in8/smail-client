/* smail-store.h — local SQLite database for everything that lives on this computer:
 * calendar events, tasks, reminders, mail filters, the mail log, LAN chat history
 * and small settings. Use from the main thread only. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct _SMailStore SMailStore;

typedef struct {
  gint64 id;              /* 0 = not saved yet */
  char  *title;
  char  *location;
  char  *notes;
  gint64 start;           /* unix time */
  gint64 end;
  int    remind_minutes;  /* -1 = no reminder */
} SMailEvent;

typedef struct {
  gint64   id;
  char    *title;
  char    *notes;
  gint64   due;           /* 0 = no due date */
  gboolean done;
  gboolean remind;        /* remind at the due time */
  gint64   created;
} SMailTodo;

typedef struct {
  gint64   id;
  char    *title;
  gint64   at;
  gboolean fired;
  char    *source;        /* "", "event" or "todo" */
  gint64   source_id;
} SMailReminder;

typedef struct {
  gint64   id;
  char    *name;
  gboolean enabled;
  char    *field;         /* "from", "to", "subject", "any" */
  char    *op;            /* "contains", "not-contains", "equals", "starts-with", "ends-with" */
  char    *value;
  char    *action;        /* "move", "read", "star", "trash" */
  char    *target;        /* folder for "move" */
} SMailFilter;

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
} SMailMailLogEntry;

typedef struct {
  gint64   id;
  char    *peer_id;
  char    *peer_name;
  gboolean outgoing;
  char    *text;
  gint64   ts;
} SMailChatMessage;

typedef struct {
  gint64   id;
  char    *name;
  char    *email;
  char    *phone;
  char    *notes;
  gboolean favorite;
  gboolean collected;   /* added automatically from mail, not by you */
  gint64   use_count;   /* how often you wrote to them */
  gint64   last_used;
} SMailContact;

void smail_contact_free    (SMailContact *c);
SMailContact *smail_contact_copy (const SMailContact *c);
void smail_event_free      (SMailEvent *e);
void smail_todo_free       (SMailTodo *t);
void smail_reminder_free   (SMailReminder *r);
void smail_filter_free     (SMailFilter *f);
void smail_mail_log_free   (SMailMailLogEntry *e);
void smail_chat_message_free (SMailChatMessage *m);
SMailFilter *smail_filter_copy (const SMailFilter *f);
SMailEvent  *smail_event_copy  (const SMailEvent *e);
SMailTodo   *smail_todo_copy   (const SMailTodo *t);

/* path NULL = ~/.local/share/smail/smail.db; ":memory:" works for tests */
SMailStore *smail_store_open  (const char *path, GError **error);
void          smail_store_close (SMailStore *store);
char         *smail_store_data_dir (void);

/* settings */
char     *smail_store_get_setting (SMailStore *s, const char *key);
void      smail_store_set_setting (SMailStore *s, const char *key, const char *value);

/* events: saving also (re)creates the linked reminder */
GPtrArray *smail_store_list_events (SMailStore *s, gint64 from, gint64 to); /* overlapping [from,to) */
gboolean   smail_store_save_event  (SMailStore *s, SMailEvent *e, GError **error);
gboolean   smail_store_delete_event (SMailStore *s, gint64 id, GError **error);

/* todos */
GPtrArray *smail_store_list_todos  (SMailStore *s, gboolean done);
gboolean   smail_store_save_todo   (SMailStore *s, SMailTodo *t, GError **error);
gboolean   smail_store_delete_todo (SMailStore *s, gint64 id, GError **error);

/* reminders */
GPtrArray *smail_store_list_reminders  (SMailStore *s, gboolean fired, int limit);
GPtrArray *smail_store_due_reminders   (SMailStore *s, gint64 now);   /* not fired, at <= now */
gboolean   smail_store_save_reminder   (SMailStore *s, SMailReminder *r, GError **error);
gboolean   smail_store_mark_fired      (SMailStore *s, gint64 id);
gboolean   smail_store_snooze_reminder (SMailStore *s, gint64 id, gint64 until);
gboolean   smail_store_delete_reminder (SMailStore *s, gint64 id, GError **error);

/* filters (in evaluation order) */
GPtrArray *smail_store_list_filters  (SMailStore *s);
gboolean   smail_store_save_filter   (SMailStore *s, SMailFilter *f, GError **error);
gboolean   smail_store_delete_filter (SMailStore *s, gint64 id, GError **error);

/* mail log; received entries are unique per (account, folder, uid) */
gboolean   smail_store_log_mail    (SMailStore *s, SMailMailLogEntry *e, GError **error);
GPtrArray *smail_store_list_mail_log (SMailStore *s, const char *direction /* NULL = all */,
                                        const char *query /* NULL = all */, int limit);

/* highest UID already seen per account+folder (0 = never synced) */
gint64     smail_store_get_last_uid (SMailStore *s, const char *account, const char *folder, gboolean *known);
void       smail_store_set_last_uid (SMailStore *s, const char *account, const char *folder, gint64 uid);

/* Downloaded messages: a full .eml copy of every message opened, kept under
 * ~/.local/share/smail/mail/<account>/<folder>/<uid>.eml */
char      *smail_store_mail_dir      (void);
gboolean   smail_store_save_message  (SMailStore *s, const char *account, const char *folder, const char *uid,
                                      GBytes *raw, char **path_out, GError **error);
GBytes    *smail_store_load_message  (SMailStore *s, const char *account, const char *folder, const char *uid);
void       smail_store_message_stats (SMailStore *s, gint64 *count, gint64 *bytes);

/* Address book. Searching matches name or address, best matches first. */
GPtrArray *smail_store_list_contacts  (SMailStore *s, const char *query, int limit);  /* SMailContact* */
SMailContact *smail_store_find_contact (SMailStore *s, const char *email);
gboolean   smail_store_save_contact   (SMailStore *s, SMailContact *c, GError **error);
gboolean   smail_store_delete_contact (SMailStore *s, gint64 id, GError **error);
/* Remember an address seen in mail (like Thunderbird's "Collected Addresses").
 * `outgoing`: you wrote to them, which ranks them higher in autocomplete. */
void       smail_store_collect_address (SMailStore *s, const char *name, const char *email, gboolean outgoing);
/* People you added yourself or have written to. */
gboolean   smail_store_is_trusted_sender (SMailStore *s, const char *email);

/* LAN chat history */
gboolean   smail_store_add_chat_message (SMailStore *s, SMailChatMessage *m, GError **error);
GPtrArray *smail_store_list_chat_messages (SMailStore *s, const char *peer_id, int limit);

G_END_DECLS
