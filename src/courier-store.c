/* courier-store.c — SQLite persistence */
#include "courier-store.h"

#include <gio/gio.h>
#include <sqlite3.h>
#include <string.h>

struct _CourierStore {
  sqlite3 *db;
};

#define SCHEMA_VERSION 1

static const char *schema =
  "CREATE TABLE IF NOT EXISTS settings (key TEXT PRIMARY KEY, value TEXT);"
  "CREATE TABLE IF NOT EXISTS events ("
  "  id INTEGER PRIMARY KEY AUTOINCREMENT, title TEXT NOT NULL, location TEXT, notes TEXT,"
  "  start INTEGER NOT NULL, end INTEGER NOT NULL, remind_minutes INTEGER NOT NULL DEFAULT -1);"
  "CREATE INDEX IF NOT EXISTS events_start ON events(start);"
  "CREATE TABLE IF NOT EXISTS todos ("
  "  id INTEGER PRIMARY KEY AUTOINCREMENT, title TEXT NOT NULL, notes TEXT, due INTEGER NOT NULL DEFAULT 0,"
  "  done INTEGER NOT NULL DEFAULT 0, remind INTEGER NOT NULL DEFAULT 0, created INTEGER NOT NULL);"
  "CREATE TABLE IF NOT EXISTS reminders ("
  "  id INTEGER PRIMARY KEY AUTOINCREMENT, title TEXT NOT NULL, at INTEGER NOT NULL,"
  "  fired INTEGER NOT NULL DEFAULT 0, source TEXT NOT NULL DEFAULT '', source_id INTEGER NOT NULL DEFAULT 0);"
  "CREATE INDEX IF NOT EXISTS reminders_at ON reminders(fired, at);"
  "CREATE TABLE IF NOT EXISTS filters ("
  "  id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL, enabled INTEGER NOT NULL DEFAULT 1,"
  "  field TEXT NOT NULL, op TEXT NOT NULL, value TEXT NOT NULL, action TEXT NOT NULL, target TEXT,"
  "  position INTEGER NOT NULL DEFAULT 0);"
  "CREATE TABLE IF NOT EXISTS mail_log ("
  "  id INTEGER PRIMARY KEY AUTOINCREMENT, direction TEXT NOT NULL, account TEXT, folder TEXT, uid TEXT,"
  "  message_id TEXT, sender TEXT, recipients TEXT, subject TEXT, date INTEGER, logged INTEGER NOT NULL,"
  "  preview TEXT, eml_path TEXT);"
  "CREATE UNIQUE INDEX IF NOT EXISTS mail_log_received ON mail_log(account, folder, uid) WHERE direction = 'received';"
  "CREATE INDEX IF NOT EXISTS mail_log_date ON mail_log(date);"
  "CREATE TABLE IF NOT EXISTS sync_state (account TEXT, folder TEXT, last_uid INTEGER NOT NULL,"
  "  PRIMARY KEY (account, folder));"
  "CREATE TABLE IF NOT EXISTS lan_messages ("
  "  id INTEGER PRIMARY KEY AUTOINCREMENT, peer_id TEXT NOT NULL, peer_name TEXT, outgoing INTEGER NOT NULL,"
  "  text TEXT NOT NULL, ts INTEGER NOT NULL);"
  "CREATE INDEX IF NOT EXISTS lan_messages_peer ON lan_messages(peer_id, ts);";

/* ---------------- free / copy ---------------- */

void
courier_event_free (CourierEvent *e)
{
  if (!e) return;
  g_free (e->title); g_free (e->location); g_free (e->notes);
  g_free (e);
}

CourierEvent *
courier_event_copy (const CourierEvent *src)
{
  CourierEvent *e = g_memdup2 (src, sizeof *src);
  e->title = g_strdup (src->title);
  e->location = g_strdup (src->location);
  e->notes = g_strdup (src->notes);
  return e;
}

void
courier_todo_free (CourierTodo *t)
{
  if (!t) return;
  g_free (t->title); g_free (t->notes);
  g_free (t);
}

CourierTodo *
courier_todo_copy (const CourierTodo *src)
{
  CourierTodo *t = g_memdup2 (src, sizeof *src);
  t->title = g_strdup (src->title);
  t->notes = g_strdup (src->notes);
  return t;
}

void
courier_reminder_free (CourierReminder *r)
{
  if (!r) return;
  g_free (r->title); g_free (r->source);
  g_free (r);
}

void
courier_filter_free (CourierFilter *f)
{
  if (!f) return;
  g_free (f->name); g_free (f->field); g_free (f->op); g_free (f->value);
  g_free (f->action); g_free (f->target);
  g_free (f);
}

CourierFilter *
courier_filter_copy (const CourierFilter *src)
{
  CourierFilter *f = g_memdup2 (src, sizeof *src);
  f->name = g_strdup (src->name);
  f->field = g_strdup (src->field);
  f->op = g_strdup (src->op);
  f->value = g_strdup (src->value);
  f->action = g_strdup (src->action);
  f->target = g_strdup (src->target);
  return f;
}

void
courier_mail_log_free (CourierMailLogEntry *e)
{
  if (!e) return;
  g_free (e->direction); g_free (e->account); g_free (e->folder); g_free (e->uid);
  g_free (e->message_id); g_free (e->sender); g_free (e->recipients); g_free (e->subject);
  g_free (e->preview); g_free (e->eml_path);
  g_free (e);
}

void
courier_chat_message_free (CourierChatMessage *m)
{
  if (!m) return;
  g_free (m->peer_id); g_free (m->peer_name); g_free (m->text);
  g_free (m);
}

/* ---------------- helpers ---------------- */

static gboolean
set_sql_error (CourierStore *s, GError **error)
{
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Database error: %s", sqlite3_errmsg (s->db));
  return FALSE;
}

static sqlite3_stmt *
prepare (CourierStore *s, const char *sql)
{
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2 (s->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
    g_critical ("SQL prepare failed: %s\n%s", sqlite3_errmsg (s->db), sql);
    return NULL;
  }
  return stmt;
}

static void
bind_text (sqlite3_stmt *st, int i, const char *v)
{
  if (v)
    sqlite3_bind_text (st, i, v, -1, SQLITE_TRANSIENT);
  else
    sqlite3_bind_null (st, i);
}

static char *
col_text (sqlite3_stmt *st, int i)
{
  const unsigned char *t = sqlite3_column_text (st, i);
  return t ? g_strdup ((const char *) t) : NULL;
}

/* run a statement that returns no rows */
static gboolean
step_done (CourierStore *s, sqlite3_stmt *st, GError **error)
{
  if (!st)
    return set_sql_error (s, error);
  int rc = sqlite3_step (st);
  sqlite3_finalize (st);
  if (rc != SQLITE_DONE)
    return set_sql_error (s, error);
  return TRUE;
}

/* ---------------- open ---------------- */

char *
courier_store_data_dir (void)
{
  char *dir = g_build_filename (g_get_user_data_dir (), "courier", NULL);
  g_mkdir_with_parents (dir, 0700);
  return dir;
}

CourierStore *
courier_store_open (const char *path, GError **error)
{
  g_autofree char *file = NULL;
  if (!path) {
    g_autofree char *dir = courier_store_data_dir ();
    file = g_build_filename (dir, "courier.db", NULL);
    path = file;
  }

  CourierStore *s = g_new0 (CourierStore, 1);
  if (sqlite3_open (path, &s->db) != SQLITE_OK) {
    set_sql_error (s, error);
    courier_store_close (s);
    return NULL;
  }
  sqlite3_busy_timeout (s->db, 2000);
  char *msg = NULL;
  if (sqlite3_exec (s->db, "PRAGMA journal_mode=WAL; PRAGMA foreign_keys=ON;", NULL, NULL, NULL) != SQLITE_OK ||
      sqlite3_exec (s->db, schema, NULL, NULL, &msg) != SQLITE_OK) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not create database: %s", msg ? msg : "?");
    sqlite3_free (msg);
    courier_store_close (s);
    return NULL;
  }
  g_autofree char *v = g_strdup_printf ("PRAGMA user_version=%d;", SCHEMA_VERSION);
  sqlite3_exec (s->db, v, NULL, NULL, NULL);
  return s;
}

void
courier_store_close (CourierStore *s)
{
  if (!s)
    return;
  if (s->db)
    sqlite3_close (s->db);
  g_free (s);
}

/* ---------------- settings ---------------- */

char *
courier_store_get_setting (CourierStore *s, const char *key)
{
  sqlite3_stmt *st = prepare (s, "SELECT value FROM settings WHERE key = ?");
  if (!st)
    return NULL;
  bind_text (st, 1, key);
  char *v = sqlite3_step (st) == SQLITE_ROW ? col_text (st, 0) : NULL;
  sqlite3_finalize (st);
  return v;
}

void
courier_store_set_setting (CourierStore *s, const char *key, const char *value)
{
  sqlite3_stmt *st = prepare (s, "INSERT INTO settings(key, value) VALUES(?, ?) "
                                 "ON CONFLICT(key) DO UPDATE SET value = excluded.value");
  bind_text (st, 1, key);
  bind_text (st, 2, value);
  step_done (s, st, NULL);
}

/* ---------------- reminders (needed by events/todos) ---------------- */

static void
replace_linked_reminder (CourierStore *s, const char *source, gint64 source_id, const char *title, gint64 at)
{
  sqlite3_stmt *st = prepare (s, "DELETE FROM reminders WHERE source = ? AND source_id = ?");
  bind_text (st, 1, source);
  sqlite3_bind_int64 (st, 2, source_id);
  step_done (s, st, NULL);

  if (at <= 0 || at <= g_get_real_time () / G_USEC_PER_SEC)
    return; /* nothing to remind about, or already in the past */
  st = prepare (s, "INSERT INTO reminders(title, at, source, source_id) VALUES(?, ?, ?, ?)");
  bind_text (st, 1, title);
  sqlite3_bind_int64 (st, 2, at);
  bind_text (st, 3, source);
  sqlite3_bind_int64 (st, 4, source_id);
  step_done (s, st, NULL);
}

static CourierReminder *
read_reminder (sqlite3_stmt *st)
{
  CourierReminder *r = g_new0 (CourierReminder, 1);
  r->id = sqlite3_column_int64 (st, 0);
  r->title = col_text (st, 1);
  r->at = sqlite3_column_int64 (st, 2);
  r->fired = sqlite3_column_int (st, 3);
  r->source = col_text (st, 4);
  r->source_id = sqlite3_column_int64 (st, 5);
  return r;
}

#define REMINDER_COLS "id, title, at, fired, source, source_id"

GPtrArray *
courier_store_list_reminders (CourierStore *s, gboolean fired, int limit)
{
  GPtrArray *out = g_ptr_array_new_with_free_func ((GDestroyNotify) courier_reminder_free);
  sqlite3_stmt *st = prepare (s, fired
    ? "SELECT " REMINDER_COLS " FROM reminders WHERE fired = 1 ORDER BY at DESC LIMIT ?"
    : "SELECT " REMINDER_COLS " FROM reminders WHERE fired = 0 ORDER BY at ASC LIMIT ?");
  sqlite3_bind_int (st, 1, limit > 0 ? limit : -1);
  while (sqlite3_step (st) == SQLITE_ROW)
    g_ptr_array_add (out, read_reminder (st));
  sqlite3_finalize (st);
  return out;
}

GPtrArray *
courier_store_due_reminders (CourierStore *s, gint64 now)
{
  GPtrArray *out = g_ptr_array_new_with_free_func ((GDestroyNotify) courier_reminder_free);
  sqlite3_stmt *st = prepare (s, "SELECT " REMINDER_COLS " FROM reminders WHERE fired = 0 AND at <= ? ORDER BY at");
  sqlite3_bind_int64 (st, 1, now);
  while (sqlite3_step (st) == SQLITE_ROW)
    g_ptr_array_add (out, read_reminder (st));
  sqlite3_finalize (st);
  return out;
}

gboolean
courier_store_save_reminder (CourierStore *s, CourierReminder *r, GError **error)
{
  sqlite3_stmt *st;
  if (r->id) {
    st = prepare (s, "UPDATE reminders SET title = ?, at = ?, fired = ? WHERE id = ?");
    sqlite3_bind_int64 (st, 4, r->id);
  } else {
    st = prepare (s, "INSERT INTO reminders(title, at, fired, source, source_id) VALUES(?, ?, ?, ?, ?)");
    bind_text (st, 4, r->source ? r->source : "");
    sqlite3_bind_int64 (st, 5, r->source_id);
  }
  bind_text (st, 1, r->title);
  sqlite3_bind_int64 (st, 2, r->at);
  sqlite3_bind_int (st, 3, r->fired);
  if (!step_done (s, st, error))
    return FALSE;
  if (!r->id)
    r->id = sqlite3_last_insert_rowid (s->db);
  return TRUE;
}

gboolean
courier_store_mark_fired (CourierStore *s, gint64 id)
{
  sqlite3_stmt *st = prepare (s, "UPDATE reminders SET fired = 1 WHERE id = ?");
  sqlite3_bind_int64 (st, 1, id);
  return step_done (s, st, NULL);
}

gboolean
courier_store_snooze_reminder (CourierStore *s, gint64 id, gint64 until)
{
  sqlite3_stmt *st = prepare (s, "UPDATE reminders SET fired = 0, at = ? WHERE id = ?");
  sqlite3_bind_int64 (st, 1, until);
  sqlite3_bind_int64 (st, 2, id);
  return step_done (s, st, NULL);
}

gboolean
courier_store_delete_reminder (CourierStore *s, gint64 id, GError **error)
{
  sqlite3_stmt *st = prepare (s, "DELETE FROM reminders WHERE id = ?");
  sqlite3_bind_int64 (st, 1, id);
  return step_done (s, st, error);
}

/* ---------------- events ---------------- */

GPtrArray *
courier_store_list_events (CourierStore *s, gint64 from, gint64 to)
{
  GPtrArray *out = g_ptr_array_new_with_free_func ((GDestroyNotify) courier_event_free);
  sqlite3_stmt *st = prepare (s, "SELECT id, title, location, notes, start, end, remind_minutes FROM events "
                                 "WHERE start < ? AND end > ? OR (start >= ? AND start < ?) ORDER BY start");
  sqlite3_bind_int64 (st, 1, to);
  sqlite3_bind_int64 (st, 2, from);
  sqlite3_bind_int64 (st, 3, from);
  sqlite3_bind_int64 (st, 4, to);
  while (sqlite3_step (st) == SQLITE_ROW) {
    CourierEvent *e = g_new0 (CourierEvent, 1);
    e->id = sqlite3_column_int64 (st, 0);
    e->title = col_text (st, 1);
    e->location = col_text (st, 2);
    e->notes = col_text (st, 3);
    e->start = sqlite3_column_int64 (st, 4);
    e->end = sqlite3_column_int64 (st, 5);
    e->remind_minutes = sqlite3_column_int (st, 6);
    g_ptr_array_add (out, e);
  }
  sqlite3_finalize (st);
  return out;
}

gboolean
courier_store_save_event (CourierStore *s, CourierEvent *e, GError **error)
{
  if (!e->title || !*e->title) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "The event needs a title");
    return FALSE;
  }
  if (e->end < e->start)
    e->end = e->start;
  sqlite3_stmt *st;
  if (e->id) {
    st = prepare (s, "UPDATE events SET title=?, location=?, notes=?, start=?, end=?, remind_minutes=? WHERE id=?");
    sqlite3_bind_int64 (st, 7, e->id);
  } else {
    st = prepare (s, "INSERT INTO events(title, location, notes, start, end, remind_minutes) VALUES(?,?,?,?,?,?)");
  }
  bind_text (st, 1, e->title);
  bind_text (st, 2, e->location);
  bind_text (st, 3, e->notes);
  sqlite3_bind_int64 (st, 4, e->start);
  sqlite3_bind_int64 (st, 5, e->end);
  sqlite3_bind_int (st, 6, e->remind_minutes);
  if (!step_done (s, st, error))
    return FALSE;
  if (!e->id)
    e->id = sqlite3_last_insert_rowid (s->db);

  g_autofree char *title = g_strdup_printf ("📅 %s", e->title);
  replace_linked_reminder (s, "event", e->id, title,
                           e->remind_minutes >= 0 ? e->start - (gint64) e->remind_minutes * 60 : 0);
  return TRUE;
}

gboolean
courier_store_delete_event (CourierStore *s, gint64 id, GError **error)
{
  replace_linked_reminder (s, "event", id, NULL, 0);
  sqlite3_stmt *st = prepare (s, "DELETE FROM events WHERE id = ?");
  sqlite3_bind_int64 (st, 1, id);
  return step_done (s, st, error);
}

/* ---------------- todos ---------------- */

GPtrArray *
courier_store_list_todos (CourierStore *s, gboolean done)
{
  GPtrArray *out = g_ptr_array_new_with_free_func ((GDestroyNotify) courier_todo_free);
  sqlite3_stmt *st = prepare (s, done
    ? "SELECT id, title, notes, due, done, remind, created FROM todos WHERE done = 1 ORDER BY created DESC LIMIT 200"
    : "SELECT id, title, notes, due, done, remind, created FROM todos WHERE done = 0 "
      "ORDER BY due = 0, due, created");
  while (sqlite3_step (st) == SQLITE_ROW) {
    CourierTodo *t = g_new0 (CourierTodo, 1);
    t->id = sqlite3_column_int64 (st, 0);
    t->title = col_text (st, 1);
    t->notes = col_text (st, 2);
    t->due = sqlite3_column_int64 (st, 3);
    t->done = sqlite3_column_int (st, 4);
    t->remind = sqlite3_column_int (st, 5);
    t->created = sqlite3_column_int64 (st, 6);
    g_ptr_array_add (out, t);
  }
  sqlite3_finalize (st);
  return out;
}

gboolean
courier_store_save_todo (CourierStore *s, CourierTodo *t, GError **error)
{
  if (!t->title || !*t->title) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "The task needs a title");
    return FALSE;
  }
  sqlite3_stmt *st;
  if (t->id) {
    st = prepare (s, "UPDATE todos SET title=?, notes=?, due=?, done=?, remind=? WHERE id=?");
    sqlite3_bind_int64 (st, 6, t->id);
  } else {
    if (!t->created)
      t->created = g_get_real_time () / G_USEC_PER_SEC;
    st = prepare (s, "INSERT INTO todos(title, notes, due, done, remind, created) VALUES(?,?,?,?,?,?)");
    sqlite3_bind_int64 (st, 6, t->created);
  }
  bind_text (st, 1, t->title);
  bind_text (st, 2, t->notes);
  sqlite3_bind_int64 (st, 3, t->due);
  sqlite3_bind_int (st, 4, t->done);
  sqlite3_bind_int (st, 5, t->remind);
  if (!step_done (s, st, error))
    return FALSE;
  if (!t->id)
    t->id = sqlite3_last_insert_rowid (s->db);

  g_autofree char *title = g_strdup_printf ("✅ %s", t->title);
  replace_linked_reminder (s, "todo", t->id, title, t->remind && !t->done ? t->due : 0);
  return TRUE;
}

gboolean
courier_store_delete_todo (CourierStore *s, gint64 id, GError **error)
{
  replace_linked_reminder (s, "todo", id, NULL, 0);
  sqlite3_stmt *st = prepare (s, "DELETE FROM todos WHERE id = ?");
  sqlite3_bind_int64 (st, 1, id);
  return step_done (s, st, error);
}

/* ---------------- filters ---------------- */

GPtrArray *
courier_store_list_filters (CourierStore *s)
{
  GPtrArray *out = g_ptr_array_new_with_free_func ((GDestroyNotify) courier_filter_free);
  sqlite3_stmt *st = prepare (s, "SELECT id, name, enabled, field, op, value, action, target FROM filters "
                                 "ORDER BY position, id");
  while (sqlite3_step (st) == SQLITE_ROW) {
    CourierFilter *f = g_new0 (CourierFilter, 1);
    f->id = sqlite3_column_int64 (st, 0);
    f->name = col_text (st, 1);
    f->enabled = sqlite3_column_int (st, 2);
    f->field = col_text (st, 3);
    f->op = col_text (st, 4);
    f->value = col_text (st, 5);
    f->action = col_text (st, 6);
    f->target = col_text (st, 7);
    g_ptr_array_add (out, f);
  }
  sqlite3_finalize (st);
  return out;
}

gboolean
courier_store_save_filter (CourierStore *s, CourierFilter *f, GError **error)
{
  if (!f->value || !*f->value) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Enter the text to match");
    return FALSE;
  }
  if (g_strcmp0 (f->action, "move") == 0 && (!f->target || !*f->target)) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Choose the folder to move messages to");
    return FALSE;
  }
  sqlite3_stmt *st;
  if (f->id) {
    st = prepare (s, "UPDATE filters SET name=?, enabled=?, field=?, op=?, value=?, action=?, target=? WHERE id=?");
    sqlite3_bind_int64 (st, 8, f->id);
  } else {
    st = prepare (s, "INSERT INTO filters(name, enabled, field, op, value, action, target, position) "
                     "VALUES(?,?,?,?,?,?,?, (SELECT IFNULL(MAX(position), 0) + 1 FROM filters))");
  }
  bind_text (st, 1, f->name && *f->name ? f->name : f->value);
  sqlite3_bind_int (st, 2, f->enabled);
  bind_text (st, 3, f->field);
  bind_text (st, 4, f->op);
  bind_text (st, 5, f->value);
  bind_text (st, 6, f->action);
  bind_text (st, 7, f->target);
  if (!step_done (s, st, error))
    return FALSE;
  if (!f->id)
    f->id = sqlite3_last_insert_rowid (s->db);
  return TRUE;
}

gboolean
courier_store_delete_filter (CourierStore *s, gint64 id, GError **error)
{
  sqlite3_stmt *st = prepare (s, "DELETE FROM filters WHERE id = ?");
  sqlite3_bind_int64 (st, 1, id);
  return step_done (s, st, error);
}

/* ---------------- mail log ---------------- */

gboolean
courier_store_log_mail (CourierStore *s, CourierMailLogEntry *e, GError **error)
{
  sqlite3_stmt *st = prepare (s,
    "INSERT OR IGNORE INTO mail_log(direction, account, folder, uid, message_id, sender, recipients, subject,"
    " date, logged, preview, eml_path) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)");
  if (!e->logged)
    e->logged = g_get_real_time () / G_USEC_PER_SEC;
  bind_text (st, 1, e->direction);
  bind_text (st, 2, e->account);
  bind_text (st, 3, e->folder);
  bind_text (st, 4, e->uid);
  bind_text (st, 5, e->message_id);
  bind_text (st, 6, e->sender);
  bind_text (st, 7, e->recipients);
  bind_text (st, 8, e->subject);
  sqlite3_bind_int64 (st, 9, e->date);
  sqlite3_bind_int64 (st, 10, e->logged);
  bind_text (st, 11, e->preview);
  bind_text (st, 12, e->eml_path);
  if (!step_done (s, st, error))
    return FALSE;
  e->id = sqlite3_changes (s->db) ? sqlite3_last_insert_rowid (s->db) : 0;
  return TRUE;
}

GPtrArray *
courier_store_list_mail_log (CourierStore *s, const char *direction, const char *query, int limit)
{
  GPtrArray *out = g_ptr_array_new_with_free_func ((GDestroyNotify) courier_mail_log_free);
  sqlite3_stmt *st = prepare (s,
    "SELECT id, direction, account, folder, uid, message_id, sender, recipients, subject, date, logged,"
    " preview, eml_path FROM mail_log WHERE (?1 IS NULL OR direction = ?1) AND (?2 IS NULL OR"
    " sender LIKE ?2 OR recipients LIKE ?2 OR subject LIKE ?2 OR preview LIKE ?2)"
    " ORDER BY date DESC, id DESC LIMIT ?3");
  bind_text (st, 1, direction);
  g_autofree char *like = query && *query ? g_strdup_printf ("%%%s%%", query) : NULL;
  bind_text (st, 2, like);
  sqlite3_bind_int (st, 3, limit > 0 ? limit : -1);
  while (sqlite3_step (st) == SQLITE_ROW) {
    CourierMailLogEntry *e = g_new0 (CourierMailLogEntry, 1);
    e->id = sqlite3_column_int64 (st, 0);
    e->direction = col_text (st, 1);
    e->account = col_text (st, 2);
    e->folder = col_text (st, 3);
    e->uid = col_text (st, 4);
    e->message_id = col_text (st, 5);
    e->sender = col_text (st, 6);
    e->recipients = col_text (st, 7);
    e->subject = col_text (st, 8);
    e->date = sqlite3_column_int64 (st, 9);
    e->logged = sqlite3_column_int64 (st, 10);
    e->preview = col_text (st, 11);
    e->eml_path = col_text (st, 12);
    g_ptr_array_add (out, e);
  }
  sqlite3_finalize (st);
  return out;
}

gint64
courier_store_get_last_uid (CourierStore *s, const char *account, const char *folder, gboolean *known)
{
  sqlite3_stmt *st = prepare (s, "SELECT last_uid FROM sync_state WHERE account = ? AND folder = ?");
  bind_text (st, 1, account);
  bind_text (st, 2, folder);
  gint64 uid = 0;
  gboolean found = sqlite3_step (st) == SQLITE_ROW;
  if (found)
    uid = sqlite3_column_int64 (st, 0);
  sqlite3_finalize (st);
  if (known)
    *known = found;
  return uid;
}

void
courier_store_set_last_uid (CourierStore *s, const char *account, const char *folder, gint64 uid)
{
  sqlite3_stmt *st = prepare (s, "INSERT INTO sync_state(account, folder, last_uid) VALUES(?,?,?) "
                                 "ON CONFLICT(account, folder) DO UPDATE SET last_uid = excluded.last_uid");
  bind_text (st, 1, account);
  bind_text (st, 2, folder);
  sqlite3_bind_int64 (st, 3, uid);
  step_done (s, st, NULL);
}

/* ---------------- chat ---------------- */

gboolean
courier_store_add_chat_message (CourierStore *s, CourierChatMessage *m, GError **error)
{
  sqlite3_stmt *st = prepare (s, "INSERT INTO lan_messages(peer_id, peer_name, outgoing, text, ts) VALUES(?,?,?,?,?)");
  bind_text (st, 1, m->peer_id);
  bind_text (st, 2, m->peer_name);
  sqlite3_bind_int (st, 3, m->outgoing);
  bind_text (st, 4, m->text);
  sqlite3_bind_int64 (st, 5, m->ts);
  if (!step_done (s, st, error))
    return FALSE;
  m->id = sqlite3_last_insert_rowid (s->db);
  return TRUE;
}

GPtrArray *
courier_store_list_chat_messages (CourierStore *s, const char *peer_id, int limit)
{
  GPtrArray *out = g_ptr_array_new_with_free_func ((GDestroyNotify) courier_chat_message_free);
  /* newest N, returned oldest first */
  sqlite3_stmt *st = prepare (s, "SELECT * FROM (SELECT id, peer_id, peer_name, outgoing, text, ts FROM lan_messages "
                                 "WHERE peer_id = ? ORDER BY ts DESC, id DESC LIMIT ?) ORDER BY ts, id");
  bind_text (st, 1, peer_id);
  sqlite3_bind_int (st, 2, limit > 0 ? limit : -1);
  while (sqlite3_step (st) == SQLITE_ROW) {
    CourierChatMessage *m = g_new0 (CourierChatMessage, 1);
    m->id = sqlite3_column_int64 (st, 0);
    m->peer_id = col_text (st, 1);
    m->peer_name = col_text (st, 2);
    m->outgoing = sqlite3_column_int (st, 3);
    m->text = col_text (st, 4);
    m->ts = sqlite3_column_int64 (st, 5);
    g_ptr_array_add (out, m);
  }
  sqlite3_finalize (st);
  return out;
}
