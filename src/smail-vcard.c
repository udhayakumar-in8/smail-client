/* smail-vcard.c — vCard and CSV address book import/export */
#include "smail-vcard.h"

#include <gio/gio.h>
#include <string.h>

/* ---------------- export ---------------- */

static void
append_escaped (GString *out, const char *s)
{
  for (const char *p = s; p && *p; p++) {
    switch (*p) {
    case '\\': g_string_append (out, "\\\\"); break;
    case ',':  g_string_append (out, "\\,"); break;
    case ';':  g_string_append (out, "\\;"); break;
    case '\n': g_string_append (out, "\\n"); break;
    case '\r': break;
    default:   g_string_append_c (out, *p);
    }
  }
}

char *
smail_contacts_to_vcard (GPtrArray *contacts)
{
  GString *out = g_string_new (NULL);
  for (guint i = 0; i < contacts->len; i++) {
    SMailContact *c = g_ptr_array_index (contacts, i);
    const char *name = c->name && *c->name ? c->name : c->email;
    g_string_append (out, "BEGIN:VCARD\r\nVERSION:3.0\r\nFN:");
    append_escaped (out, name);
    g_string_append (out, "\r\nN:");
    /* "Ada Lovelace" -> family "Lovelace", given "Ada" */
    const char *space = c->name ? strrchr (c->name, ' ') : NULL;
    if (space) {
      append_escaped (out, space + 1);
      g_string_append_c (out, ';');
      g_autofree char *given = g_strndup (c->name, space - c->name);
      append_escaped (out, given);
    } else {
      append_escaped (out, name);
      g_string_append_c (out, ';');
    }
    g_string_append (out, ";;;\r\nEMAIL;TYPE=INTERNET:");
    append_escaped (out, c->email);
    g_string_append (out, "\r\n");
    if (c->phone && *c->phone) {
      g_string_append (out, "TEL:");
      append_escaped (out, c->phone);
      g_string_append (out, "\r\n");
    }
    if (c->notes && *c->notes) {
      g_string_append (out, "NOTE:");
      append_escaped (out, c->notes);
      g_string_append (out, "\r\n");
    }
    if (c->favorite)
      g_string_append (out, "CATEGORIES:Favorites\r\n");
    g_string_append (out, "END:VCARD\r\n");
  }
  return g_string_free (out, FALSE);
}

/* ---------------- vCard import ---------------- */

static char *
unescape (const char *s)
{
  GString *out = g_string_new (NULL);
  for (const char *p = s; *p; p++) {
    if (*p == '\\' && p[1]) {
      p++;
      g_string_append_c (out, *p == 'n' || *p == 'N' ? '\n' : *p);
    } else {
      g_string_append_c (out, *p);
    }
  }
  return g_strstrip (g_string_free (out, FALSE));
}

static char *
decode_qp (const char *s)
{
  GString *out = g_string_new (NULL);
  for (const char *p = s; *p; p++) {
    if (*p == '=' && g_ascii_isxdigit (p[1]) && g_ascii_isxdigit (p[2])) {
      g_string_append_c (out, (char) (g_ascii_xdigit_value (p[1]) * 16 + g_ascii_xdigit_value (p[2])));
      p += 2;
    } else {
      g_string_append_c (out, *p);
    }
  }
  return g_string_free (out, FALSE);
}

typedef struct {
  char      *fn, *given, *family, *phone, *notes;
  GPtrArray *emails;
  gboolean   favorite;
} Card;

static void
card_reset (Card *c)
{
  g_clear_pointer (&c->fn, g_free);
  g_clear_pointer (&c->given, g_free);
  g_clear_pointer (&c->family, g_free);
  g_clear_pointer (&c->phone, g_free);
  g_clear_pointer (&c->notes, g_free);
  if (c->emails)
    g_ptr_array_set_size (c->emails, 0);
  else
    c->emails = g_ptr_array_new_with_free_func (g_free);
  c->favorite = FALSE;
}

static void
card_emit (Card *c, GPtrArray *out)
{
  g_autofree char *name = NULL;
  if (c->fn && *c->fn)
    name = g_strdup (c->fn);
  else if ((c->given && *c->given) || (c->family && *c->family))
    name = g_strstrip (g_strdup_printf ("%s %s", c->given ? c->given : "", c->family ? c->family : ""));
  for (guint i = 0; i < c->emails->len; i++) {
    SMailContact *ct = g_new0 (SMailContact, 1);
    ct->name = g_strdup (name);
    ct->email = g_strdup (g_ptr_array_index (c->emails, i));
    ct->phone = g_strdup (c->phone);
    ct->notes = g_strdup (c->notes);
    ct->favorite = c->favorite;
    g_ptr_array_add (out, ct);
  }
}

static GPtrArray *
parse_vcard (const char *data)
{
  GPtrArray *out = g_ptr_array_new_with_free_func ((GDestroyNotify) smail_contact_free);

  /* unfold: a line starting with a space or tab continues the previous one */
  g_autofree char *norm = g_strdup (data);
  g_autoptr (GString) unfolded = g_string_new (NULL);
  g_auto (GStrv) raw_lines = g_strsplit (norm, "\n", -1);
  for (int i = 0; raw_lines[i]; i++) {
    char *l = raw_lines[i];
    gsize n = strlen (l);
    if (n && l[n - 1] == '\r')
      l[--n] = '\0';
    if ((l[0] == ' ' || l[0] == '\t') && unfolded->len)
      g_string_append (unfolded, l + 1);
    else {
      if (unfolded->len)
        g_string_append_c (unfolded, '\n');
      g_string_append (unfolded, l);
    }
  }

  Card card = { 0 };
  card_reset (&card);
  gboolean in_card = FALSE;
  g_auto (GStrv) lines = g_strsplit (unfolded->str, "\n", -1);
  for (int i = 0; lines[i]; i++) {
    char *line = lines[i];
    char *colon = strchr (line, ':');
    if (!colon)
      continue;
    *colon = '\0';
    const char *value = colon + 1;
    g_autofree char *key = g_ascii_strup (line, -1);
    /* strip a group prefix like "item1.EMAIL" */
    char *dot = strchr (key, '.');
    char *semi = strchr (key, ';');
    char *prop = dot && (!semi || dot < semi) ? dot + 1 : key;
    g_autofree char *params = NULL;
    semi = strchr (prop, ';');
    if (semi) {
      params = g_strdup (semi + 1);
      *semi = '\0';
    }
    g_autofree char *decoded = params && strstr (params, "QUOTED-PRINTABLE") ? decode_qp (value) : g_strdup (value);

    if (g_str_equal (prop, "BEGIN") && g_ascii_strcasecmp (value, "VCARD") == 0) {
      card_reset (&card);
      in_card = TRUE;
    } else if (g_str_equal (prop, "END") && in_card) {
      card_emit (&card, out);
      in_card = FALSE;
    } else if (!in_card) {
      continue;
    } else if (g_str_equal (prop, "FN")) {
      g_free (card.fn);
      card.fn = unescape (decoded);
    } else if (g_str_equal (prop, "N")) {
      g_auto (GStrv) parts = g_strsplit (decoded, ";", 3);
      g_free (card.family);
      g_free (card.given);
      card.family = parts[0] ? unescape (parts[0]) : NULL;
      card.given = parts[0] && parts[1] ? unescape (parts[1]) : NULL;
    } else if (g_str_equal (prop, "EMAIL")) {
      char *e = unescape (decoded);
      if (g_str_has_prefix (e, "mailto:"))
        memmove (e, e + 7, strlen (e + 7) + 1);
      if (strchr (e, '@'))
        g_ptr_array_add (card.emails, e);
      else
        g_free (e);
    } else if (g_str_equal (prop, "TEL") && !card.phone) {
      card.phone = unescape (decoded);
      if (g_str_has_prefix (card.phone, "tel:"))
        memmove (card.phone, card.phone + 4, strlen (card.phone + 4) + 1);
    } else if (g_str_equal (prop, "NOTE")) {
      g_free (card.notes);
      card.notes = unescape (decoded);
    } else if (g_str_equal (prop, "CATEGORIES")) {
      g_autofree char *cats = g_ascii_strdown (decoded, -1);
      card.favorite = strstr (cats, "favorite") || strstr (cats, "starred");
    }
  }
  card_reset (&card);
  g_ptr_array_unref (card.emails);
  return out;
}

/* ---------------- CSV import ---------------- */

/* Split CSV text into rows of fields (RFC 4180 quoting). */
static GPtrArray *
parse_csv_rows (const char *data)
{
  GPtrArray *rows = g_ptr_array_new_with_free_func ((GDestroyNotify) g_ptr_array_unref);
  GPtrArray *row = g_ptr_array_new_with_free_func (g_free);
  GString *field = g_string_new (NULL);
  gboolean quoted = FALSE;
  /* the delimiter is ',' unless the header line has more ';' (some locales) */
  const char *eol = strchr (data, '\n');
  gsize hlen = eol ? (gsize) (eol - data) : strlen (data);
  int commas = 0, semis = 0;
  for (gsize i = 0; i < hlen; i++) {
    commas += data[i] == ',';
    semis += data[i] == ';';
  }
  char delim = semis > commas ? ';' : ',';

  for (const char *p = data; ; p++) {
    char ch = *p;
    if (quoted) {
      if (ch == '"' && p[1] == '"') { g_string_append_c (field, '"'); p++; }
      else if (ch == '"') quoted = FALSE;
      else if (ch == '\0') break;
      else g_string_append_c (field, ch);
      continue;
    }
    if (ch == '"' && field->len == 0) { quoted = TRUE; continue; }
    if (ch == delim) {
      g_ptr_array_add (row, g_strdup (field->str));
      g_string_truncate (field, 0);
      continue;
    }
    if (ch == '\r')
      continue;
    if (ch == '\n' || ch == '\0') {
      g_ptr_array_add (row, g_strdup (field->str));
      g_string_truncate (field, 0);
      gboolean empty = row->len == 1 && !*(char *) g_ptr_array_index (row, 0);
      if (!empty)
        g_ptr_array_add (rows, row);
      else
        g_ptr_array_unref (row);
      if (ch == '\0')
        break;
      row = g_ptr_array_new_with_free_func (g_free);
      continue;
    }
    g_string_append_c (field, ch);
  }
  g_string_free (field, TRUE);
  return rows;
}

static int
find_column (GPtrArray *header, const char * const *names)
{
  for (int n = 0; names[n]; n++)
    for (guint i = 0; i < header->len; i++) {
      g_autofree char *h = g_strstrip (g_strdup (g_ptr_array_index (header, i)));
      /* a UTF-8 byte order mark may sit before the first heading */
      const char *hh = g_str_has_prefix (h, "\xEF\xBB\xBF") ? h + 3 : h;
      if (g_ascii_strcasecmp (hh, names[n]) == 0)
        return i;
    }
  return -1;
}

static const char *
cell (GPtrArray *row, int col)
{
  return col >= 0 && (guint) col < row->len ? g_ptr_array_index (row, col) : NULL;
}

static GPtrArray *
parse_csv (const char *data, GError **error)
{
  static const char *name_cols[]  = { "Display Name", "Name", "Full Name", NULL };
  static const char *first_cols[] = { "First Name", "Given Name", NULL };
  static const char *last_cols[]  = { "Last Name", "Family Name", "Surname", NULL };
  static const char *email_cols[] = { "Primary Email", "E-mail Address", "E-mail 1 - Value", "Email",
                                      "Email Address", "E-mail", "Mail", NULL };
  static const char *email2_cols[] = { "Secondary Email", "E-mail 2 Address", "E-mail 2 - Value", NULL };
  static const char *phone_cols[] = { "Mobile Number", "Mobile Phone", "Phone 1 - Value", "Work Phone",
                                      "Home Phone", "Phone", "Business Phone", NULL };
  static const char *notes_cols[] = { "Notes", "Note", NULL };

  g_autoptr (GPtrArray) rows = parse_csv_rows (data);
  if (rows->len == 0) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "The file is empty");
    return NULL;
  }
  GPtrArray *header = g_ptr_array_index (rows, 0);
  int c_name = find_column (header, name_cols), c_first = find_column (header, first_cols);
  int c_last = find_column (header, last_cols), c_email = find_column (header, email_cols);
  int c_email2 = find_column (header, email2_cols), c_phone = find_column (header, phone_cols);
  int c_notes = find_column (header, notes_cols);
  if (c_email < 0) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                 "No email column found. The first line must name the columns, e.g. “Name,Email”.");
    return NULL;
  }

  GPtrArray *out = g_ptr_array_new_with_free_func ((GDestroyNotify) smail_contact_free);
  for (guint r = 1; r < rows->len; r++) {
    GPtrArray *row = g_ptr_array_index (rows, r);
    g_autofree char *name = NULL;
    if (cell (row, c_name) && *cell (row, c_name))
      name = g_strdup (cell (row, c_name));
    else if ((cell (row, c_first) && *cell (row, c_first)) || (cell (row, c_last) && *cell (row, c_last)))
      name = g_strdup_printf ("%s %s", cell (row, c_first) ? cell (row, c_first) : "",
                              cell (row, c_last) ? cell (row, c_last) : "");
    const char *emails[] = { cell (row, c_email), cell (row, c_email2) };
    for (gsize e = 0; e < G_N_ELEMENTS (emails); e++) {
      if (!emails[e] || !strchr (emails[e], '@'))
        continue;
      /* Google puts several addresses in one cell, separated by " ::: " */
      g_auto (GStrv) parts = g_strsplit (emails[e], ":::", -1);
      for (int k = 0; parts[k]; k++) {
        g_strstrip (parts[k]);
        if (!strchr (parts[k], '@'))
          continue;
        SMailContact *c = g_new0 (SMailContact, 1);
        c->name = name ? g_strstrip (g_strdup (name)) : NULL;
        c->email = g_strdup (parts[k]);
        c->phone = g_strdup (cell (row, c_phone));
        c->notes = g_strdup (cell (row, c_notes));
        g_ptr_array_add (out, c);
      }
    }
  }
  return out;
}

GPtrArray *
smail_contacts_parse (const char *data, gsize len, GError **error)
{
  g_autofree char *text = g_strndup (data, len);
  if (!g_utf8_validate (text, -1, NULL)) {
    /* old exports are often Latin-1 */
    char *conv = g_convert (text, -1, "UTF-8", "ISO-8859-1", NULL, NULL, NULL);
    if (!conv) {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "The file isn't text");
      return NULL;
    }
    g_free (text);
    text = conv;
  }
  g_autofree char *upper = g_ascii_strup (text, MIN (strlen (text), 4096));
  if (strstr (upper, "BEGIN:VCARD"))
    return parse_vcard (text);
  return parse_csv (text, error);
}
