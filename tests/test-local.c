/* test-local.c — database, filters, mail log and LAN messaging */
#include "smail-store.h"
#include "smail-automation.h"
#include "smail-lan.h"
#include "smail-vcard.h"
#include "smail-mbox.h"
#include "smail-gmime.h"

#include <glib/gstdio.h>

#include <string.h>

static gint64
now (void)
{
  return g_get_real_time () / G_USEC_PER_SEC;
}

static SMailStore *
open_memory (void)
{
  g_autoptr (GError) error = NULL;
  SMailStore *s = smail_store_open (":memory:", &error);
  g_assert_no_error (error);
  return s;
}

/* ---------------- store ---------------- */

static void
test_settings (void)
{
  SMailStore *s = open_memory ();
  g_assert_null (smail_store_get_setting (s, "x"));
  smail_store_set_setting (s, "x", "1");
  smail_store_set_setting (s, "x", "2");
  g_autofree char *v = smail_store_get_setting (s, "x");
  g_assert_cmpstr (v, ==, "2");
  smail_store_close (s);
}

static void
test_events_and_reminders (void)
{
  SMailStore *s = open_memory ();
  g_autoptr (GError) error = NULL;
  gint64 t = now () + 7200;

  SMailEvent e = { 0, "Team meeting", "Room 4", "bring slides", t, t + 1800, 15 };
  g_assert_true (smail_store_save_event (s, &e, &error));
  g_assert_no_error (error);
  g_assert_cmpint (e.id, >, 0);

  /* found when the range overlaps, not otherwise */
  g_autoptr (GPtrArray) day = smail_store_list_events (s, t - 100, t + 100);
  g_assert_cmpuint (day->len, ==, 1);
  g_autoptr (GPtrArray) before = smail_store_list_events (s, t - 7200, t - 3600);
  g_assert_cmpuint (before->len, ==, 0);

  /* linked reminder 15 minutes before */
  g_autoptr (GPtrArray) rem = smail_store_list_reminders (s, FALSE, 10);
  g_assert_cmpuint (rem->len, ==, 1);
  SMailReminder *r = g_ptr_array_index (rem, 0);
  g_assert_cmpint (r->at, ==, t - 15 * 60);
  g_assert_cmpstr (r->source, ==, "event");
  g_assert_nonnull (strstr (r->title, "Team meeting"));

  /* changing the reminder replaces it; removing it deletes it */
  e.remind_minutes = 60;
  g_assert_true (smail_store_save_event (s, &e, &error));
  g_autoptr (GPtrArray) rem2 = smail_store_list_reminders (s, FALSE, 10);
  g_assert_cmpuint (rem2->len, ==, 1);
  g_assert_cmpint (((SMailReminder *) g_ptr_array_index (rem2, 0))->at, ==, t - 3600);
  e.remind_minutes = -1;
  smail_store_save_event (s, &e, &error);
  g_autoptr (GPtrArray) rem3 = smail_store_list_reminders (s, FALSE, 10);
  g_assert_cmpuint (rem3->len, ==, 0);

  /* events without title are refused */
  SMailEvent bad = { 0, "", NULL, NULL, t, t, -1 };
  g_assert_false (smail_store_save_event (s, &bad, &error));
  g_clear_error (&error);

  e.remind_minutes = 5;
  smail_store_save_event (s, &e, &error);
  g_assert_true (smail_store_delete_event (s, e.id, &error));
  g_autoptr (GPtrArray) none = smail_store_list_events (s, 0, G_MAXINT64);
  g_assert_cmpuint (none->len, ==, 0);
  g_autoptr (GPtrArray) rem4 = smail_store_list_reminders (s, FALSE, 10);
  g_assert_cmpuint (rem4->len, ==, 0);
  smail_store_close (s);
}

static void
test_todos_and_due (void)
{
  SMailStore *s = open_memory ();
  g_autoptr (GError) error = NULL;

  SMailTodo a = { 0, "Pay rent", NULL, now () + 60, FALSE, TRUE, 0 };
  SMailTodo b = { 0, "Buy milk", NULL, 0, FALSE, FALSE, 0 };
  g_assert_true (smail_store_save_todo (s, &a, &error));
  g_assert_true (smail_store_save_todo (s, &b, &error));

  g_autoptr (GPtrArray) open = smail_store_list_todos (s, FALSE);
  g_assert_cmpuint (open->len, ==, 2);
  g_assert_cmpstr (((SMailTodo *) g_ptr_array_index (open, 0))->title, ==, "Pay rent"); /* due first */

  g_autoptr (GPtrArray) r = smail_store_list_reminders (s, FALSE, 10);
  g_assert_cmpuint (r->len, ==, 1);

  /* completing the task removes its reminder */
  a.done = TRUE;
  smail_store_save_todo (s, &a, &error);
  g_autoptr (GPtrArray) r2 = smail_store_list_reminders (s, FALSE, 10);
  g_assert_cmpuint (r2->len, ==, 0);
  g_autoptr (GPtrArray) done = smail_store_list_todos (s, TRUE);
  g_assert_cmpuint (done->len, ==, 1);

  /* standalone reminder: due → fired → snoozed */
  SMailReminder rem = { 0, "Call mom", now () - 5, FALSE, NULL, 0 };
  g_assert_true (smail_store_save_reminder (s, &rem, &error));
  g_autoptr (GPtrArray) due = smail_store_due_reminders (s, now ());
  g_assert_cmpuint (due->len, ==, 1);
  smail_store_mark_fired (s, rem.id);
  g_autoptr (GPtrArray) due2 = smail_store_due_reminders (s, now ());
  g_assert_cmpuint (due2->len, ==, 0);
  smail_store_snooze_reminder (s, rem.id, now () + 600);
  g_autoptr (GPtrArray) due3 = smail_store_due_reminders (s, now () + 601);
  g_assert_cmpuint (due3->len, ==, 1);
  smail_store_close (s);
}

/* ---------------- filters ---------------- */

static SMailMessage *
make_message (const char *uid, const char *headers)
{
  g_autoptr (GBytes) b = g_bytes_new (headers, strlen (headers));
  return smail_message_new_from_headers (uid, b, FALSE, FALSE);
}

static void
test_filters (void)
{
  g_autoptr (SMailMessage) m = make_message ("7",
    "From: Big Bank <alerts@bigbank.com>\r\nTo: Me <me@example.org>, Other <o@example.org>\r\n"
    "Subject: Your Monthly Statement\r\nDate: Tue, 03 Nov 2026 10:00:00 +0000\r\n\r\n");

  SMailFilter f = { 0, "bank", TRUE, "from", "contains", "BIGBANK", "move", "Finance" };
  g_assert_true (smail_filter_matches (&f, m));
  g_assert_true (smail_filter_is_final (&f));

  f.op = "ends-with"; f.value = "@bigbank.com";
  g_assert_true (smail_filter_matches (&f, m));
  f.op = "equals"; f.value = "alerts@bigbank.com";
  g_assert_true (smail_filter_matches (&f, m));
  f.op = "not-contains"; f.value = "bigbank";
  g_assert_false (smail_filter_matches (&f, m));

  SMailFilter subj = { 0, NULL, TRUE, "subject", "starts-with", "your monthly", "read", NULL };
  g_assert_true (smail_filter_matches (&subj, m));
  g_assert_false (smail_filter_is_final (&subj));
  subj.enabled = FALSE;
  g_assert_false (smail_filter_matches (&subj, m));

  SMailFilter to = { 0, NULL, TRUE, "to", "contains", "o@example.org", "star", NULL };
  g_assert_true (smail_filter_matches (&to, m));
  SMailFilter any = { 0, NULL, TRUE, "any", "contains", "statement", "trash", NULL };
  g_assert_true (smail_filter_matches (&any, m));
  any.value = "nothing-like-this";
  g_assert_false (smail_filter_matches (&any, m));

  g_autofree char *desc = smail_filter_describe (&f);
  g_assert_nonnull (strstr (desc, "From"));

  /* stored in order, edits keep the id */
  SMailStore *s = open_memory ();
  g_autoptr (GError) error = NULL;
  SMailFilter s1 = { 0, "a", TRUE, "from", "contains", "x", "move", "" };
  g_assert_false (smail_store_save_filter (s, &s1, &error)); /* move needs a folder */
  g_clear_error (&error);
  s1.target = "Folder";
  g_assert_true (smail_store_save_filter (s, &s1, &error));
  SMailFilter s2 = { 0, "b", TRUE, "subject", "contains", "y", "read", NULL };
  g_assert_true (smail_store_save_filter (s, &s2, &error));
  g_autoptr (GPtrArray) list = smail_store_list_filters (s);
  g_assert_cmpuint (list->len, ==, 2);
  g_assert_cmpstr (((SMailFilter *) g_ptr_array_index (list, 0))->name, ==, "a");
  smail_store_close (s);
}

/* ---------------- mail log ---------------- */

static void
test_mail_log (void)
{
  SMailStore *s = open_memory ();
  g_autoptr (GError) error = NULL;
  g_autoptr (SMailMessage) m = make_message ("42",
    "From: Alice <alice@example.org>\r\nTo: me@example.org\r\nSubject: Lunch?\r\n"
    "Date: Tue, 03 Nov 2026 10:00:00 +0000\r\n\r\n");

  SMailMailLogEntry *e = smail_mail_log_from_message (m, "me@example.org", "INBOX");
  g_assert_true (smail_store_log_mail (s, e, &error));
  g_assert_cmpint (e->id, >, 0);
  smail_mail_log_free (e);
  /* the same message is logged once */
  e = smail_mail_log_from_message (m, "me@example.org", "INBOX");
  g_assert_true (smail_store_log_mail (s, e, &error));
  g_assert_cmpint (e->id, ==, 0);
  smail_mail_log_free (e);

  const char *raw = "From: Me <me@example.org>\r\nTo: bob@example.org\r\nCc: carol@example.org\r\n"
                    "Subject: Report\r\nDate: Tue, 03 Nov 2026 11:00:00 +0000\r\nMessage-ID: <x1@example.org>\r\n"
                    "Content-Type: text/plain; charset=utf-8\r\n\r\nHere is the report.\r\n";
  g_autoptr (GBytes) bytes = g_bytes_new (raw, strlen (raw));
  g_autofree char *dir = g_dir_make_tmp ("smail-test-XXXXXX", NULL);
  e = smail_mail_log_from_sent (bytes, "me@example.org", dir, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (e->direction, ==, "sent");
  g_assert_cmpstr (e->preview, ==, "Here is the report.");
  g_assert_nonnull (strstr (e->recipients, "carol@example.org"));
  g_assert_true (g_file_test (e->eml_path, G_FILE_TEST_EXISTS));
  g_assert_true (smail_store_log_mail (s, e, &error));
  g_unlink (e->eml_path);
  g_rmdir (dir);
  smail_mail_log_free (e);

  g_autoptr (GPtrArray) all = smail_store_list_mail_log (s, NULL, NULL, 100);
  g_assert_cmpuint (all->len, ==, 2);
  g_assert_cmpstr (((SMailMailLogEntry *) g_ptr_array_index (all, 0))->direction, ==, "sent"); /* newest first */
  g_autoptr (GPtrArray) received = smail_store_list_mail_log (s, "received", NULL, 100);
  g_assert_cmpuint (received->len, ==, 1);
  g_autoptr (GPtrArray) search = smail_store_list_mail_log (s, NULL, "lunch", 100);
  g_assert_cmpuint (search->len, ==, 1);

  gboolean known;
  g_assert_cmpint (smail_store_get_last_uid (s, "me", "INBOX", &known), ==, 0);
  g_assert_false (known);
  smail_store_set_last_uid (s, "me", "INBOX", 99);
  g_assert_cmpint (smail_store_get_last_uid (s, "me", "INBOX", &known), ==, 99);
  g_assert_true (known);
  smail_store_close (s);
}

/* ---------------- contacts ---------------- */

static void
test_contacts (void)
{
  SMailStore *s = open_memory ();
  g_autoptr (GError) error = NULL;

  /* collected addresses: outgoing ones rank first, names fill in once */
  smail_store_collect_address (s, NULL, "newsletter@shop.example", FALSE);
  smail_store_collect_address (s, "Priya Raman", "priya@example.org", TRUE);
  smail_store_collect_address (s, "Priya R.", "PRIYA@example.org", TRUE);   /* same address, any case */
  smail_store_collect_address (s, "Bad", "not-an-address", TRUE);
  g_autoptr (GPtrArray) all = smail_store_list_contacts (s, NULL, 50);
  g_assert_cmpuint (all->len, ==, 2);
  SMailContact *first = g_ptr_array_index (all, 0);
  g_assert_cmpstr (first->email, ==, "priya@example.org");
  g_assert_cmpstr (first->name, ==, "Priya Raman");
  g_assert_cmpint (first->use_count, ==, 2);
  g_assert_true (first->collected);
  g_assert_true (smail_store_is_trusted_sender (s, "priya@example.org"));
  g_assert_false (smail_store_is_trusted_sender (s, "newsletter@shop.example"));

  /* adding a contact by hand */
  SMailContact c = { 0, "Ada Lovelace", " ada@example.org ", "+44 20 7946 0000", "Analytical engine", TRUE };
  g_assert_true (smail_store_save_contact (s, &c, &error));
  g_assert_no_error (error);
  g_assert_cmpint (c.id, >, 0);
  g_assert_true (smail_store_is_trusted_sender (s, "ada@example.org"));
  SMailContact bad = { 0, "x", "nope", NULL, NULL, FALSE };
  g_assert_false (smail_store_save_contact (s, &bad, &error));
  g_clear_error (&error);

  /* promoting a collected address keeps one entry */
  SMailContact promote = { 0, "Shop News", "newsletter@shop.example", NULL, NULL, FALSE };
  g_assert_true (smail_store_save_contact (s, &promote, &error));
  g_autoptr (GPtrArray) after = smail_store_list_contacts (s, NULL, 50);
  g_assert_cmpuint (after->len, ==, 3);

  /* search: word prefix beats substring, favorites and real contacts first */
  g_autoptr (GPtrArray) q = smail_store_list_contacts (s, "lov", 10);
  g_assert_cmpuint (q->len, ==, 1);
  g_assert_cmpstr (((SMailContact *) g_ptr_array_index (q, 0))->email, ==, "ada@example.org");
  g_autoptr (GPtrArray) q2 = smail_store_list_contacts (s, "example.org", 10);
  g_assert_cmpuint (q2->len, ==, 2);
  g_assert_cmpstr (((SMailContact *) g_ptr_array_index (q2, 0))->email, ==, "ada@example.org");

  /* vCard round trip */
  g_autofree char *vcf = smail_contacts_to_vcard (after);
  g_assert_nonnull (strstr (vcf, "FN:Ada Lovelace"));
  g_assert_nonnull (strstr (vcf, "N:Lovelace;Ada;;;"));
  g_autoptr (GPtrArray) back = smail_contacts_parse (vcf, strlen (vcf), &error);
  g_assert_no_error (error);
  g_assert_cmpuint (back->len, ==, 3);
  gboolean saw_ada = FALSE;
  for (guint i = 0; i < back->len; i++) {
    SMailContact *b = g_ptr_array_index (back, i);
    if (g_str_equal (b->email, "ada@example.org")) {
      saw_ada = TRUE;
      g_assert_cmpstr (b->name, ==, "Ada Lovelace");
      g_assert_cmpstr (b->phone, ==, "+44 20 7946 0000");
      g_assert_true (b->favorite);
    }
  }
  g_assert_true (saw_ada);

  g_assert_true (smail_store_delete_contact (s, c.id, &error));
  smail_store_close (s);
}

static void
test_contact_imports (void)
{
  g_autoptr (GError) error = NULL;

  /* vCard 2.1 from a phone: folding, groups, quoted-printable, two addresses */
  const char *vcf =
    "BEGIN:VCARD\r\nVERSION:2.1\r\nN:Dupont;Ren=C3=A9e;;;\r\n"
    "FN;CHARSET=UTF-8;ENCODING=QUOTED-PRINTABLE:Ren=C3=A9e Dupont\r\n"
    "item1.EMAIL;TYPE=INTERNET:renee@exam\r\n ple.fr\r\n"
    "EMAIL;WORK:r.dupont@work.example\r\nTEL;CELL:tel:+33 6 12 34 56 78\r\n"
    "NOTE:Met at\\, the conference\\nParis\r\nEND:VCARD\r\n"
    "BEGIN:VCARD\r\nVERSION:4.0\r\nN:;Solo;;;\r\nEMAIL:mailto:solo@example.com\r\nEND:VCARD\r\n"
    "BEGIN:VCARD\r\nVERSION:3.0\r\nFN:No Email\r\nEND:VCARD\r\n";
  g_autoptr (GPtrArray) v = smail_contacts_parse (vcf, strlen (vcf), &error);
  g_assert_no_error (error);
  g_assert_cmpuint (v->len, ==, 3);
  SMailContact *r1 = g_ptr_array_index (v, 0);
  g_assert_cmpstr (r1->name, ==, "Renée Dupont");
  g_assert_cmpstr (r1->email, ==, "renee@example.fr");
  g_assert_cmpstr (r1->phone, ==, "+33 6 12 34 56 78");
  g_assert_cmpstr (r1->notes, ==, "Met at, the conference\nParis");
  g_assert_cmpstr (((SMailContact *) g_ptr_array_index (v, 1))->email, ==, "r.dupont@work.example");
  SMailContact *solo = g_ptr_array_index (v, 2);
  g_assert_cmpstr (solo->name, ==, "Solo");
  g_assert_cmpstr (solo->email, ==, "solo@example.com");

  /* Thunderbird CSV export */
  const char *tb = "First Name,Last Name,Display Name,Nickname,Primary Email,Secondary Email,Mobile Number,Notes\r\n"
                   "Ada,Lovelace,Ada Lovelace,,ada@example.org,ada@home.example,+44 1,\"Likes engines, maths\"\r\n"
                   "Bob,,,,bob@example.org,,,\r\n\r\n";
  g_autoptr (GPtrArray) t = smail_contacts_parse (tb, strlen (tb), &error);
  g_assert_no_error (error);
  g_assert_cmpuint (t->len, ==, 3);
  SMailContact *a = g_ptr_array_index (t, 0);
  g_assert_cmpstr (a->name, ==, "Ada Lovelace");
  g_assert_cmpstr (a->notes, ==, "Likes engines, maths");
  g_assert_cmpstr (((SMailContact *) g_ptr_array_index (t, 1))->email, ==, "ada@home.example");
  g_assert_cmpstr (((SMailContact *) g_ptr_array_index (t, 2))->name, ==, "Bob");

  /* Google CSV with a BOM and several addresses in one cell */
  const char *gg = "\xEF\xBB\xBFName,Given Name,E-mail 1 - Type,E-mail 1 - Value,Phone 1 - Value\n"
                   "Grace Hopper,Grace,* Work,grace@navy.example ::: grace@home.example,555\n";
  g_autoptr (GPtrArray) g = smail_contacts_parse (gg, strlen (gg), &error);
  g_assert_no_error (error);
  g_assert_cmpuint (g->len, ==, 2);
  g_assert_cmpstr (((SMailContact *) g_ptr_array_index (g, 1))->email, ==, "grace@home.example");

  /* semicolon-separated (some locales) */
  const char *semi = "Name;Email\nZoe;zoe@example.net\n";
  g_autoptr (GPtrArray) z = smail_contacts_parse (semi, strlen (semi), &error);
  g_assert_cmpuint (z->len, ==, 1);

  /* a CSV without an email column is refused with a clear message */
  const char *junk = "foo,bar\n1,2\n";
  g_assert_null (smail_contacts_parse (junk, strlen (junk), &error));
  g_assert_nonnull (strstr (error->message, "email column"));
}

/* ---------------- mbox ---------------- */

static void
test_mbox (void)
{
  const char *m1 = "From: a@example.org\r\nSubject: one\r\n\r\nHello\r\nFrom here on, a line that looks like a separator\r\n";
  const char *m2 = "From: b@example.org\r\nSubject: two\r\n\r\n>From quoted already\r\nbye\r\n";
  g_autoptr (GBytes) b1 = g_bytes_new_static (m1, strlen (m1));
  g_autoptr (GBytes) b2 = g_bytes_new_static (m2, strlen (m2));

  g_autoptr (GString) mbox = g_string_new (NULL);
  g_autoptr (GDateTime) when = g_date_time_new_utc (2026, 11, 3, 10, 0, 0);
  smail_mbox_append (mbox, b1, "a@example.org", when);
  smail_mbox_append (mbox, b2, NULL, NULL);
  g_assert_true (g_str_has_prefix (mbox->str, "From a@example.org Tue Nov  3 10:00:00 2026\n"));
  g_assert_nonnull (strstr (mbox->str, "\n>From here on"));
  g_assert_nonnull (strstr (mbox->str, "\n>>From quoted already"));
  g_assert_null (strstr (mbox->str, "\r"));

  /* round trip restores both messages exactly (CRLF) */
  g_autoptr (GPtrArray) msgs = smail_mbox_split (mbox->str, mbox->len);
  g_assert_cmpuint (msgs->len, ==, 2);
  g_assert_true (g_bytes_equal (g_ptr_array_index (msgs, 0), b1));
  g_assert_true (g_bytes_equal (g_ptr_array_index (msgs, 1), b2));

  /* a single .eml file (no separator) is one message, LF input becomes CRLF */
  const char *eml = "From: c@example.org\nSubject: three\n\nbody\n";
  g_autoptr (GPtrArray) one = smail_mbox_split (eml, strlen (eml));
  g_assert_cmpuint (one->len, ==, 1);
  g_assert_cmpstr (g_bytes_get_data (g_ptr_array_index (one, 0), NULL), ==,
                   "From: c@example.org\r\nSubject: three\r\n\r\nbody\r\n");

  /* an mbox "From " line inside a body only counts after a blank line */
  const char *tricky = "From x Tue Nov  3 10:00:00 2026\nSubject: a\n\nline\nFrom y in the text\n\nFrom z Tue Nov  3 10:00:00 2026\nSubject: b\n\nok\n";
  g_autoptr (GPtrArray) two = smail_mbox_split (tricky, strlen (tricky));
  g_assert_cmpuint (two->len, ==, 2);
  g_assert_nonnull (strstr (g_bytes_get_data (g_ptr_array_index (two, 0), NULL), "From y in the text"));

  g_autoptr (GPtrArray) none = smail_mbox_split ("", 0);
  g_assert_cmpuint (none->len, ==, 0);
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
on_message (SMailLan *lan, const char *id, const char *name, const char *text, gint64 ts, Wait *w)
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
  w->peer_id = smail_lan_add_peer_finish (SMAIL_LAN (src), res, &w->error);
  g_main_loop_quit (w->loop);
}

static void
on_sent (GObject *src, GAsyncResult *res, gpointer data)
{
  Wait *w = data;
  w->ok = smail_lan_send_finish (SMAIL_LAN (src), res, &w->error);
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
  g_autoptr (SMailLan) alice = smail_lan_new ("alice-id", "Alice");
  g_autoptr (SMailLan) bob = smail_lan_new ("bob-id", "Bob");
  smail_lan_set_ports (alice, 47990, 0);
  smail_lan_set_ports (bob, 47991, 0);
  g_autoptr (GError) error = NULL;
  g_assert_true (smail_lan_start (alice, &error));
  g_assert_no_error (error);
  g_assert_true (smail_lan_start (bob, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (smail_lan_get_port (alice), >, 0);

  Wait w = { g_main_loop_new (NULL, FALSE) };
  guint guard = g_timeout_add_seconds (10, timeout_quit, w.loop);

  /* Alice adds Bob by address */
  smail_lan_add_peer_async (alice, "127.0.0.1", smail_lan_get_port (bob), NULL, on_added, &w);
  g_main_loop_run (w.loop);
  g_assert_no_error (w.error);
  g_assert_cmpstr (w.peer_id, ==, "bob-id");
  const SMailLanPeer *p = smail_lan_find_peer (alice, "bob-id");
  g_assert_nonnull (p);
  g_assert_cmpstr (p->name, ==, "Bob");
  g_assert_true (smail_lan_peer_online (p));

  /* adding yourself is refused */
  smail_lan_add_peer_async (alice, "127.0.0.1", smail_lan_get_port (alice), NULL, on_added, &w);
  g_main_loop_run (w.loop);
  g_assert_nonnull (w.error);
  g_clear_error (&w.error);

  /* Alice → Bob */
  g_clear_pointer (&w.peer_id, g_free);
  g_signal_connect (bob, "message-received", G_CALLBACK (on_message), &w);
  smail_lan_send_async (alice, "bob-id", "Hello Bob 👋", NULL, on_sent, &w);
  g_main_loop_run (w.loop);
  g_assert_no_error (w.error);
  g_assert_cmpstr (w.peer_id, ==, "alice-id");
  g_assert_cmpstr (w.text, ==, "Hello Bob 👋");
  /* Bob learned about Alice from the message and can answer */
  g_assert_nonnull (smail_lan_find_peer (bob, "alice-id"));

  g_clear_pointer (&w.peer_id, g_free);
  g_clear_pointer (&w.text, g_free);
  g_signal_connect (alice, "message-received", G_CALLBACK (on_message), &w);
  smail_lan_send_async (bob, "alice-id", "Hi Alice", NULL, on_sent, &w);
  g_main_loop_run (w.loop);
  g_assert_no_error (w.error);
  g_assert_cmpstr (w.text, ==, "Hi Alice");

  /* unknown peer */
  w.ok = TRUE;
  smail_lan_send_async (alice, "nobody", "x", NULL, on_sent, &w);
  g_main_loop_run (w.loop);
  g_assert_false (w.ok);
  g_clear_error (&w.error);

  smail_lan_stop (alice);
  smail_lan_stop (bob);
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
  g_test_add_func ("/contacts/store", test_contacts);
  g_test_add_func ("/contacts/import", test_contact_imports);
  g_test_add_func ("/mbox/round-trip", test_mbox);
  g_test_add_func ("/lan/chat", test_lan);
  int ret = g_test_run ();
  g_mime_shutdown ();
  return ret;
}
