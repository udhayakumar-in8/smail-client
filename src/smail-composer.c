/* smail-composer.c — compose and send plain-text mail with attachments */
#include "smail-composer.h"
#include "smail-util.h"
#include "smail-address-completion.h"

#include "smail-gmime.h"
#include <string.h>

typedef struct {
  char   *filename;
  char   *content_type;
  GBytes *data;
} ComposeAttachment;

struct _SMailComposer {
  AdwWindow       parent_instance;

  SMailBackend *backend;
  GtkWidget      *title;
  GtkWidget      *to_entry;
  GtkWidget      *cc_entry;
  GtkWidget      *bcc_entry;
  GtkWidget      *cc_revealer;
  GtkWidget      *subject_entry;
  GtkWidget      *text_view;
  GtkWidget      *attach_box;
  GtkWidget      *send_button;
  GtkWidget      *toast_overlay;

  GPtrArray      *attachments; /* ComposeAttachment* */
  char           *in_reply_to;
  char           *references;
  gboolean        dirty;
  gboolean        sending;
  gboolean        force_close;
  GBytes         *outgoing;    /* the message being sent */

  SMailStore     *store;       /* address book, may be NULL */
  char           *message_id;  /* stable across draft saves and the final send */
  gboolean        has_draft;   /* a copy of this message is in the Drafts folder */
  gboolean        saving;
  gboolean        close_after_save;
  GtkWidget      *draft_button;
};

G_DEFINE_FINAL_TYPE (SMailComposer, smail_composer, ADW_TYPE_WINDOW)

enum { SIGNAL_SENT, SIGNAL_DRAFT_SAVED, N_SIGNALS };
static guint signals[N_SIGNALS];

static void
compose_attachment_free (ComposeAttachment *a)
{
  g_free (a->filename);
  g_free (a->content_type);
  g_bytes_unref (a->data);
  g_free (a);
}

static void
show_error (SMailComposer *self, const char *heading, const char *body)
{
  AdwDialog *alert = adw_alert_dialog_new (heading, body);
  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (alert), "ok", "OK");
  adw_dialog_present (alert, GTK_WIDGET (self));
}

/* ---------------- attachments ---------------- */

static void rebuild_attachments (SMailComposer *self);

static void
on_remove_attachment (GtkButton *button, SMailComposer *self)
{
  ComposeAttachment *a = g_object_get_data (G_OBJECT (button), "attachment");
  g_ptr_array_remove (self->attachments, a);
  rebuild_attachments (self);
}

static void
rebuild_attachments (SMailComposer *self)
{
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (self->attach_box)))
    gtk_flow_box_remove (GTK_FLOW_BOX (self->attach_box), child);

  for (guint i = 0; i < self->attachments->len; i++) {
    ComposeAttachment *a = g_ptr_array_index (self->attachments, i);
    g_autofree char *size = g_format_size (g_bytes_get_size (a->data));
    g_autofree char *text = g_strdup_printf ("%s  (%s)", a->filename, size);

    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class (box, "attachment-chip");
    gtk_box_append (GTK_BOX (box), gtk_image_new_from_icon_name ("mail-attachment-symbolic"));
    GtkWidget *label = gtk_label_new (text);
    gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars (GTK_LABEL (label), 30);
    gtk_box_append (GTK_BOX (box), label);
    GtkWidget *remove = gtk_button_new_from_icon_name ("window-close-symbolic");
    gtk_widget_add_css_class (remove, "flat");
    gtk_widget_add_css_class (remove, "circular");
    gtk_widget_set_tooltip_text (remove, "Remove attachment");
    g_object_set_data (G_OBJECT (remove), "attachment", a);
    g_signal_connect (remove, "clicked", G_CALLBACK (on_remove_attachment), self);
    gtk_box_append (GTK_BOX (box), remove);
    gtk_flow_box_append (GTK_FLOW_BOX (self->attach_box), box);
  }
  gtk_widget_set_visible (self->attach_box, self->attachments->len > 0);
}

static void
add_attachment (SMailComposer *self, const char *filename, const char *content_type, GBytes *data)
{
  ComposeAttachment *a = g_new0 (ComposeAttachment, 1);
  a->filename = g_strdup (filename);
  a->content_type = g_strdup (content_type ? content_type : "application/octet-stream");
  a->data = g_bytes_ref (data);
  g_ptr_array_add (self->attachments, a);
  self->dirty = TRUE;
}

static void
on_files_chosen (GObject *source, GAsyncResult *res, gpointer data)
{
  SMailComposer *self = data;
  g_autoptr (GListModel) files = gtk_file_dialog_open_multiple_finish (GTK_FILE_DIALOG (source), res, NULL);
  if (files) {
    for (guint i = 0; i < g_list_model_get_n_items (files); i++) {
      g_autoptr (GFile) file = g_list_model_get_item (files, i);
      g_autoptr (GError) error = NULL;
      g_autoptr (GBytes) bytes = g_file_load_bytes (file, NULL, NULL, &error);
      if (!bytes) {
        show_error (self, "Could not attach file", error->message);
        continue;
      }
      g_autofree char *name = g_file_get_basename (file);
      g_autofree char *ctype = g_content_type_guess (name, g_bytes_get_data (bytes, NULL),
                                                     MIN (g_bytes_get_size (bytes), 4096), NULL);
      g_autofree char *mime = g_content_type_get_mime_type (ctype);
      add_attachment (self, name, mime, bytes);
    }
    rebuild_attachments (self);
  }
  g_object_unref (self);
}

static void
on_attach_clicked (GtkButton *button, SMailComposer *self)
{
  g_autoptr (GtkFileDialog) dialog = gtk_file_dialog_new ();
  gtk_file_dialog_set_title (dialog, "Attach Files");
  gtk_file_dialog_open_multiple (dialog, GTK_WINDOW (self), NULL, on_files_chosen, g_object_ref (self));
}

/* ---------------- sending ---------------- */

static char *
entry_text (GtkWidget *entry)
{
  return g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (entry))));
}

/* Parse a comma separated address field; collect bare addresses into `rcpts`. */
static InternetAddressList *
parse_addresses (const char *text, GPtrArray *rcpts, gboolean *ok)
{
  *ok = TRUE;
  if (!text || !*text)
    return NULL;
  InternetAddressList *list = internet_address_list_parse (NULL, text);
  if (!list || internet_address_list_length (list) == 0) {
    *ok = FALSE;
    g_clear_object (&list);
    return NULL;
  }
  for (int i = 0; i < internet_address_list_length (list); i++) {
    InternetAddress *ia = internet_address_list_get_address (list, i);
    if (INTERNET_ADDRESS_IS_MAILBOX (ia)) {
      const char *addr = internet_address_mailbox_get_addr (INTERNET_ADDRESS_MAILBOX (ia));
      if (!addr || !strchr (addr, '@'))
        *ok = FALSE;
      else
        g_ptr_array_add (rcpts, g_strdup (addr));
    } else if (INTERNET_ADDRESS_IS_GROUP (ia)) {
      InternetAddressList *members = internet_address_group_get_members (INTERNET_ADDRESS_GROUP (ia));
      for (int j = 0; j < internet_address_list_length (members); j++) {
        InternetAddress *m = internet_address_list_get_address (members, j);
        if (INTERNET_ADDRESS_IS_MAILBOX (m))
          g_ptr_array_add (rcpts, g_strdup (internet_address_mailbox_get_addr (INTERNET_ADDRESS_MAILBOX (m))));
      }
    }
  }
  return list;
}

static GBytes *
build_message (SMailComposer *self, GPtrArray *rcpts, gboolean draft, GError **error)
{
  const SMailAccount *account = smail_backend_get_account (self->backend);
  g_autofree char *to = entry_text (self->to_entry);
  g_autofree char *cc = entry_text (self->cc_entry);
  g_autofree char *bcc = entry_text (self->bcc_entry);
  g_autofree char *subject = entry_text (self->subject_entry);

  gboolean ok_to, ok_cc, ok_bcc;
  g_autoptr (InternetAddressList) to_list = parse_addresses (to, rcpts, &ok_to);
  g_autoptr (InternetAddressList) cc_list = parse_addresses (cc, rcpts, &ok_cc);
  g_autoptr (InternetAddressList) bcc_list = parse_addresses (bcc, rcpts, &ok_bcc); /* envelope only */
  if (!draft && (!ok_to || !ok_cc || !ok_bcc)) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                 "One of the recipient addresses is not valid. Separate addresses with commas.");
    return NULL;
  }
  if (!draft && rcpts->len == 0) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Add at least one recipient.");
    return NULL;
  }

  g_autoptr (GMimeMessage) msg = g_mime_message_new (TRUE);
  g_mime_message_add_mailbox (msg, GMIME_ADDRESS_TYPE_FROM, account->name, account->email);
  if (to_list)
    internet_address_list_append (g_mime_message_get_addresses (msg, GMIME_ADDRESS_TYPE_TO), to_list);
  if (cc_list)
    internet_address_list_append (g_mime_message_get_addresses (msg, GMIME_ADDRESS_TYPE_CC), cc_list);
  if (draft && bcc_list) /* drafts remember Bcc; sent mail never shows it */
    internet_address_list_append (g_mime_message_get_addresses (msg, GMIME_ADDRESS_TYPE_BCC), bcc_list);
  g_mime_message_set_subject (msg, subject, "utf-8");

  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  g_mime_message_set_date (msg, now);
  const char *at = account->email ? strchr (account->email, '@') : NULL;
  if (!self->message_id)
    self->message_id = g_mime_utils_generate_message_id (at ? at + 1 : "smail.local");
  g_mime_message_set_message_id (msg, self->message_id);
  if (self->in_reply_to)
    g_mime_object_set_header (GMIME_OBJECT (msg), "In-Reply-To", self->in_reply_to, NULL);
  if (self->references)
    g_mime_object_set_header (GMIME_OBJECT (msg), "References", self->references, NULL);
  g_mime_object_set_header (GMIME_OBJECT (msg), "User-Agent", "SMail/" PACKAGE_VERSION, NULL);

  /* body */
  GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (self->text_view));
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds (buffer, &start, &end);
  g_autofree char *body = gtk_text_buffer_get_text (buffer, &start, &end, FALSE);

  GMimeTextPart *text = g_mime_text_part_new_with_subtype ("plain");
  g_mime_text_part_set_text (text, body);
  g_mime_part_set_content_encoding (GMIME_PART (text),
    g_mime_part_get_best_content_encoding (GMIME_PART (text), GMIME_ENCODING_CONSTRAINT_7BIT));

  if (self->attachments->len == 0) {
    g_mime_message_set_mime_part (msg, GMIME_OBJECT (text));
  } else {
    GMimeMultipart *mixed = g_mime_multipart_new_with_subtype ("mixed");
    g_mime_multipart_add (mixed, GMIME_OBJECT (text));
    for (guint i = 0; i < self->attachments->len; i++) {
      ComposeAttachment *a = g_ptr_array_index (self->attachments, i);
      g_auto (GStrv) type = g_strsplit (a->content_type, "/", 2);
      GMimePart *part = g_mime_part_new_with_type (type[0], type[1] ? type[1] : "octet-stream");
      gsize len;
      const char *data = g_bytes_get_data (a->data, &len);
      g_autoptr (GMimeStream) stream = g_mime_stream_mem_new_with_buffer (data, len);
      g_autoptr (GMimeDataWrapper) wrapper = g_mime_data_wrapper_new_with_stream (stream, GMIME_CONTENT_ENCODING_DEFAULT);
      g_mime_part_set_content (part, wrapper);
      g_mime_part_set_filename (part, a->filename);
      g_mime_object_set_disposition (GMIME_OBJECT (part), GMIME_DISPOSITION_ATTACHMENT);
      g_mime_part_set_content_encoding (part, GMIME_CONTENT_ENCODING_BASE64);
      g_mime_multipart_add (mixed, GMIME_OBJECT (part));
      g_object_unref (part);
    }
    g_mime_message_set_mime_part (msg, GMIME_OBJECT (mixed));
    g_object_unref (mixed);
  }
  g_object_unref (text);

  g_autoptr (GMimeStream) out = g_mime_stream_mem_new ();
  GMimeFormatOptions *opts = g_mime_format_options_clone (NULL);
  g_mime_format_options_set_newline_format (opts, GMIME_NEWLINE_FORMAT_DOS);
  g_mime_object_write_to_stream (GMIME_OBJECT (msg), opts, out);
  g_mime_format_options_free (opts);
  GByteArray *arr = g_mime_stream_mem_get_byte_array (GMIME_STREAM_MEM (out));
  return g_bytes_new (arr->data, arr->len);
}

static void
set_sending (SMailComposer *self, gboolean sending)
{
  self->sending = sending;
  gtk_widget_set_sensitive (self->send_button, !sending);
  gtk_widget_set_sensitive (self->to_entry, !sending);
  gtk_widget_set_sensitive (self->subject_entry, !sending);
  gtk_text_view_set_editable (GTK_TEXT_VIEW (self->text_view), !sending);
  adw_button_content_set_label (ADW_BUTTON_CONTENT (gtk_button_get_child (GTK_BUTTON (self->send_button))),
                                sending ? "Sending…" : "Send");
}

static void
on_sent (GObject *source, GAsyncResult *res, gpointer data)
{
  g_autoptr (SMailComposer) self = data;
  g_autoptr (GError) error = NULL;
  if (!smail_backend_finish_boolean (SMAIL_BACKEND (source), res, &error)) {
    set_sending (self, FALSE);
    show_error (self, "Message Not Sent", error->message);
    return;
  }
  g_signal_emit (self, signals[SIGNAL_SENT], 0, self->outgoing);
  const SMailFolder *drafts = smail_backend_find_role (self->backend, "drafts");
  if (self->has_draft && drafts && self->message_id)
    smail_backend_save_draft (self->backend, drafts->path, self->message_id, NULL, NULL, NULL, NULL);
  self->force_close = TRUE;
  gtk_window_close (GTK_WINDOW (self));
}

static void
send_message (SMailComposer *self)
{
  if (self->sending)
    return;
  g_autoptr (GPtrArray) rcpts = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) message = build_message (self, rcpts, FALSE, &error);
  if (!message) {
    show_error (self, "Can't Send Message", error->message);
    return;
  }
  g_ptr_array_add (rcpts, NULL);
  g_clear_pointer (&self->outgoing, g_bytes_unref);
  self->outgoing = g_bytes_ref (message);
  set_sending (self, TRUE);
  smail_backend_send (self->backend, message, smail_backend_get_account (self->backend)->email,
                        (const char * const *) rcpts->pdata, on_sent, g_object_ref (self));
}

static void
on_send_clicked (GtkButton *button, SMailComposer *self)
{
  send_message (self);
}

static void
send_action (GtkWidget *widget, const char *name, GVariant *param)
{
  send_message (SMAIL_COMPOSER (widget));
}

/* ---------------- drafts ---------------- */

static void
toast (SMailComposer *self, const char *text)
{
  adw_toast_overlay_add_toast (ADW_TOAST_OVERLAY (self->toast_overlay), adw_toast_new (text));
}

static void
on_draft_saved (GObject *source, GAsyncResult *res, gpointer data)
{
  g_autoptr (SMailComposer) self = data;
  g_autoptr (GError) error = NULL;
  self->saving = FALSE;
  gtk_widget_set_sensitive (self->draft_button, TRUE);
  if (!smail_backend_finish_boolean (SMAIL_BACKEND (source), res, &error)) {
    self->close_after_save = FALSE;
    show_error (self, "Draft Not Saved", error->message);
    return;
  }
  self->has_draft = TRUE;
  self->dirty = FALSE;
  g_signal_emit (self, signals[SIGNAL_DRAFT_SAVED], 0);
  if (self->close_after_save) {
    self->force_close = TRUE;
    gtk_window_close (GTK_WINDOW (self));
  } else {
    toast (self, "Draft saved");
  }
}

static void
save_draft (SMailComposer *self)
{
  if (self->saving || self->sending)
    return;
  const SMailFolder *drafts = smail_backend_find_role (self->backend, "drafts");
  if (!drafts) {
    self->close_after_save = FALSE;
    show_error (self, "Draft Not Saved", "This account has no Drafts folder.");
    return;
  }
  g_autoptr (GPtrArray) rcpts = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GBytes) message = build_message (self, rcpts, TRUE, NULL);
  self->saving = TRUE;
  gtk_widget_set_sensitive (self->draft_button, FALSE);
  /* replaces the previous copy of this draft (same Message-ID) */
  smail_backend_save_draft (self->backend, drafts->path, self->message_id, message, "\\Draft \\Seen",
                            on_draft_saved, g_object_ref (self));
}

static void
on_save_draft_clicked (GtkButton *b, SMailComposer *self)
{
  save_draft (self);
}

static void
save_draft_action (GtkWidget *widget, const char *name, GVariant *param)
{
  save_draft (SMAIL_COMPOSER (widget));
}

/* ---------------- closing ---------------- */

static void
on_discard_response (AdwAlertDialog *dialog, const char *response, SMailComposer *self)
{
  if (g_str_equal (response, "discard")) {
    self->force_close = TRUE;
    gtk_window_close (GTK_WINDOW (self));
  } else if (g_str_equal (response, "save")) {
    self->close_after_save = TRUE;
    save_draft (self);
  }
}

static gboolean
smail_composer_close_request (GtkWindow *window)
{
  SMailComposer *self = SMAIL_COMPOSER (window);
  if (self->force_close || !self->dirty)
    return GTK_WINDOW_CLASS (smail_composer_parent_class)->close_request (window);
  if (self->sending || self->saving)
    return TRUE;

  gboolean can_save = smail_backend_find_role (self->backend, "drafts") != NULL;
  AdwDialog *alert = adw_alert_dialog_new ("Save Draft?",
                                           can_save ? "Save this message to Drafts to finish it later?"
                                                    : "The message you wrote will be lost.");
  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (alert), "cancel", "_Keep Editing", "discard", "_Discard", NULL);
  if (can_save) {
    adw_alert_dialog_add_response (ADW_ALERT_DIALOG (alert), "save", "_Save Draft");
    adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (alert), "save", ADW_RESPONSE_SUGGESTED);
  }
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (alert), "discard", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (alert), can_save ? "save" : "cancel");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (alert), "cancel");
  g_signal_connect (alert, "response", G_CALLBACK (on_discard_response), self);
  adw_dialog_present (alert, GTK_WIDGET (self));
  return TRUE;
}

static void
mark_dirty (SMailComposer *self)
{
  self->dirty = TRUE;
}

/* ---------------- construction ---------------- */

static void
smail_composer_finalize (GObject *object)
{
  SMailComposer *self = SMAIL_COMPOSER (object);
  g_clear_object (&self->backend);
  g_clear_pointer (&self->outgoing, g_bytes_unref);
  g_free (self->message_id);
  g_ptr_array_unref (self->attachments);
  g_free (self->in_reply_to);
  g_free (self->references);
  G_OBJECT_CLASS (smail_composer_parent_class)->finalize (object);
}

static void
smail_composer_class_init (SMailComposerClass *klass)
{
  GtkWidgetClass *wc = GTK_WIDGET_CLASS (klass);
  G_OBJECT_CLASS (klass)->finalize = smail_composer_finalize;
  GTK_WINDOW_CLASS (klass)->close_request = smail_composer_close_request;

  signals[SIGNAL_DRAFT_SAVED] = g_signal_new ("draft-saved", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                                              0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  gtk_widget_class_install_action (wc, "composer.save-draft", NULL, save_draft_action);
  gtk_widget_class_add_binding_action (wc, GDK_KEY_s, GDK_CONTROL_MASK, "composer.save-draft", NULL);
  /* "sent" (GBytes *message): the complete message as it was handed to the server */
  signals[SIGNAL_SENT] = g_signal_new ("sent", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                                       0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_BYTES);

  gtk_widget_class_install_action (wc, "composer.send", NULL, send_action);
  gtk_widget_class_add_binding_action (wc, GDK_KEY_Escape, 0, "window.close", NULL);
}

static GtkWidget *
field_row (const char *label_text, GtkWidget **entry_out, GtkWidget *extra)
{
  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class (row, "composer-field");
  GtkWidget *label = gtk_label_new (label_text);
  gtk_label_set_xalign (GTK_LABEL (label), 1);
  gtk_label_set_width_chars (GTK_LABEL (label), 8);
  gtk_widget_add_css_class (label, "dim-label");
  gtk_box_append (GTK_BOX (row), label);

  GtkWidget *entry = gtk_text_new ();
  gtk_widget_set_hexpand (entry, TRUE);
  gtk_box_append (GTK_BOX (row), entry);
  gtk_accessible_update_property (GTK_ACCESSIBLE (entry), GTK_ACCESSIBLE_PROPERTY_LABEL, label_text, -1);
  if (extra)
    gtk_box_append (GTK_BOX (row), extra);
  *entry_out = entry;
  return row;
}

static void
smail_composer_init (SMailComposer *self)
{
  self->attachments = g_ptr_array_new_with_free_func ((GDestroyNotify) compose_attachment_free);

  gtk_window_set_default_size (GTK_WINDOW (self), 720, 640);
  gtk_window_set_title (GTK_WINDOW (self), "New Message");

  GtkWidget *toolbar = adw_toolbar_view_new ();
  GtkWidget *header = adw_header_bar_new ();
  self->title = adw_window_title_new ("New Message", NULL);
  adw_header_bar_set_title_widget (ADW_HEADER_BAR (header), self->title);

  GtkWidget *attach = gtk_button_new_from_icon_name ("mail-attachment-symbolic");
  gtk_widget_set_tooltip_text (attach, "Attach Files");
  g_signal_connect (attach, "clicked", G_CALLBACK (on_attach_clicked), self);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), attach);
  self->draft_button = gtk_button_new_from_icon_name ("document-save-symbolic");
  gtk_widget_set_tooltip_text (self->draft_button, "Save Draft (Ctrl+S)");
  g_signal_connect (self->draft_button, "clicked", G_CALLBACK (on_save_draft_clicked), self);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), self->draft_button);

  self->send_button = gtk_button_new ();
  GtkWidget *content = adw_button_content_new ();
  adw_button_content_set_icon_name (ADW_BUTTON_CONTENT (content), "mail-send-symbolic");
  adw_button_content_set_label (ADW_BUTTON_CONTENT (content), "Send");
  gtk_button_set_child (GTK_BUTTON (self->send_button), content);
  gtk_widget_add_css_class (self->send_button, "suggested-action");
  gtk_widget_set_tooltip_text (self->send_button, "Send (Ctrl+Enter)");
  g_signal_connect (self->send_button, "clicked", G_CALLBACK (on_send_clicked), self);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), self->send_button);
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), header);

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  GtkWidget *cc_toggle = gtk_toggle_button_new_with_label ("Cc/Bcc");
  gtk_widget_add_css_class (cc_toggle, "flat");
  gtk_box_append (GTK_BOX (box), field_row ("To", &self->to_entry, cc_toggle));
  gtk_box_append (GTK_BOX (box), gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));

  GtkWidget *ccbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (ccbox), field_row ("Cc", &self->cc_entry, NULL));
  gtk_box_append (GTK_BOX (ccbox), gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));
  gtk_box_append (GTK_BOX (ccbox), field_row ("Bcc", &self->bcc_entry, NULL));
  gtk_box_append (GTK_BOX (ccbox), gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));
  self->cc_revealer = gtk_revealer_new ();
  gtk_revealer_set_child (GTK_REVEALER (self->cc_revealer), ccbox);
  g_object_bind_property (cc_toggle, "active", self->cc_revealer, "reveal-child", G_BINDING_BIDIRECTIONAL);
  gtk_box_append (GTK_BOX (box), self->cc_revealer);

  gtk_box_append (GTK_BOX (box), field_row ("Subject", &self->subject_entry, NULL));
  gtk_box_append (GTK_BOX (box), gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));

  self->attach_box = gtk_flow_box_new ();
  gtk_flow_box_set_selection_mode (GTK_FLOW_BOX (self->attach_box), GTK_SELECTION_NONE);
  gtk_widget_add_css_class (self->attach_box, "composer-attachments");
  gtk_widget_set_visible (self->attach_box, FALSE);
  gtk_box_append (GTK_BOX (box), self->attach_box);

  self->text_view = gtk_text_view_new ();
  gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (self->text_view), GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_left_margin (GTK_TEXT_VIEW (self->text_view), 18);
  gtk_text_view_set_right_margin (GTK_TEXT_VIEW (self->text_view), 18);
  gtk_text_view_set_top_margin (GTK_TEXT_VIEW (self->text_view), 18);
  gtk_text_view_set_bottom_margin (GTK_TEXT_VIEW (self->text_view), 18);
  gtk_widget_add_css_class (self->text_view, "composer-body");
  GtkWidget *scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), self->text_view);
  gtk_widget_set_vexpand (scroll, TRUE);
  gtk_box_append (GTK_BOX (box), scroll);

  self->toast_overlay = adw_toast_overlay_new ();
  adw_toast_overlay_set_child (ADW_TOAST_OVERLAY (self->toast_overlay), box);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), self->toast_overlay);
  adw_window_set_content (ADW_WINDOW (self), toolbar);

  /* Ctrl+Enter must win over the text view's own Return handling */
  GtkEventController *shortcuts = gtk_shortcut_controller_new ();
  gtk_event_controller_set_propagation_phase (shortcuts, GTK_PHASE_CAPTURE);
  gtk_shortcut_controller_add_shortcut (GTK_SHORTCUT_CONTROLLER (shortcuts),
    gtk_shortcut_new (gtk_alternative_trigger_new (gtk_keyval_trigger_new (GDK_KEY_Return, GDK_CONTROL_MASK),
                                                   gtk_keyval_trigger_new (GDK_KEY_KP_Enter, GDK_CONTROL_MASK)),
                      gtk_named_action_new ("composer.send")));
  gtk_widget_add_controller (GTK_WIDGET (self), shortcuts);

  g_signal_connect_swapped (self->to_entry, "changed", G_CALLBACK (mark_dirty), self);
  g_signal_connect_swapped (self->subject_entry, "changed", G_CALLBACK (mark_dirty), self);
  g_signal_connect_swapped (self->cc_entry, "changed", G_CALLBACK (mark_dirty), self);
  g_signal_connect_swapped (self->bcc_entry, "changed", G_CALLBACK (mark_dirty), self);
  g_signal_connect_swapped (gtk_text_view_get_buffer (GTK_TEXT_VIEW (self->text_view)), "changed",
                            G_CALLBACK (mark_dirty), self);
}

/* ---------------- prefilling replies & forwards ---------------- */

static gboolean
has_prefix_ci (const char *s, const char *prefix)
{
  return g_ascii_strncasecmp (s, prefix, strlen (prefix)) == 0;
}

/* Append addresses from `src` to `dst`, skipping `self_addr` and duplicates. */
static void
merge_addresses (GString *dst, const char *src, const char *self_addr, GHashTable *seen)
{
  if (!src)
    return;
  g_autoptr (InternetAddressList) list = internet_address_list_parse (NULL, src);
  if (!list)
    return;
  for (int i = 0; i < internet_address_list_length (list); i++) {
    InternetAddress *ia = internet_address_list_get_address (list, i);
    if (!INTERNET_ADDRESS_IS_MAILBOX (ia))
      continue;
    const char *addr = internet_address_mailbox_get_addr (INTERNET_ADDRESS_MAILBOX (ia));
    g_autofree char *key = g_ascii_strdown (addr, -1);
    if ((self_addr && g_ascii_strcasecmp (addr, self_addr) == 0) || g_hash_table_contains (seen, key))
      continue;
    g_hash_table_add (seen, g_steal_pointer (&key));
    g_autofree char *str = internet_address_to_string (ia, NULL, FALSE);
    g_string_append_printf (dst, "%s%s", dst->len ? ", " : "", str);
  }
}

/* "\n\n-- \nsignature", or "" */
static char *
signature_block (const SMailAccount *account)
{
  if (!account->signature || !*account->signature)
    return g_strdup ("");
  return g_strdup_printf ("\n\n-- \n%s", account->signature);
}

static void
prefill_draft (SMailComposer *self, const SMailMessageDetail *d)
{
  GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (self->text_view));
  gtk_editable_set_text (GTK_EDITABLE (self->to_entry), d->to ? d->to : "");
  gtk_editable_set_text (GTK_EDITABLE (self->cc_entry), d->cc ? d->cc : "");
  gtk_editable_set_text (GTK_EDITABLE (self->bcc_entry), d->bcc ? d->bcc : "");
  if (d->cc || d->bcc)
    gtk_revealer_set_reveal_child (GTK_REVEALER (self->cc_revealer), TRUE);
  gtk_editable_set_text (GTK_EDITABLE (self->subject_entry),
                         g_strcmp0 (d->subject, "(No Subject)") == 0 ? "" : d->subject);
  g_autofree char *text = d->text ? g_strdup (d->text) : smail_html_to_text (d->html ? d->html : "");
  gtk_text_buffer_set_text (buffer, text, -1);
  for (guint i = 0; i < d->attachments->len; i++) {
    SMailAttachment *a = g_ptr_array_index (d->attachments, i);
    add_attachment (self, a->filename, a->content_type, a->data);
  }
  rebuild_attachments (self);
  self->in_reply_to = g_strdup (d->in_reply_to);
  self->references = g_strdup (d->references);
  /* keep the draft's Message-ID so saving or sending replaces this draft */
  self->message_id = g_strdup (d->message_id);
  self->has_draft = d->message_id != NULL;
  gtk_widget_grab_focus (d->to ? self->text_view : self->to_entry);
}

static void
prefill (SMailComposer *self, SMailComposeMode mode, const SMailMessageDetail *orig)
{
  const SMailAccount *account = smail_backend_get_account (self->backend);
  GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (self->text_view));
  const char *title = "New Message";
  g_autofree char *sig = signature_block (account);

  if (orig && mode == SMAIL_COMPOSE_DRAFT) {
    title = "Edit Draft";
    prefill_draft (self, orig);
  } else if (orig && mode != SMAIL_COMPOSE_NEW) {
    g_autofree char *text = orig->text ? g_strdup (orig->text)
                                       : smail_html_to_text (orig->html ? orig->html : "");
    g_autofree char *date = smail_format_date_long (orig->date);
    g_autofree char *sender = orig->from_name && *orig->from_name
                              ? g_strdup_printf ("%s <%s>", orig->from_name, orig->from_addr ? orig->from_addr : "")
                              : g_strdup (orig->from_addr ? orig->from_addr : "");

    if (mode == SMAIL_COMPOSE_FORWARD) {
      title = "Forward";
      g_autofree char *subject = has_prefix_ci (orig->subject, "fwd:") ? g_strdup (orig->subject)
                                                                       : g_strdup_printf ("Fwd: %s", orig->subject);
      gtk_editable_set_text (GTK_EDITABLE (self->subject_entry), subject);
      g_autofree char *body = g_strdup_printf ("%s\n\n---------- Forwarded message ----------\n"
                                               "From: %s\nDate: %s\nSubject: %s\nTo: %s\n\n%s",
                                               sig, sender, date, orig->subject, orig->to ? orig->to : "", text);
      gtk_text_buffer_set_text (buffer, body, -1);
      for (guint i = 0; i < orig->attachments->len; i++) {
        SMailAttachment *a = g_ptr_array_index (orig->attachments, i);
        add_attachment (self, a->filename, a->content_type, a->data);
      }
      rebuild_attachments (self);
      gtk_widget_grab_focus (self->to_entry);
    } else {
      title = mode == SMAIL_COMPOSE_REPLY_ALL ? "Reply All" : "Reply";
      g_autoptr (GHashTable) seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
      g_autoptr (GString) to = g_string_new (NULL);
      merge_addresses (to, orig->reply_to ? orig->reply_to : sender, NULL, seen);
      gtk_editable_set_text (GTK_EDITABLE (self->to_entry), to->str);

      if (mode == SMAIL_COMPOSE_REPLY_ALL) {
        g_autoptr (GString) cc = g_string_new (NULL);
        merge_addresses (cc, orig->to, account->email, seen);
        merge_addresses (cc, orig->cc, account->email, seen);
        gtk_editable_set_text (GTK_EDITABLE (self->cc_entry), cc->str);
        if (cc->len)
          gtk_revealer_set_reveal_child (GTK_REVEALER (self->cc_revealer), TRUE);
      }

      g_autofree char *subject = has_prefix_ci (orig->subject, "re:") ? g_strdup (orig->subject)
                                                                      : g_strdup_printf ("Re: %s", orig->subject);
      gtk_editable_set_text (GTK_EDITABLE (self->subject_entry), subject);

      g_autofree char *quoted = smail_quote_text (text);
      g_autofree char *body = g_strdup_printf ("%s\n\nOn %s, %s wrote:\n%s", sig, date, sender, quoted);
      gtk_text_buffer_set_text (buffer, body, -1);
      GtkTextIter start;
      gtk_text_buffer_get_start_iter (buffer, &start);
      gtk_text_buffer_place_cursor (buffer, &start);
      gtk_widget_grab_focus (self->text_view);

      if (orig->message_id) {
        self->in_reply_to = g_strdup_printf ("<%s>", orig->message_id);
        self->references = orig->references ? g_strdup_printf ("%s %s", orig->references, self->in_reply_to)
                                            : g_strdup (self->in_reply_to);
      }
    }
  } else {
    if (*sig) {
      gtk_text_buffer_set_text (buffer, sig, -1);
      GtkTextIter start;
      gtk_text_buffer_get_start_iter (buffer, &start);
      gtk_text_buffer_place_cursor (buffer, &start);
    }
    gtk_widget_grab_focus (self->to_entry);
  }

  g_autofree char *from = g_strdup_printf ("From %s", account->email ? account->email : "");
  adw_window_title_set_title (ADW_WINDOW_TITLE (self->title), title);
  adw_window_title_set_subtitle (ADW_WINDOW_TITLE (self->title), from);
  gtk_window_set_title (GTK_WINDOW (self), title);
  self->dirty = FALSE; /* prefilled content is not "user work" yet */
}

void
smail_composer_set_address_book (SMailComposer *self, SMailStore *store)
{
  if (self->store || !store)
    return;
  self->store = store;
  smail_address_completion_attach (self->to_entry, store);
  smail_address_completion_attach (self->cc_entry, store);
  smail_address_completion_attach (self->bcc_entry, store);
}

void
smail_composer_set_to (SMailComposer *self, const char *to)
{
  gtk_editable_set_text (GTK_EDITABLE (self->to_entry), to ? to : "");
  gtk_editable_set_position (GTK_EDITABLE (self->to_entry), -1);
  gtk_widget_grab_focus (self->subject_entry);
  self->dirty = FALSE;
}

SMailBackend *
smail_composer_get_backend (SMailComposer *self)
{
  return self->backend;
}

SMailComposer *
smail_composer_new (GtkWindow *parent, SMailBackend *backend, SMailComposeMode mode,
                      const SMailMessageDetail *original)
{
  SMailComposer *self = g_object_new (SMAIL_TYPE_COMPOSER,
                                        "application", gtk_window_get_application (parent),
                                        NULL);
  self->backend = g_object_ref (backend);
  prefill (self, mode, original);
  return self;
}
