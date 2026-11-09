/* smail-demo.c — in-memory mailbox so the app can be explored offline */
#include "smail-backend.h"

#include <string.h>

void _smail_backend_set_account (SMailBackend *self, const SMailAccount *account);

#define SMAIL_TYPE_DEMO_BACKEND (smail_demo_backend_get_type ())
G_DECLARE_FINAL_TYPE (SMailDemoBackend, smail_demo_backend, SMAIL, DEMO_BACKEND, SMailBackend)

typedef struct {
  guint    uid;
  GBytes  *raw;
  gboolean seen;
  gboolean flagged;
} DemoMessage;

struct _SMailDemoBackend {
  SMailBackend parent_instance;
  GHashTable    *folders; /* path -> GPtrArray<DemoMessage*> */
  GPtrArray     *folder_list; /* SMailFolder*: the folders that exist */
  guint          next_uid;
};

G_DEFINE_FINAL_TYPE (SMailDemoBackend, smail_demo_backend, SMAIL_TYPE_BACKEND)

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
folder_messages (SMailDemoBackend *self, const char *path)
{
  GPtrArray *arr = g_hash_table_lookup (self->folders, path);
  if (!arr) {
    arr = g_ptr_array_new_with_free_func ((GDestroyNotify) demo_message_free);
    g_hash_table_insert (self->folders, g_strdup (path), arr);
  }
  return arr;
}

static void
add_raw (SMailDemoBackend *self, const char *folder, char *raw, gboolean seen, gboolean flagged)
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
add_plain (SMailDemoBackend *self, const char *folder, int minutes_ago, const char *from,
           const char *to, const char *subject, const char *body, gboolean seen, gboolean flagged)
{
  g_autofree char *date = rfc2822_date (minutes_ago);
  char *raw = g_strdup_printf ("From: %s\r\nTo: %s\r\nSubject: %s\r\nDate: %s\r\n"
                               "Message-ID: <demo-%u@smail.example>\r\n"
                               "MIME-Version: 1.0\r\nContent-Type: text/plain; charset=utf-8\r\n"
                               "Content-Transfer-Encoding: 8bit\r\n\r\n%s\r\n",
                               from, to, subject, date, self->next_uid, body);
  add_raw (self, folder, raw, seen, flagged);
}

static void
populate (SMailDemoBackend *self)
{
  const char *me = "You <you@smail.example>";

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
             "[smail] New issue: Support for OAuth2 login",
             "A new issue was opened by @contributor:\r\n\r\n"
             "  It would be great if SMail supported OAuth2 so we don't need app passwords.\r\n\r\n"
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
             "SMail 1.0 is here!\r\n\r\n- Three pane layout\r\n- IMAP & SMTP\r\n", TRUE, FALSE);
  add_plain (self, "Archive", 60 * 24 * 100, "Bank <statements@bank.example>", me,
             "Your statement is ready", "Your monthly statement is available online.", TRUE, FALSE);
  add_plain (self, "Junk", 60 * 7, "Prize Dept <win@totally-legit.example>", me,
             "YOU HAVE WON!!!", "Click here to claim your prize. (Don't.)", FALSE, FALSE);
  add_plain (self, "Receipts", 60 * 24 * 12, "Store <orders@store.example>", me,
             "Order #4521 shipped", "Your order has shipped and should arrive Friday.", TRUE, FALSE);
}

static SMailFolder *
demo_find_folder (SMailDemoBackend *self, const char *path, guint *index)
{
  for (guint i = 0; i < self->folder_list->len; i++) {
    SMailFolder *f = g_ptr_array_index (self->folder_list, i);
    if (g_str_equal (f->path, path)) {
      if (index)
        *index = i;
      return f;
    }
  }
  return NULL;
}

static GPtrArray *
demo_list_folders (SMailBackend *backend, GError **error)
{
  SMailDemoBackend *self = SMAIL_DEMO_BACKEND (backend);
  GPtrArray *folders = g_ptr_array_new_with_free_func ((GDestroyNotify) smail_folder_free);
  for (guint i = 0; i < self->folder_list->len; i++) {
    SMailFolder *f = smail_folder_copy (g_ptr_array_index (self->folder_list, i));
    f->unread = 0;
    GPtrArray *msgs = folder_messages (self, f->path);
    for (guint j = 0; j < msgs->len; j++)
      if (!((DemoMessage *) g_ptr_array_index (msgs, j))->seen)
        f->unread++;
    g_ptr_array_add (folders, f);
  }
  g_ptr_array_sort (folders, smail_folder_compare);
  g_usleep (150 * 1000); /* feel like a network */
  return folders;
}

static DemoMessage *
find (SMailDemoBackend *self, const char *folder, const char *uid, guint *index, GError **error)
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
demo_list_messages (SMailBackend *backend, const char *folder, guint limit, GError **error)
{
  SMailDemoBackend *self = SMAIL_DEMO_BACKEND (backend);
  GPtrArray *msgs = folder_messages (self, folder);
  GPtrArray *out = g_ptr_array_new_with_free_func (g_object_unref);
  for (gint i = msgs->len - 1; i >= 0 && out->len < limit; i--) {
    DemoMessage *m = g_ptr_array_index (msgs, i);
    g_autofree char *uid = g_strdup_printf ("%u", m->uid);
    g_ptr_array_add (out, smail_message_new_from_headers (uid, m->raw, m->seen, m->flagged));
  }
  g_usleep (200 * 1000);
  return out;
}

static GBytes *
demo_fetch_message (SMailBackend *backend, const char *folder, const char *uid, GError **error)
{
  DemoMessage *m = find (SMAIL_DEMO_BACKEND (backend), folder, uid, NULL, error);
  if (!m)
    return NULL;
  m->seen = TRUE;
  g_usleep (100 * 1000);
  return g_bytes_ref (m->raw);
}

static gboolean
demo_set_flag (SMailBackend *backend, const char *folder, const char *uid,
               const char *flag, gboolean add, GError **error)
{
  DemoMessage *m = find (SMAIL_DEMO_BACKEND (backend), folder, uid, NULL, error);
  if (!m)
    return FALSE;
  if (g_str_equal (flag, "\\Seen"))
    m->seen = add;
  else if (g_str_equal (flag, "\\Flagged"))
    m->flagged = add;
  return TRUE;
}

static gboolean
demo_move (SMailBackend *backend, const char *folder, const char *uid, const char *dest, GError **error)
{
  SMailDemoBackend *self = SMAIL_DEMO_BACKEND (backend);
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
demo_remove (SMailBackend *backend, const char *folder, const char *uid, GError **error)
{
  SMailDemoBackend *self = SMAIL_DEMO_BACKEND (backend);
  guint idx;
  if (!find (self, folder, uid, &idx, error))
    return FALSE;
  g_ptr_array_remove_index (folder_messages (self, folder), idx);
  return TRUE;
}

static gboolean
demo_append (SMailBackend *backend, const char *folder, GBytes *message, const char *flags, GError **error)
{
  SMailDemoBackend *self = SMAIL_DEMO_BACKEND (backend);
  if (!demo_find_folder (self, folder, NULL)) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Folder “%s” doesn't exist", folder);
    return FALSE;
  }
  gsize len;
  const char *data = g_bytes_get_data (message, &len);
  add_raw (self, folder, g_strndup (data, len), flags && strstr (flags, "\\Seen"),
           flags && strstr (flags, "\\Flagged"));
  return TRUE;
}

static GPtrArray *
demo_search (SMailBackend *backend, const char *folder, const char *query, guint limit, GError **error)
{
  SMailDemoBackend *self = SMAIL_DEMO_BACKEND (backend);
  GPtrArray *msgs = folder_messages (self, folder);
  GPtrArray *out = g_ptr_array_new_with_free_func (g_object_unref);
  g_autofree char *q = g_utf8_casefold (query, -1);
  for (gint i = msgs->len - 1; i >= 0 && out->len < limit; i--) {
    DemoMessage *m = g_ptr_array_index (msgs, i);
    g_autoptr (SMailMessageDetail) d = smail_message_detail_parse ("", m->raw, NULL);
    g_autofree char *hay = g_strjoin ("\n", d ? d->subject : "", d && d->from_name ? d->from_name : "",
                                      d && d->from_addr ? d->from_addr : "", d && d->to ? d->to : "",
                                      d && d->text ? d->text : "", d && d->html ? d->html : "", NULL);
    g_autofree char *h = g_utf8_casefold (hay, -1);
    if (strstr (h, q)) {
      g_autofree char *uid = g_strdup_printf ("%u", m->uid);
      g_ptr_array_add (out, smail_message_new_from_headers (uid, m->raw, m->seen, m->flagged));
    }
  }
  return out;
}

static int
demo_delete_by_message_id (SMailBackend *backend, const char *folder, const char *message_id, GError **error)
{
  SMailDemoBackend *self = SMAIL_DEMO_BACKEND (backend);
  GPtrArray *msgs = folder_messages (self, folder);
  g_autofree char *want = g_strdup (message_id[0] == '<' ? message_id + 1 : message_id);
  char *gt = strchr (want, '>');
  if (gt)
    *gt = '\0';
  int n = 0;
  for (gint i = msgs->len - 1; i >= 0; i--) {
    DemoMessage *m = g_ptr_array_index (msgs, i);
    g_autoptr (SMailMessageDetail) d = smail_message_detail_parse ("", m->raw, NULL);
    if (d && g_strcmp0 (d->message_id, want) == 0) {
      g_ptr_array_remove_index (msgs, i);
      n++;
    }
  }
  return n;
}

static gboolean
demo_create_folder (SMailBackend *backend, const char *name, GError **error)
{
  SMailDemoBackend *self = SMAIL_DEMO_BACKEND (backend);
  if (demo_find_folder (self, name, NULL)) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_EXISTS, "A folder named “%s” already exists", name);
    return FALSE;
  }
  const char *slash = strrchr (name, '/');
  SMailFolder *f = smail_folder_new (name, slash ? slash + 1 : name, NULL);
  f->delimiter = g_strdup ("/");
  g_ptr_array_add (self->folder_list, f);
  folder_messages (self, name);
  return TRUE;
}

static gboolean
demo_rename_folder (SMailBackend *backend, const char *from, const char *to, GError **error)
{
  SMailDemoBackend *self = SMAIL_DEMO_BACKEND (backend);
  SMailFolder *f = demo_find_folder (self, from, NULL);
  if (!f || f->role) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "This folder can't be renamed");
    return FALSE;
  }
  if (demo_find_folder (self, to, NULL)) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_EXISTS, "A folder named “%s” already exists", to);
    return FALSE;
  }
  GPtrArray *msgs = g_ptr_array_ref (folder_messages (self, from));
  g_hash_table_remove (self->folders, from);
  g_hash_table_insert (self->folders, g_strdup (to), msgs);
  g_free (f->path);
  f->path = g_strdup (to);
  const char *slash = strrchr (to, '/');
  g_free (f->display_name);
  f->display_name = g_strdup (slash ? slash + 1 : to);
  return TRUE;
}

static gboolean
demo_delete_folder (SMailBackend *backend, const char *name, GError **error)
{
  SMailDemoBackend *self = SMAIL_DEMO_BACKEND (backend);
  guint idx;
  SMailFolder *f = demo_find_folder (self, name, &idx);
  if (!f || f->role) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "This folder can't be deleted");
    return FALSE;
  }
  g_hash_table_remove (self->folders, name);
  g_ptr_array_remove_index (self->folder_list, idx);
  return TRUE;
}

static gboolean
demo_send (SMailBackend *backend, GBytes *message, const char *from,
           const char * const *recipients, GError **error)
{
  g_usleep (600 * 1000);
  return TRUE; /* the base class files it in Sent */
}

static void
smail_demo_backend_finalize (GObject *object)
{
  g_hash_table_unref (SMAIL_DEMO_BACKEND (object)->folders);
  g_ptr_array_unref (SMAIL_DEMO_BACKEND (object)->folder_list);
  G_OBJECT_CLASS (smail_demo_backend_parent_class)->finalize (object);
}

static void
smail_demo_backend_class_init (SMailDemoBackendClass *klass)
{
  SMailBackendClass *bc = SMAIL_BACKEND_CLASS (klass);
  G_OBJECT_CLASS (klass)->finalize = smail_demo_backend_finalize;
  bc->list_folders = demo_list_folders;
  bc->list_messages = demo_list_messages;
  bc->fetch_message = demo_fetch_message;
  bc->set_flag = demo_set_flag;
  bc->move = demo_move;
  bc->remove = demo_remove;
  bc->append = demo_append;
  bc->search = demo_search;
  bc->create_folder = demo_create_folder;
  bc->rename_folder = demo_rename_folder;
  bc->delete_folder = demo_delete_folder;
  bc->delete_by_message_id = demo_delete_by_message_id;
  bc->send = demo_send;
}

static void
smail_demo_backend_init (SMailDemoBackend *self)
{
  self->folder_list = g_ptr_array_new_with_free_func ((GDestroyNotify) smail_folder_free);
  for (gsize i = 0; i < G_N_ELEMENTS (demo_folders); i++) {
    const char *path = demo_folders[i].path;
    SMailFolder *f = smail_folder_new (path, g_str_equal (path, "INBOX") ? "Inbox" : path, demo_folders[i].role);
    f->delimiter = g_strdup ("/");
    g_ptr_array_add (self->folder_list, f);
  }
  self->folders = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) g_ptr_array_unref);
  self->next_uid = 1;
  populate (self);
}

SMailBackend *
smail_backend_new_demo (void)
{
  SMailBackend *self = g_object_new (SMAIL_TYPE_DEMO_BACKEND, NULL);
  g_autoptr (SMailAccount) account = smail_account_new ();
  account->id = g_strdup ("demo");
  account->name = g_strdup ("You");
  account->email = g_strdup ("you@smail.example");
  _smail_backend_set_account (self, account);
  return self;
}
