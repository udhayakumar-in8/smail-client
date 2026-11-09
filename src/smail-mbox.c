/* smail-mbox.c — mboxrd reading and writing */
#include "smail-mbox.h"

#include <string.h>

/* "From " separator line: "From sender@example.org Tue Nov  3 10:00:00 2026" */
static gboolean
is_separator (const char *line, gsize len)
{
  return len >= 5 && strncmp (line, "From ", 5) == 0;
}

static GBytes *
finish_message (GString *msg)
{
  /* trim the blank line that separates messages */
  while (msg->len >= 2 && g_str_has_suffix (msg->str, "\r\n\r\n"))
    g_string_truncate (msg, msg->len - 2);
  if (msg->len == 0)
    return NULL;
  return g_bytes_new (msg->str, msg->len);
}

GPtrArray *
smail_mbox_split (const char *data, gsize len)
{
  GPtrArray *out = g_ptr_array_new_with_free_func ((GDestroyNotify) g_bytes_unref);
  g_autoptr (GString) msg = g_string_new (NULL);
  gboolean mbox = len >= 5 && strncmp (data, "From ", 5) == 0;
  gboolean prev_blank = TRUE;

  const char *p = data, *end = data + len;
  while (p < end) {
    const char *nl = memchr (p, '\n', end - p);
    gsize n = nl ? (gsize) (nl - p) : (gsize) (end - p);
    gsize content = n && p[n - 1] == '\r' ? n - 1 : n;

    if (mbox && prev_blank && is_separator (p, content)) {
      GBytes *b = finish_message (msg);
      if (b)
        g_ptr_array_add (out, b);
      g_string_truncate (msg, 0);
    } else {
      const char *line = p;
      gsize l = content;
      /* mboxrd: ">From ", ">>From " ... lose one ">" */
      if (mbox && l >= 6 && line[0] == '>') {
        gsize k = 0;
        while (k < l && line[k] == '>')
          k++;
        if (l - k >= 5 && strncmp (line + k, "From ", 5) == 0) {
          line++;
          l--;
        }
      }
      g_string_append_len (msg, line, l);
      g_string_append (msg, "\r\n");
    }
    prev_blank = content == 0;
    p = nl ? nl + 1 : end;
  }
  GBytes *b = finish_message (msg);
  if (b)
    g_ptr_array_add (out, b);
  return out;
}

void
smail_mbox_append (GString *mbox, GBytes *message, const char *sender, GDateTime *date)
{
  /* the separator date is C-locale asctime() style: "Tue Nov  3 10:00:00 2026"
   * (g_date_time_format would translate names and pad with a Unicode space) */
  static const char *days[] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
  static const char *months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
  g_autoptr (GDateTime) t = date ? g_date_time_to_utc (date) : g_date_time_new_now_utc ();
  g_string_append_printf (mbox, "From %s %s %s %2d %02d:%02d:%02d %d\n",
                          sender && *sender ? sender : "MAILER-DAEMON",
                          days[g_date_time_get_day_of_week (t) - 1], months[g_date_time_get_month (t) - 1],
                          g_date_time_get_day_of_month (t), g_date_time_get_hour (t),
                          g_date_time_get_minute (t), g_date_time_get_second (t), g_date_time_get_year (t));

  gsize len;
  const char *data = g_bytes_get_data (message, &len);
  const char *p = data, *end = data + len;
  while (p < end) {
    const char *nl = memchr (p, '\n', end - p);
    gsize n = nl ? (gsize) (nl - p) : (gsize) (end - p);
    gsize content = n && p[n - 1] == '\r' ? n - 1 : n;
    gsize k = 0;
    while (k < content && p[k] == '>')
      k++;
    if (content - k >= 5 && strncmp (p + k, "From ", 5) == 0)
      g_string_append_c (mbox, '>');
    g_string_append_len (mbox, p, content);
    g_string_append_c (mbox, '\n');
    p = nl ? nl + 1 : end;
  }
  g_string_append_c (mbox, '\n');
}
