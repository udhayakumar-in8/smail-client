/* courier-message-view.c — header, attachments and a sandboxed WebKit body */
#include "courier-message-view.h"
#include "courier-util.h"

#include <webkit/webkit.h>
#include <string.h>

struct _CourierMessageView {
  GtkBox     parent_instance;

  GtkWidget *banner;
  GtkWidget *subject;
  GtkWidget *avatar;
  GtkWidget *from_name;
  GtkWidget *from_addr;
  GtkWidget *to_label;
  GtkWidget *cc_label;
  GtkWidget *date;
  GtkWidget *attachments;
  GtkWidget *web_view;

  CourierMessageDetail *detail;
  gboolean              allow_remote;
};

G_DEFINE_FINAL_TYPE (CourierMessageView, courier_message_view, GTK_TYPE_BOX)

static const char *base_css =
  "body { margin: 0; padding: 18px 24px; font-family: 'Inter', 'Cantarell', sans-serif;"
  "       font-size: 11pt; line-height: 1.45; word-wrap: break-word; overflow-wrap: anywhere; }"
  "img { max-width: 100%; height: auto; }"
  "pre { white-space: pre-wrap; }"
  "a { color: #1c71d8; }";

static const char *html_css =
  "html { background: #ffffff; color: #1e1e1e; }";

static const char *plain_light_css =
  "html { background: transparent; color: #1e1e1e; }"
  ".quote { color: #3d7aa8; } .q2 { color: #26a269; } .q3 { color: #9141ac; }";

static const char *plain_dark_css =
  "html { background: transparent; color: #f2f2f2; }"
  "a { color: #78aeed; }"
  ".quote { color: #99c1f1; } .q2 { color: #8ff0a4; } .q3 { color: #dc8add; }";

/* ---------------- body rendering ---------------- */

static gboolean
has_remote_content (const char *html)
{
  static GRegex *re = NULL;
  if (g_once_init_enter_pointer (&re))
    g_once_init_leave_pointer (&re, g_regex_new ("(src|background|srcset)\\s*=\\s*[\"']?\\s*(https?:)?//"
                                                 "|url\\(\\s*[\"']?\\s*(https?:)?//",
                                                 G_REGEX_CASELESS | G_REGEX_OPTIMIZE, 0, NULL));
  return g_regex_match (re, html, 0, NULL);
}

static char *
inline_cid_parts (CourierMessageDetail *d, const char *html)
{
  GString *s = g_string_new (html);
  for (guint i = 0; i < d->inline_parts->len; i++) {
    CourierAttachment *a = g_ptr_array_index (d->inline_parts, i);
    if (!a->content_id)
      continue;
    gsize len;
    const guchar *data = g_bytes_get_data (a->data, &len);
    g_autofree char *b64 = g_base64_encode (data, len);
    g_autofree char *cid = g_strdup_printf ("cid:%s", a->content_id);
    g_autofree char *uri = g_strdup_printf ("data:%s;base64,%s", a->content_type, b64);
    g_string_replace (s, cid, uri, 0);
  }
  return g_string_free (s, FALSE);
}

static void
render_body (CourierMessageView *self)
{
  CourierMessageDetail *d = self->detail;
  gboolean dark = adw_style_manager_get_dark (adw_style_manager_get_default ());
  g_autofree char *body = NULL;
  const char *theme_css;
  gboolean remote = FALSE;

  if (d->html) {
    body = inline_cid_parts (d, d->html);
    remote = has_remote_content (body);
    theme_css = html_css;
  } else {
    body = courier_text_to_html (d->text ? d->text : "");
    theme_css = dark ? plain_dark_css : plain_light_css;
  }

  adw_banner_set_revealed (ADW_BANNER (self->banner), remote && !self->allow_remote);

  /* Content-Security-Policy keeps scripts off and, unless the user opts in,
   * stops tracking pixels and remote images/styles from loading. */
  const char *remote_src = self->allow_remote ? " https: http:" : "";
  g_autofree char *page = g_strdup_printf (
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; "
    "img-src data:%s; style-src 'unsafe-inline'%s; font-src data:%s; media-src data:\">"
    "<meta name=\"color-scheme\" content=\"%s\">"
    "<style>%s %s</style></head><body>%s</body></html>",
    remote_src, remote_src, remote_src,
    d->html ? "light" : (dark ? "dark" : "light"),
    base_css, theme_css, body);

  webkit_web_view_load_html (WEBKIT_WEB_VIEW (self->web_view), page, "about:blank");
}

static gboolean
on_decide_policy (WebKitWebView *view, WebKitPolicyDecision *decision,
                  WebKitPolicyDecisionType type, CourierMessageView *self)
{
  if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION &&
      type != WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION)
    return FALSE;

  WebKitNavigationAction *action =
    webkit_navigation_policy_decision_get_navigation_action (WEBKIT_NAVIGATION_POLICY_DECISION (decision));
  const char *uri = webkit_uri_request_get_uri (webkit_navigation_action_get_request (action));

  /* our own load_html() */
  if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION &&
      webkit_navigation_action_get_navigation_type (action) == WEBKIT_NAVIGATION_TYPE_OTHER &&
      g_str_has_prefix (uri, "about:"))
    return FALSE;

  webkit_policy_decision_ignore (decision);

  if (g_str_has_prefix (uri, "http:") || g_str_has_prefix (uri, "https:") || g_str_has_prefix (uri, "mailto:")) {
    g_autoptr (GtkUriLauncher) launcher = gtk_uri_launcher_new (uri);
    gtk_uri_launcher_launch (launcher, GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (self))), NULL, NULL, NULL);
  }
  return TRUE;
}

static void
on_show_images (AdwBanner *banner, CourierMessageView *self)
{
  self->allow_remote = TRUE;
  render_body (self);
}

static void
on_dark_changed (AdwStyleManager *sm, GParamSpec *pspec, CourierMessageView *self)
{
  if (self->detail && !self->detail->html)
    render_body (self);
}

/* ---------------- attachments ---------------- */

static void
on_attachment_saved (GObject *source, GAsyncResult *res, gpointer data)
{
  g_autoptr (GError) error = NULL;
  if (!g_file_replace_contents_finish (G_FILE (source), res, NULL, &error))
    g_warning ("Could not save attachment: %s", error->message);
}

static void
on_save_dialog_done (GObject *source, GAsyncResult *res, gpointer data)
{
  g_autoptr (GBytes) bytes = data;
  g_autoptr (GFile) file = gtk_file_dialog_save_finish (GTK_FILE_DIALOG (source), res, NULL);
  if (file)
    g_file_replace_contents_bytes_async (file, bytes, NULL, FALSE, G_FILE_CREATE_REPLACE_DESTINATION,
                                         NULL, on_attachment_saved, NULL);
}

static void
on_attachment_clicked (GtkButton *button, CourierMessageView *self)
{
  CourierAttachment *a = g_object_get_data (G_OBJECT (button), "attachment");
  g_autoptr (GtkFileDialog) dialog = gtk_file_dialog_new ();
  gtk_file_dialog_set_title (dialog, "Save Attachment");
  gtk_file_dialog_set_initial_name (dialog, a->filename);
  gtk_file_dialog_save (dialog, GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (self))), NULL,
                        on_save_dialog_done, g_bytes_ref (a->data));
}

static GtkWidget *
attachment_button (CourierMessageView *self, CourierAttachment *a)
{
  g_autofree char *ctype = g_content_type_from_mime_type (a->content_type);
  g_autoptr (GIcon) icon = g_content_type_get_symbolic_icon (ctype ? ctype : a->content_type);
  g_autofree char *size = g_format_size (g_bytes_get_size (a->data));

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
  GtkWidget *image = gtk_image_new_from_gicon (icon);
  gtk_image_set_icon_size (GTK_IMAGE (image), GTK_ICON_SIZE_LARGE);
  gtk_box_append (GTK_BOX (box), image);

  GtkWidget *labels = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *name = gtk_label_new (a->filename);
  gtk_label_set_xalign (GTK_LABEL (name), 0);
  gtk_label_set_ellipsize (GTK_LABEL (name), PANGO_ELLIPSIZE_MIDDLE);
  gtk_label_set_max_width_chars (GTK_LABEL (name), 24);
  GtkWidget *sz = gtk_label_new (size);
  gtk_label_set_xalign (GTK_LABEL (sz), 0);
  gtk_widget_add_css_class (sz, "dim-label");
  gtk_widget_add_css_class (sz, "caption");
  gtk_box_append (GTK_BOX (labels), name);
  gtk_box_append (GTK_BOX (labels), sz);
  gtk_box_append (GTK_BOX (box), labels);

  GtkWidget *button = gtk_button_new ();
  gtk_button_set_child (GTK_BUTTON (button), box);
  gtk_widget_add_css_class (button, "attachment");
  gtk_widget_set_tooltip_text (button, "Save attachment");
  g_object_set_data (G_OBJECT (button), "attachment", a);
  g_signal_connect (button, "clicked", G_CALLBACK (on_attachment_clicked), self);
  return button;
}

/* ---------------- public ---------------- */

void
courier_message_view_set_detail (CourierMessageView *self, CourierMessageDetail *detail)
{
  /* the attachment buttons point into the old detail: drop them first */
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (self->attachments)))
    gtk_flow_box_remove (GTK_FLOW_BOX (self->attachments), child);

  g_clear_pointer (&self->detail, courier_message_detail_free);
  self->detail = detail;
  self->allow_remote = FALSE;

  if (!detail)
    return;

  const char *name = detail->from_name && *detail->from_name ? detail->from_name : detail->from_addr;
  gtk_label_set_text (GTK_LABEL (self->subject), detail->subject);
  gtk_label_set_text (GTK_LABEL (self->from_name), name ? name : "Unknown Sender");
  gtk_label_set_text (GTK_LABEL (self->from_addr), detail->from_addr ? detail->from_addr : "");
  gtk_widget_set_visible (self->from_addr, detail->from_addr && name != detail->from_addr);
  adw_avatar_set_text (ADW_AVATAR (self->avatar), name);

  g_autofree char *to = g_strdup_printf ("To: %s", detail->to ? detail->to : "undisclosed recipients");
  gtk_label_set_text (GTK_LABEL (self->to_label), to);
  gtk_widget_set_visible (self->cc_label, detail->cc != NULL);
  if (detail->cc) {
    g_autofree char *cc = g_strdup_printf ("Cc: %s", detail->cc);
    gtk_label_set_text (GTK_LABEL (self->cc_label), cc);
  }

  g_autofree char *date = courier_format_date_long (detail->date);
  gtk_label_set_text (GTK_LABEL (self->date), date);

  for (guint i = 0; i < detail->attachments->len; i++)
    gtk_flow_box_append (GTK_FLOW_BOX (self->attachments),
                         attachment_button (self, g_ptr_array_index (detail->attachments, i)));
  gtk_widget_set_visible (self->attachments, detail->attachments->len > 0);

  render_body (self);
}

const CourierMessageDetail *
courier_message_view_get_detail (CourierMessageView *self)
{
  return self->detail;
}

static GtkWidget *
dim_label (const char *css)
{
  GtkWidget *l = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (l), 0);
  gtk_label_set_selectable (GTK_LABEL (l), TRUE);
  gtk_label_set_ellipsize (GTK_LABEL (l), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class (l, "dim-label");
  if (css)
    gtk_widget_add_css_class (l, css);
  return l;
}

static void
courier_message_view_dispose (GObject *object)
{
  CourierMessageView *self = COURIER_MESSAGE_VIEW (object);
  g_signal_handlers_disconnect_by_data (adw_style_manager_get_default (), self);
  G_OBJECT_CLASS (courier_message_view_parent_class)->dispose (object);
}

static void
courier_message_view_finalize (GObject *object)
{
  courier_message_detail_free (COURIER_MESSAGE_VIEW (object)->detail);
  G_OBJECT_CLASS (courier_message_view_parent_class)->finalize (object);
}

static void
courier_message_view_class_init (CourierMessageViewClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = courier_message_view_dispose;
  G_OBJECT_CLASS (klass)->finalize = courier_message_view_finalize;
}

static void
courier_message_view_init (CourierMessageView *self)
{
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_VERTICAL);

  self->banner = adw_banner_new ("Remote images are hidden to protect your privacy");
  adw_banner_set_button_label (ADW_BANNER (self->banner), "Show Images");
  g_signal_connect (self->banner, "button-clicked", G_CALLBACK (on_show_images), self);
  gtk_box_append (GTK_BOX (self), self->banner);

  /* header */
  GtkWidget *header = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_add_css_class (header, "message-header");

  self->subject = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (self->subject), 0);
  gtk_label_set_wrap (GTK_LABEL (self->subject), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (self->subject), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_selectable (GTK_LABEL (self->subject), TRUE);
  gtk_widget_add_css_class (self->subject, "title-2");
  gtk_box_append (GTK_BOX (header), self->subject);

  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  self->avatar = adw_avatar_new (44, NULL, TRUE);
  gtk_widget_set_valign (self->avatar, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (row), self->avatar);

  GtkWidget *who = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand (who, TRUE);
  GtkWidget *name_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  self->from_name = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (self->from_name), 0);
  gtk_label_set_selectable (GTK_LABEL (self->from_name), TRUE);
  gtk_label_set_ellipsize (GTK_LABEL (self->from_name), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class (self->from_name, "heading");
  self->from_addr = dim_label (NULL);
  gtk_box_append (GTK_BOX (name_row), self->from_name);
  gtk_box_append (GTK_BOX (name_row), self->from_addr);
  gtk_box_append (GTK_BOX (who), name_row);
  self->to_label = dim_label ("caption");
  self->cc_label = dim_label ("caption");
  gtk_box_append (GTK_BOX (who), self->to_label);
  gtk_box_append (GTK_BOX (who), self->cc_label);
  gtk_box_append (GTK_BOX (row), who);

  self->date = dim_label ("caption");
  gtk_box_append (GTK_BOX (who), self->date);
  gtk_box_append (GTK_BOX (header), row);

  self->attachments = gtk_flow_box_new ();
  gtk_flow_box_set_selection_mode (GTK_FLOW_BOX (self->attachments), GTK_SELECTION_NONE);
  gtk_flow_box_set_max_children_per_line (GTK_FLOW_BOX (self->attachments), 6);
  gtk_flow_box_set_column_spacing (GTK_FLOW_BOX (self->attachments), 6);
  gtk_flow_box_set_row_spacing (GTK_FLOW_BOX (self->attachments), 6);
  gtk_widget_set_visible (self->attachments, FALSE);
  gtk_box_append (GTK_BOX (header), self->attachments);

  gtk_box_append (GTK_BOX (self), header);
  gtk_box_append (GTK_BOX (self), gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));

  /* body: an ephemeral, script-free web view */
  g_autoptr (WebKitNetworkSession) session = webkit_network_session_new_ephemeral ();
  self->web_view = g_object_new (WEBKIT_TYPE_WEB_VIEW, "network-session", session, NULL);
  WebKitSettings *settings = webkit_web_view_get_settings (WEBKIT_WEB_VIEW (self->web_view));
  webkit_settings_set_enable_javascript (settings, FALSE);
  webkit_settings_set_enable_javascript_markup (settings, FALSE);
  webkit_settings_set_enable_page_cache (settings, FALSE);
  webkit_settings_set_enable_html5_database (settings, FALSE);
  webkit_settings_set_enable_html5_local_storage (settings, FALSE);
  GdkRGBA transparent = { 0, 0, 0, 0 };
  webkit_web_view_set_background_color (WEBKIT_WEB_VIEW (self->web_view), &transparent);
  gtk_widget_set_vexpand (self->web_view, TRUE);
  g_signal_connect (self->web_view, "decide-policy", G_CALLBACK (on_decide_policy), self);
  gtk_box_append (GTK_BOX (self), self->web_view);

  g_signal_connect (adw_style_manager_get_default (), "notify::dark", G_CALLBACK (on_dark_changed), self);
}

GtkWidget *
courier_message_view_new (void)
{
  return g_object_new (COURIER_TYPE_MESSAGE_VIEW, NULL);
}
