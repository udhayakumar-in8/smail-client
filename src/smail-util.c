/* smail-util.c — formatting helpers */
#include "smail-util.h"

#include <string.h>

char *
smail_format_date_short (GDateTime *date)
{
  if (!date)
    return g_strdup ("");
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  int y1, m1, d1, y2, m2, d2;
  g_date_time_get_ymd (now, &y1, &m1, &d1);
  g_date_time_get_ymd (date, &y2, &m2, &d2);

  if (y1 == y2 && m1 == m2 && d1 == d2)
    return g_date_time_format (date, "%H:%M");

  g_autoptr (GDateTime) yesterday = g_date_time_add_days (now, -1);
  g_date_time_get_ymd (yesterday, &y1, &m1, &d1);
  if (y1 == y2 && m1 == m2 && d1 == d2)
    return g_strdup ("Yesterday");

  GTimeSpan diff = g_date_time_difference (now, date);
  if (diff >= 0 && diff < 6 * G_TIME_SPAN_DAY)
    return g_date_time_format (date, "%A");
  if (y1 == y2)
    return g_date_time_format (date, "%-d %b");
  return g_date_time_format (date, "%x");
}

char *
smail_format_date_long (GDateTime *date)
{
  if (!date)
    return g_strdup ("");
  return g_date_time_format (date, "%A, %-d %B %Y at %H:%M");
}

/* ---------- HTML → plain text (for reply quoting) ---------- */

static const struct { const char *name; const char *value; } entities[] = {
  { "amp", "&" }, { "lt", "<" }, { "gt", ">" }, { "quot", "\"" }, { "apos", "'" },
  { "nbsp", " " }, { "copy", "©" }, { "reg", "®" }, { "mdash", "—" }, { "ndash", "–" },
  { "hellip", "…" }, { "rsquo", "’" }, { "lsquo", "‘" }, { "rdquo", "”" }, { "ldquo", "“" },
};

static const char *
decode_entity (const char *p, GString *out)
{
  const char *semi = strchr (p, ';');
  if (!semi || semi - p > 10) {
    g_string_append_c (out, '&');
    return p + 1;
  }
  g_autofree char *name = g_strndup (p + 1, semi - p - 1);
  if (name[0] == '#') {
    gunichar c = name[1] == 'x' || name[1] == 'X' ? g_ascii_strtoull (name + 2, NULL, 16)
                                                  : g_ascii_strtoull (name + 1, NULL, 10);
    if (g_unichar_validate (c) && c != 0)
      g_string_append_unichar (out, c == 0xa0 ? ' ' : c);
    return semi + 1;
  }
  for (gsize i = 0; i < G_N_ELEMENTS (entities); i++)
    if (g_str_equal (name, entities[i].name)) {
      g_string_append (out, entities[i].value);
      return semi + 1;
    }
  g_string_append_c (out, '&');
  return p + 1;
}

char *
smail_html_to_text (const char *html)
{
  GString *out = g_string_new (NULL);
  const char *p = html;
  gboolean skip = FALSE; /* inside <style>/<script>/<head> */
  gboolean space = FALSE;

  while (*p) {
    if (*p == '<') {
      const char *end = strchr (p, '>');
      if (!end)
        break;
      g_autofree char *tag = g_ascii_strdown (p + 1, end - p - 1);
      char *t = tag;
      gboolean closing = *t == '/';
      if (closing)
        t++;
      gsize n = strcspn (t, " \t\r\n/");
      t[n] = '\0';

      if (g_str_equal (t, "style") || g_str_equal (t, "script") || g_str_equal (t, "head"))
        skip = !closing;
      else if (g_str_equal (t, "br") || (closing && (g_str_equal (t, "p") || g_str_equal (t, "div") ||
               g_str_equal (t, "tr") || g_str_equal (t, "li") || g_str_equal (t, "h1") ||
               g_str_equal (t, "h2") || g_str_equal (t, "h3") || g_str_equal (t, "table")))) {
        g_string_append_c (out, '\n');
        space = FALSE;
      } else if (!closing && g_str_equal (t, "li")) {
        g_string_append (out, "• ");
      }
      p = end + 1;
      continue;
    }
    if (skip) {
      p++;
      continue;
    }
    if (*p == '&') {
      p = decode_entity (p, out);
      space = FALSE;
      continue;
    }
    if (g_ascii_isspace (*p)) {
      if (!space && out->len && out->str[out->len - 1] != '\n')
        g_string_append_c (out, ' ');
      space = TRUE;
      p++;
      continue;
    }
    space = FALSE;
    g_string_append_c (out, *p++);
  }

  /* collapse 3+ newlines */
  g_autoptr (GRegex) re = g_regex_new ("\n[ \t]*\n(\\s*\n)+", 0, 0, NULL);
  g_autofree char *raw = g_string_free (out, FALSE);
  char *res = g_regex_replace_literal (re, raw, -1, 0, "\n\n", 0, NULL);
  return g_strstrip (res);
}

/* ---------- plain text → HTML ---------- */

char *
smail_text_to_html (const char *text)
{
  g_autoptr (GRegex) url_re = g_regex_new ("(https?://[^\\s<>\"]+[^\\s<>\".,;:!?)\\]'])", G_REGEX_OPTIMIZE, 0, NULL);
  GString *out = g_string_new ("<div class=\"plain\">");
  g_auto (GStrv) lines = g_strsplit (text, "\n", -1);

  for (int i = 0; lines[i]; i++) {
    char *line = lines[i];
    gsize len = strlen (line);
    if (len && line[len - 1] == '\r')
      line[len - 1] = '\0';

    int depth = 0;
    for (const char *q = line; *q == '>' || *q == ' '; q++)
      if (*q == '>')
        depth++;

    g_autofree char *esc = g_markup_escape_text (line, -1);
    g_autofree char *linked = g_regex_replace (url_re, esc, -1, 0, "<a href=\"\\1\">\\1</a>", 0, NULL);
    if (depth > 0)
      g_string_append_printf (out, "<span class=\"quote q%d\">%s</span>", MIN (depth, 3), linked);
    else
      g_string_append (out, linked);
    if (lines[i + 1])
      g_string_append (out, "<br>\n");
  }
  g_string_append (out, "</div>");
  return g_string_free (out, FALSE);
}

char *
smail_quote_text (const char *text)
{
  GString *out = g_string_new (NULL);
  g_auto (GStrv) lines = g_strsplit (text, "\n", -1);
  for (int i = 0; lines[i]; i++) {
    g_strchomp (lines[i]);
    g_string_append_printf (out, ">%s%s\n", lines[i][0] == '>' || !lines[i][0] ? "" : " ", lines[i]);
  }
  return g_string_free (out, FALSE);
}
