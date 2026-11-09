/* test-backup.c — downloaded-message store and backup/restore round trip */
#include "smail-backup.h"
#include "smail-gmime.h"

#include <glib/gstdio.h>
#include <string.h>

static const char *raw_mail = "From: Alice <alice@example.org>\r\nTo: me@example.org\r\nSubject: Saved\r\n"
                              "Date: Tue, 03 Nov 2026 10:00:00 +0000\r\n\r\nKeep me.\r\n";

static char *tmp_root;

static SMailStore *
open_store (const char *name)
{
  g_autofree char *path = g_build_filename (tmp_root, name, NULL);
  g_autoptr (GError) error = NULL;
  SMailStore *s = smail_store_open (path, &error);
  g_assert_no_error (error);
  return s;
}

static void
fill (SMailStore *s)
{
  g_autoptr (GError) error = NULL;
  SMailFilter f = { 0, "bank", TRUE, "from", "contains", "bank", "move", "Finance" };
  g_assert_true (smail_store_save_filter (s, &f, &error));
  gint64 t = g_get_real_time () / G_USEC_PER_SEC + 86400;
  SMailEvent e = { 0, "Dentist", "Main St", NULL, t, t + 1800, 30 };
  g_assert_true (smail_store_save_event (s, &e, &error));
  SMailTodo todo = { 0, "Renew passport", NULL, 0, FALSE, FALSE, 0 };
  g_assert_true (smail_store_save_todo (s, &todo, &error));
  SMailChatMessage m = { 0, "peer-1", "Bob", TRUE, "hello", 1 };
  g_assert_true (smail_store_add_chat_message (s, &m, &error));
  SMailMailLogEntry log = { 0, "received", "me@example.org", "INBOX", "5", NULL, "Alice", "me", "Saved", 1, 0, NULL, NULL };
  g_assert_true (smail_store_log_mail (s, &log, &error));
  smail_store_set_setting (s, "theme", "matte-black");
  smail_store_set_setting (s, "lan-id", "machine-A");

  g_autoptr (GBytes) bytes = g_bytes_new_static (raw_mail, strlen (raw_mail));
  g_autofree char *path = NULL;
  g_assert_true (smail_store_save_message (s, "me@example.org", "INBOX", "5", bytes, &path, &error));
  g_assert_true (g_file_test (path, G_FILE_TEST_EXISTS));
}

static GPtrArray *
make_accounts (void)
{
  GPtrArray *accounts = g_ptr_array_new_with_free_func ((GDestroyNotify) smail_account_free);
  SMailAccount *a = smail_account_new ();
  a->id = g_strdup ("acc-1");
  a->name = g_strdup ("Me");
  a->email = g_strdup ("me@example.org");
  a->username = g_strdup ("me@example.org");
  a->password = g_strdup ("s3cret");
  a->imap_host = g_strdup ("imap.example.org");
  a->smtp_host = g_strdup ("smtp.example.org");
  a->smtp_security = SMAIL_SECURITY_STARTTLS;
  a->smtp_port = 587;
  a->signature = g_strdup ("Cheers,\nMe");
  g_ptr_array_add (accounts, a);
  return accounts;
}

static void
test_message_store (void)
{
  SMailStore *s = open_store ("msgs.db");
  g_autoptr (GError) error = NULL;
  SMailMailLogEntry log = { 0, "received", "me@example.org", "INBOX", "9", NULL, "A", "B", "Subj", 1, 0, NULL, NULL };
  smail_store_log_mail (s, &log, &error);

  g_assert_null (smail_store_load_message (s, "me@example.org", "INBOX", "9"));
  g_autoptr (GBytes) bytes = g_bytes_new_static (raw_mail, strlen (raw_mail));
  g_autofree char *path = NULL;
  /* hostile folder / uid names stay inside the mail folder */
  g_assert_true (smail_store_save_message (s, "me@example.org", "../../etc", "../9", bytes, &path, &error));
  g_autofree char *root = smail_store_mail_dir ();
  g_assert_true (g_str_has_prefix (path, root));
  g_assert_null (strstr (path + strlen (root), ".."));
  g_clear_pointer (&path, g_free);

  g_assert_true (smail_store_save_message (s, "me@example.org", "INBOX", "9", bytes, &path, &error));
  g_autoptr (GBytes) back = smail_store_load_message (s, "me@example.org", "INBOX", "9");
  g_assert_nonnull (back);
  g_assert_true (g_bytes_equal (back, bytes));

  /* the mail log points at the saved copy */
  g_autoptr (GPtrArray) entries = smail_store_list_mail_log (s, "received", NULL, 10);
  g_assert_cmpstr (((SMailMailLogEntry *) g_ptr_array_index (entries, 0))->eml_path, ==, path);

  gint64 count = 0, size = 0;
  smail_store_message_stats (s, &count, &size);
  g_assert_cmpint (count, ==, 2);
  g_assert_cmpint (size, ==, 2 * (gint64) strlen (raw_mail));
  smail_store_close (s);
}

static void
test_round_trip (void)
{
  g_autoptr (GError) error = NULL;
  SMailStore *a = open_store ("a.db");
  fill (a);
  g_autoptr (GPtrArray) accounts = make_accounts ();
  g_autofree char *file = g_build_filename (tmp_root, "full" SMAIL_BACKUP_EXTENSION, NULL);

  SMailBackupSummary sum = { 0 };
  g_assert_true (smail_backup_create (a, accounts, file, TRUE, TRUE, &sum, &error));
  g_assert_no_error (error);
  g_assert_cmpint (sum.accounts, ==, 1);
  g_assert_cmpint (sum.filters, ==, 1);
  g_assert_cmpint (sum.events, ==, 1);
  g_assert_cmpint (sum.todos, ==, 1);
  g_assert_cmpint (sum.chat_messages, ==, 1);
  g_assert_cmpint (sum.emails, ==, 1);
  g_assert_true (sum.passwords);
  smail_backup_summary_clear (&sum);

  SMailBackupSummary info = { 0 };
  g_assert_true (smail_backup_inspect (file, &info, &error));
  g_assert_cmpint (info.emails, ==, 1);
  g_assert_cmpint (info.reminders, ==, 1);  /* the event's reminder */
  g_assert_nonnull (info.created);
  smail_backup_summary_clear (&info);

  /* "new computer": the saved message file is gone and the database is empty */
  g_autofree char *mail_dir = smail_store_mail_dir ();
  g_autofree char *eml = g_build_filename (mail_dir, "me@example.org", "INBOX", "5.eml", NULL);
  g_assert_cmpint (g_unlink (eml), ==, 0);
  SMailStore *b = open_store ("b.db");
  smail_store_set_setting (b, "lan-id", "machine-B");

  g_autoptr (GPtrArray) restored = NULL;
  g_assert_true (smail_backup_restore (b, file, &restored, NULL, &error));
  g_assert_no_error (error);

  g_assert_cmpuint (restored->len, ==, 1);
  SMailAccount *ra = g_ptr_array_index (restored, 0);
  g_assert_cmpstr (ra->email, ==, "me@example.org");
  g_assert_cmpstr (ra->password, ==, "s3cret");
  g_assert_cmpint (ra->smtp_port, ==, 587);
  g_assert_cmpint (ra->smtp_security, ==, SMAIL_SECURITY_STARTTLS);
  g_assert_cmpstr (ra->signature, ==, "Cheers,\nMe");

  g_autoptr (GPtrArray) filters = smail_store_list_filters (b);
  g_assert_cmpuint (filters->len, ==, 1);
  g_autoptr (GPtrArray) events = smail_store_list_events (b, 0, G_MAXINT64);
  g_assert_cmpuint (events->len, ==, 1);
  g_autoptr (GPtrArray) todos = smail_store_list_todos (b, FALSE);
  g_assert_cmpuint (todos->len, ==, 1);
  g_autoptr (GPtrArray) chat = smail_store_list_chat_messages (b, "peer-1", 10);
  g_assert_cmpuint (chat->len, ==, 1);
  g_autofree char *theme = smail_store_get_setting (b, "theme");
  g_assert_cmpstr (theme, ==, "matte-black");
  g_autofree char *lan = smail_store_get_setting (b, "lan-id");
  g_assert_cmpstr (lan, ==, "machine-B");     /* identity is not copied */

  g_autoptr (GBytes) msg = smail_store_load_message (b, "me@example.org", "INBOX", "5");
  g_assert_nonnull (msg);
  g_assert_cmpuint (g_bytes_get_size (msg), ==, strlen (raw_mail));
  g_assert_true (g_file_test (eml, G_FILE_TEST_EXISTS));

  /* restoring replaces, it doesn't duplicate */
  g_assert_true (smail_backup_restore (b, file, NULL, NULL, &error));
  g_autoptr (GPtrArray) filters2 = smail_store_list_filters (b);
  g_assert_cmpuint (filters2->len, ==, 1);

  smail_store_close (a);
  smail_store_close (b);
}

static void
test_options_and_errors (void)
{
  g_autoptr (GError) error = NULL;
  SMailStore *a = open_store ("c.db");
  fill (a);
  g_autoptr (GPtrArray) accounts = make_accounts ();
  g_autofree char *file = g_build_filename (tmp_root, "lite" SMAIL_BACKUP_EXTENSION, NULL);
  SMailBackupSummary sum = { 0 };
  g_assert_true (smail_backup_create (a, accounts, file, FALSE, FALSE, &sum, &error));
  g_assert_cmpint (sum.emails, ==, 0);
  g_assert_false (sum.passwords);
  smail_backup_summary_clear (&sum);

  SMailStore *b = open_store ("d.db");
  g_autoptr (GPtrArray) restored = NULL;
  g_assert_true (smail_backup_restore (b, file, &restored, NULL, &error));
  g_assert_null (((SMailAccount *) g_ptr_array_index (restored, 0))->password);
  g_assert_null (smail_store_load_message (b, "me@example.org", "INBOX", "5"));

  /* not a backup */
  g_autofree char *junk = g_build_filename (tmp_root, "junk.smailbackup", NULL);
  g_file_set_contents (junk, "hello", -1, NULL);
  g_assert_false (smail_backup_inspect (junk, &sum, &error));
  g_assert_nonnull (error);
  g_clear_error (&error);
  g_assert_false (smail_backup_restore (b, a ? junk : NULL, NULL, NULL, &error));
  g_clear_error (&error);
  /* a plain SMail database is not a backup either */
  g_autofree char *plain = g_build_filename (tmp_root, "c.db", NULL);
  g_assert_false (smail_backup_inspect (plain, &sum, &error));
  g_clear_error (&error);

  smail_store_close (a);
  smail_store_close (b);
}

int
main (int argc, char *argv[])
{
  tmp_root = g_dir_make_tmp ("smail-backup-test-XXXXXX", NULL);
  g_autofree char *data = g_build_filename (tmp_root, "data", NULL);
  g_setenv ("XDG_DATA_HOME", data, TRUE);
  g_mime_init ();
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/store/messages", test_message_store);
  g_test_add_func ("/backup/round-trip", test_round_trip);
  g_test_add_func ("/backup/options", test_options_and_errors);
  int ret = g_test_run ();
  g_mime_shutdown ();
  const char *argv_rm[] = { "rm", "-rf", tmp_root, NULL };
  g_spawn_sync (NULL, (char **) argv_rm, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
  return ret;
}
