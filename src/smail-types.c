/* smail-types.c — accounts, folders, messages and MIME parsing */
#include "smail-types.h"

#include "smail-gmime.h"
#include <string.h>

/* ================= Account ================= */

SMailAccount *
smail_account_new (void)
{
  SMailAccount *a = g_new0 (SMailAccount, 1);
  a->imap_port = 993;
  a->smtp_port = 465;
  return a;
}

SMailAccount *
smail_account_copy (const SMailAccount *src)
{
  SMailAccount *a = g_new0 (SMailAccount, 1);
  a->id = g_strdup (src->id);
  a->name = g_strdup (src->name);
  a->email = g_strdup (src->email);
  a->username = g_strdup (src->username);
  a->password = g_strdup (src->password);
  a->imap_host = g_strdup (src->imap_host);
  a->imap_port = src->imap_port;
  a->imap_security = src->imap_security;
  a->smtp_host = g_strdup (src->smtp_host);
  a->smtp_port = src->smtp_port;
  a->smtp_security = src->smtp_security;
  return a;
}

void
smail_account_free (SMailAccount *a)
{
  if (!a)
    return;
  g_free (a->id);
  g_free (a->name);
  g_free (a->email);
  g_free (a->username);
  if (a->password)
    memset (a->password, 0, strlen (a->password));
  g_free (a->password);
  g_free (a->imap_host);
  g_free (a->smtp_host);
  g_free (a);
}

/* ================= Folder ================= */

static const char *role_order[] = {
  "inbox", "flagged", "drafts", "sent", "archive", "all", "junk", "trash", NULL
};

SMailFolder *
smail_folder_new (const char *path, const char *display_name, const char *role)
{
  SMailFolder *f = g_new0 (SMailFolder, 1);
  f->path = g_strdup (path);
  f->display_name = g_strdup (display_name);
  f->role = g_strdup (role);
  return f;
}

SMailFolder *
smail_folder_copy (const SMailFolder *src)
{
  SMailFolder *f = smail_folder_new (src->path, src->display_name, src->role);
  f->unread = src->unread;
  return f;
}

void
smail_folder_free (SMailFolder *f)
{
  if (!f)
    return;
  g_free (f->path);
  g_free (f->display_name);
  g_free (f->role);
  g_free (f);
}

const char *
smail_folder_get_icon (const SMailFolder *f)
{
  const char *r = f->role;
  if (!r)                          return "folder-symbolic";
  if (g_str_equal (r, "inbox"))    return "mail-unread-symbolic";
  if (g_str_equal (r, "flagged"))  return "starred-symbolic";
  if (g_str_equal (r, "drafts"))   return "document-edit-symbolic";
  if (g_str_equal (r, "sent"))     return "mail-send-symbolic";
  if (g_str_equal (r, "archive"))  return "folder-documents-symbolic";
  if (g_str_equal (r, "all"))      return "folder-documents-symbolic";
  if (g_str_equal (r, "junk"))     return "mail-mark-junk-symbolic";
  if (g_str_equal (r, "trash"))    return "user-trash-symbolic";
  return "folder-symbolic";
}

static int
role_rank (const char *role)
{
  if (role)
    for (int i = 0; role_order[i]; i++)
      if (g_str_equal (role, role_order[i]))
        return i;
  return G_N_ELEMENTS (role_order);
}

int
smail_folder_compare (gconstpointer pa, gconstpointer pb)
{
  const SMailFolder *a = *(SMailFolder **) pa;
  const SMailFolder *b = *(SMailFolder **) pb;
  int ra = role_rank (a->role), rb = role_rank (b->role);
  if (ra != rb)
    return ra - rb;
  return g_utf8_collate (a->display_name, b->display_name);
}

/* ================= Message summary ================= */

enum { PROP_0, PROP_SEEN, PROP_FLAGGED, N_PROPS };
static GParamSpec *props[N_PROPS];

G_DEFINE_FINAL_TYPE (SMailMessage, smail_message, G_TYPE_OBJECT)

static void
smail_message_finalize (GObject *object)
{
  SMailMessage *self = SMAIL_MESSAGE (object);
  g_free (self->uid);
  g_free (self->subject);
  g_free (self->sender_name);
  g_free (self->sender_addr);
  g_free (self->recipient);
  g_free (self->to);
  g_clear_pointer (&self->date, g_date_time_unref);
  G_OBJECT_CLASS (smail_message_parent_class)->finalize (object);
}

static void
smail_message_get_property (GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
  SMailMessage *self = SMAIL_MESSAGE (object);
  switch (id) {
  case PROP_SEEN:    g_value_set_boolean (value, self->seen); break;
  case PROP_FLAGGED: g_value_set_boolean (value, self->flagged); break;
  default: G_OBJECT_WARN_INVALID_PROPERTY_ID (object, id, pspec);
  }
}

static void
smail_message_set_property (GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
  SMailMessage *self = SMAIL_MESSAGE (object);
  switch (id) {
  case PROP_SEEN:    smail_message_set_seen (self, g_value_get_boolean (value)); break;
  case PROP_FLAGGED: smail_message_set_flagged (self, g_value_get_boolean (value)); break;
  default: G_OBJECT_WARN_INVALID_PROPERTY_ID (object, id, pspec);
  }
}

static void
smail_message_class_init (SMailMessageClass *klass)
{
  GObjectClass *oc = G_OBJECT_CLASS (klass);
  oc->finalize = smail_message_finalize;
  oc->get_property = smail_message_get_property;
  oc->set_property = smail_message_set_property;
  props[PROP_SEEN] = g_param_spec_boolean ("seen", NULL, NULL, FALSE,
                                           G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  props[PROP_FLAGGED] = g_param_spec_boolean ("flagged", NULL, NULL, FALSE,
                                              G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties (oc, N_PROPS, props);
}

static void
smail_message_init (SMailMessage *self)
{
}

void
smail_message_set_seen (SMailMessage *self, gboolean seen)
{
  if (self->seen == !!seen)
    return;
  self->seen = !!seen;
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_SEEN]);
}

void
smail_message_set_flagged (SMailMessage *self, gboolean flagged)
{
  if (self->flagged == !!flagged)
    return;
  self->flagged = !!flagged;
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_FLAGGED]);
}

const char *
smail_message_get_sender (SMailMessage *self)
{
  if (self->sender_name && *self->sender_name)
    return self->sender_name;
  if (self->sender_addr && *self->sender_addr)
    return self->sender_addr;
  return "Unknown Sender";
}

static GMimeMessage *
parse_mime (GBytes *bytes)
{
  gsize len;
  const char *data = g_bytes_get_data (bytes, &len);
  g_autoptr (GMimeStream) stream = g_mime_stream_mem_new_with_buffer (data, len);
  g_autoptr (GMimeParser) parser = g_mime_parser_new_with_stream (stream);
  return g_mime_parser_construct_message (parser, NULL);
}

static void
first_mailbox (InternetAddressList *list, char **name, char **addr)
{
  *name = *addr = NULL;
  if (!list || internet_address_list_length (list) == 0)
    return;
  InternetAddress *ia = internet_address_list_get_address (list, 0);
  *name = g_strdup (internet_address_get_name (ia));
  if (INTERNET_ADDRESS_IS_MAILBOX (ia))
    *addr = g_strdup (internet_address_mailbox_get_addr (INTERNET_ADDRESS_MAILBOX (ia)));
}

SMailMessage *
smail_message_new_from_headers (const char *uid, GBytes *headers, gboolean seen, gboolean flagged)
{
  SMailMessage *self = g_object_new (SMAIL_TYPE_MESSAGE, NULL);
  self->uid = g_strdup (uid);
  self->seen = seen;
  self->flagged = flagged;

  g_autoptr (GMimeMessage) msg = parse_mime (headers);
  if (msg) {
    self->subject = g_strdup (g_mime_message_get_subject (msg));
    first_mailbox (g_mime_message_get_from (msg), &self->sender_name, &self->sender_addr);
    g_autofree char *rname = NULL, *raddr = NULL;
    first_mailbox (g_mime_message_get_to (msg), &rname, &raddr);
    self->recipient = g_strdup (rname && *rname ? rname : raddr);
    InternetAddressList *to = g_mime_message_get_to (msg);
    if (to && internet_address_list_length (to))
      self->to = internet_address_list_to_string (to, NULL, FALSE);
    GDateTime *date = g_mime_message_get_date (msg);
    if (date)
      self->date = g_date_time_to_local (date);
    GMimeObject *body = g_mime_message_get_mime_part (msg);
    GMimeContentType *ct = body ? g_mime_object_get_content_type (body) : NULL;
    if (ct && g_mime_content_type_is_type (ct, "multipart", "mixed"))
      self->has_attachment = TRUE;
  }
  if (!self->subject || !*self->subject) {
    g_free (self->subject);
    self->subject = g_strdup ("(No Subject)");
  }
  return self;
}

/* ================= Full message ================= */

void
smail_attachment_free (SMailAttachment *a)
{
  if (!a)
    return;
  g_free (a->filename);
  g_free (a->content_type);
  g_free (a->content_id);
  g_clear_pointer (&a->data, g_bytes_unref);
  g_free (a);
}

void
smail_message_detail_free (SMailMessageDetail *d)
{
  if (!d)
    return;
  g_free (d->uid);
  g_free (d->subject);
  g_free (d->from_name);
  g_free (d->from_addr);
  g_free (d->to);
  g_free (d->cc);
  g_free (d->reply_to);
  g_clear_pointer (&d->date, g_date_time_unref);
  g_free (d->message_id);
  g_free (d->references);
  g_free (d->html);
  g_free (d->text);
  g_clear_pointer (&d->attachments, g_ptr_array_unref);
  g_clear_pointer (&d->inline_parts, g_ptr_array_unref);
  g_clear_pointer (&d->raw, g_bytes_unref);
  g_free (d);
}

static GBytes *
part_decoded_bytes (GMimePart *part)
{
  GMimeDataWrapper *content = g_mime_part_get_content (part);
  if (!content)
    return g_bytes_new (NULL, 0);
  g_autoptr (GMimeStream) mem = g_mime_stream_mem_new ();
  g_mime_data_wrapper_write_to_stream (content, mem);
  GByteArray *arr = g_mime_stream_mem_get_byte_array (GMIME_STREAM_MEM (mem));
  return g_bytes_new (arr->data, arr->len);
}

static void
collect_part (GMimeObject *parent, GMimeObject *part, gpointer user_data)
{
  SMailMessageDetail *d = user_data;

  if (GMIME_IS_MESSAGE_PART (part)) {
    /* forwarded message attached as message/rfc822: offer it as a .eml */
    GMimeMessage *inner = g_mime_message_part_get_message (GMIME_MESSAGE_PART (part));
    if (inner) {
      g_autoptr (GMimeStream) mem = g_mime_stream_mem_new ();
      g_mime_object_write_to_stream (GMIME_OBJECT (inner), NULL, mem);
      GByteArray *arr = g_mime_stream_mem_get_byte_array (GMIME_STREAM_MEM (mem));
      SMailAttachment *a = g_new0 (SMailAttachment, 1);
      const char *subj = g_mime_message_get_subject (inner);
      a->filename = g_strdup_printf ("%s.eml", subj && *subj ? subj : "message");
      a->content_type = g_strdup ("message/rfc822");
      a->data = g_bytes_new (arr->data, arr->len);
      g_ptr_array_add (d->attachments, a);
    }
    return;
  }

  if (!GMIME_IS_PART (part))
    return;

  GMimePart *mp = GMIME_PART (part);
  GMimeContentType *ct = g_mime_object_get_content_type (part);
  const char *disposition = g_mime_object_get_disposition (part);
  const char *filename = g_mime_part_get_filename (mp);
  gboolean is_attachment = g_mime_part_is_attachment (mp) ||
                           (filename && !g_mime_content_type_is_type (ct, "text", "*"));

  if (!is_attachment && GMIME_IS_TEXT_PART (part)) {
    if (g_mime_content_type_is_type (ct, "text", "html") && !d->html) {
      d->html = g_mime_text_part_get_text (GMIME_TEXT_PART (part));
      return;
    }
    if (g_mime_content_type_is_type (ct, "text", "plain") && !d->text) {
      d->text = g_mime_text_part_get_text (GMIME_TEXT_PART (part));
      return;
    }
  }

  SMailAttachment *a = g_new0 (SMailAttachment, 1);
  a->content_type = g_mime_content_type_get_mime_type (ct);
  a->filename = g_strdup (filename ? filename : "attachment");
  a->content_id = g_strdup (g_mime_part_get_content_id (mp));
  a->data = part_decoded_bytes (mp);
  a->is_inline = a->content_id && (!disposition || g_ascii_strcasecmp (disposition, "inline") == 0);

  if (a->is_inline)
    g_ptr_array_add (d->inline_parts, a);
  else
    g_ptr_array_add (d->attachments, a);
}

SMailMessageDetail *
smail_message_detail_parse (const char *uid, GBytes *raw, GError **error)
{
  g_autoptr (GMimeMessage) msg = parse_mime (raw);
  if (!msg) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Could not parse message");
    return NULL;
  }

  SMailMessageDetail *d = g_new0 (SMailMessageDetail, 1);
  d->uid = g_strdup (uid);
  d->raw = g_bytes_ref (raw);
  d->attachments = g_ptr_array_new_with_free_func ((GDestroyNotify) smail_attachment_free);
  d->inline_parts = g_ptr_array_new_with_free_func ((GDestroyNotify) smail_attachment_free);

  d->subject = g_strdup (g_mime_message_get_subject (msg));
  if (!d->subject || !*d->subject) {
    g_free (d->subject);
    d->subject = g_strdup ("(No Subject)");
  }
  first_mailbox (g_mime_message_get_from (msg), &d->from_name, &d->from_addr);

  InternetAddressList *to = g_mime_message_get_to (msg);
  InternetAddressList *cc = g_mime_message_get_cc (msg);
  InternetAddressList *rt = g_mime_message_get_reply_to (msg);
  if (to && internet_address_list_length (to))
    d->to = internet_address_list_to_string (to, NULL, FALSE);
  if (cc && internet_address_list_length (cc))
    d->cc = internet_address_list_to_string (cc, NULL, FALSE);
  if (rt && internet_address_list_length (rt))
    d->reply_to = internet_address_list_to_string (rt, NULL, FALSE);

  GDateTime *date = g_mime_message_get_date (msg);
  if (date)
    d->date = g_date_time_to_local (date);
  d->message_id = g_strdup (g_mime_message_get_message_id (msg));
  d->references = g_strdup (g_mime_object_get_header (GMIME_OBJECT (msg), "References"));

  g_mime_message_foreach (msg, collect_part, d);

  /* A single-part message is not visited by foreach */
  GMimeObject *body = g_mime_message_get_mime_part (msg);
  if (body && GMIME_IS_PART (body) && !d->html && !d->text && d->attachments->len == 0)
    collect_part (NULL, body, d);

  return d;
}
