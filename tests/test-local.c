/* test-local.c — database, filters, mail log and LAN messaging */
#include "courier-store.h"
#include "courier-automation.h"
#include "courier-lan.h"
#include "courier-gmime.h"

#include <glib/gstdio.h>

#include <string.h>

static gint64
now (void)
{
  return g_get_real_time () / G_USEC_PER_SEC;
}

static CourierStore *
open_memory (void)
{
  g_autoptr (GError) error = NULL;
  CourierStore *s = courier_store_open (":memory:", &error);
  g_assert_no_error (error);
  return s;
}

/* ---------------- store ---------------- */

static void
test_settings (void)
{
  CourierStore *s = open_memory ();
  g_assert_null (courier_store_get_setting (s, "x"));
  courier_store_set_setting (s, "x", "1");
  courier_store_set_setting (s, "x", "2");
  g_autofree char *v = courier_store_get_setting (s, "x");
  g_assert_cmpstr (v, ==, "2");
  courier_store_close (s);
}

static void
test_events_and_reminders (void)
{
  CourierStore *s = open_memory ();
  g_autoptr (GError) error = NULL;
  gint64 t = now () + 7200;

  CourierEvent e = { 0, "Team meeting", "Room 4", "bring slides", t, t + 1800, 15 };
  g_assert_true (courier_store_save_event (s, &e, &error));
  g_assert_no_error (error);
  g_assert_cmpint (e.id, >, 0);

  /* found when the range overlaps, not otherwise */
  g_autoptr (GPtrArray) day = courier_store_list_events (s, t - 100, t + 100);
  g_assert_cmpuint (day->len, ==, 1);
  g_autoptr (GPtrArray) before = courier_store_list_events (s, t - 7200, t - 3600);
  g_assert_cmpuint (before->len, ==, 0);

  /* linked reminder 15 minutes before */
  g_autoptr (GPtrArray) rem = courier_store_list_reminders (s, FALSE, 10);
  g_assert_cmpuint (rem->len, ==, 1);
  CourierReminder *r = g_ptr_array_index (rem, 0);
  g_assert_cmpint (r->at, ==, t - 15 * 60);
  g_assert_cmpstr (r->source, ==, "event");
  g_assert_nonnull (strstr (r->title, "Team meeting"));

  /* changing the reminder replaces it; removing it deletes it */
  e.remind_minutes = 60;
  g_assert_true (courier_store_save_event (s, &e, &error));
  g_autoptr (GPtrArray) rem2 = courier_store_list_reminders (s, FALSE, 10);
  g_assert_cmpuint (rem2->len, ==, 1);
  g_assert_cmpint (((CourierReminder *) g_ptr_array_index (rem2, 0))->at, ==, t - 3600);
  e.remind_minutes = -1;
  courier_store_save_event (s, &e, &error);
  g_autoptr (GPtrArray) rem3 = courier_store_list_reminders (s, FALSE, 10);
  g_assert_cmpuint (rem3->len, ==, 0);

  /* events without title are refused */
  CourierEvent bad = { 0, "", NULL, NULL, t, t, -1 };
  g_assert_false (courier_store_save_event (s, &bad, &error));
  g_clear_error (&error);

  e.remind_minutes = 5;
  courier_store_save_event (s, &e, &error);
  g_assert_true (courier_store_delete_event (s, e.id, &error));
  g_autoptr (GPtrArray) none = courier_store_list_events (s, 0, G_MAXINT64);
  g_assert_cmpuint (none->len, ==, 0);
  g_autoptr (GPtrArray) rem4 = courier_store_list_reminders (s, FALSE, 10);
  g_assert_cmpuint (rem4->len, ==, 0);
  courier_store_close (s);
}

static void
test_todos_and_due (void)
{
  CourierStore *s = open_memory ();
  g_autoptr (GError) error = NULL;

  CourierTodo a = { 0, "Pay rent", NULL, now () + 60, FALSE, TRUE, 0 };
  CourierTodo b = { 0, "Buy milk", NULL, 0, FALSE, FALSE, 0 };
  g_assert_true (courier_store_save_todo (s, &a, &error));
  g_assert_true (courier_store_save_todo (s, &b, &error));

  g_autoptr (GPtrArray) open = courier_store_list_todos (s, FALSE);
  g_assert_cmpuint (open->len, ==, 2);
  g_assert_cmpstr (((CourierTodo *) g_ptr_array_index (open, 0))->title, ==, "Pay rent"); /* due first */

  g_autoptr (GPtrArray) r = courier_store_list_reminders (s, FALSE, 10);
  g_assert_cmpuint (r->len, ==, 1);

  /* completing the task removes its reminder */
  a.done = TRUE;
  courier_store_save_todo (s, &a, &error);
  g_autoptr (GPtrArray) r2 = courier_store_list_reminders (s, FALSE, 10);
  g_assert_cmpuint (r2->len, ==, 0);
  g_autoptr (GPtrArray) done = courier_store_list_todos (s, TRUE);
  g_assert_cmpuint (done->len, ==, 1);

  /* standalone reminder: due → fired → snoozed */
  CourierReminder rem = { 0, "Call mom", now () - 5, FALSE, NULL, 0 };
  g_assert_true (courier_store_save_reminder (s, &rem, &error));
  g_autoptr (GPtrArray) due = courier_store_due_reminders (s, now ());
  g_assert_cmpuint (due->len, ==, 1);
  courier_store_mark_fired (s, rem.id);
  g_autoptr (GPtrArray) due2 = courier_store_due_reminders (s, now ());
  g_assert_cmpuint (due2->len, ==, 0);
  courier_store_snooze_reminder (s, rem.id, now () + 600);
  g_autoptr (GPtrArray) due3 = courier_store_due_reminders (s, now () + 601);
  g_assert_cmpuint (due3->len, ==, 1);
  courier_store_close (s);
}

/* ---------------- filters ---------------- */

static CourierMessage *
make_message (const char *uid, const char *headers)
{
  g_autoptr (GBytes) b = g_bytes_new (headers, strlen (headers));
  return courier_message_new_from_headers (uid, b, FALSE, FALSE);
}

static void
test_filters (void)
{
  g_autoptr (CourierMessage) m = make_message ("7",
    "From: Big Bank <alerts@bigbank.com>\r\nTo: Me <me@example.org>, Other <o@example.org>\r\n"
    "Subject: Your Monthly Statement\r\nDate: Tue, 03 Nov 2026 10:00:00 +0000\r\n\r\n");

  CourierFilter f = { 0, "bank", TRUE, "from", "contains", "BIGBANK", "move", "Finance" };
  g_assert_true (courier_filter_matches (&f, m));
  g_assert_true (courier_filter_is_final (&f));

  f.op = "ends-with"; f.value = "@bigbank.com";
  g_assert_true (courier_filter_matches (&f, m));
  f.op = "equals"; f.value = "alerts@bigbank.com";
  g_assert_true (courier_filter_matches (&f, m));
  f.op = "not-contains"; f.value = "bigbank";
  g_assert_false (courier_filter_matches (&f, m));

  CourierFilter subj = { 0, NULL, TRUE, "subject", "starts-with", "your monthly", "read", NULL };
  g_assert_true (courier_filter_matches (&subj, m));
  g_assert_false (courier_filter_is_final (&subj));
  subj.enabled = FALSE;
  g_assert_false (courier_filter_matches (&subj, m));

  CourierFilter to = { 0, NULL, TRUE, "to", "contains", "o@example.org", "star", NULL };
  g_assert_true (courier_filter_matches (&to, m));
  CourierFilter any = { 0, NULL, TRUE, "any", "contains", "statement", "trash", NULL };
  g_assert_true (courier_filter_matches (&any, m));
  any.value = "nothing-like-this";
  g_assert_false (courier_filter_matches (&any, m));

  g_autofree char *desc = courier_filter_describe (&f);
  g_assert_nonnull (strstr (desc, "From"));

  /* stored in order, edits keep the id */
  CourierStore *s = open_memory ();
  g_autoptr (GError) error = NULL;
  CourierFilter s1 = { 0, "a", TRUE, "from", "contains", "x", "move", "" };
  g_assert_false (courier_store_save_filter (s, &s1, &error)); /* move needs a folder */
  g_clear_error (&error);
  s1.target = "Folder";
  g_assert_true (courier_store_save_filter (s, &s1, &error));
  CourierFilter s2 = { 0, "b", TRUE, "subject", "contains", "y", "read", NULL };
  g_assert_true (courier_store_save_filter (s, &s2, &error));
  g_autoptr (GPtrArray) list = courier_store_list_filters (s);
  g_assert_cmpuint (list->len, ==, 2);
  g_assert_cmpstr (((CourierFilter *) g_ptr_array_index (list, 0))->name, ==, "a");
  courier_store_close (s);
}

/* ---------------- mail log ---------------- */

static void
test_mail_log (void)
{
  CourierStore *s = open_memory ();
  g_autoptr (GError) error = NULL;
  g_autoptr (CourierMessage) m = make_message ("42",
    "From: Alice <alice@example.org>\r\nTo: me@example.org\r\nSubject: Lunch?\r\n"
    "Date: Tue, 03 Nov 2026 10:00:00 +0000\r\n\r\n");

  CourierMailLogEntry *e = courier_mail_log_from_message (m, "me@example.org", "INBOX");
  g_assert_true (courier_store_log_mail (s, e, &error));
  g_assert_cmpint (e->id, >, 0);
  courier_mail_log_free (e);
  /* the same message is logged once */
  e = courier_mail_log_from_message (m, "me@example.org", "INBOX");
  g_assert_true (courier_store_log_mail (s, e, &error));
  g_assert_cmpint (e->id, ==, 0);
  courier_mail_log_free (e);

  const char *raw = "From: Me <me@example.org>\r\nTo: bob@example.org\r\nCc: carol@example.org\r\n"
                    "Subject: Report\r\nDate: Tue, 03 Nov 2026 11:00:00 +0000\r\nMessage-ID: <x1@example.org>\r\n"
                    "Content-Type: text/plain; charset=utf-8\r\n\r\nHere is the report.\r\n";
  g_autoptr (GBytes) bytes = g_bytes_new (raw, strlen (raw));
  g_autofree char *dir = g_dir_make_tmp ("courier-test-XXXXXX", NULL);
  e = courier_mail_log_from_sent (bytes, "me@example.org", dir, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (e->direction, ==, "sent");
  g_assert_cmpstr (e->preview, ==, "Here is the report.");
  g_assert_nonnull (strstr (e->recipients, "carol@example.org"));
  g_assert_true (g_file_test (e->eml_path, G_FILE_TEST_EXISTS));
  g_assert_true (courier_store_log_mail (s, e, &error));
  g_unlink (e->eml_path);
  g_rmdir (dir);
  courier_mail_log_free (e);

  g_autoptr (GPtrArray) all = courier_store_list_mail_log (s, NULL, NULL, 100);
  g_assert_cmpuint (all->len, ==, 2);
  g_assert_cmpstr (((CourierMailLogEntry *) g_ptr_array_index (all, 0))->direction, ==, "sent"); /* newest first */
  g_autoptr (GPtrArray) received = courier_store_list_mail_log (s, "received", NULL, 100);
  g_assert_cmpuint (received->len, ==, 1);
  g_autoptr (GPtrArray) search = courier_store_list_mail_log (s, NULL, "lunch", 100);
  g_assert_cmpuint (search->len, ==, 1);

  gboolean known;
  g_assert_cmpint (courier_store_get_last_uid (s, "me", "INBOX", &known), ==, 0);
  g_assert_false (known);
  courier_store_set_last_uid (s, "me", "INBOX", 99);
  g_assert_cmpint (courier_store_get_last_uid (s, "me", "INBOX", &known), ==, 99);
  g_assert_true (known);
  courier_store_close (s);
}

/* ---------------- LAN ---------------- */

typedef struct {
  GMainLoop *loop;
  char      *peer_id;
  char      *text;
  gboolean   ok;
  GError    *error;
} Wait;

static void
on_message (CourierLan *lan, const char *id, const char *name, const char *text, gint64 ts, Wait *w)
{
  w->peer_id = g_strdup (id);
  w->text = g_strdup (text);
  g_main_loop_quit (w->loop);
}

static void
on_added (GObject *src, GAsyncResult *res, gpointer data)
{
  Wait *w = data;
  g_free (w->peer_id);
  w->peer_id = courier_lan_add_peer_finish (COURIER_LAN (src), res, &w->error);
  g_main_loop_quit (w->loop);
}

static void
on_sent (GObject *src, GAsyncResult *res, gpointer data)
{
  Wait *w = data;
  w->ok = courier_lan_send_finish (COURIER_LAN (src), res, &w->error);
  if (!w->ok)
    g_main_loop_quit (w->loop);
}

static gboolean
timeout_quit (gpointer data)
{
  g_main_loop_quit (data);
  return G_SOURCE_REMOVE;
}

static void
test_lan (void)
{
  g_autoptr (CourierLan) alice = courier_lan_new ("alice-id", "Alice");
  g_autoptr (CourierLan) bob = courier_lan_new ("bob-id", "Bob");
  courier_lan_set_ports (alice, 47990, 0);
  courier_lan_set_ports (bob, 47991, 0);
  g_autoptr (GError) error = NULL;
  g_assert_true (courier_lan_start (alice, &error));
  g_assert_no_error (error);
  g_assert_true (courier_lan_start (bob, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (courier_lan_get_port (alice), >, 0);

  Wait w = { g_main_loop_new (NULL, FALSE) };
  guint guard = g_timeout_add_seconds (10, timeout_quit, w.loop);

  /* Alice adds Bob by address */
  courier_lan_add_peer_async (alice, "127.0.0.1", courier_lan_get_port (bob), NULL, on_added, &w);
  g_main_loop_run (w.loop);
  g_assert_no_error (w.error);
  g_assert_cmpstr (w.peer_id, ==, "bob-id");
  const CourierLanPeer *p = courier_lan_find_peer (alice, "bob-id");
  g_assert_nonnull (p);
  g_assert_cmpstr (p->name, ==, "Bob");
  g_assert_true (courier_lan_peer_online (p));

  /* adding yourself is refused */
  courier_lan_add_peer_async (alice, "127.0.0.1", courier_lan_get_port (alice), NULL, on_added, &w);
  g_main_loop_run (w.loop);
  g_assert_nonnull (w.error);
  g_clear_error (&w.error);

  /* Alice → Bob */
  g_clear_pointer (&w.peer_id, g_free);
  g_signal_connect (bob, "message-received", G_CALLBACK (on_message), &w);
  courier_lan_send_async (alice, "bob-id", "Hello Bob 👋", NULL, on_sent, &w);
  g_main_loop_run (w.loop);
  g_assert_no_error (w.error);
  g_assert_cmpstr (w.peer_id, ==, "alice-id");
  g_assert_cmpstr (w.text, ==, "Hello Bob 👋");
  /* Bob learned about Alice from the message and can answer */
  g_assert_nonnull (courier_lan_find_peer (bob, "alice-id"));

  g_clear_pointer (&w.peer_id, g_free);
  g_clear_pointer (&w.text, g_free);
  g_signal_connect (alice, "message-received", G_CALLBACK (on_message), &w);
  courier_lan_send_async (bob, "alice-id", "Hi Alice", NULL, on_sent, &w);
  g_main_loop_run (w.loop);
  g_assert_no_error (w.error);
  g_assert_cmpstr (w.text, ==, "Hi Alice");

  /* unknown peer */
  w.ok = TRUE;
  courier_lan_send_async (alice, "nobody", "x", NULL, on_sent, &w);
  g_main_loop_run (w.loop);
  g_assert_false (w.ok);
  g_clear_error (&w.error);

  courier_lan_stop (alice);
  courier_lan_stop (bob);
  g_source_remove (guard);
  g_main_loop_unref (w.loop);
  g_free (w.peer_id);
  g_free (w.text);
}

int
main (int argc, char *argv[])
{
  g_mime_init ();
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/store/settings", test_settings);
  g_test_add_func ("/store/events", test_events_and_reminders);
  g_test_add_func ("/store/todos", test_todos_and_due);
  g_test_add_func ("/filters/match", test_filters);
  g_test_add_func ("/maillog/log", test_mail_log);
  g_test_add_func ("/lan/chat", test_lan);
  int ret = g_test_run ();
  g_mime_shutdown ();
  return ret;
}
