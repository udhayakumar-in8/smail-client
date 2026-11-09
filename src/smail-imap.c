/* smail-imap.c — minimal synchronous IMAP4rev1 client over GIO + TLS */
#include "smail-imap.h"

#include <string.h>
#include <stdlib.h>

G_DEFINE_QUARK (smail-imap-error-quark, smail_imap_error)

struct _SMailImap {
  GSocketClient    *client;
  GIOStream        *stream;
  GDataInputStream *in;
  GOutputStream    *out;
  guint             tag;
  char             *selected;      /* currently SELECTed mailbox (decoded) */
  char             *capabilities;  /* space separated, upper-case */
  gboolean          broken;        /* an I/O error happened */
};

/* One untagged response: the text with literals left as "{N}" markers,
 * and the literal payloads in order. */
typedef struct {
  GString   *text;
  GPtrArray *literals; /* GBytes* */
} ImapResponse;

static void
imap_response_free (ImapResponse *r)
{
  g_string_free (r->text, TRUE);
  g_ptr_array_unref (r->literals);
  g_free (r);
}

/* ---------------- modified UTF-7 ---------------- */

char *
smail_imap_utf7_decode (const char *s)
{
  GString *out = g_string_new (NULL);
  for (const char *p = s; *p;) {
    if (*p != '&') {
      g_string_append_c (out, *p++);
      continue;
    }
    const char *end = strchr (p, '-');
    if (!end) {
      g_string_append (out, p);
      break;
    }
    if (end == p + 1) {
      g_string_append_c (out, '&');
    } else {
      g_autofree char *b64 = g_strndup (p + 1, end - p - 1);
      g_strdelimit (b64, ",", '/');
      gsize pad = (4 - strlen (b64) % 4) % 4;
      g_autofree char *padded = g_strconcat (b64, pad > 0 ? "===" + (3 - pad) : "", NULL);
      gsize len = 0;
      g_autofree guchar *raw = g_base64_decode (padded, &len);
      gsize n = len / 2;
      g_autofree gunichar2 *u16 = g_new (gunichar2, n + 1);
      for (gsize i = 0; i < n; i++)
        u16[i] = (raw[2 * i] << 8) | raw[2 * i + 1];
      g_autofree char *utf8 = g_utf16_to_utf8 (u16, n, NULL, NULL, NULL);
      g_string_append (out, utf8 ? utf8 : "?");
    }
    p = end + 1;
  }
  return g_string_free (out, FALSE);
}

static void
utf7_flush (GString *out, GString *pending)
{
  if (pending->len == 0)
    return;
  glong n = 0;
  g_autofree gunichar2 *u16 = g_utf8_to_utf16 (pending->str, -1, NULL, &n, NULL);
  g_autofree guchar *be = g_new (guchar, n * 2);
  for (glong i = 0; i < n; i++) {
    be[2 * i] = u16[i] >> 8;
    be[2 * i + 1] = u16[i] & 0xff;
  }
  g_autofree char *b64 = g_base64_encode (be, n * 2);
  char *eq = strchr (b64, '=');
  if (eq)
    *eq = '\0';
  g_strdelimit (b64, "/", ',');
  g_string_append_printf (out, "&%s-", b64);
  g_string_truncate (pending, 0);
}

char *
smail_imap_utf7_encode (const char *s)
{
  GString *out = g_string_new (NULL);
  g_autoptr (GString) pending = g_string_new (NULL);
  for (const char *p = s; *p; p = g_utf8_next_char (p)) {
    gunichar c = g_utf8_get_char (p);
    if (c >= 0x20 && c <= 0x7e) {
      utf7_flush (out, pending);
      if (c == '&')
        g_string_append (out, "&-");
      else
        g_string_append_c (out, (char) c);
    } else {
      g_string_append_unichar (pending, c);
    }
  }
  utf7_flush (out, pending);
  return g_string_free (out, FALSE);
}

/* Quoted IMAP string, mailbox names are UTF-7 encoded first. */
static char *
imap_quote (const char *s)
{
  GString *out = g_string_new ("\"");
  for (const char *p = s; *p; p++) {
    if (*p == '"' || *p == '\\')
      g_string_append_c (out, '\\');
    g_string_append_c (out, *p);
  }
  g_string_append_c (out, '"');
  return g_string_free (out, FALSE);
}

static char *
imap_quote_mailbox (const char *name)
{
  g_autofree char *enc = smail_imap_utf7_encode (name);
  return imap_quote (enc);
}

/* ---------------- low-level I/O ---------------- */

static void
set_streams (SMailImap *imap, GIOStream *stream)
{
  g_clear_object (&imap->in);
  g_set_object (&imap->stream, stream);
  imap->in = g_data_input_stream_new (g_io_stream_get_input_stream (stream));
  g_data_input_stream_set_newline_type (imap->in, G_DATA_STREAM_NEWLINE_TYPE_CR_LF);
  imap->out = g_io_stream_get_output_stream (stream);
}

static gboolean
write_all (SMailImap *imap, const void *data, gsize len, GError **error)
{
  if (!g_output_stream_write_all (imap->out, data, len, NULL, NULL, error)) {
    imap->broken = TRUE;
    return FALSE;
  }
  return TRUE;
}

/* Read one complete server response, following any {N} literals. */
static ImapResponse *
read_response (SMailImap *imap, GError **error)
{
  ImapResponse *r = g_new0 (ImapResponse, 1);
  r->text = g_string_new (NULL);
  r->literals = g_ptr_array_new_with_free_func ((GDestroyNotify) g_bytes_unref);

  for (;;) {
    gsize len = 0;
    g_autofree char *line = g_data_input_stream_read_line (imap->in, &len, NULL, error);
    if (!line) {
      if (error && !*error)
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_CONNECTION_CLOSED, "Server closed the connection");
      imap->broken = TRUE;
      imap_response_free (r);
      return NULL;
    }
    g_string_append_len (r->text, line, len);

    /* literal marker at end of line: {123} or {123+} */
    if (len > 2 && line[len - 1] == '}') {
      char *open = strrchr (line, '{');
      if (open) {
        char *endp = NULL;
        guint64 n = g_ascii_strtoull (open + 1, &endp, 10);
        if (endp && (*endp == '}' || *endp == '+')) {
          guint8 *buf = g_malloc (n + 1);
          gsize got = 0;
          if (!g_input_stream_read_all (G_INPUT_STREAM (imap->in), buf, n, &got, NULL, error) || got != n) {
            g_free (buf);
            if (error && !*error)
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_CONNECTION_CLOSED, "Short literal from server");
            imap->broken = TRUE;
            imap_response_free (r);
            return NULL;
          }
          g_ptr_array_add (r->literals, g_bytes_new_take (buf, n));
          continue; /* the response continues on the next line */
        }
      }
    }
    return r;
  }
}

/* Send a command and collect untagged responses until the tagged reply.
 * If `literal` is given, the command must end with "{N}" and the payload
 * is sent after the server's "+" continuation. */
static gboolean
imap_command (SMailImap *imap, GPtrArray **untagged, GBytes *literal, GError **error, const char *fmt, ...)
{
  va_list ap;
  va_start (ap, fmt);
  g_autofree char *cmd = g_strdup_vprintf (fmt, ap);
  va_end (ap);

  g_autofree char *tag = g_strdup_printf ("C%04u", ++imap->tag);
  g_autofree char *line = g_strdup_printf ("%s %s\r\n", tag, cmd);
  if (!write_all (imap, line, strlen (line), error))
    return FALSE;

  g_autoptr (GPtrArray) responses = g_ptr_array_new_with_free_func ((GDestroyNotify) imap_response_free);
  gsize taglen = strlen (tag);

  for (;;) {
    ImapResponse *r = read_response (imap, error);
    if (!r)
      return FALSE;
    const char *t = r->text->str;

    if (t[0] == '+') {
      if (literal) {
        gsize n;
        const void *data = g_bytes_get_data (literal, &n);
        if (!write_all (imap, data, n, error) || !write_all (imap, "\r\n", 2, error)) {
          imap_response_free (r);
          return FALSE;
        }
        literal = NULL;
      }
      imap_response_free (r);
      continue;
    }

    if (strncmp (t, tag, taglen) == 0 && t[taglen] == ' ') {
      const char *status = t + taglen + 1;
      gboolean ok = g_ascii_strncasecmp (status, "OK", 2) == 0;
      if (!ok) {
        gboolean bad = g_ascii_strncasecmp (status, "BAD", 3) == 0;
        const char *msg = strchr (status, ' ');
        g_set_error (error, SMAIL_IMAP_ERROR,
                     bad ? SMAIL_IMAP_ERROR_BAD : SMAIL_IMAP_ERROR_NO,
                     "%s", msg ? msg + 1 : status);
      }
      imap_response_free (r);
      if (ok && untagged)
        *untagged = g_steal_pointer (&responses);
      return ok;
    }

    if (t[0] == '*')
      g_ptr_array_add (responses, r);
    else
      imap_response_free (r);
  }
}

static void
update_capabilities (SMailImap *imap, GPtrArray *responses)
{
  for (guint i = 0; responses && i < responses->len; i++) {
    ImapResponse *r = g_ptr_array_index (responses, i);
    if (g_ascii_strncasecmp (r->text->str, "* CAPABILITY ", 13) == 0) {
      g_free (imap->capabilities);
      imap->capabilities = g_ascii_strup (r->text->str + 13, -1);
    }
  }
}

gboolean
smail_imap_has_capability (SMailImap *imap, const char *cap)
{
  if (!imap->capabilities)
    return FALSE;
  g_auto (GStrv) caps = g_strsplit (imap->capabilities, " ", -1);
  return g_strv_contains ((const char * const *) caps, cap);
}

/* ---------------- connection ---------------- */

SMailImap *
smail_imap_connect (const SMailAccount *account, GCancellable *cancellable, GError **error)
{
  g_autoptr (GSocketConnection) conn = NULL;
  g_autoptr (GSocketConnectable) id = NULL;
  g_autoptr (GIOStream) tls = NULL;
  g_autoptr (GPtrArray) resp = NULL;
  g_autofree char *user = NULL;
  g_autofree char *pass = NULL;
  ImapResponse *greeting;

  SMailImap *imap = g_new0 (SMailImap, 1);
  imap->client = g_socket_client_new ();
  g_socket_client_set_timeout (imap->client, 30);
  if (account->imap_security == SMAIL_SECURITY_SSL)
    g_socket_client_set_tls (imap->client, TRUE);

  conn =
    g_socket_client_connect_to_host (imap->client, account->imap_host, account->imap_port, cancellable, error);
  if (!conn)
    goto fail;
  set_streams (imap, G_IO_STREAM (conn));

  /* greeting */
  greeting = read_response (imap, error);
  if (!greeting)
    goto fail;
  if (g_ascii_strncasecmp (greeting->text->str, "* BYE", 5) == 0) {
    g_set_error (error, SMAIL_IMAP_ERROR, SMAIL_IMAP_ERROR_NO, "%s", greeting->text->str + 2);
    imap_response_free (greeting);
    goto fail;
  }
  imap_response_free (greeting);

  if (account->imap_security == SMAIL_SECURITY_STARTTLS) {
    if (!imap_command (imap, NULL, NULL, error, "STARTTLS"))
      goto fail;
    id = g_network_address_new (account->imap_host, account->imap_port);
    tls = g_tls_client_connection_new (imap->stream, id, error);
    if (!tls || !g_tls_connection_handshake (G_TLS_CONNECTION (tls), cancellable, error))
      goto fail;
    set_streams (imap, tls);
  }

  user = imap_quote (account->username ? account->username : account->email);
  pass = imap_quote (account->password ? account->password : "");
  if (!imap_command (imap, &resp, NULL, error, "LOGIN %s %s", user, pass))
    goto fail;
  g_clear_pointer (&resp, g_ptr_array_unref);
  if (imap_command (imap, &resp, NULL, NULL, "CAPABILITY"))
    update_capabilities (imap, resp);

  return imap;

fail:
  smail_imap_free (imap);
  return NULL;
}

gboolean
smail_imap_is_connected (SMailImap *imap)
{
  return imap && !imap->broken;
}

void
smail_imap_logout (SMailImap *imap)
{
  if (imap && !imap->broken)
    imap_command (imap, NULL, NULL, NULL, "LOGOUT");
}

void
smail_imap_free (SMailImap *imap)
{
  if (!imap)
    return;
  if (imap->stream)
    g_io_stream_close (imap->stream, NULL, NULL);
  g_clear_object (&imap->in);
  g_clear_object (&imap->stream);
  g_clear_object (&imap->client);
  g_free (imap->selected);
  g_free (imap->capabilities);
  g_free (imap);
}

static gboolean
imap_select (SMailImap *imap, const char *folder, GError **error)
{
  if (imap->selected && g_str_equal (imap->selected, folder))
    return TRUE;
  g_autofree char *q = imap_quote_mailbox (folder);
  g_clear_pointer (&imap->selected, g_free);
  if (!imap_command (imap, NULL, NULL, error, "SELECT %s", q))
    return FALSE;
  imap->selected = g_strdup (folder);
  return TRUE;
}

/* ---------------- tokenizer for LIST ---------------- */

static void
skip_spaces (const char **p)
{
  while (**p == ' ')
    (*p)++;
}

/* Parse an astring / nstring: atom, "quoted", {N} literal or NIL. */
static char *
parse_string (const char **p, ImapResponse *r, guint *lit)
{
  skip_spaces (p);
  const char *s = *p;
  if (*s == '"') {
    GString *out = g_string_new (NULL);
    s++;
    while (*s && *s != '"') {
      if (*s == '\\' && s[1])
        s++;
      g_string_append_c (out, *s++);
    }
    if (*s == '"')
      s++;
    *p = s;
    return g_string_free (out, FALSE);
  }
  if (*s == '{') {
    const char *end = strchr (s, '}');
    *p = end ? end + 1 : s + strlen (s);
    if (*lit < r->literals->len) {
      GBytes *b = g_ptr_array_index (r->literals, (*lit)++);
      gsize n;
      const char *d = g_bytes_get_data (b, &n);
      return g_strndup (d, n);
    }
    return g_strdup ("");
  }
  const char *start = s;
  while (*s && *s != ' ' && *s != ')')
    s++;
  *p = s;
  char *atom = g_strndup (start, s - start);
  if (g_ascii_strcasecmp (atom, "NIL") == 0) {
    g_free (atom);
    return NULL;
  }
  return atom;
}

static const char *
role_from_flags (const char *flags, const char *name)
{
  struct { const char *flag, *role; } map[] = {
    { "\\sent", "sent" }, { "\\drafts", "drafts" }, { "\\trash", "trash" },
    { "\\junk", "junk" }, { "\\archive", "archive" }, { "\\all", "all" },
    { "\\flagged", "flagged" },
  };
  g_autofree char *lf = g_ascii_strdown (flags, -1);
  for (gsize i = 0; i < G_N_ELEMENTS (map); i++)
    if (strstr (lf, map[i].flag))
      return map[i].role;

  if (g_ascii_strcasecmp (name, "INBOX") == 0)
    return "inbox";

  /* servers without SPECIAL-USE: guess from common names */
  struct { const char *name, *role; } names[] = {
    { "sent", "sent" }, { "sent items", "sent" }, { "sent messages", "sent" }, { "sent mail", "sent" },
    { "drafts", "drafts" }, { "trash", "trash" }, { "deleted items", "trash" },
    { "deleted messages", "trash" }, { "junk", "junk" }, { "spam", "junk" },
    { "junk e-mail", "junk" }, { "archive", "archive" }, { "archives", "archive" },
  };
  for (gsize i = 0; i < G_N_ELEMENTS (names); i++)
    if (g_ascii_strcasecmp (name, names[i].name) == 0)
      return names[i].role;
  return NULL;
}

static const char *
role_display_name (const char *role)
{
  if (!role) return NULL;
  if (g_str_equal (role, "inbox"))   return "Inbox";
  if (g_str_equal (role, "flagged")) return "Starred";
  if (g_str_equal (role, "drafts"))  return "Drafts";
  if (g_str_equal (role, "sent"))    return "Sent";
  if (g_str_equal (role, "archive")) return "Archive";
  if (g_str_equal (role, "all"))     return "All Mail";
  if (g_str_equal (role, "junk"))    return "Junk";
  if (g_str_equal (role, "trash"))   return "Trash";
  return NULL;
}

GPtrArray *
smail_imap_list_folders (SMailImap *imap, GError **error)
{
  g_autoptr (GPtrArray) resp = NULL;
  if (!imap_command (imap, &resp, NULL, error, "LIST \"\" \"*\""))
    return NULL;

  GPtrArray *folders = g_ptr_array_new_with_free_func ((GDestroyNotify) smail_folder_free);
  GHashTable *seen_roles = g_hash_table_new (g_str_hash, g_str_equal);

  for (guint i = 0; i < resp->len; i++) {
    ImapResponse *r = g_ptr_array_index (resp, i);
    const char *p = r->text->str;
    if (g_ascii_strncasecmp (p, "* LIST ", 7) != 0)
      continue;
    p += 7;
    skip_spaces (&p);
    if (*p != '(')
      continue;
    const char *close = strchr (p, ')');
    if (!close)
      continue;
    g_autofree char *flags = g_strndup (p + 1, close - p - 1);
    p = close + 1;

    guint lit = 0;
    g_autofree char *delim = parse_string (&p, r, &lit);
    g_autofree char *raw_name = parse_string (&p, r, &lit);
    if (!raw_name)
      continue;

    g_autofree char *lflags = g_ascii_strdown (flags, -1);
    if (strstr (lflags, "\\noselect") || strstr (lflags, "\\nonexistent"))
      continue;

    g_autofree char *name = smail_imap_utf7_decode (raw_name);
    const char *role = role_from_flags (flags, name);
    if (role && g_hash_table_contains (seen_roles, role))
      role = NULL; /* keep the first folder for each role */
    if (role)
      g_hash_table_add (seen_roles, (gpointer) role);

    const char *display = role_display_name (role);
    if (!display) {
      display = name;
      if (delim && *delim) {
        const char *last = g_strrstr (name, delim);
        if (last && last[strlen (delim)])
          display = last + strlen (delim);
      }
    }
    SMailFolder *f = smail_folder_new (name, display, role);
    f->delimiter = g_strdup (delim);

    /* unread counter */
    g_autofree char *q = imap_quote_mailbox (name);
    g_autoptr (GPtrArray) st = NULL;
    if (imap_command (imap, &st, NULL, NULL, "STATUS %s (UNSEEN)", q)) {
      for (guint j = 0; j < st->len; j++) {
        ImapResponse *sr = g_ptr_array_index (st, j);
        const char *u = strstr (sr->text->str, "UNSEEN ");
        if (u)
          f->unread = atoi (u + 7);
      }
    }
    g_ptr_array_add (folders, f);
  }
  g_hash_table_unref (seen_roles);
  g_ptr_array_sort (folders, smail_folder_compare);
  return folders;
}

/* Run a UID SEARCH (optionally with one literal argument) and return the UIDs, ascending. */
static GArray *
search_uids (SMailImap *imap, GBytes *literal, GError **error, const char *criteria)
{
  g_autoptr (GPtrArray) resp = NULL;
  if (!imap_command (imap, &resp, literal, error, "UID SEARCH %s", criteria))
    return NULL;
  GArray *uids = g_array_new (FALSE, FALSE, sizeof (guint64));
  for (guint i = 0; i < resp->len; i++) {
    ImapResponse *r = g_ptr_array_index (resp, i);
    if (g_ascii_strncasecmp (r->text->str, "* SEARCH", 8) != 0)
      continue;
    g_auto (GStrv) parts = g_strsplit (r->text->str + 8, " ", -1);
    for (int j = 0; parts[j]; j++)
      if (*parts[j]) {
        guint64 u = g_ascii_strtoull (parts[j], NULL, 10);
        g_array_append_val (uids, u);
      }
  }
  return uids;
}

static int
compare_u64 (gconstpointer a, gconstpointer b)
{
  guint64 x = *(const guint64 *) a, y = *(const guint64 *) b;
  return x < y ? -1 : x > y;
}

/* Fetch list summaries (headers + flags) for the newest `limit` of `uids`. */
static GPtrArray *
fetch_summaries (SMailImap *imap, GArray *uids, guint limit, GError **error)
{
  GPtrArray *messages = g_ptr_array_new_with_free_func (g_object_unref);
  if (uids->len == 0)
    return messages;
  g_array_sort (uids, compare_u64);

  guint start = uids->len > limit ? uids->len - limit : 0;
  GString *set = g_string_new (NULL);
  for (guint i = start; i < uids->len; i++)
    g_string_append_printf (set, "%s%" G_GUINT64_FORMAT, set->len ? "," : "", g_array_index (uids, guint64, i));

  g_autoptr (GPtrArray) resp = NULL;
  gboolean ok = imap_command (imap, &resp, NULL, error,
                              "UID FETCH %s (UID FLAGS BODY.PEEK[HEADER.FIELDS (FROM TO CC SUBJECT DATE CONTENT-TYPE)])",
                              set->str);
  g_string_free (set, TRUE);
  if (!ok) {
    g_ptr_array_unref (messages);
    return NULL;
  }

  for (guint i = 0; i < resp->len; i++) {
    ImapResponse *r = g_ptr_array_index (resp, i);
    const char *t = r->text->str;
    if (!strstr (t, " FETCH ") || r->literals->len == 0)
      continue;
    const char *u = strstr (t, "UID ");
    if (!u)
      continue;
    g_autofree char *uid = g_strdup_printf ("%" G_GUINT64_FORMAT, g_ascii_strtoull (u + 4, NULL, 10));
    gboolean seen = FALSE, flagged = FALSE;
    const char *f = strstr (t, "FLAGS (");
    if (f) {
      const char *end = strchr (f, ')');
      g_autofree char *fl = g_strndup (f, end ? end - f : (gssize) strlen (f));
      seen = strstr (fl, "\\Seen") != NULL;
      flagged = strstr (fl, "\\Flagged") != NULL;
    }
    g_ptr_array_add (messages,
                     smail_message_new_from_headers (uid, g_ptr_array_index (r->literals, 0), seen, flagged));
  }
  return messages;
}

GPtrArray *
smail_imap_list_messages (SMailImap *imap, const char *folder, guint limit, GError **error)
{
  /* force a fresh SELECT so new mail is seen */
  g_clear_pointer (&imap->selected, g_free);
  if (!imap_select (imap, folder, error))
    return NULL;
  g_autoptr (GArray) uids = search_uids (imap, NULL, error, "ALL");
  return uids ? fetch_summaries (imap, uids, limit, error) : NULL;
}

GPtrArray *
smail_imap_search (SMailImap *imap, const char *folder, const char *query, guint limit, GError **error)
{
  g_clear_pointer (&imap->selected, g_free);
  if (!imap_select (imap, folder, error))
    return NULL;

  /* TEXT matches headers and body. Plain ASCII goes in a quoted string;
   * anything else is sent as a UTF-8 literal. */
  gboolean ascii = TRUE;
  for (const char *p = query; *p; p++)
    if ((guchar) *p >= 0x7f || *p < 0x20)
      ascii = FALSE;

  g_autoptr (GArray) uids = NULL;
  if (ascii) {
    g_autofree char *q = imap_quote (query);
    g_autofree char *criteria = g_strdup_printf ("TEXT %s", q);
    uids = search_uids (imap, NULL, error, criteria);
  } else {
    g_autoptr (GBytes) lit = g_bytes_new (query, strlen (query));
    g_autofree char *criteria = g_strdup_printf ("CHARSET UTF-8 TEXT {%" G_GSIZE_FORMAT "}", strlen (query));
    uids = search_uids (imap, lit, error, criteria);
  }
  return uids ? fetch_summaries (imap, uids, limit, error) : NULL;
}

gboolean
smail_imap_create_folder (SMailImap *imap, const char *name, GError **error)
{
  g_autofree char *q = imap_quote_mailbox (name);
  if (!imap_command (imap, NULL, NULL, error, "CREATE %s", q))
    return FALSE;
  imap_command (imap, NULL, NULL, NULL, "SUBSCRIBE %s", q);
  return TRUE;
}

gboolean
smail_imap_rename_folder (SMailImap *imap, const char *from, const char *to, GError **error)
{
  g_autofree char *a = imap_quote_mailbox (from);
  g_autofree char *b = imap_quote_mailbox (to);
  /* a selected mailbox can't always be renamed: drop the selection first */
  if (imap->selected && g_str_equal (imap->selected, from)) {
    imap_command (imap, NULL, NULL, NULL, "CLOSE");
    g_clear_pointer (&imap->selected, g_free);
  }
  if (!imap_command (imap, NULL, NULL, error, "RENAME %s %s", a, b))
    return FALSE;
  imap_command (imap, NULL, NULL, NULL, "UNSUBSCRIBE %s", a);
  imap_command (imap, NULL, NULL, NULL, "SUBSCRIBE %s", b);
  return TRUE;
}

gboolean
smail_imap_delete_folder (SMailImap *imap, const char *name, GError **error)
{
  g_autofree char *q = imap_quote_mailbox (name);
  if (imap->selected && g_str_equal (imap->selected, name)) {
    imap_command (imap, NULL, NULL, NULL, "CLOSE");
    g_clear_pointer (&imap->selected, g_free);
  }
  imap_command (imap, NULL, NULL, NULL, "UNSUBSCRIBE %s", q);
  return imap_command (imap, NULL, NULL, error, "DELETE %s", q);
}

GBytes *
smail_imap_fetch_message (SMailImap *imap, const char *folder, const char *uid, GError **error)
{
  if (!imap_select (imap, folder, error))
    return NULL;
  g_autoptr (GPtrArray) resp = NULL;
  /* BODY[] (not PEEK) marks the message as read, like any mail client */
  if (!imap_command (imap, &resp, NULL, error, "UID FETCH %s (UID BODY[])", uid))
    return NULL;
  for (guint i = 0; i < resp->len; i++) {
    ImapResponse *r = g_ptr_array_index (resp, i);
    if (strstr (r->text->str, " FETCH ") && r->literals->len > 0)
      return g_bytes_ref (g_ptr_array_index (r->literals, 0));
  }
  g_set_error (error, SMAIL_IMAP_ERROR, SMAIL_IMAP_ERROR_NO, "The message no longer exists on the server");
  return NULL;
}

gboolean
smail_imap_store_flag (SMailImap *imap, const char *folder, const char *uid,
                         const char *flag, gboolean add, GError **error)
{
  if (!imap_select (imap, folder, error))
    return FALSE;
  return imap_command (imap, NULL, NULL, error, "UID STORE %s %cFLAGS.SILENT (%s)", uid, add ? '+' : '-', flag);
}

static gboolean
expunge_uid (SMailImap *imap, const char *uid, GError **error)
{
  if (smail_imap_has_capability (imap, "UIDPLUS"))
    return imap_command (imap, NULL, NULL, error, "UID EXPUNGE %s", uid);
  return imap_command (imap, NULL, NULL, error, "EXPUNGE");
}

gboolean
smail_imap_move (SMailImap *imap, const char *folder, const char *uid, const char *dest, GError **error)
{
  if (!imap_select (imap, folder, error))
    return FALSE;
  g_autofree char *q = imap_quote_mailbox (dest);
  if (smail_imap_has_capability (imap, "MOVE"))
    return imap_command (imap, NULL, NULL, error, "UID MOVE %s %s", uid, q);

  return imap_command (imap, NULL, NULL, error, "UID COPY %s %s", uid, q) &&
         imap_command (imap, NULL, NULL, error, "UID STORE %s +FLAGS.SILENT (\\Deleted)", uid) &&
         expunge_uid (imap, uid, error);
}

gboolean
smail_imap_delete (SMailImap *imap, const char *folder, const char *uid, GError **error)
{
  if (!imap_select (imap, folder, error))
    return FALSE;
  return imap_command (imap, NULL, NULL, error, "UID STORE %s +FLAGS.SILENT (\\Deleted)", uid) &&
         expunge_uid (imap, uid, error);
}

gboolean
smail_imap_append (SMailImap *imap, const char *folder, GBytes *message, const char *flags, GError **error)
{
  g_autofree char *q = imap_quote_mailbox (folder);
  return imap_command (imap, NULL, message, error, "APPEND %s (%s) {%" G_GSIZE_FORMAT "}",
                       q, flags ? flags : "", g_bytes_get_size (message));
}
