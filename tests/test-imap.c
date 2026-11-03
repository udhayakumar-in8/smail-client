/* test-imap.c — exercises the IMAP client, MIME parsing and SMTP against real servers.
 *
 * Needs an IMAP server with a test login, e.g. a local Dovecot:
 *   SMAIL_TEST_IMAP=127.0.0.1:11430 SMAIL_TEST_USER=test@local SMAIL_TEST_PASS=secret
 * Optionally an SMTP sink for the send test:
 *   SMAIL_TEST_SMTP=127.0.0.1:2525
 * Without SMAIL_TEST_IMAP only the offline tests run. */
#include "smail-imap.h"
#include "smail-backend.h"
#include "smail-gmime.h"
#include "smail-util.h"

#include <string.h>

static SMailAccount *
test_account (void)
{
  const char *server = g_getenv ("SMAIL_TEST_IMAP");
  if (!server)
    return NULL;
  g_auto (GStrv) hp = g_strsplit (server, ":", 2);
  SMailAccount *a = smail_account_new ();
  a->id = g_strdup ("test");
  a->name = g_strdup ("Test User");
  a->email = g_strdup ("test@local");
  a->username = g_strdup (g_getenv ("SMAIL_TEST_USER"));
  a->password = g_strdup (g_getenv ("SMAIL_TEST_PASS"));
  a->imap_host = g_strdup (hp[0]);
  a->imap_port = hp[1] ? atoi (hp[1]) : 143;
  a->imap_security = SMAIL_SECURITY_NONE;
  const char *smtp = g_getenv ("SMAIL_TEST_SMTP");
  if (smtp) {
    g_auto (GStrv) sp = g_strsplit (smtp, ":", 2);
    a->smtp_host = g_strdup (sp[0]);
    a->smtp_port = sp[1] ? atoi (sp[1]) : 25;
    a->smtp_security = SMAIL_SECURITY_NONE;
  }
  return a;
}

static GBytes *
raw (const char *s)
{
  return g_bytes_new (s, strlen (s));
}

/* ---------------- offline ---------------- */

static void
test_utf7 (void)
{
  const char *names[] = { "INBOX", "Café Ünïcode", "Tom & Jerry", "日本語", "Ünï & Cödé/Sub", NULL };
  for (int i = 0; names[i]; i++) {
    g_autofree char *enc = smail_imap_utf7_encode (names[i]);
    g_autofree char *dec = smail_imap_utf7_decode (enc);
    g_assert_cmpstr (dec, ==, names[i]);
  }
  g_autofree char *dovecot = smail_imap_utf7_decode ("Caf&AOk- &ANw-n&AO8-code");
  g_assert_cmpstr (dovecot, ==, "Café Ünïcode");
  g_autofree char *amp = smail_imap_utf7_encode ("A&B");
  g_assert_cmpstr (amp, ==, "A&-B");
}

static void
test_parse (void)
{
  g_autoptr (GBytes) bytes = raw (
    "From: =?utf-8?q?Ren=C3=A9e?= <renee@example.org>\r\nTo: a@x.org, b@y.org\r\nCc: c@z.org\r\n"
    "Subject: =?utf-8?b?SGVsbG8g8J+Mjg==?=\r\nDate: Tue, 03 Nov 2026 10:00:00 +0000\r\n"
    "Message-ID: <abc@example.org>\r\nMIME-Version: 1.0\r\n"
    "Content-Type: multipart/mixed; boundary=B\r\n\r\n"
    "--B\r\nContent-Type: multipart/related; boundary=R\r\n\r\n"
    "--R\r\nContent-Type: text/html; charset=iso-8859-1\r\nContent-Transfer-Encoding: quoted-printable\r\n\r\n"
    "<p>Caf=E9 <img src=3D\"cid:logo@x\"></p>\r\n"
    "--R\r\nContent-Type: image/png\r\nContent-ID: <logo@x>\r\nContent-Transfer-Encoding: base64\r\n\r\niVBORw0K\r\n"
    "--R--\r\n"
    "--B\r\nContent-Type: text/plain; name=notes.txt\r\nContent-Disposition: attachment; filename=notes.txt\r\n\r\n"
    "attached text\r\n--B--\r\n");
  g_autoptr (GError) error = NULL;
  g_autoptr (SMailMessageDetail) d = smail_message_detail_parse ("1", bytes, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (d->subject, ==, "Hello 🌎");
  g_assert_cmpstr (d->from_name, ==, "Renée");
  g_assert_cmpstr (d->from_addr, ==, "renee@example.org");
  g_assert_cmpstr (d->message_id, ==, "abc@example.org");
  g_assert_nonnull (strstr (d->html, "Café"));
  g_assert_null (d->text);
  g_assert_cmpuint (d->attachments->len, ==, 1);
  g_assert_cmpstr (((SMailAttachment *) g_ptr_array_index (d->attachments, 0))->filename, ==, "notes.txt");
  g_assert_cmpuint (d->inline_parts->len, ==, 1);
  g_assert_cmpstr (((SMailAttachment *) g_ptr_array_index (d->inline_parts, 0))->content_id, ==, "logo@x");
  g_assert_nonnull (strstr (d->to, "b@y.org"));
  g_assert_cmpstr (d->cc, ==, "c@z.org");
}

static void
test_text_helpers (void)
{
  g_autofree char *t = smail_html_to_text ("<html><head><style>p{}</style></head><body>"
                                             "<p>Hello&nbsp;<b>world</b> &amp; you</p><ul><li>one</li></ul></body></html>");
  g_assert_cmpstr (t, ==, "Hello world & you\n• one");
  g_autofree char *q = smail_quote_text ("a\n> b\n");
  g_assert_cmpstr (q, ==, "> a\n>> b\n>\n");
  g_autofree char *h = smail_text_to_html ("see https://example.org/x?a=1&b=2.\n> quoted <tag>");
  g_assert_nonnull (strstr (h, "<a href=\"https://example.org/x?a=1&amp;b=2\">"));
  g_assert_nonnull (strstr (h, "class=\"quote q1\">&gt; quoted &lt;tag&gt;"));
}

/* ---------------- online ---------------- */

static SMailMessage *
find_subject (GPtrArray *msgs, const char *subject)
{
  for (guint i = 0; i < msgs->len; i++) {
    SMailMessage *m = g_ptr_array_index (msgs, i);
    if (g_str_equal (m->subject, subject))
      return m;
  }
  return NULL;
}

static guint
count_in (SMailImap *imap, const char *folder)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) msgs = smail_imap_list_messages (imap, folder, 100, &error);
  g_assert_no_error (error);
  return msgs->len;
}

static void
test_imap (void)
{
  g_autoptr (SMailAccount) account = test_account ();
  if (!account) {
    g_test_skip ("SMAIL_TEST_IMAP not set");
    return;
  }
  g_autoptr (GError) error = NULL;

  /* wrong password is reported as a server NO */
  {
    g_autoptr (SMailAccount) bad = smail_account_copy (account);
    g_free (bad->password);
    bad->password = g_strdup ("wrong");
    SMailImap *nope = smail_imap_connect (bad, NULL, &error);
    g_assert_null (nope);
    g_assert_error (error, SMAIL_IMAP_ERROR, SMAIL_IMAP_ERROR_NO);
    g_clear_error (&error);
  }

  SMailImap *imap = smail_imap_connect (account, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (imap);
  g_assert_true (smail_imap_has_capability (imap, "IMAP4REV1"));

  /* clean slate */
  const char *folders_to_clear[] = { "INBOX", "Archive", "Trash", "Sent", "Café Ünïcode", NULL };
  for (int i = 0; folders_to_clear[i]; i++) {
    g_autoptr (GPtrArray) msgs = smail_imap_list_messages (imap, folders_to_clear[i], 1000, &error);
    g_assert_no_error (error);
    for (guint j = 0; j < msgs->len; j++) {
      smail_imap_delete (imap, folders_to_clear[i], ((SMailMessage *) g_ptr_array_index (msgs, j))->uid, &error);
      g_assert_no_error (error);
    }
  }

  /* APPEND three messages (literal upload) */
  g_autoptr (GBytes) m1 = raw ("From: Alice <alice@example.org>\r\nTo: test@local\r\nSubject: First\r\n"
                               "Date: Mon, 02 Nov 2026 09:00:00 +0000\r\n\r\nHello one\r\n");
  g_autoptr (GBytes) m2 = raw ("From: Bob <bob@example.org>\r\nTo: test@local\r\nSubject: =?utf-8?q?Gr=C3=BC=C3=9Fe?=\r\n"
                               "Date: Tue, 03 Nov 2026 09:00:00 +0000\r\nMIME-Version: 1.0\r\n"
                               "Content-Type: multipart/mixed; boundary=X\r\n\r\n--X\r\nContent-Type: text/plain\r\n\r\nbody\r\n"
                               "--X\r\nContent-Type: application/octet-stream\r\n"
                               "Content-Disposition: attachment; filename=data.bin\r\n"
                               "Content-Transfer-Encoding: base64\r\n\r\nAAECAw==\r\n--X--\r\n");
  g_autoptr (GBytes) m3 = raw ("From: carol@example.org\r\nTo: test@local\r\nSubject: Third\r\n"
                               "Date: Sun, 01 Nov 2026 09:00:00 +0000\r\n\r\nHello three\r\n");
  g_assert_true (smail_imap_append (imap, "INBOX", m1, NULL, &error));
  g_assert_no_error (error);
  g_assert_true (smail_imap_append (imap, "INBOX", m2, NULL, &error));
  g_assert_true (smail_imap_append (imap, "INBOX", m3, "\\Seen", &error));
  g_assert_no_error (error);

  /* LIST + STATUS + special-use roles + UTF-7 names */
  g_autoptr (GPtrArray) folders = smail_imap_list_folders (imap, &error);
  g_assert_no_error (error);
  gboolean saw_unicode = FALSE;
  for (guint i = 0; i < folders->len; i++) {
    SMailFolder *f = g_ptr_array_index (folders, i);
    if (g_str_equal (f->path, "INBOX")) {
      g_assert_cmpstr (f->role, ==, "inbox");
      g_assert_cmpint (f->unread, ==, 2);
    }
    if (g_str_equal (f->path, "Sent"))    g_assert_cmpstr (f->role, ==, "sent");
    if (g_str_equal (f->path, "Trash"))   g_assert_cmpstr (f->role, ==, "trash");
    if (g_str_equal (f->path, "Archive")) g_assert_cmpstr (f->role, ==, "archive");
    if (g_str_equal (f->path, "Café Ünïcode"))
      saw_unicode = TRUE;
  }
  g_assert_true (saw_unicode);
  g_assert_cmpstr (((SMailFolder *) g_ptr_array_index (folders, 0))->role, ==, "inbox");

  /* FETCH headers + flags */
  g_autoptr (GPtrArray) msgs = smail_imap_list_messages (imap, "INBOX", 100, &error);
  g_assert_no_error (error);
  g_assert_cmpuint (msgs->len, ==, 3);
  SMailMessage *first = find_subject (msgs, "First");
  SMailMessage *grusse = find_subject (msgs, "Grüße");
  SMailMessage *third = find_subject (msgs, "Third");
  g_assert_nonnull (first);
  g_assert_nonnull (grusse);
  g_assert_nonnull (third);
  g_assert_cmpstr (first->sender_name, ==, "Alice");
  g_assert_false (first->seen);
  g_assert_true (third->seen);
  g_assert_true (grusse->has_attachment);

  /* limit keeps the newest UIDs */
  g_autoptr (GPtrArray) limited = smail_imap_list_messages (imap, "INBOX", 2, &error);
  g_assert_cmpuint (limited->len, ==, 2);
  g_assert_null (find_subject (limited, "First"));

  /* full FETCH marks as read and parses */
  g_autoptr (GBytes) body = smail_imap_fetch_message (imap, "INBOX", grusse->uid, &error);
  g_assert_no_error (error);
  g_autoptr (SMailMessageDetail) d = smail_message_detail_parse (grusse->uid, body, &error);
  g_assert_cmpstr (d->text, ==, "body");
  g_assert_cmpuint (d->attachments->len, ==, 1);
  g_assert_cmpuint (g_bytes_get_size (((SMailAttachment *) g_ptr_array_index (d->attachments, 0))->data), ==, 4);

  /* STORE */
  g_assert_true (smail_imap_store_flag (imap, "INBOX", first->uid, "\\Flagged", TRUE, &error));
  g_assert_no_error (error);
  {
    g_autoptr (GPtrArray) again = smail_imap_list_messages (imap, "INBOX", 100, &error);
    g_assert_true (find_subject (again, "First")->flagged);
    g_assert_true (find_subject (again, "Grüße")->seen);
  }

  /* MOVE + EXPUNGE */
  g_assert_true (smail_imap_move (imap, "INBOX", first->uid, "Archive", &error));
  g_assert_no_error (error);
  g_assert_cmpuint (count_in (imap, "INBOX"), ==, 2);
  g_assert_cmpuint (count_in (imap, "Archive"), ==, 1);
  g_assert_true (smail_imap_delete (imap, "INBOX", third->uid, &error));
  g_assert_cmpuint (count_in (imap, "INBOX"), ==, 1);

  /* unicode mailbox round trip */
  g_assert_true (smail_imap_append (imap, "Café Ünïcode", m1, NULL, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (count_in (imap, "Café Ünïcode"), ==, 1);

  /* server NO on a missing folder */
  g_autoptr (GPtrArray) none = smail_imap_list_messages (imap, "Does Not Exist", 10, &error);
  g_assert_null (none);
  g_assert_error (error, SMAIL_IMAP_ERROR, SMAIL_IMAP_ERROR_NO);
  g_clear_error (&error);
  g_assert_true (smail_imap_is_connected (imap));

  smail_imap_logout (imap);
  smail_imap_free (imap);
}

static void
test_smtp (void)
{
  g_autoptr (SMailAccount) account = test_account ();
  if (!account || !account->smtp_host) {
    g_test_skip ("SMAIL_TEST_SMTP not set");
    return;
  }
  g_autoptr (GBytes) msg = raw ("From: test@local\r\nTo: friend@example.org\r\nSubject: hi\r\n\r\nhello\r\n");
  const char *rcpts[] = { "friend@example.org", "other@example.org", NULL };
  g_autoptr (GError) error = NULL;
  g_assert_true (smail_smtp_send (account, msg, "test@local", rcpts, &error));
  g_assert_no_error (error);
}

int
main (int argc, char *argv[])
{
  g_mime_init ();
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/utf7/roundtrip", test_utf7);
  g_test_add_func ("/mime/parse", test_parse);
  g_test_add_func ("/util/text", test_text_helpers);
  g_test_add_func ("/imap/server", test_imap);
  g_test_add_func ("/smtp/send", test_smtp);
  int ret = g_test_run ();
  g_mime_shutdown ();
  return ret;
}
