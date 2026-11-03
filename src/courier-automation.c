/* courier-automation.c — filter matching and mail log entries */
#include "courier-automation.h"
#include "courier-util.h"

#include <string.h>

const char * const courier_filter_fields[]        = { "from", "to", "subject", "any", NULL };
const char * const courier_filter_field_labels[]  = { "From", "To", "Subject", "From, To or Subject", NULL };
const char * const courier_filter_ops[]           = { "contains", "not-contains", "equals", "starts-with", "ends-with", NULL };
const char * const courier_filter_op_labels[]     = { "contains", "doesn't contain", "is", "starts with", "ends with", NULL };
const char * const courier_filter_actions[]       = { "move", "read", "star", "trash", NULL };
const char * const courier_filter_action_labels[] = { "Move to folder", "Mark as read", "Star", "Move to Trash", NULL };

guint
courier_filter_index_of (const char * const *keys, const char *key)
{
  for (guint i = 0; keys[i]; i++)
    if (g_strcmp0 (keys[i], key) == 0)
      return i;
  return 0;
}

static gboolean
match_one (const char *op, const char *haystack, const char *needle)
{
  g_autofree char *h = g_utf8_casefold (haystack ? haystack : "", -1);
  g_autofree char *n = g_utf8_casefold (needle ? needle : "", -1);
  g_strstrip (n);

  if (g_str_equal (op, "contains"))     return strstr (h, n) != NULL;
  if (g_str_equal (op, "not-contains")) return strstr (h, n) == NULL;
  if (g_str_equal (op, "equals"))       return g_str_equal (g_strstrip (h), n);
  if (g_str_equal (op, "starts-with"))  return g_str_has_prefix (h, n);
  if (g_str_equal (op, "ends-with"))    return g_str_has_suffix (g_strstrip (h), n);
  return FALSE;
}

gboolean
courier_filter_matches (const CourierFilter *f, CourierMessage *m)
{
  if (!f->enabled || !f->value || !*f->value || !f->field || !f->op)
    return FALSE;

  g_autofree char *from = g_strdup_printf ("%s <%s>", m->sender_name ? m->sender_name : "",
                                           m->sender_addr ? m->sender_addr : "");
  const char *to = m->to ? m->to : (m->recipient ? m->recipient : "");

  if (g_str_equal (f->field, "from"))
    /* "equals"/"ends-with" should work on a bare address too */
    return match_one (f->op, from, f->value) ||
           (!g_str_equal (f->op, "not-contains") && match_one (f->op, m->sender_addr, f->value));
  if (g_str_equal (f->field, "to"))
    return match_one (f->op, to, f->value);
  if (g_str_equal (f->field, "subject"))
    return match_one (f->op, m->subject, f->value);
  if (g_str_equal (f->field, "any")) {
    g_autofree char *all = g_strjoin ("\n", from, to, m->subject ? m->subject : "", NULL);
    return match_one (f->op, all, f->value);
  }
  return FALSE;
}

gboolean
courier_filter_is_final (const CourierFilter *f)
{
  return g_strcmp0 (f->action, "move") == 0 || g_strcmp0 (f->action, "trash") == 0;
}

char *
courier_filter_describe (const CourierFilter *f)
{
  const char *field = courier_filter_field_labels[courier_filter_index_of (courier_filter_fields, f->field)];
  const char *op = courier_filter_op_labels[courier_filter_index_of (courier_filter_ops, f->op)];
  g_autofree char *action = NULL;
  if (g_strcmp0 (f->action, "move") == 0)
    action = g_strdup_printf ("move to “%s”", f->target ? f->target : "");
  else
    action = g_ascii_strdown (courier_filter_action_labels[courier_filter_index_of (courier_filter_actions, f->action)], -1);
  return g_strdup_printf ("If %s %s “%s” → %s", field, op, f->value ? f->value : "", action);
}

/* ---------------- mail log ---------------- */

CourierMailLogEntry *
courier_mail_log_from_message (CourierMessage *m, const char *account, const char *folder)
{
  CourierMailLogEntry *e = g_new0 (CourierMailLogEntry, 1);
  e->direction = g_strdup ("received");
  e->account = g_strdup (account);
  e->folder = g_strdup (folder);
  e->uid = g_strdup (m->uid);
  e->sender = m->sender_name && *m->sender_name
              ? g_strdup_printf ("%s <%s>", m->sender_name, m->sender_addr ? m->sender_addr : "")
              : g_strdup (m->sender_addr);
  e->recipients = g_strdup (m->to);
  e->subject = g_strdup (m->subject);
  e->date = m->date ? g_date_time_to_unix (m->date) : g_get_real_time () / G_USEC_PER_SEC;
  return e;
}

CourierMailLogEntry *
courier_mail_log_from_sent (GBytes *raw, const char *account, const char *eml_dir, GError **error)
{
  g_autoptr (CourierMessageDetail) d = courier_message_detail_parse ("", raw, error);
  if (!d)
    return NULL;

  CourierMailLogEntry *e = g_new0 (CourierMailLogEntry, 1);
  e->direction = g_strdup ("sent");
  e->account = g_strdup (account);
  e->message_id = g_strdup (d->message_id);
  e->sender = d->from_name && *d->from_name ? g_strdup_printf ("%s <%s>", d->from_name, d->from_addr ? d->from_addr : "")
                                            : g_strdup (d->from_addr);
  if (d->cc)
    e->recipients = g_strdup_printf ("%s, %s", d->to ? d->to : "", d->cc);
  else
    e->recipients = g_strdup (d->to);
  e->subject = g_strdup (d->subject);
  e->date = d->date ? g_date_time_to_unix (d->date) : g_get_real_time () / G_USEC_PER_SEC;

  g_autofree char *text = d->text ? g_strdup (d->text) : courier_html_to_text (d->html ? d->html : "");
  g_strstrip (text);
  if (g_utf8_strlen (text, -1) > 500) {
    char *cut = g_utf8_offset_to_pointer (text, 500);
    *cut = '\0';
  }
  e->preview = g_steal_pointer (&text);

  if (eml_dir) {
    g_mkdir_with_parents (eml_dir, 0700);
    g_autoptr (GDateTime) now = g_date_time_new_now_local ();
    g_autofree char *stamp = g_date_time_format (now, "%Y%m%d-%H%M%S");
    g_autofree char *rand = g_uuid_string_random ();
    rand[8] = '\0';
    g_autofree char *name = g_strdup_printf ("%s-%s.eml", stamp, rand);
    g_autofree char *path = g_build_filename (eml_dir, name, NULL);
    gsize len;
    const char *data = g_bytes_get_data (raw, &len);
    if (g_file_set_contents_full (path, data, len, G_FILE_SET_CONTENTS_CONSISTENT, 0600, NULL))
      e->eml_path = g_steal_pointer (&path);
  }
  return e;
}
