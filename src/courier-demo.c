/* courier-demo.c — in-memory mailbox so the app can be explored offline */
#include "courier-backend.h"

#include <string.h>

void _courier_backend_set_account (CourierBackend *self, const CourierAccount *account);

#define COURIER_TYPE_DEMO_BACKEND (courier_demo_backend_get_type ())
G_DECLARE_FINAL_TYPE (CourierDemoBackend, courier_demo_backend, COURIER, DEMO_BACKEND, CourierBackend)

typedef struct {
  guint    uid;
  GBytes  *raw;
  gboolean seen;
  gboolean flagged;
} DemoMessage;

struct _CourierDemoBackend {
  CourierBackend parent_instance;
  GHashTable    *folders; /* path -> GPtrArray<DemoMessage*> */
  guint          next_uid;
};

G_DEFINE_FINAL_TYPE (CourierDemoBackend, courier_demo_backend, COURIER_TYPE_BACKEND)

static void
demo_message_free (DemoMessage *m)
{
  g_bytes_unref (m->raw);
  g_free (m);
}

static const struct { const char *path, *role; } demo_folders[] = {
  { "INBOX", "inbox" }, { "Drafts", "drafts" }, { "Sent", "sent" },
  { "Archive", "archive" }, { "Junk", "junk" }, { "Trash", "trash" },
  { "Projects", NULL }, { "Receipts", NULL },
};

static GPtrArray *
folder_messages (CourierDemoBackend *self, const char *path)
{
  GPtrArray *arr = g_hash_table_lookup (self->folders, path);
  if (!arr) {
    arr = g_ptr_array_new_with_free_func ((GDestroyNotify) demo_message_free);
    g_hash_table_insert (self->folders, g_strdup (path), arr);
  }
  return arr;
}

static void
add_raw (CourierDemoBackend *self, const char *folder, char *raw, gboolean seen, gboolean flagged)
{
  DemoMessage *m = g_new0 (DemoMessage, 1);
  m->uid = self->next_uid++;
  m->raw = g_bytes_new_take (raw, strlen (raw));
  m->seen = seen;
  m->flagged = flagged;
  g_ptr_array_add (folder_messages (self, folder), m);
}

static char *
rfc2822_date (int minutes_ago)
{
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  g_autoptr (GDateTime) t = g_date_time_add_minutes (now, -minutes_ago);
  return g_date_time_format (t, "%a, %d %b %Y %H:%M:%S %z");
}

static void
add_plain (CourierDemoBackend *self, const char *folder, int minutes_ago, const char *from,
           const char *to, const char *subject, const char *body, gboolean seen, gboolean flagged)
{
  g_autofree char *date = rfc2822_date (minutes_ago);
  char *raw = g_strdup_printf ("From: %s\r\nTo: %s\r\nSubject: %s\r\nDate: %s\r\n"
                               "Message-ID: <demo-%u@courier.example>\r\n"
                               "MIME-Version: 1.0\r\nContent-Type: text/plain; charset=utf-8\r\n"
                               "Content-Transfer-Encoding: 8bit\r\n\r\n%s\r\n",
                               from, to, subject, date, self->next_uid, body);
  add_raw (self, folder, raw, seen, flagged);
}

static void
populate (CourierDemoBackend *self)
{
  const char *me = "You <you@courier.example>";

  add_plain (self, "INBOX", 12, "Daniel Foré <daniel@example.org>", me,
             "Design review for the new sidebar",
             "Hey!\r\n\r\nI pushed the mockups for the sidebar refresh. The main ideas:\r\n\r\n"
             "  * folders get symbolic icons and unread badges\r\n"
             "  * the message list keeps sender, date and subject on two lines\r\n"
             "  * the reading pane gets a big readable header\r\n\r\n"
             "Take a look when you get a chance: https://example.org/mockups/sidebar\r\n\r\n"
             "Cheers,\r\nDaniel", FALSE, TRUE);

  {
    g_autofree char *date = rfc2822_date (95);
    char *raw = g_strdup_printf (
      "From: Weekly Digest <digest@news.example.com>\r\nTo: %s\r\nSubject: Your weekly digest: 5 stories\r\n"
      "Date: %s\r\nMessage-ID: <digest-1@news.example.com>\r\nMIME-Version: 1.0\r\n"
      "Content-Type: multipart/alternative; boundary=\"ALT\"\r\n\r\n"
      "--ALT\r\nContent-Type: text/plain; charset=utf-8\r\n\r\nThis week's top stories...\r\n"
      "--ALT\r\nContent-Type: text/html; charset=utf-8\r\n\r\n"
      "<html><body style=\"font-family:sans-serif;margin:0;background:#f4f4f7\">"
      "<div style=\"max-width:560px;margin:24px auto;background:#fff;border-radius:12px;padding:24px\">"
      "<img src=\"https://tracker.example.com/pixel.gif\" width=\"1\" height=\"1\">"
      "<h1 style=\"color:#3584e4;margin-top:0\">Weekly Digest</h1>"
      "<p>Here is what happened this week in the world of open source desktops.</p>"
      "<h3>1. GTK 4 keeps getting faster</h3><p>The new renderers bring smoother scrolling.</p>"
      "<h3>2. Libadwaita adaptive layouts</h3><p>Split views and breakpoints make apps fit any screen.</p>"
      "<h3>3. Writing apps in C in 2026</h3><p>Still a great choice for lean, fast native apps.</p>"
      "<p><a href=\"https://news.example.com/digest\" style=\"background:#3584e4;color:#fff;padding:10px 16px;"
      "border-radius:8px;text-decoration:none\">Read more</a></p>"
      "</div></body></html>\r\n--ALT--\r\n", me, date);
    add_raw (self, "INBOX", raw, FALSE, FALSE);
  }

  {
    g_autofree char *date = rfc2822_date (60 * 5);
    char *raw = g_strdup_printf (
      "From: Cassidy James <cassidy@example.org>\r\nTo: %s\r\nCc: Team <team@example.org>\r\n"
      "Subject: Quarterly report (PDF attached)\r\nDate: %s\r\nMessage-ID: <report@example.org>\r\n"
      "MIME-Version: 1.0\r\nContent-Type: multipart/mixed; boundary=\"MIX\"\r\n\r\n"
      "--MIX\r\nContent-Type: text/plain; charset=utf-8\r\n\r\n"
      "Hi all,\r\n\r\nAttached is the quarterly report. Highlights are on page 2.\r\n\r\n"
      "> Can you also include the download numbers?\r\n\r\nDone, see the appendix.\r\n\r\nCassidy\r\n"
      "--MIX\r\nContent-Type: application/pdf; name=\"report-q3.pdf\"\r\n"
      "Content-Disposition: attachment; filename=\"report-q3.pdf\"\r\n"
      "Content-Transfer-Encoding: base64\r\n\r\n"
      "JVBERi0xLjQKJcOkw7zDtsOfCjEgMCBvYmoKPDwvVHlwZS9DYXRhbG9nPj4KZW5kb2JqCnRyYWlsZXIKPDwvUm9vdCAxIDAgUj4+CiUlRU9GCg==\r\n"
      "--MIX\r\nContent-Type: text/csv; name=\"downloads.csv\"\r\n"
      "Content-Disposition: attachment; filename=\"downloads.csv\"\r\n\r\n"
      "month,downloads\r\njuly,1200\r\naugust,1530\r\nseptember,1810\r\n"
      "--MIX--\r\n", me, date);
    add_raw (self, "INBOX", raw, FALSE, FALSE);
  }

  add_plain (self, "INBOX", 60 * 26, "Mom <mom@family.example>", me, "Dinner on Sunday?",
             "Are you coming for dinner on Sunday? I'm making your favourite.\r\n\r\nLove, Mom",
             TRUE, FALSE);
  add_plain (self, "INBOX", 60 * 24 * 3, "GitHub <noreply@github.example>", me,
             "[courier] New issue: Support for OAuth2 login",
             "A new issue was opened by @contributor:\r\n\r\n"
             "  It would be great if Courier supported OAuth2 so we don't need app passwords.\r\n\r\n"
             "--\r\nReply to this email directly or view it on the web.", TRUE, FALSE);
  add_plain (self, "INBOX", 60 * 24 * 9, "Airline <booking@airline.example>", me,
             "Your flight confirmation ABC123",
             "Thank you for booking with us.\r\n\r\nFlight: XY 123\r\nDeparture: 08:45\r\nSeat: 14A\r\n",
             TRUE, TRUE);
  add_plain (self, "INBOX", 60 * 24 * 40, "Alex Example <alex@example.net>", me, "Re: Old photos",
             "Found the photos from the trip! Will share the album soon.\r\n\r\n"
             "On Mon, Alex wrote:\r\n> Do you still have those photos?\r\n", TRUE, FALSE);

  add_plain (self, "Sent", 60 * 30, me, "Daniel Foré <daniel@example.org>", "Re: Design review",
             "Looks great, I'll review it this afternoon.", TRUE, FALSE);
  add_plain (self, "Drafts", 60 * 2, me, "team@example.org", "Release notes (draft)",
             "Courier 1.0 is here!\r\n\r\n- Three pane layout\r\n- IMAP & SMTP\r\n", TRUE, FALSE);
  add_plain (self, "Archive", 60 * 24 * 100, "Bank <statements@bank.example>", me,
             "Your statement is ready", "Your monthly statement is available online.", TRUE, FALSE);
  add_plain (self, "Junk", 60 * 7, "Prize Dept <win@totally-legit.example>", me,
             "YOU HAVE WON!!!", "Click here to claim your prize. (Don't.)", FALSE, FALSE);
  add_plain (self, "Receipts", 60 * 24 * 12, "Store <orders@store.example>", me,
             "Order #4521 shipped", "Your order has shipped and should arrive Friday.", TRUE, FALSE);
}

static GPtrArray *
demo_list_folders (CourierBackend *backend, GError **error)
{
  CourierDemoBackend *self = COURIER_DEMO_BACKEND (backend);
  GPtrArray *folders = g_ptr_array_new_with_free_func ((GDestroyNotify) courier_folder_free);
  for (gsize i = 0; i < G_N_ELEMENTS (demo_folders); i++) {
    const char *display = demo_folders[i].path;
    if (g_str_equal (display, "INBOX"))
      display = "Inbox";
    CourierFolder *f = courier_folder_new (demo_folders[i].path, display, demo_folders[i].role);
    GPtrArray *msgs = folder_messages (self, f->path);
    for (guint j = 0; j < msgs->len; j++)
      if (!((DemoMessage *) g_ptr_array_index (msgs, j))->seen)
        f->unread++;
    g_ptr_array_add (folders, f);
  }
  g_ptr_array_sort (folders, courier_folder_compare);
  g_usleep (150 * 1000); /* feel like a network */
  return folders;
}

static DemoMessage *
find (CourierDemoBackend *self, const char *folder, const char *uid, guint *index, GError **error)
{
  GPtrArray *msgs = folder_messages (self, folder);
  guint u = (guint) g_ascii_strtoull (uid, NULL, 10);
  for (guint i = 0; i < msgs->len; i++) {
    DemoMessage *m = g_ptr_array_index (msgs, i);
    if (m->uid == u) {
      if (index)
        *index = i;
      return m;
    }
  }
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Message not found");
  return NULL;
}

static GPtrArray *
demo_list_messages (CourierBackend *backend, const char *folder, guint limit, GError **error)
{
  CourierDemoBackend *self = COURIER_DEMO_BACKEND (backend);
  GPtrArray *msgs = folder_messages (self, folder);
  GPtrArray *out = g_ptr_array_new_with_free_func (g_object_unref);
  for (gint i = msgs->len - 1; i >= 0 && out->len < limit; i--) {
    DemoMessage *m = g_ptr_array_index (msgs, i);
    g_autofree char *uid = g_strdup_printf ("%u", m->uid);
    g_ptr_array_add (out, courier_message_new_from_headers (uid, m->raw, m->seen, m->flagged));
  }
  g_usleep (200 * 1000);
  return out;
}

static GBytes *
demo_fetch_message (CourierBackend *backend, const char *folder, const char *uid, GError **error)
{
  DemoMessage *m = find (COURIER_DEMO_BACKEND (backend), folder, uid, NULL, error);
  if (!m)
    return NULL;
  m->seen = TRUE;
  g_usleep (100 * 1000);
  return g_bytes_ref (m->raw);
}

static gboolean
demo_set_flag (CourierBackend *backend, const char *folder, const char *uid,
               const char *flag, gboolean add, GError **error)
{
  DemoMessage *m = find (COURIER_DEMO_BACKEND (backend), folder, uid, NULL, error);
  if (!m)
    return FALSE;
  if (g_str_equal (flag, "\\Seen"))
    m->seen = add;
  else if (g_str_equal (flag, "\\Flagged"))
    m->flagged = add;
  return TRUE;
}

static gboolean
demo_move (CourierBackend *backend, const char *folder, const char *uid, const char *dest, GError **error)
{
  CourierDemoBackend *self = COURIER_DEMO_BACKEND (backend);
  guint idx;
  DemoMessage *m = find (self, folder, uid, &idx, error);
  if (!m)
    return FALSE;
  g_ptr_array_steal_index (folder_messages (self, folder), idx);
  m->uid = self->next_uid++;
  g_ptr_array_add (folder_messages (self, dest), m);
  return TRUE;
}

static gboolean
demo_remove (CourierBackend *backend, const char *folder, const char *uid, GError **error)
{
  CourierDemoBackend *self = COURIER_DEMO_BACKEND (backend);
  guint idx;
  if (!find (self, folder, uid, &idx, error))
    return FALSE;
  g_ptr_array_remove_index (folder_messages (self, folder), idx);
  return TRUE;
}

static gboolean
demo_append (CourierBackend *backend, const char *folder, GBytes *message, GError **error)
{
  CourierDemoBackend *self = COURIER_DEMO_BACKEND (backend);
  gsize len;
  const char *data = g_bytes_get_data (message, &len);
  add_raw (self, folder, g_strndup (data, len), TRUE, FALSE);
  return TRUE;
}

static gboolean
demo_send (CourierBackend *backend, GBytes *message, const char *from,
           const char * const *recipients, GError **error)
{
  g_usleep (600 * 1000);
  return TRUE; /* the base class files it in Sent */
}

static void
courier_demo_backend_finalize (GObject *object)
{
  g_hash_table_unref (COURIER_DEMO_BACKEND (object)->folders);
  G_OBJECT_CLASS (courier_demo_backend_parent_class)->finalize (object);
}

static void
courier_demo_backend_class_init (CourierDemoBackendClass *klass)
{
  CourierBackendClass *bc = COURIER_BACKEND_CLASS (klass);
  G_OBJECT_CLASS (klass)->finalize = courier_demo_backend_finalize;
  bc->list_folders = demo_list_folders;
  bc->list_messages = demo_list_messages;
  bc->fetch_message = demo_fetch_message;
  bc->set_flag = demo_set_flag;
  bc->move = demo_move;
  bc->remove = demo_remove;
  bc->append = demo_append;
  bc->send = demo_send;
}

static void
courier_demo_backend_init (CourierDemoBackend *self)
{
  self->folders = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) g_ptr_array_unref);
  self->next_uid = 1;
  populate (self);
}

CourierBackend *
courier_backend_new_demo (void)
{
  CourierBackend *self = g_object_new (COURIER_TYPE_DEMO_BACKEND, NULL);
  g_autoptr (CourierAccount) account = courier_account_new ();
  account->id = g_strdup ("demo");
  account->name = g_strdup ("You");
  account->email = g_strdup ("you@courier.example");
  _courier_backend_set_account (self, account);
  return self;
}
