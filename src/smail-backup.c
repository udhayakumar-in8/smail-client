/* smail-backup.c — single-file backups using SQLite */
#include "smail-backup.h"
#include "smail-store-private.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define FORMAT_NAME    "smail-backup"
#define FORMAT_VERSION 1

/* tables copied as a whole, in this order */
static const char *data_tables[] = {
  "events", "todos", "reminders", "filters", "mail_log", "sync_state", "lan_messages", "messages", "contacts", NULL
};

void
smail_backup_summary_clear (SMailBackupSummary *s)
{
  g_free (s->created);
  memset (s, 0, sizeof *s);
}

static gboolean
sql_error (sqlite3 *db, GError **error, const char *what)
{
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s: %s", what, sqlite3_errmsg (db));
  return FALSE;
}

static gboolean
exec (sqlite3 *db, const char *sql, GError **error)
{
  char *msg = NULL;
  if (sqlite3_exec (db, sql, NULL, NULL, &msg) != SQLITE_OK) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Backup database error: %s", msg ? msg : "?");
    sqlite3_free (msg);
    return FALSE;
  }
  return TRUE;
}

static gboolean
table_exists (sqlite3 *db, const char *schema, const char *table)
{
  g_autofree char *sql = g_strdup_printf ("SELECT 1 FROM %s.sqlite_master WHERE type = 'table' AND name = ?", schema);
  sqlite3_stmt *st = NULL;
  gboolean yes = FALSE;
  if (sqlite3_prepare_v2 (db, sql, -1, &st, NULL) == SQLITE_OK) {
    sqlite3_bind_text (st, 1, table, -1, SQLITE_TRANSIENT);
    yes = sqlite3_step (st) == SQLITE_ROW;
  }
  sqlite3_finalize (st);
  return yes;
}

static gint64
count_rows (sqlite3 *db, const char *schema, const char *table)
{
  if (!table_exists (db, schema, table))
    return 0;
  g_autofree char *sql = g_strdup_printf ("SELECT COUNT(*) FROM %s.%s", schema, table);
  sqlite3_stmt *st = NULL;
  gint64 n = 0;
  if (sqlite3_prepare_v2 (db, sql, -1, &st, NULL) == SQLITE_OK && sqlite3_step (st) == SQLITE_ROW)
    n = sqlite3_column_int64 (st, 0);
  sqlite3_finalize (st);
  return n;
}

static void
set_meta (sqlite3 *db, const char *key, const char *value)
{
  sqlite3_stmt *st = NULL;
  sqlite3_prepare_v2 (db, "INSERT OR REPLACE INTO backup_meta(key, value) VALUES(?, ?)", -1, &st, NULL);
  sqlite3_bind_text (st, 1, key, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text (st, 2, value, -1, SQLITE_TRANSIENT);
  sqlite3_step (st);
  sqlite3_finalize (st);
}

static char *
get_meta (sqlite3 *db, const char *key)
{
  sqlite3_stmt *st = NULL;
  char *v = NULL;
  if (sqlite3_prepare_v2 (db, "SELECT value FROM backup_meta WHERE key = ?", -1, &st, NULL) == SQLITE_OK) {
    sqlite3_bind_text (st, 1, key, -1, SQLITE_TRANSIENT);
    if (sqlite3_step (st) == SQLITE_ROW && sqlite3_column_text (st, 0))
      v = g_strdup ((const char *) sqlite3_column_text (st, 0));
  }
  sqlite3_finalize (st);
  return v;
}

/* Path relative to the data folder, so a backup can be restored for another user. */
static char *
relative_path (const char *data_dir, const char *path, const char *fallback_dir)
{
  g_autofree char *prefix = g_strconcat (data_dir, G_DIR_SEPARATOR_S, NULL);
  if (g_str_has_prefix (path, prefix))
    return g_strdup (path + strlen (prefix));
  g_autofree char *base = g_path_get_basename (path);
  return g_build_filename (fallback_dir, base, NULL);
}

static gboolean
add_file (sqlite3 *db, const char *orig_path, const char *rel, SMailBackupSummary *summary, GError **error)
{
  g_autoptr (GMappedFile) mf = g_mapped_file_new (orig_path, FALSE, NULL);
  if (!mf)
    return TRUE; /* missing file: skip it rather than failing the whole backup */
  sqlite3_stmt *st = NULL;
  if (sqlite3_prepare_v2 (db, "INSERT OR REPLACE INTO backup_files(orig_path, rel_path, data) VALUES(?,?,?)",
                          -1, &st, NULL) != SQLITE_OK)
    return sql_error (db, error, "Could not store a message");
  sqlite3_bind_text (st, 1, orig_path, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text (st, 2, rel, -1, SQLITE_TRANSIENT);
  sqlite3_bind_blob64 (st, 3, g_mapped_file_get_contents (mf), g_mapped_file_get_length (mf), SQLITE_STATIC);
  int rc = sqlite3_step (st);
  sqlite3_finalize (st);
  if (rc != SQLITE_DONE)
    return sql_error (db, error, "Could not store a message");
  summary->emails++;
  summary->bytes += g_mapped_file_get_length (mf);
  return TRUE;
}

static void
fill_counts (sqlite3 *db, const char *schema, SMailBackupSummary *s)
{
  s->filters = count_rows (db, schema, "filters");
  s->events = count_rows (db, schema, "events");
  s->todos = count_rows (db, schema, "todos");
  s->reminders = count_rows (db, schema, "reminders");
  s->log_entries = count_rows (db, schema, "mail_log");
  s->chat_messages = count_rows (db, schema, "lan_messages");
  s->contacts = count_rows (db, schema, "contacts");
}

/* ======================= create ======================= */

gboolean
smail_backup_create (SMailStore *store, GPtrArray *accounts, const char *path,
                     gboolean include_emails, gboolean include_passwords,
                     SMailBackupSummary *summary, GError **error)
{
  SMailBackupSummary local = { 0 };
  if (!summary)
    summary = &local;
  memset (summary, 0, sizeof *summary);

  g_autofree char *tmp = g_strconcat (path, ".part", NULL);
  g_unlink (tmp);
  g_autofree char *data_dir = smail_store_data_dir ();
  sqlite3 *live = smail_store_get_db (store);
  sqlite3 *db = NULL;
  gboolean ok = FALSE;
  g_autoptr (GDateTime) now = NULL;
  g_autofree char *version = NULL;

  /* 1. consistent snapshot of the live database */
  if (sqlite3_open (tmp, &db) != SQLITE_OK) {
    sql_error (db, error, "Could not create the backup file");
    goto out;
  }
  sqlite3_backup *bk = sqlite3_backup_init (db, "main", live, "main");
  if (!bk) {
    sql_error (db, error, "Could not copy the database");
    goto out;
  }
  sqlite3_backup_step (bk, -1);
  if (sqlite3_backup_finish (bk) != SQLITE_OK) {
    sql_error (db, error, "Could not copy the database");
    goto out;
  }

  /* 2. backup-only tables */
  if (!exec (db, "PRAGMA journal_mode=DELETE;"
                 "CREATE TABLE backup_meta (key TEXT PRIMARY KEY, value TEXT);"
                 "CREATE TABLE backup_accounts (id TEXT, name TEXT, email TEXT, username TEXT, password TEXT,"
                 "  imap_host TEXT, imap_port INTEGER, imap_security INTEGER,"
                 "  smtp_host TEXT, smtp_port INTEGER, smtp_security INTEGER, signature TEXT);"
                 "CREATE TABLE backup_files (orig_path TEXT PRIMARY KEY, rel_path TEXT NOT NULL, data BLOB NOT NULL);"
                 "BEGIN;", error))
    goto out;

  for (guint i = 0; accounts && i < accounts->len; i++) {
    SMailAccount *a = g_ptr_array_index (accounts, i);
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2 (db, "INSERT INTO backup_accounts VALUES(?,?,?,?,?,?,?,?,?,?,?,?)", -1, &st, NULL);
    sqlite3_bind_text (st, 1, a->id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, 2, a->name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, 3, a->email, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, 4, a->username, -1, SQLITE_TRANSIENT);
    if (include_passwords && a->password)
      sqlite3_bind_text (st, 5, a->password, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, 6, a->imap_host, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int (st, 7, a->imap_port);
    sqlite3_bind_int (st, 8, a->imap_security);
    sqlite3_bind_text (st, 9, a->smtp_host, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int (st, 10, a->smtp_port);
    sqlite3_bind_int (st, 11, a->smtp_security);
    sqlite3_bind_text (st, 12, a->signature, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step (st);
    sqlite3_finalize (st);
    if (rc != SQLITE_DONE) {
      sql_error (db, error, "Could not save accounts");
      goto out;
    }
    summary->accounts++;
  }
  summary->passwords = include_passwords;

  /* 3. downloaded messages and copies of sent mail */
  if (include_emails) {
    g_autoptr (GPtrArray) paths = g_ptr_array_new_with_free_func (g_free);
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2 (db, "SELECT path FROM messages UNION SELECT eml_path FROM mail_log WHERE eml_path IS NOT NULL",
                        -1, &st, NULL);
    while (sqlite3_step (st) == SQLITE_ROW)
      g_ptr_array_add (paths, g_strdup ((const char *) sqlite3_column_text (st, 0)));
    sqlite3_finalize (st);
    for (guint i = 0; i < paths->len; i++) {
      const char *p = g_ptr_array_index (paths, i);
      g_autofree char *rel = relative_path (data_dir, p, "mail/imported");
      if (!add_file (db, p, rel, summary, error))
        goto out;
    }
  } else {
    if (!exec (db, "DELETE FROM messages; UPDATE mail_log SET eml_path = NULL;", error))
      goto out;
  }

  fill_counts (db, "main", summary);
  now = g_date_time_new_now_local ();
  summary->created = g_date_time_format_iso8601 (now);
  set_meta (db, "format", FORMAT_NAME);
  version = g_strdup_printf ("%d", FORMAT_VERSION);
  set_meta (db, "version", version);
  set_meta (db, "created", summary->created);
  set_meta (db, "app-version", PACKAGE_VERSION);
  set_meta (db, "passwords", include_passwords ? "1" : "0");
  if (!exec (db, "COMMIT;", error))
    goto out;
  exec (db, "VACUUM;", NULL);

  sqlite3_close (db);
  db = NULL;
  g_chmod (tmp, 0600);
  if (g_rename (tmp, path) != 0) {
    g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno), "Could not save %s: %s", path, g_strerror (errno));
    goto out;
  }
  ok = TRUE;

out:
  if (db)
    sqlite3_close (db);
  if (!ok)
    g_unlink (tmp);
  smail_backup_summary_clear (&local);
  return ok;
}

/* ======================= open / inspect ======================= */

static sqlite3 *
open_backup (const char *path, GError **error)
{
  sqlite3 *db = NULL;
  if (sqlite3_open_v2 (path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not open the backup: %s", sqlite3_errmsg (db));
    sqlite3_close (db);
    return NULL;
  }
  g_autofree char *format = get_meta (db, "format");
  g_autofree char *version = get_meta (db, "version");
  if (g_strcmp0 (format, FORMAT_NAME) != 0) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "This file is not an SMail backup");
    sqlite3_close (db);
    return NULL;
  }
  if (!version || atoi (version) > FORMAT_VERSION) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                 "This backup was made by a newer version of SMail. Update SMail to restore it.");
    sqlite3_close (db);
    return NULL;
  }
  return db;
}

static void
read_summary (sqlite3 *db, SMailBackupSummary *s)
{
  memset (s, 0, sizeof *s);
  fill_counts (db, "main", s);
  s->accounts = count_rows (db, "main", "backup_accounts");
  sqlite3_stmt *st = NULL;
  if (sqlite3_prepare_v2 (db, "SELECT COUNT(*), IFNULL(SUM(LENGTH(data)), 0) FROM backup_files", -1, &st, NULL) == SQLITE_OK &&
      sqlite3_step (st) == SQLITE_ROW) {
    s->emails = sqlite3_column_int64 (st, 0);
    s->bytes = sqlite3_column_int64 (st, 1);
  }
  sqlite3_finalize (st);
  g_autofree char *pw = get_meta (db, "passwords");
  s->passwords = g_strcmp0 (pw, "1") == 0;
  s->created = get_meta (db, "created");
}

gboolean
smail_backup_inspect (const char *path, SMailBackupSummary *summary, GError **error)
{
  sqlite3 *db = open_backup (path, error);
  if (!db)
    return FALSE;
  read_summary (db, summary);
  sqlite3_close (db);
  return TRUE;
}

/* ======================= restore ======================= */

static char *
col_dup (sqlite3_stmt *st, int i)
{
  const unsigned char *t = sqlite3_column_text (st, i);
  return t ? g_strdup ((const char *) t) : NULL;
}

gboolean
smail_backup_restore (SMailStore *store, const char *path, GPtrArray **accounts_out,
                      SMailBackupSummary *summary, GError **error)
{
  /* validate and read accounts + files from the backup first */
  sqlite3 *bdb = open_backup (path, error);
  if (!bdb)
    return FALSE;
  SMailBackupSummary local = { 0 };
  read_summary (bdb, summary ? summary : &local);
  smail_backup_summary_clear (&local);

  g_autoptr (GPtrArray) accounts = g_ptr_array_new_with_free_func ((GDestroyNotify) smail_account_free);
  sqlite3_stmt *st = NULL;
  /* backups made before signatures existed don't have that column */
  gboolean has_signature = sqlite3_prepare_v2 (bdb, "SELECT id, name, email, username, password, imap_host,"
                                               " imap_port, imap_security, smtp_host, smtp_port, smtp_security,"
                                               " signature FROM backup_accounts", -1, &st, NULL) == SQLITE_OK;
  if (!has_signature) {
    sqlite3_finalize (st);
    sqlite3_prepare_v2 (bdb, "SELECT id, name, email, username, password, imap_host, imap_port, imap_security,"
                             " smtp_host, smtp_port, smtp_security FROM backup_accounts", -1, &st, NULL);
  }
  while (sqlite3_step (st) == SQLITE_ROW) {
    SMailAccount *a = smail_account_new ();
    a->id = col_dup (st, 0);
    a->name = col_dup (st, 1);
    a->email = col_dup (st, 2);
    a->username = col_dup (st, 3);
    a->password = col_dup (st, 4);
    a->imap_host = col_dup (st, 5);
    a->imap_port = sqlite3_column_int (st, 6);
    a->imap_security = sqlite3_column_int (st, 7);
    a->smtp_host = col_dup (st, 8);
    a->smtp_port = sqlite3_column_int (st, 9);
    a->smtp_security = sqlite3_column_int (st, 10);
    if (has_signature)
      a->signature = col_dup (st, 11);
    g_ptr_array_add (accounts, a);
  }
  sqlite3_finalize (st);

  /* write the stored files back under the data folder */
  g_autofree char *data_dir = smail_store_data_dir ();
  g_autoptr (GHashTable) moved = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  sqlite3_prepare_v2 (bdb, "SELECT orig_path, rel_path, data FROM backup_files", -1, &st, NULL);
  while (sqlite3_step (st) == SQLITE_ROW) {
    const char *orig = (const char *) sqlite3_column_text (st, 0);
    const char *rel = (const char *) sqlite3_column_text (st, 1);
    if (!orig || !rel || strstr (rel, "..") || g_path_is_absolute (rel))
      continue; /* never write outside the data folder */
    g_autofree char *dest = g_build_filename (data_dir, rel, NULL);
    g_autofree char *dir = g_path_get_dirname (dest);
    g_mkdir_with_parents (dir, 0700);
    if (!g_file_set_contents_full (dest, sqlite3_column_blob (st, 2), sqlite3_column_bytes (st, 2),
                                   G_FILE_SET_CONTENTS_CONSISTENT, 0600, error)) {
      sqlite3_finalize (st);
      sqlite3_close (bdb);
      return FALSE;
    }
    g_hash_table_insert (moved, g_strdup (orig), g_steal_pointer (&dest));
  }
  sqlite3_finalize (st);
  sqlite3_close (bdb);

  /* replace the local tables in one transaction */
  sqlite3 *db = smail_store_get_db (store);
  char *attach = sqlite3_mprintf ("ATTACH DATABASE %Q AS b", path);
  gboolean attached = exec (db, attach, error);
  sqlite3_free (attach);
  if (!attached)
    return FALSE;
  gboolean ok = exec (db, "BEGIN IMMEDIATE;", error);
  for (int i = 0; ok && data_tables[i]; i++) {
    /* backups from older versions may not have every table: keep ours then */
    if (!table_exists (db, "b", data_tables[i]))
      continue;
    g_autofree char *sql = g_strdup_printf ("DELETE FROM main.%s; INSERT INTO main.%s SELECT * FROM b.%s;",
                                            data_tables[i], data_tables[i], data_tables[i]);
    ok = exec (db, sql, error);
  }
  if (ok)
    ok = exec (db, "DELETE FROM main.settings WHERE key <> 'lan-id';"
                   "INSERT OR IGNORE INTO main.settings SELECT * FROM b.settings WHERE key <> 'lan-id';", error);

  /* point stored paths at the restored files; forget files that weren't in the backup */
  if (ok) {
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init (&it, moved);
    while (ok && g_hash_table_iter_next (&it, &k, &v)) {
      char *sql = sqlite3_mprintf ("UPDATE main.messages SET path = %Q WHERE path = %Q;"
                                   "UPDATE main.mail_log SET eml_path = %Q WHERE eml_path = %Q;",
                                   (char *) v, (char *) k, (char *) v, (char *) k);
      ok = exec (db, sql, error);
      sqlite3_free (sql);
    }
  }
  if (ok)
    ok = exec (db, "COMMIT;", error);
  else
    exec (db, "ROLLBACK;", NULL);
  exec (db, "DETACH DATABASE b;", NULL);
  if (!ok)
    return FALSE;

  if (accounts_out)
    *accounts_out = g_steal_pointer (&accounts);
  return TRUE;
}

char *
smail_backup_auto_path (const char *prefix)
{
  g_autofree char *data_dir = smail_store_data_dir ();
  g_autofree char *dir = g_build_filename (data_dir, "backups", NULL);
  g_mkdir_with_parents (dir, 0700);
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  g_autofree char *stamp = g_date_time_format (now, "%Y%m%d-%H%M%S");
  g_autofree char *name = g_strdup_printf ("%s-%s" SMAIL_BACKUP_EXTENSION, prefix, stamp);
  return g_build_filename (dir, name, NULL);
}
