/* smail-window.c — folders | message list | reading pane */
#include "smail-window.h"
#include "smail-accounts.h"
#include "smail-account-dialog.h"
#include "smail-backend.h"
#include "smail-composer.h"
#include "smail-message-view.h"
#include "smail-util.h"
#include "smail-store.h"
#include "smail-automation.h"
#include "smail-lan.h"
#include "smail-organizer.h"
#include "smail-mail-tools.h"
#include "smail-chat-page.h"
#include "smail-ui.h"
#include "smail-theme.h"
#include "smail-backup-dialog.h"

#include <string.h>

#define MESSAGE_LIMIT        100
#define AUTO_REFRESH_SECONDS (5 * 60)
#define REMINDER_CHECK_SECONDS 15

/* Organizer pages shown in place of the mail panes */
static const struct { const char *id, *title, *icon; } tools[] = {
  { "calendar",  "Calendar",   "x-office-calendar-symbolic" },
  { "tasks",     "Tasks",      "checkbox-checked-symbolic" },
  { "reminders", "Reminders",  "alarm-symbolic" },
  { "chat",      "Local Chat", "network-workgroup-symbolic" },
  { "log",       "Mail Log",   "document-open-recent-symbolic" },
  { "filters",   "Filters",    "emblem-system-symbolic" },
};

typedef struct {
  SMailBackend *backend;
  GPtrArray      *folders; /* SMailFolder*, NULL until listed */
  char           *error;   /* last folder-listing error */
} AccountCtx;

struct _SMailWindow {
  AdwApplicationWindow parent_instance;

  gboolean        demo;
  GPtrArray      *accounts;   /* SMailAccount* (persisted) */
  GPtrArray      *ctxs;       /* AccountCtx* */

  /* widgets */
  GtkWidget      *toast_overlay;
  GtkWidget      *main_stack;
  GtkWidget      *outer_split;
  GtkWidget      *inner_split;
  GtkWidget      *sidebar;
  GtkWidget      *list_page;
  GtkWidget      *list_title;
  GtkWidget      *list_stack;
  GtkWidget      *list_empty;
  GtkWidget      *message_list;
  GtkWidget      *search_bar;
  GtkWidget      *search_entry;
  GtkWidget      *refresh_button;
  GtkWidget      *viewer_page;
  GtkWidget      *viewer_stack;
  GtkWidget      *viewer;
  GtkWidget      *star_button;
  GtkWidget      *read_button;
  GtkWidget      *reply_all_button;

  GListStore     *messages;   /* SMailMessage* */
  GtkFilter      *filter;

  /* selection */
  AccountCtx     *current_ctx;
  char           *current_folder;
  SMailMessage *current_message;
  GCancellable   *list_cancellable;
  GCancellable   *fetch_cancellable;
  gboolean        rebuilding_sidebar;
  guint           refresh_source;
  int             pending_folder_lists;

  /* local features */
  SMailStore   *store;
  SMailLan     *lan;
  GtkWidget      *content_stack;
  GtkWidget      *content_page;
  GtkWidget      *calendar_page;
  GtkWidget      *tasks_page;
  GtkWidget      *reminders_page;
  GtkWidget      *chat_page;
  GtkWidget      *log_page;
  GtkWidget      *filters_page;
  char           *current_tool;   /* NULL = mail */
  int             chat_unread;
  guint           reminder_source;
  gboolean        skip_welcome;
};

G_DEFINE_FINAL_TYPE (SMailWindow, smail_window, ADW_TYPE_APPLICATION_WINDOW)

static void select_folder      (SMailWindow *self, AccountCtx *ctx, const char *path);
static void rebuild_sidebar    (SMailWindow *self);
static void update_actions     (SMailWindow *self);
static void reload_accounts    (SMailWindow *self);
static void refresh_all        (SMailWindow *self);
static void check_new_mail     (SMailWindow *self, AccountCtx *ctx);
static void update_filter_folders (SMailWindow *self);
static void load_messages      (SMailWindow *self);

static void
toast (SMailWindow *self, const char *text)
{
  adw_toast_overlay_add_toast (ADW_TOAST_OVERLAY (self->toast_overlay), adw_toast_new (text));
}

static void
account_ctx_free (AccountCtx *ctx)
{
  g_clear_object (&ctx->backend);
  g_clear_pointer (&ctx->folders, g_ptr_array_unref);
  g_free (ctx->error);
  g_free (ctx);
}

static SMailFolder *
find_folder (AccountCtx *ctx, const char *path)
{
  for (guint i = 0; ctx && ctx->folders && i < ctx->folders->len; i++) {
    SMailFolder *f = g_ptr_array_index (ctx->folders, i);
    if (g_str_equal (f->path, path))
      return f;
  }
  return NULL;
}

static SMailFolder *
current_folder (SMailWindow *self)
{
  return self->current_folder ? find_folder (self->current_ctx, self->current_folder) : NULL;
}

/* ====================================================================== */
/*  Sidebar                                                               */
/* ====================================================================== */

static GtkWidget *
folder_row_new (AccountCtx *ctx, SMailFolder *folder)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_box_append (GTK_BOX (box), gtk_image_new_from_icon_name (smail_folder_get_icon (folder)));

  GtkWidget *label = gtk_label_new (folder->display_name);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand (label, TRUE);
  gtk_box_append (GTK_BOX (box), label);

  GtkWidget *badge = gtk_label_new (NULL);
  gtk_widget_add_css_class (badge, "unread-badge");
  gtk_widget_set_valign (badge, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (box), badge);

  GtkWidget *row = gtk_list_box_row_new ();
  gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);
  g_object_set_data (G_OBJECT (row), "ctx", ctx);
  if (folder->path)
    g_object_set_data_full (G_OBJECT (row), "path", g_strdup (folder->path), g_free);
  g_object_set_data (G_OBJECT (row), "badge", badge);
  return row;
}

static void
update_badge (GtkWidget *row, SMailFolder *folder)
{
  GtkWidget *badge = g_object_get_data (G_OBJECT (row), "badge");
  /* only show counts where they mean something */
  gboolean show = folder->unread > 0 &&
                  !(folder->role && (g_str_equal (folder->role, "sent") || g_str_equal (folder->role, "trash") ||
                                     g_str_equal (folder->role, "drafts") || g_str_equal (folder->role, "all")));
  g_autofree char *text = g_strdup_printf ("%d", folder->unread);
  gtk_label_set_text (GTK_LABEL (badge), text);
  gtk_widget_set_visible (badge, show);
}

static void
refresh_badges (SMailWindow *self)
{
  for (GtkWidget *row = gtk_widget_get_first_child (self->sidebar); row; row = gtk_widget_get_next_sibling (row)) {
    AccountCtx *ctx = g_object_get_data (G_OBJECT (row), "ctx");
    const char *path = g_object_get_data (G_OBJECT (row), "path");
    SMailFolder *f = path ? find_folder (ctx, path) : NULL;
    if (f)
      update_badge (row, f);
  }
}

static void
rebuild_sidebar (SMailWindow *self)
{
  self->rebuilding_sidebar = TRUE;
  gtk_list_box_remove_all (GTK_LIST_BOX (self->sidebar));
  GtkWidget *to_select = NULL;

  for (guint i = 0; i < self->ctxs->len; i++) {
    AccountCtx *ctx = g_ptr_array_index (self->ctxs, i);
    const SMailAccount *account = smail_backend_get_account (ctx->backend);

    GtkWidget *header = gtk_label_new (account->email);
    gtk_label_set_xalign (GTK_LABEL (header), 0);
    gtk_label_set_ellipsize (GTK_LABEL (header), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class (header, "account-header");
    GtkWidget *hrow = gtk_list_box_row_new ();
    gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (hrow), header);
    gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (hrow), FALSE);
    gtk_list_box_row_set_selectable (GTK_LIST_BOX_ROW (hrow), FALSE);
    gtk_list_box_append (GTK_LIST_BOX (self->sidebar), hrow);

    if (!ctx->folders) {
      GtkWidget *status = gtk_label_new (ctx->error ? ctx->error : "Connecting…");
      gtk_label_set_xalign (GTK_LABEL (status), 0);
      gtk_label_set_wrap (GTK_LABEL (status), TRUE);
      gtk_widget_add_css_class (status, "dim-label");
      gtk_widget_add_css_class (status, ctx->error ? "error" : "caption");
      gtk_widget_add_css_class (status, "sidebar-status");
      GtkWidget *srow = gtk_list_box_row_new ();
      gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (srow), status);
      gtk_list_box_row_set_selectable (GTK_LIST_BOX_ROW (srow), FALSE);
      gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (srow), FALSE);
      gtk_list_box_append (GTK_LIST_BOX (self->sidebar), srow);
      continue;
    }

    for (guint j = 0; j < ctx->folders->len; j++) {
      SMailFolder *f = g_ptr_array_index (ctx->folders, j);
      GtkWidget *row = folder_row_new (ctx, f);
      update_badge (row, f);
      gtk_list_box_append (GTK_LIST_BOX (self->sidebar), row);
      if (!self->current_tool && ctx == self->current_ctx && g_strcmp0 (self->current_folder, f->path) == 0)
        to_select = row;
    }
  }

  if (self->ctxs->len == 0) {
    GtkWidget *add = gtk_label_new ("No email account yet");
    gtk_label_set_xalign (GTK_LABEL (add), 0);
    gtk_widget_add_css_class (add, "dim-label");
    gtk_widget_add_css_class (add, "sidebar-status");
    GtkWidget *arow = gtk_list_box_row_new ();
    gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (arow), add);
    gtk_list_box_row_set_selectable (GTK_LIST_BOX_ROW (arow), FALSE);
    gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (arow), FALSE);
    gtk_list_box_append (GTK_LIST_BOX (self->sidebar), arow);
  }

  /* organizer */
  GtkWidget *oheader = gtk_label_new ("Organizer");
  gtk_label_set_xalign (GTK_LABEL (oheader), 0);
  gtk_widget_add_css_class (oheader, "account-header");
  GtkWidget *ohrow = gtk_list_box_row_new ();
  gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (ohrow), oheader);
  gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (ohrow), FALSE);
  gtk_list_box_row_set_selectable (GTK_LIST_BOX_ROW (ohrow), FALSE);
  gtk_list_box_append (GTK_LIST_BOX (self->sidebar), ohrow);
  for (gsize i = 0; i < G_N_ELEMENTS (tools); i++) {
    SMailFolder pseudo = { NULL, (char *) tools[i].title, NULL, 0 };
    GtkWidget *row = folder_row_new (NULL, &pseudo);
    GtkWidget *icon = gtk_widget_get_first_child (gtk_list_box_row_get_child (GTK_LIST_BOX_ROW (row)));
    gtk_image_set_from_icon_name (GTK_IMAGE (icon), tools[i].icon);
    g_object_set_data (G_OBJECT (row), "tool", (gpointer) tools[i].id);
    if (g_str_equal (tools[i].id, "chat")) {
      GtkWidget *badge = g_object_get_data (G_OBJECT (row), "badge");
      g_autofree char *n = g_strdup_printf ("%d", self->chat_unread);
      gtk_label_set_text (GTK_LABEL (badge), n);
      gtk_widget_set_visible (badge, self->chat_unread > 0);
    } else {
      gtk_widget_set_visible (g_object_get_data (G_OBJECT (row), "badge"), FALSE);
    }
    gtk_list_box_append (GTK_LIST_BOX (self->sidebar), row);
    if (g_strcmp0 (self->current_tool, tools[i].id) == 0)
      to_select = row;
  }

  if (to_select)
    gtk_list_box_select_row (GTK_LIST_BOX (self->sidebar), GTK_LIST_BOX_ROW (to_select));
  self->rebuilding_sidebar = FALSE;
}

static void
show_tool (SMailWindow *self, const char *tool)
{
  char *copy = g_strdup (tool); /* `tool` may be self->current_tool itself */
  g_free (self->current_tool);
  self->current_tool = copy;
  tool = copy;
  gtk_stack_set_visible_child_name (GTK_STACK (self->content_stack), tool ? tool : "mail");
  smail_chat_page_set_active (self->chat_page, g_strcmp0 (tool, "chat") == 0);

  const char *title = "Mail";
  for (gsize i = 0; tool && i < G_N_ELEMENTS (tools); i++)
    if (g_str_equal (tools[i].id, tool))
      title = tools[i].title;
  adw_navigation_page_set_title (ADW_NAVIGATION_PAGE (self->content_page), title);

  /* pages read the database when shown, so they're always current */
  if (g_strcmp0 (tool, "calendar") == 0)  smail_calendar_page_refresh (self->calendar_page);
  if (g_strcmp0 (tool, "tasks") == 0)     smail_tasks_page_refresh (self->tasks_page);
  if (g_strcmp0 (tool, "reminders") == 0) smail_reminders_page_refresh (self->reminders_page);
  if (g_strcmp0 (tool, "log") == 0)       smail_mail_log_page_refresh (self->log_page);
  if (g_strcmp0 (tool, "filters") == 0)   smail_filters_page_refresh (self->filters_page);
}

static void
on_sidebar_row_activated (GtkListBox *box, GtkListBoxRow *row, SMailWindow *self)
{
  const char *tool = g_object_get_data (G_OBJECT (row), "tool");
  if (tool) {
    adw_navigation_split_view_set_show_content (ADW_NAVIGATION_SPLIT_VIEW (self->outer_split), TRUE);
    show_tool (self, tool);
    return;
  }
  AccountCtx *ctx = g_object_get_data (G_OBJECT (row), "ctx");
  const char *path = g_object_get_data (G_OBJECT (row), "path");
  if (!ctx || !path)
    return;
  adw_navigation_split_view_set_show_content (ADW_NAVIGATION_SPLIT_VIEW (self->outer_split), TRUE);
  gboolean was_tool = self->current_tool != NULL;
  if (was_tool)
    show_tool (self, NULL);
  if (!was_tool && ctx == self->current_ctx && g_strcmp0 (path, self->current_folder) == 0)
    return;
  select_folder (self, ctx, path);
}

static void
on_folders_listed (GObject *source, GAsyncResult *res, gpointer data)
{
  g_autoptr (SMailWindow) self = data;
  SMailBackend *backend = SMAIL_BACKEND (source);
  g_autoptr (GError) error = NULL;
  GPtrArray *folders = smail_backend_finish_pointer (backend, res, &error);

  AccountCtx *ctx = NULL;
  for (guint i = 0; i < self->ctxs->len; i++)
    if (((AccountCtx *) g_ptr_array_index (self->ctxs, i))->backend == backend)
      ctx = g_ptr_array_index (self->ctxs, i);
  self->pending_folder_lists--;
  if (!ctx) { /* account removed meanwhile */
    g_clear_pointer (&folders, g_ptr_array_unref);
    return;
  }

  g_clear_pointer (&ctx->error, g_free);
  if (folders) {
    g_clear_pointer (&ctx->folders, g_ptr_array_unref);
    ctx->folders = folders;
  } else if (!ctx->folders) {
    ctx->error = g_strdup (error->message);
  } else {
    g_autofree char *msg = g_strdup_printf ("%s: %s", smail_backend_get_account (backend)->email, error->message);
    toast (self, msg);
  }
  rebuild_sidebar (self);
  update_filter_folders (self);
  if (folders)
    check_new_mail (self, ctx);

  /* first time: open the inbox of the first account that has one */
  if (!self->current_ctx && ctx->folders && ctx->folders->len > 0) {
    SMailFolder *inbox = g_ptr_array_index (ctx->folders, 0);
    select_folder (self, ctx, inbox->path);
  } else if ((self->current_ctx == ctx || (!self->current_ctx && self->ctxs->len == 1)) && !ctx->folders) {
    gtk_stack_set_visible_child_name (GTK_STACK (self->list_stack), "error");
    adw_status_page_set_title (ADW_STATUS_PAGE (self->list_empty), "Couldn't Connect");
    adw_status_page_set_icon_name (ADW_STATUS_PAGE (self->list_empty), "dialog-error-symbolic");
    adw_status_page_set_description (ADW_STATUS_PAGE (self->list_empty), ctx->error);
  }
}

static void
list_folders (SMailWindow *self, AccountCtx *ctx)
{
  self->pending_folder_lists++;
  smail_backend_list_folders (ctx->backend, NULL, on_folders_listed, g_object_ref (self));
}

/* ====================================================================== */
/*  Message list                                                          */
/* ====================================================================== */

static void
update_row_style (SMailMessage *msg, GParamSpec *pspec, GtkWidget *row)
{
  if (msg->seen)
    gtk_widget_remove_css_class (row, "unread");
  else
    gtk_widget_add_css_class (row, "unread");
}

static GtkWidget *
create_message_row (gpointer item, gpointer data)
{
  SMailMessage *msg = item;
  SMailFolder *folder = current_folder (data);
  gboolean outgoing = folder && folder->role &&
                      (g_str_equal (folder->role, "sent") || g_str_equal (folder->role, "drafts"));
  g_autofree char *who = outgoing ? g_strdup_printf ("To: %s", msg->recipient ? msg->recipient : "(no recipient)")
                                  : g_strdup (smail_message_get_sender (msg));

  GtkWidget *grid = gtk_grid_new ();
  gtk_grid_set_column_spacing (GTK_GRID (grid), 8);
  gtk_grid_set_row_spacing (GTK_GRID (grid), 3);

  GtkWidget *dot = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (dot, "unread-dot");
  gtk_widget_set_valign (dot, GTK_ALIGN_CENTER);
  gtk_widget_set_halign (dot, GTK_ALIGN_CENTER);
  gtk_grid_attach (GTK_GRID (grid), dot, 0, 0, 1, 1);

  GtkWidget *sender = gtk_label_new (who);
  gtk_label_set_xalign (GTK_LABEL (sender), 0);
  gtk_label_set_ellipsize (GTK_LABEL (sender), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand (sender, TRUE);
  gtk_widget_add_css_class (sender, "sender");
  gtk_grid_attach (GTK_GRID (grid), sender, 1, 0, 1, 1);

  GtkWidget *meta = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  if (msg->has_attachment) {
    GtkWidget *clip = gtk_image_new_from_icon_name ("mail-attachment-symbolic");
    gtk_widget_add_css_class (clip, "dim-label");
    gtk_box_append (GTK_BOX (meta), clip);
  }
  g_autofree char *date = smail_format_date_short (msg->date);
  GtkWidget *date_label = gtk_label_new (date);
  gtk_widget_add_css_class (date_label, "dim-label");
  gtk_widget_add_css_class (date_label, "caption");
  gtk_box_append (GTK_BOX (meta), date_label);
  gtk_grid_attach (GTK_GRID (grid), meta, 2, 0, 1, 1);

  GtkWidget *subject = gtk_label_new (msg->subject);
  gtk_label_set_xalign (GTK_LABEL (subject), 0);
  gtk_label_set_ellipsize (GTK_LABEL (subject), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class (subject, "subject");
  gtk_grid_attach (GTK_GRID (grid), subject, 1, 1, 1, 1);

  GtkWidget *star = gtk_image_new_from_icon_name ("starred-symbolic");
  gtk_widget_add_css_class (star, "star");
  gtk_widget_set_halign (star, GTK_ALIGN_END);
  g_object_bind_property (msg, "flagged", star, "visible", G_BINDING_SYNC_CREATE);
  gtk_grid_attach (GTK_GRID (grid), star, 2, 1, 1, 1);

  GtkWidget *row = gtk_list_box_row_new ();
  gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), grid);
  gtk_widget_add_css_class (row, "message-row");
  g_object_set_data_full (G_OBJECT (row), "message", g_object_ref (msg), g_object_unref);
  g_signal_connect_object (msg, "notify::seen", G_CALLBACK (update_row_style), row, 0);
  update_row_style (msg, NULL, row);

  g_autofree char *a11y = g_strdup_printf ("%s%s, %s, %s", msg->seen ? "" : "Unread, ",
                                           who, msg->subject, date);
  gtk_accessible_update_property (GTK_ACCESSIBLE (row), GTK_ACCESSIBLE_PROPERTY_LABEL, a11y, -1);
  return row;
}

static gboolean
filter_message (gpointer item, gpointer data)
{
  SMailWindow *self = data;
  const char *query = gtk_editable_get_text (GTK_EDITABLE (self->search_entry));
  if (!query || !*query)
    return TRUE;
  SMailMessage *msg = item;
  g_autofree char *q = g_utf8_casefold (query, -1);
  g_autofree char *hay = g_strjoin (" ", msg->subject ? msg->subject : "", msg->sender_name ? msg->sender_name : "",
                                    msg->sender_addr ? msg->sender_addr : "", NULL);
  g_autofree char *h = g_utf8_casefold (hay, -1);
  return strstr (h, q) != NULL;
}

static void
on_search_changed (GtkSearchEntry *entry, SMailWindow *self)
{
  gtk_filter_changed (self->filter, GTK_FILTER_CHANGE_DIFFERENT);
}

static void
clear_viewer (SMailWindow *self)
{
  g_cancellable_cancel (self->fetch_cancellable);
  g_clear_object (&self->current_message);
  smail_message_view_set_detail (SMAIL_MESSAGE_VIEW (self->viewer), NULL);
  gtk_stack_set_visible_child_name (GTK_STACK (self->viewer_stack), "empty");
  adw_navigation_page_set_title (ADW_NAVIGATION_PAGE (self->viewer_page), "Message");
  update_actions (self);
}

static void
show_empty_folder (SMailWindow *self)
{
  adw_status_page_set_title (ADW_STATUS_PAGE (self->list_empty), "No Messages");
  adw_status_page_set_description (ADW_STATUS_PAGE (self->list_empty), "This folder is empty");
  adw_status_page_set_icon_name (ADW_STATUS_PAGE (self->list_empty), "mail-read-symbolic");
  gtk_stack_set_visible_child_name (GTK_STACK (self->list_stack), "error");
}

typedef struct {
  SMailWindow *self;
  AccountCtx    *ctx;
  char          *folder;
} ListRequest;

static void
list_request_free (ListRequest *r)
{
  g_object_unref (r->self);
  g_free (r->folder);
  g_free (r);
}

static void
on_messages_listed (GObject *source, GAsyncResult *res, gpointer data)
{
  ListRequest *req = data;
  SMailWindow *self = req->self;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) msgs = smail_backend_finish_pointer (SMAIL_BACKEND (source), res, &error);

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    list_request_free (req);
    return;
  }
  gtk_widget_set_sensitive (self->refresh_button, TRUE);

  /* stale answer for a folder we already left? */
  if (req->ctx != self->current_ctx || g_strcmp0 (req->folder, self->current_folder) != 0) {
    list_request_free (req);
    return;
  }

  if (!msgs) {
    adw_status_page_set_title (ADW_STATUS_PAGE (self->list_empty), "Couldn't Load Messages");
    adw_status_page_set_description (ADW_STATUS_PAGE (self->list_empty), error->message);
    adw_status_page_set_icon_name (ADW_STATUS_PAGE (self->list_empty), "dialog-error-symbolic");
    gtk_stack_set_visible_child_name (GTK_STACK (self->list_stack), "error");
    list_request_free (req);
    return;
  }

  /* keep the open message selected across refreshes */
  g_autofree char *keep_uid = self->current_message ? g_strdup (self->current_message->uid) : NULL;
  g_list_store_splice (self->messages, 0, g_list_model_get_n_items (G_LIST_MODEL (self->messages)),
                       msgs->pdata, msgs->len);

  if (msgs->len == 0) {
    show_empty_folder (self);
  } else {
    gtk_stack_set_visible_child_name (GTK_STACK (self->list_stack), "list");
  }

  if (keep_uid) {
    gboolean found = FALSE;
    for (GtkWidget *row = gtk_widget_get_first_child (self->message_list); row; row = gtk_widget_get_next_sibling (row)) {
      SMailMessage *m = g_object_get_data (G_OBJECT (row), "message");
      if (m && g_str_equal (m->uid, keep_uid)) {
        g_set_object (&self->current_message, m);
        gtk_list_box_select_row (GTK_LIST_BOX (self->message_list), GTK_LIST_BOX_ROW (row));
        found = TRUE;
        break;
      }
    }
    if (!found)
      clear_viewer (self);
  }
  list_request_free (req);
}

static void
load_messages (SMailWindow *self)
{
  if (!self->current_ctx || !self->current_folder)
    return;
  g_cancellable_cancel (self->list_cancellable);
  g_clear_object (&self->list_cancellable);
  self->list_cancellable = g_cancellable_new ();

  if (g_list_model_get_n_items (G_LIST_MODEL (self->messages)) == 0)
    gtk_stack_set_visible_child_name (GTK_STACK (self->list_stack), "loading");
  gtk_widget_set_sensitive (self->refresh_button, FALSE);

  ListRequest *req = g_new0 (ListRequest, 1);
  req->self = g_object_ref (self);
  req->ctx = self->current_ctx;
  req->folder = g_strdup (self->current_folder);
  smail_backend_list_messages (self->current_ctx->backend, self->current_folder, MESSAGE_LIMIT,
                                 self->list_cancellable, on_messages_listed, req);
}

static void
select_folder (SMailWindow *self, AccountCtx *ctx, const char *path)
{
  self->current_ctx = ctx;
  g_free (self->current_folder);
  self->current_folder = g_strdup (path);

  SMailFolder *f = find_folder (ctx, path);
  const char *name = f ? f->display_name : path;
  adw_navigation_page_set_title (ADW_NAVIGATION_PAGE (self->list_page), name);
  adw_window_title_set_title (ADW_WINDOW_TITLE (self->list_title), name);
  adw_window_title_set_subtitle (ADW_WINDOW_TITLE (self->list_title),
                                 smail_backend_get_account (ctx->backend)->email);

  /* reflect selection in the sidebar */
  for (GtkWidget *row = gtk_widget_get_first_child (self->sidebar); row; row = gtk_widget_get_next_sibling (row))
    if (g_object_get_data (G_OBJECT (row), "ctx") == ctx &&
        g_strcmp0 (g_object_get_data (G_OBJECT (row), "path"), path) == 0)
      gtk_list_box_select_row (GTK_LIST_BOX (self->sidebar), GTK_LIST_BOX_ROW (row));

  gtk_editable_set_text (GTK_EDITABLE (self->search_entry), "");
  g_list_store_remove_all (self->messages);
  clear_viewer (self);
  load_messages (self);
}

/* ====================================================================== */
/*  Reading                                                               */
/* ====================================================================== */

static void
adjust_unread (SMailWindow *self, int delta)
{
  SMailFolder *f = current_folder (self);
  if (!f)
    return;
  f->unread = MAX (0, f->unread + delta);
  refresh_badges (self);
}

typedef struct {
  SMailWindow  *self;
  SMailMessage *msg;
  char         *account;  /* NULL for the demo: nothing is stored */
  char         *folder;
} FetchRequest;

static void
on_message_fetched (GObject *source, GAsyncResult *res, gpointer data)
{
  FetchRequest *req = data;
  SMailWindow *self = req->self;
  g_autoptr (GError) error = NULL;
  SMailMessageDetail *detail = smail_backend_finish_pointer (SMAIL_BACKEND (source), res, &error);

  if (req->msg != self->current_message || g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    smail_message_detail_free (detail);
  } else if (!detail) {
    gtk_stack_set_visible_child_name (GTK_STACK (self->viewer_stack), "empty");
    toast (self, error->message);
  } else {
    /* keep a copy of every downloaded message in the local store */
    if (req->account && detail->raw) {
      g_autoptr (GError) save_error = NULL;
      if (!smail_store_save_message (self->store, req->account, req->folder, detail->uid, detail->raw,
                                     NULL, &save_error))
        g_warning ("Could not save message locally: %s", save_error->message);
    }
    if (!req->msg->seen) {
      smail_message_set_seen (req->msg, TRUE); /* the fetch marked it read */
      adjust_unread (self, -1);
    }
    adw_navigation_page_set_title (ADW_NAVIGATION_PAGE (self->viewer_page), detail->subject);
    smail_message_view_set_detail (SMAIL_MESSAGE_VIEW (self->viewer), detail);
    gtk_stack_set_visible_child_name (GTK_STACK (self->viewer_stack), "message");
  }
  update_actions (self);
  g_object_unref (req->self);
  g_object_unref (req->msg);
  g_free (req->account);
  g_free (req->folder);
  g_free (req);
}

static void
on_seen_stored (GObject *source, GAsyncResult *res, gpointer data)
{
  g_autoptr (GError) error = NULL;
  if (!smail_backend_finish_boolean (SMAIL_BACKEND (source), res, &error))
    g_message ("Could not mark message as read on the server: %s", error->message);
}

/* Show a message from the local store. Returns FALSE if there is no usable copy. */
static gboolean
open_local_copy (SMailWindow *self, SMailMessage *msg, const char *account)
{
  g_autoptr (GBytes) raw = smail_store_load_message (self->store, account, self->current_folder, msg->uid);
  if (!raw)
    return FALSE;
  SMailMessageDetail *detail = smail_message_detail_parse (msg->uid, raw, NULL);
  if (!detail)
    return FALSE;
  if (!msg->seen) {
    smail_message_set_seen (msg, TRUE);
    adjust_unread (self, -1);
    smail_backend_set_flag (self->current_ctx->backend, self->current_folder, msg->uid, "\\Seen", TRUE,
                            on_seen_stored, NULL);
  }
  adw_navigation_page_set_title (ADW_NAVIGATION_PAGE (self->viewer_page), detail->subject);
  smail_message_view_set_detail (SMAIL_MESSAGE_VIEW (self->viewer), detail);
  gtk_stack_set_visible_child_name (GTK_STACK (self->viewer_stack), "message");
  update_actions (self);
  return TRUE;
}

static void
open_message (SMailWindow *self, SMailMessage *msg)
{
  adw_navigation_split_view_set_show_content (ADW_NAVIGATION_SPLIT_VIEW (self->inner_split), TRUE);
  if (msg == self->current_message && smail_message_view_get_detail (SMAIL_MESSAGE_VIEW (self->viewer)))
    return;

  g_cancellable_cancel (self->fetch_cancellable);
  g_clear_object (&self->fetch_cancellable);
  self->fetch_cancellable = g_cancellable_new ();
  g_set_object (&self->current_message, msg);
  smail_message_view_set_detail (SMAIL_MESSAGE_VIEW (self->viewer), NULL);
  gtk_stack_set_visible_child_name (GTK_STACK (self->viewer_stack), "loading");
  update_actions (self);

  SMailBackend *backend = self->current_ctx->backend;
  const char *account = smail_backend_is_demo (backend) ? NULL : smail_backend_get_account (backend)->email;
  if (account && open_local_copy (self, msg, account))
    return;

  FetchRequest *req = g_new0 (FetchRequest, 1);
  req->self = g_object_ref (self);
  req->msg = g_object_ref (msg);
  req->account = g_strdup (account);
  req->folder = g_strdup (self->current_folder);
  smail_backend_fetch_message (self->current_ctx->backend, self->current_folder, msg->uid,
                                 self->fetch_cancellable, on_message_fetched, req);
}

static void
on_message_row_activated (GtkListBox *box, GtkListBoxRow *row, SMailWindow *self)
{
  SMailMessage *msg = g_object_get_data (G_OBJECT (row), "message");
  if (msg)
    open_message (self, msg);
}

/* ====================================================================== */
/*  New mail: mail log + filters                                          */
/* ====================================================================== */

static const char *
account_key (AccountCtx *ctx)
{
  return smail_backend_is_demo (ctx->backend) ? "demo" : smail_backend_get_account (ctx->backend)->email;
}

typedef struct {
  SMailWindow *self;
  int            pending;
  int            moved;
  int            changed;
} FilterRun;

static void
filter_run_step_done (FilterRun *run)
{
  if (--run->pending > 0)
    return;
  SMailWindow *self = run->self;
  if (run->moved || run->changed) {
    for (guint i = 0; i < self->ctxs->len; i++)
      list_folders (self, g_ptr_array_index (self->ctxs, i));
    SMailFolder *f = current_folder (self);
    if (!self->current_tool && f && g_strcmp0 (f->role, "inbox") == 0)
      load_messages (self);
  }
  g_object_unref (self);
  g_free (run);
}

static void
on_filter_action_done (GObject *source, GAsyncResult *res, gpointer data)
{
  FilterRun *run = data;
  g_autoptr (GError) error = NULL;
  if (!smail_backend_finish_boolean (SMAIL_BACKEND (source), res, &error)) {
    g_autofree char *msg = g_strdup_printf ("A filter failed: %s", error->message);
    toast (run->self, msg);
  }
  filter_run_step_done (run);
}

/* Apply the enabled filters to `messages` (SMailMessage*) in `folder`.
 * Returns how many messages matched at least one rule. */
static int
apply_filters (SMailWindow *self, AccountCtx *ctx, const char *folder, GPtrArray *messages)
{
  g_autoptr (GPtrArray) filters = smail_store_list_filters (self->store);
  if (filters->len == 0)
    return 0;

  FilterRun *run = g_new0 (FilterRun, 1);
  run->self = g_object_ref (self);
  run->pending = 1; /* released at the end */
  int matched = 0;

  for (guint i = 0; i < messages->len; i++) {
    SMailMessage *m = g_ptr_array_index (messages, i);
    gboolean any = FALSE;
    for (guint j = 0; j < filters->len; j++) {
      SMailFilter *f = g_ptr_array_index (filters, j);
      if (!smail_filter_matches (f, m))
        continue;
      any = TRUE;
      run->pending++;
      if (g_str_equal (f->action, "move")) {
        run->moved++;
        smail_backend_move (ctx->backend, folder, m->uid, f->target, on_filter_action_done, run);
      } else if (g_str_equal (f->action, "trash")) {
        run->moved++;
        smail_backend_trash (ctx->backend, folder, m->uid, on_filter_action_done, run);
      } else if (g_str_equal (f->action, "read")) {
        run->changed++;
        smail_message_set_seen (m, TRUE);
        smail_backend_set_flag (ctx->backend, folder, m->uid, "\\Seen", TRUE, on_filter_action_done, run);
      } else if (g_str_equal (f->action, "star")) {
        run->changed++;
        smail_message_set_flagged (m, TRUE);
        smail_backend_set_flag (ctx->backend, folder, m->uid, "\\Flagged", TRUE, on_filter_action_done, run);
      } else {
        run->pending--;
      }
      if (smail_filter_is_final (f))
        break;
    }
    if (any)
      matched++;
  }
  filter_run_step_done (run);
  return matched;
}

typedef struct {
  SMailWindow *self;
  SMailBackend *backend;
  char          *folder;
  gboolean       run_all; /* "Run on Inbox": apply filters to everything */
} InboxCheck;

static void
inbox_check_free (InboxCheck *c)
{
  g_object_unref (c->self);
  g_object_unref (c->backend);
  g_free (c->folder);
  g_free (c);
}

static void
on_inbox_checked (GObject *source, GAsyncResult *res, gpointer data)
{
  InboxCheck *check = data;
  SMailWindow *self = check->self;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) msgs = smail_backend_finish_pointer (SMAIL_BACKEND (source), res, &error);

  AccountCtx *ctx = NULL;
  for (guint i = 0; i < self->ctxs->len; i++)
    if (((AccountCtx *) g_ptr_array_index (self->ctxs, i))->backend == check->backend)
      ctx = g_ptr_array_index (self->ctxs, i);
  if (!msgs || !ctx) {
    if (check->run_all && error)
      toast (self, error->message);
    inbox_check_free (check);
    return;
  }

  if (check->run_all) {
    int n = apply_filters (self, ctx, check->folder, msgs);
    g_autofree char *msg = n ? g_strdup_printf (n == 1 ? "Filters applied to 1 message"
                                                       : "Filters applied to %d messages", n)
                             : g_strdup ("No messages matched your filters");
    toast (self, msg);
    inbox_check_free (check);
    return;
  }

  const char *key = account_key (ctx);
  gboolean known = FALSE;
  gint64 last = smail_store_get_last_uid (self->store, key, check->folder, &known);
  gint64 highest = last;
  const char *account_email = smail_backend_get_account (ctx->backend)->email;
  g_autoptr (GPtrArray) fresh = g_ptr_array_new ();

  for (guint i = 0; i < msgs->len; i++) {
    SMailMessage *m = g_ptr_array_index (msgs, i);
    gint64 uid = g_ascii_strtoll (m->uid, NULL, 10);
    highest = MAX (highest, uid);
    if (known && uid <= last)
      continue;
    /* record every message we haven't seen: the first sync records what's already there */
    SMailMailLogEntry *e = smail_mail_log_from_message (m, account_email, check->folder);
    smail_store_log_mail (self->store, e, NULL);
    smail_mail_log_free (e);
    if (known)
      g_ptr_array_add (fresh, m);
  }
  smail_store_set_last_uid (self->store, key, check->folder, highest);

  if (fresh->len > 0) {
    apply_filters (self, ctx, check->folder, fresh);

    g_autoptr (GNotification) n = NULL;
    if (fresh->len == 1) {
      SMailMessage *m = g_ptr_array_index (fresh, 0);
      n = g_notification_new (smail_message_get_sender (m));
      g_notification_set_body (n, m->subject);
    } else {
      g_autofree char *title = g_strdup_printf ("%u new messages", fresh->len);
      n = g_notification_new (title);
      g_notification_set_body (n, account_email);
    }
    g_notification_set_default_action (n, "app.activate");
    g_application_send_notification (G_APPLICATION (gtk_window_get_application (GTK_WINDOW (self))),
                                     "new-mail", n);
    g_autofree char *msg = g_strdup_printf (fresh->len == 1 ? "1 new message" : "%u new messages", fresh->len);
    toast (self, msg);
  }
  if (g_strcmp0 (self->current_tool, "log") == 0)
    smail_mail_log_page_refresh (self->log_page);
  inbox_check_free (check);
}

static void
start_inbox_check (SMailWindow *self, AccountCtx *ctx, gboolean run_all)
{
  const SMailFolder *inbox = smail_backend_find_role (ctx->backend, "inbox");
  if (!inbox)
    return;
  InboxCheck *check = g_new0 (InboxCheck, 1);
  check->self = g_object_ref (self);
  check->backend = g_object_ref (ctx->backend);
  check->folder = g_strdup (inbox->path);
  check->run_all = run_all;
  smail_backend_list_messages (ctx->backend, inbox->path, MESSAGE_LIMIT, NULL, on_inbox_checked, check);
}

static void
check_new_mail (SMailWindow *self, AccountCtx *ctx)
{
  start_inbox_check (self, ctx, FALSE);
}

static void
run_filters_now (gpointer data)
{
  SMailWindow *self = data;
  AccountCtx *ctx = self->current_ctx ? self->current_ctx
                  : self->ctxs->len ? g_ptr_array_index (self->ctxs, 0) : NULL;
  if (!ctx) {
    toast (self, "Add an email account first");
    return;
  }
  if (!smail_backend_find_role (ctx->backend, "inbox")) {
    toast (self, "This account has no Inbox");
    return;
  }
  start_inbox_check (self, ctx, TRUE);
}

static void
update_filter_folders (SMailWindow *self)
{
  g_autoptr (GStrvBuilder) b = g_strv_builder_new ();
  g_autoptr (GHashTable) seen = g_hash_table_new (g_str_hash, g_str_equal);
  for (guint i = 0; i < self->ctxs->len; i++) {
    AccountCtx *ctx = g_ptr_array_index (self->ctxs, i);
    for (guint j = 0; ctx->folders && j < ctx->folders->len; j++) {
      SMailFolder *f = g_ptr_array_index (ctx->folders, j);
      if (g_strcmp0 (f->role, "inbox") == 0 || g_hash_table_contains (seen, f->path))
        continue;
      g_hash_table_add (seen, f->path);
      g_strv_builder_add (b, f->path);
    }
  }
  g_auto (GStrv) folders = g_strv_builder_end (b);
  smail_filters_page_set_folders (self->filters_page, (const char * const *) folders);
}

/* ====================================================================== */
/*  Reminders                                                             */
/* ====================================================================== */

static gboolean
check_reminders (gpointer data)
{
  SMailWindow *self = data;
  g_autoptr (GPtrArray) due = smail_store_due_reminders (self->store, smail_unix_now ());
  GApplication *app = G_APPLICATION (gtk_window_get_application (GTK_WINDOW (self)));
  for (guint i = 0; i < due->len; i++) {
    SMailReminder *r = g_ptr_array_index (due, i);
    smail_store_mark_fired (self->store, r->id);

    g_autoptr (GNotification) n = g_notification_new ("Reminder");
    g_notification_set_body (n, r->title);
    g_notification_set_priority (n, G_NOTIFICATION_PRIORITY_HIGH);
    g_notification_add_button_with_target_value (n, "Snooze 10 Minutes", "app.snooze-reminder",
                                                 g_variant_new_int64 (r->id));
    g_notification_set_default_action (n, "app.activate");
    g_autofree char *nid = g_strdup_printf ("reminder-%" G_GINT64_FORMAT, r->id);
    if (app)
      g_application_send_notification (app, nid, n);

    g_autofree char *text = g_strdup_printf ("🔔 %s", r->title);
    AdwToast *t = adw_toast_new (text);
    adw_toast_set_timeout (t, 0); /* stays until dismissed */
    adw_toast_set_button_label (t, "Snooze 10 min");
    adw_toast_set_action_name (t, "win.snooze-reminder");
    adw_toast_set_action_target_value (t, g_variant_new_int64 (r->id));
    adw_toast_overlay_add_toast (ADW_TOAST_OVERLAY (self->toast_overlay), t);
  }
  if (due->len && g_strcmp0 (self->current_tool, "reminders") == 0)
    smail_reminders_page_refresh (self->reminders_page);
  return G_SOURCE_CONTINUE;
}

static gboolean
check_reminders_once (gpointer data)
{
  check_reminders (data);
  return G_SOURCE_REMOVE;
}

static void
snooze_activated (GSimpleAction *a, GVariant *param, gpointer data)
{
  SMailWindow *self = data;
  smail_store_snooze_reminder (self->store, g_variant_get_int64 (param), smail_unix_now () + 10 * 60);
  toast (self, "Snoozed for 10 minutes");
  if (g_strcmp0 (self->current_tool, "reminders") == 0)
    smail_reminders_page_refresh (self->reminders_page);
}

/* ====================================================================== */
/*  Local network chat                                                    */
/* ====================================================================== */

static void
on_chat_unread (int unread, gpointer data)
{
  SMailWindow *self = data;
  self->chat_unread = unread;
  for (GtkWidget *row = gtk_widget_get_first_child (self->sidebar); row; row = gtk_widget_get_next_sibling (row))
    if (g_strcmp0 (g_object_get_data (G_OBJECT (row), "tool"), "chat") == 0) {
      GtkWidget *badge = g_object_get_data (G_OBJECT (row), "badge");
      g_autofree char *n = g_strdup_printf ("%d", unread);
      gtk_label_set_text (GTK_LABEL (badge), n);
      gtk_widget_set_visible (badge, unread > 0);
    }
}

static char *
start_lan (SMailWindow *self)
{
  g_autofree char *id = smail_store_get_setting (self->store, "lan-id");
  if (!id) {
    id = g_uuid_string_random ();
    smail_store_set_setting (self->store, "lan-id", id);
  }
  const char *name = self->accounts->len ? ((SMailAccount *) g_ptr_array_index (self->accounts, 0))->name : NULL;
  if (!name || !*name)
    name = g_get_real_name ();
  if (!name || !*name || g_str_equal (name, "Unknown"))
    name = g_get_user_name ();
  self->lan = smail_lan_new (id, name);

  /* SMAIL_LAN_PORT lets two copies run on one computer (for testing) */
  const char *port = g_getenv ("SMAIL_LAN_PORT");
  if (port)
    smail_lan_set_ports (self->lan, SMAIL_LAN_DISCOVERY_PORT, (guint16) atoi (port));

  g_autoptr (GError) error = NULL;
  if (!smail_lan_start (self->lan, &error))
    return g_strdup_printf ("Local chat is off: %s", error->message);
  return NULL;
}

/* ====================================================================== */
/*  Actions                                                               */
/* ====================================================================== */

static void
set_action_enabled (SMailWindow *self, const char *name, gboolean enabled)
{
  GAction *action = g_action_map_lookup_action (G_ACTION_MAP (self), name);
  if (action)
    g_simple_action_set_enabled (G_SIMPLE_ACTION (action), enabled);
}

static void
update_actions (SMailWindow *self)
{
  gboolean has_msg = self->current_message != NULL;
  gboolean has_detail = smail_message_view_get_detail (SMAIL_MESSAGE_VIEW (self->viewer)) != NULL;
  gboolean has_account = self->ctxs->len > 0;

  set_action_enabled (self, "compose", has_account);
  set_action_enabled (self, "refresh", has_account);
  set_action_enabled (self, "search", has_account);
  set_action_enabled (self, "reply", has_detail);
  set_action_enabled (self, "reply-all", has_detail);
  set_action_enabled (self, "forward", has_detail);
  set_action_enabled (self, "archive", has_msg);
  set_action_enabled (self, "trash", has_msg);
  set_action_enabled (self, "toggle-star", has_msg);
  set_action_enabled (self, "toggle-read", has_msg);
  set_action_enabled (self, "edit-account", has_account && !self->demo);
  set_action_enabled (self, "remove-account", has_account && !self->demo);

  if (has_msg) {
    gtk_button_set_icon_name (GTK_BUTTON (self->star_button),
                              self->current_message->flagged ? "starred-symbolic" : "non-starred-symbolic");
    gtk_widget_set_tooltip_text (self->star_button, self->current_message->flagged ? "Unstar (S)" : "Star (S)");
    gtk_button_set_icon_name (GTK_BUTTON (self->read_button),
                              self->current_message->seen ? "mail-unread-symbolic" : "mail-read-symbolic");
    gtk_widget_set_tooltip_text (self->read_button,
                                 self->current_message->seen ? "Mark as Unread (U)" : "Mark as Read (U)");
  }
}

static void
on_composer_sent (SMailComposer *composer, GBytes *message, SMailWindow *self)
{
  toast (self, "Message sent");

  /* save what was sent: details in the mail log, the full message as .eml */
  const SMailAccount *account = smail_backend_get_account (smail_composer_get_backend (composer));
  g_autofree char *data_dir = smail_store_data_dir ();
  g_autofree char *eml_dir = g_build_filename (data_dir, "sent-mail", NULL);
  g_autoptr (GError) error = NULL;
  SMailMailLogEntry *entry = smail_mail_log_from_sent (message, account->email, eml_dir, &error);
  if (!entry || !smail_store_log_mail (self->store, entry, &error))
    g_warning ("Could not record sent mail: %s", error ? error->message : "?");
  smail_mail_log_free (entry);
  if (g_strcmp0 (self->current_tool, "log") == 0)
    smail_mail_log_page_refresh (self->log_page);
  SMailFolder *f = current_folder (self);
  if (f && f->role && g_str_equal (f->role, "sent"))
    load_messages (self);
}

static void
open_composer (SMailWindow *self, SMailComposeMode mode)
{
  AccountCtx *ctx = self->current_ctx;
  if (!ctx && self->ctxs->len)
    ctx = g_ptr_array_index (self->ctxs, 0);
  if (!ctx)
    return;
  const SMailMessageDetail *detail = mode == SMAIL_COMPOSE_NEW ? NULL
    : smail_message_view_get_detail (SMAIL_MESSAGE_VIEW (self->viewer));
  SMailComposer *composer = smail_composer_new (GTK_WINDOW (self), ctx->backend, mode, detail);
  g_signal_connect_object (composer, "sent", G_CALLBACK (on_composer_sent), self, 0);
  gtk_window_present (GTK_WINDOW (composer));
}

static void compose_activated   (GSimpleAction *a, GVariant *p, gpointer d) { open_composer (d, SMAIL_COMPOSE_NEW); }
static void reply_activated     (GSimpleAction *a, GVariant *p, gpointer d) { open_composer (d, SMAIL_COMPOSE_REPLY); }
static void reply_all_activated (GSimpleAction *a, GVariant *p, gpointer d) { open_composer (d, SMAIL_COMPOSE_REPLY_ALL); }
static void forward_activated   (GSimpleAction *a, GVariant *p, gpointer d) { open_composer (d, SMAIL_COMPOSE_FORWARD); }

/* --- flags --- */

typedef struct {
  SMailWindow  *self;
  SMailMessage *msg;
  gboolean        star; /* else: seen */
  gboolean        value;
} FlagRequest;

static void
on_flag_set (GObject *source, GAsyncResult *res, gpointer data)
{
  FlagRequest *req = data;
  g_autoptr (GError) error = NULL;
  if (!smail_backend_finish_boolean (SMAIL_BACKEND (source), res, &error)) {
    /* roll back the optimistic change */
    if (req->star) {
      smail_message_set_flagged (req->msg, !req->value);
    } else {
      smail_message_set_seen (req->msg, !req->value);
      adjust_unread (req->self, req->value ? 1 : -1);
    }
    update_actions (req->self);
    toast (req->self, error->message);
  }
  g_object_unref (req->self);
  g_object_unref (req->msg);
  g_free (req);
}

static void
set_flag (SMailWindow *self, gboolean star)
{
  SMailMessage *msg = self->current_message;
  if (!msg)
    return;
  FlagRequest *req = g_new0 (FlagRequest, 1);
  req->self = g_object_ref (self);
  req->msg = g_object_ref (msg);
  req->star = star;
  if (star) {
    req->value = !msg->flagged;
    smail_message_set_flagged (msg, req->value);
  } else {
    req->value = !msg->seen;
    smail_message_set_seen (msg, req->value);
    adjust_unread (self, req->value ? -1 : 1);
  }
  update_actions (self);
  smail_backend_set_flag (self->current_ctx->backend, self->current_folder, msg->uid,
                            star ? "\\Flagged" : "\\Seen", req->value, on_flag_set, req);
}

static void star_activated (GSimpleAction *a, GVariant *p, gpointer d) { set_flag (d, TRUE); }
static void read_activated (GSimpleAction *a, GVariant *p, gpointer d) { set_flag (d, FALSE); }

/* --- archive / trash --- */

typedef struct {
  SMailWindow *self;
  char          *done_text;
} MoveRequest;

static void
on_moved (GObject *source, GAsyncResult *res, gpointer data)
{
  MoveRequest *req = data;
  SMailWindow *self = req->self;
  g_autoptr (GError) error = NULL;
  if (smail_backend_finish_boolean (SMAIL_BACKEND (source), res, &error)) {
    toast (self, req->done_text);
  } else {
    toast (self, error->message);
    load_messages (self); /* put the message back */
  }
  /* counters for the destination folder changed too */
  for (guint i = 0; i < self->ctxs->len; i++)
    if (((AccountCtx *) g_ptr_array_index (self->ctxs, i))->backend == SMAIL_BACKEND (source))
      list_folders (self, g_ptr_array_index (self->ctxs, i));
  g_object_unref (req->self);
  g_free (req->done_text);
  g_free (req);
}

static void
move_current (SMailWindow *self, gboolean archive)
{
  SMailMessage *msg = self->current_message;
  if (!msg)
    return;
  g_autoptr (SMailMessage) keep = g_object_ref (msg);

  /* remove from the list right away and move selection to the next message */
  guint pos;
  if (g_list_store_find (self->messages, msg, &pos)) {
    if (!msg->seen)
      adjust_unread (self, -1);
    g_list_store_remove (self->messages, pos);
  }
  clear_viewer (self);
  guint n = g_list_model_get_n_items (G_LIST_MODEL (self->messages));
  if (n == 0) {
    show_empty_folder (self);
    adw_navigation_split_view_set_show_content (ADW_NAVIGATION_SPLIT_VIEW (self->inner_split), FALSE);
  } else if (!adw_navigation_split_view_get_collapsed (ADW_NAVIGATION_SPLIT_VIEW (self->inner_split))) {
    GtkListBoxRow *row = gtk_list_box_get_row_at_index (GTK_LIST_BOX (self->message_list), MIN (pos, n - 1));
    if (row) {
      gtk_list_box_select_row (GTK_LIST_BOX (self->message_list), row);
      open_message (self, g_object_get_data (G_OBJECT (row), "message"));
    }
  } else {
    adw_navigation_split_view_set_show_content (ADW_NAVIGATION_SPLIT_VIEW (self->inner_split), FALSE);
  }

  SMailFolder *f = current_folder (self);
  const SMailFolder *trash = smail_backend_find_role (self->current_ctx->backend, "trash");
  gboolean in_trash = f && f->role && g_str_equal (f->role, "trash");

  MoveRequest *req = g_new0 (MoveRequest, 1);
  req->self = g_object_ref (self);
  if (archive) {
    req->done_text = g_strdup ("Message archived");
    smail_backend_archive (self->current_ctx->backend, self->current_folder, keep->uid, on_moved, req);
  } else {
    req->done_text = g_strdup (!trash || in_trash ? "Message deleted" : "Message moved to Trash");
    smail_backend_trash (self->current_ctx->backend, self->current_folder, keep->uid, on_moved, req);
  }
}

static void archive_activated (GSimpleAction *a, GVariant *p, gpointer d) { move_current (d, TRUE); }
static void trash_activated   (GSimpleAction *a, GVariant *p, gpointer d) { move_current (d, FALSE); }

/* --- refresh / search --- */

static void
refresh_all (SMailWindow *self)
{
  for (guint i = 0; i < self->ctxs->len; i++)
    list_folders (self, g_ptr_array_index (self->ctxs, i));
  load_messages (self);
}

static void refresh_activated (GSimpleAction *a, GVariant *p, gpointer d) { refresh_all (d); }

static gboolean
auto_refresh (gpointer data)
{
  refresh_all (data);
  return G_SOURCE_CONTINUE;
}

static void
search_activated (GSimpleAction *a, GVariant *p, gpointer data)
{
  SMailWindow *self = data;
  gboolean on = !gtk_search_bar_get_search_mode (GTK_SEARCH_BAR (self->search_bar));
  adw_navigation_split_view_set_show_content (ADW_NAVIGATION_SPLIT_VIEW (self->outer_split), TRUE);
  adw_navigation_split_view_set_show_content (ADW_NAVIGATION_SPLIT_VIEW (self->inner_split), FALSE);
  gtk_search_bar_set_search_mode (GTK_SEARCH_BAR (self->search_bar), on);
  if (on)
    gtk_widget_grab_focus (self->search_entry);
}

/* --- accounts --- */

static void
on_account_saved (const SMailAccount *account, gpointer data)
{
  SMailWindow *self = data;
  gboolean replaced = FALSE;
  for (guint i = 0; i < self->accounts->len; i++) {
    SMailAccount *a = g_ptr_array_index (self->accounts, i);
    if (g_str_equal (a->id, account->id)) {
      g_ptr_array_index (self->accounts, i) = smail_account_copy (account);
      smail_account_free (a);
      replaced = TRUE;
    }
  }
  if (!replaced)
    g_ptr_array_add (self->accounts, smail_account_copy (account));

  g_autoptr (GError) error = NULL;
  if (!smail_accounts_save (self->accounts, &error))
    toast (self, error->message);
  g_clear_error (&error);
  if (!smail_accounts_store_password (account, &error)) {
    g_warning ("keyring: %s", error->message);
    toast (self, "The password couldn't be saved to the keyring. Next time, enter it again in Account Settings.");
  }

  self->demo = FALSE;
  reload_accounts (self);
}

static void
add_account_activated (GSimpleAction *a, GVariant *p, gpointer data)
{
  smail_account_dialog_present (GTK_WIDGET (data), NULL, on_account_saved, data);
}

static const SMailAccount *
current_saved_account (SMailWindow *self)
{
  if (!self->current_ctx)
    return self->accounts->len ? g_ptr_array_index (self->accounts, 0) : NULL;
  const char *id = smail_backend_get_account (self->current_ctx->backend)->id;
  for (guint i = 0; i < self->accounts->len; i++) {
    SMailAccount *acc = g_ptr_array_index (self->accounts, i);
    if (g_str_equal (acc->id, id))
      return acc;
  }
  return NULL;
}

static void
edit_account_activated (GSimpleAction *a, GVariant *p, gpointer data)
{
  SMailWindow *self = data;
  const SMailAccount *acc = current_saved_account (self);
  if (acc)
    smail_account_dialog_present (GTK_WIDGET (self), acc, on_account_saved, self);
}

static void
on_remove_response (AdwAlertDialog *dialog, const char *response, SMailWindow *self)
{
  if (!g_str_equal (response, "remove"))
    return;
  const SMailAccount *acc = current_saved_account (self);
  if (!acc)
    return;
  smail_accounts_clear_password (acc->id);
  g_ptr_array_remove (self->accounts, (gpointer) acc);
  g_autoptr (GError) error = NULL;
  if (!smail_accounts_save (self->accounts, &error))
    toast (self, error->message);
  reload_accounts (self);
}

static void
remove_account_activated (GSimpleAction *a, GVariant *p, gpointer data)
{
  SMailWindow *self = data;
  const SMailAccount *acc = current_saved_account (self);
  if (!acc)
    return;
  g_autofree char *body = g_strdup_printf ("%s will be removed from SMail. Mail on the server is not deleted.",
                                           acc->email);
  AdwDialog *alert = adw_alert_dialog_new ("Remove Account?", body);
  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (alert), "cancel", "_Cancel", "remove", "_Remove", NULL);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (alert), "remove", ADW_RESPONSE_DESTRUCTIVE);
  g_signal_connect (alert, "response", G_CALLBACK (on_remove_response), self);
  adw_dialog_present (alert, GTK_WIDGET (self));
}

static void
skip_welcome_activated (GSimpleAction *a, GVariant *p, gpointer data)
{
  SMailWindow *self = data;
  self->skip_welcome = TRUE;
  smail_store_set_setting (self->store, "skip-welcome", "1");
  gtk_stack_set_visible_child_name (GTK_STACK (self->main_stack), "mail");
  rebuild_sidebar (self);
  show_tool (self, "calendar");
}

static void
try_demo_activated (GSimpleAction *a, GVariant *p, gpointer data)
{
  SMailWindow *self = data;
  self->demo = TRUE;
  reload_accounts (self);
}

/* --- style --- */

static void
theme_change_state (GSimpleAction *action, GVariant *value, gpointer data)
{
  SMailWindow *self = data;
  const char *name = g_variant_get_string (value, NULL);
  if (!smail_theme_is_valid (name))
    return;
  smail_theme_apply (name);
  smail_store_set_setting (self->store, "theme", name);
  g_simple_action_set_state (action, value);
}

static void
sync_theme_from_store (SMailWindow *self)
{
  g_autofree char *theme = smail_store_get_setting (self->store, "theme");
  if (!smail_theme_is_valid (theme)) {
    g_free (theme);
    theme = g_strdup (SMAIL_THEME_SYSTEM);
  }
  smail_theme_apply (theme);
  GAction *action = g_action_map_lookup_action (G_ACTION_MAP (self), "theme");
  if (action)
    g_simple_action_set_state (G_SIMPLE_ACTION (action), g_variant_new_string (theme));
}

/* --- backup & restore --- */

static void
on_restored (GPtrArray *accounts, gboolean had_passwords, gpointer data)
{
  SMailWindow *self = data;

  /* forget passwords of accounts that are going away */
  for (guint i = 0; i < self->accounts->len; i++)
    smail_accounts_clear_password (((SMailAccount *) g_ptr_array_index (self->accounts, i))->id);
  g_ptr_array_set_size (self->accounts, 0);

  gboolean keyring_failed = FALSE;
  for (guint i = 0; i < accounts->len; i++) {
    SMailAccount *a = smail_account_copy (g_ptr_array_index (accounts, i));
    if (a->password && *a->password && !smail_accounts_store_password (a, NULL))
      keyring_failed = TRUE;
    g_ptr_array_add (self->accounts, a);
  }
  g_autoptr (GError) error = NULL;
  if (!smail_accounts_save (self->accounts, &error))
    toast (self, error->message);

  self->demo = FALSE;
  g_autofree char *skip = smail_store_get_setting (self->store, "skip-welcome");
  self->skip_welcome = g_strcmp0 (skip, "1") == 0;
  sync_theme_from_store (self);
  reload_accounts (self);
  if (self->current_tool)
    show_tool (self, self->current_tool);

  if (accounts->len && !had_passwords)
    toast (self, "Backup restored. Enter your passwords in Account Settings to connect.");
  else if (keyring_failed)
    toast (self, "Backup restored, but passwords couldn't be saved to the keyring");
  else
    toast (self, "Backup restored");
}

static void
backup_activated (GSimpleAction *a, GVariant *p, gpointer data)
{
  SMailWindow *self = data;
  smail_backup_dialog_present (GTK_WIDGET (self), self->store, self->accounts, on_restored, self);
}

static const GActionEntry win_actions[] = {
  { "theme",          NULL, "s", "'system'", theme_change_state },
  { "backup",         backup_activated },
  { "compose",        compose_activated },
  { "reply",          reply_activated },
  { "reply-all",      reply_all_activated },
  { "forward",        forward_activated },
  { "archive",        archive_activated },
  { "trash",          trash_activated },
  { "toggle-star",    star_activated },
  { "toggle-read",    read_activated },
  { "refresh",        refresh_activated },
  { "search",         search_activated },
  { "add-account",    add_account_activated },
  { "edit-account",   edit_account_activated },
  { "remove-account", remove_account_activated },
  { "try-demo",       try_demo_activated },
  { "skip-welcome",   skip_welcome_activated },
  { "snooze-reminder", snooze_activated, "x" },
};

/* ====================================================================== */
/*  Accounts lifecycle                                                    */
/* ====================================================================== */

static void
reload_accounts (SMailWindow *self)
{
  g_list_store_remove_all (self->messages);
  clear_viewer (self);
  self->current_ctx = NULL;
  g_clear_pointer (&self->current_folder, g_free);
  g_ptr_array_set_size (self->ctxs, 0);

  if (self->demo) {
    AccountCtx *ctx = g_new0 (AccountCtx, 1);
    ctx->backend = smail_backend_new_demo ();
    g_ptr_array_add (self->ctxs, ctx);
  } else {
    for (guint i = 0; i < self->accounts->len; i++) {
      AccountCtx *ctx = g_new0 (AccountCtx, 1);
      ctx->backend = smail_backend_new_imap (g_ptr_array_index (self->accounts, i));
      g_ptr_array_add (self->ctxs, ctx);
    }
  }

  gtk_stack_set_visible_child_name (GTK_STACK (self->main_stack),
                                    self->ctxs->len || self->skip_welcome ? "mail" : "welcome");
  gtk_stack_set_visible_child_name (GTK_STACK (self->list_stack), "loading");
  adw_navigation_page_set_title (ADW_NAVIGATION_PAGE (self->list_page), "Mail");
  adw_window_title_set_title (ADW_WINDOW_TITLE (self->list_title), "Mail");
  adw_window_title_set_subtitle (ADW_WINDOW_TITLE (self->list_title), NULL);
  rebuild_sidebar (self);
  for (guint i = 0; i < self->ctxs->len; i++)
    list_folders (self, g_ptr_array_index (self->ctxs, i));
  update_actions (self);
}

/* ====================================================================== */
/*  UI construction                                                       */
/* ====================================================================== */

static GtkWidget *
header_button (const char *icon, const char *tooltip, const char *action)
{
  GtkWidget *b = gtk_button_new_from_icon_name (icon);
  gtk_widget_set_tooltip_text (b, tooltip);
  gtk_actionable_set_action_name (GTK_ACTIONABLE (b), action);
  return b;
}

static GMenuModel *
primary_menu (void)
{
  GMenu *menu = g_menu_new ();
  GMenu *accounts = g_menu_new ();
  g_menu_append (accounts, "_Add Account…", "win.add-account");
  g_menu_append (accounts, "Account _Settings…", "win.edit-account");
  g_menu_append (accounts, "_Remove Account…", "win.remove-account");
  g_menu_append_section (menu, NULL, G_MENU_MODEL (accounts));
  GMenu *style = g_menu_new ();
  g_menu_append (style, "_System Style", "win.theme::system");
  g_menu_append (style, "Matte _White", "win.theme::matte-white");
  g_menu_append (style, "Matte _Black", "win.theme::matte-black");
  g_menu_append_section (menu, "Style", G_MENU_MODEL (style));
  g_object_unref (style);
  GMenu *data = g_menu_new ();
  g_menu_append (data, "_Backup & Restore…", "win.backup");
  g_menu_append_section (menu, NULL, G_MENU_MODEL (data));
  g_object_unref (data);
  GMenu *app = g_menu_new ();
  g_menu_append (app, "_Keyboard Shortcuts", "app.shortcuts");
  g_menu_append (app, "_About SMail", "app.about");
  g_menu_append_section (menu, NULL, G_MENU_MODEL (app));
  g_object_unref (accounts);
  g_object_unref (app);
  return G_MENU_MODEL (menu);
}

static GtkWidget *
build_sidebar_page (SMailWindow *self)
{
  GtkWidget *toolbar = adw_toolbar_view_new ();
  GtkWidget *header = adw_header_bar_new ();

  GtkWidget *compose = header_button ("mail-message-new-symbolic", "Compose (Ctrl+N)", "win.compose");
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), compose);

  GtkWidget *menu = gtk_menu_button_new ();
  gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (menu), "open-menu-symbolic");
  gtk_menu_button_set_primary (GTK_MENU_BUTTON (menu), TRUE);
  gtk_widget_set_tooltip_text (menu, "Main Menu");
  g_autoptr (GMenuModel) model = primary_menu ();
  gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (menu), model);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), menu);
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), header);

  self->sidebar = gtk_list_box_new ();
  gtk_widget_add_css_class (self->sidebar, "navigation-sidebar");
  g_signal_connect (self->sidebar, "row-activated", G_CALLBACK (on_sidebar_row_activated), self);
  GtkWidget *scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), self->sidebar);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), scroll);

  AdwNavigationPage *page = adw_navigation_page_new (toolbar, "SMail");
  return GTK_WIDGET (page);
}

static GtkWidget *
build_list_page (SMailWindow *self)
{
  GtkWidget *toolbar = adw_toolbar_view_new ();
  GtkWidget *header = adw_header_bar_new ();
  self->list_title = adw_window_title_new ("Mail", NULL);
  adw_header_bar_set_title_widget (ADW_HEADER_BAR (header), self->list_title);

  self->refresh_button = header_button ("view-refresh-symbolic", "Refresh (F5)", "win.refresh");
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), self->refresh_button);
  GtkWidget *search = gtk_toggle_button_new ();
  gtk_button_set_icon_name (GTK_BUTTON (search), "system-search-symbolic");
  gtk_widget_set_tooltip_text (search, "Search (Ctrl+F)");
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), search);
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), header);

  self->search_entry = gtk_search_entry_new ();
  gtk_search_entry_set_placeholder_text (GTK_SEARCH_ENTRY (self->search_entry), "Search messages");
  gtk_widget_set_hexpand (self->search_entry, TRUE);
  g_signal_connect (self->search_entry, "search-changed", G_CALLBACK (on_search_changed), self);
  self->search_bar = gtk_search_bar_new ();
  gtk_search_bar_set_child (GTK_SEARCH_BAR (self->search_bar), self->search_entry);
  gtk_search_bar_connect_entry (GTK_SEARCH_BAR (self->search_bar), GTK_EDITABLE (self->search_entry));
  g_object_bind_property (search, "active", self->search_bar, "search-mode-enabled",
                          G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), self->search_bar);

  /* list */
  self->filter = GTK_FILTER (gtk_custom_filter_new (filter_message, self, NULL));
  GtkFilterListModel *filtered = gtk_filter_list_model_new (G_LIST_MODEL (g_object_ref (self->messages)),
                                                            g_object_ref (self->filter));
  self->message_list = gtk_list_box_new ();
  gtk_widget_add_css_class (self->message_list, "message-list");
  gtk_list_box_bind_model (GTK_LIST_BOX (self->message_list), G_LIST_MODEL (filtered),
                           create_message_row, self, NULL);
  g_object_unref (filtered);
  g_signal_connect (self->message_list, "row-activated", G_CALLBACK (on_message_row_activated), self);
  GtkWidget *scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), self->message_list);

  GtkWidget *spinner = gtk_spinner_new ();
  gtk_spinner_set_spinning (GTK_SPINNER (spinner), TRUE);
  gtk_widget_set_size_request (spinner, 32, 32);
  gtk_widget_set_halign (spinner, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (spinner, GTK_ALIGN_CENTER);

  self->list_empty = adw_status_page_new ();
  gtk_widget_add_css_class (self->list_empty, "compact");

  self->list_stack = gtk_stack_new ();
  gtk_stack_add_named (GTK_STACK (self->list_stack), spinner, "loading");
  gtk_stack_add_named (GTK_STACK (self->list_stack), self->list_empty, "error");
  gtk_stack_add_named (GTK_STACK (self->list_stack), scroll, "list");
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), self->list_stack);

  self->list_page = GTK_WIDGET (adw_navigation_page_new (toolbar, "Mail"));
  return self->list_page;
}

static GtkWidget *
build_viewer_page (SMailWindow *self)
{
  GtkWidget *toolbar = adw_toolbar_view_new ();
  GtkWidget *header = adw_header_bar_new ();
  adw_header_bar_set_show_title (ADW_HEADER_BAR (header), FALSE);

  adw_header_bar_pack_start (ADW_HEADER_BAR (header),
                             header_button ("mail-reply-sender-symbolic", "Reply (Ctrl+R)", "win.reply"));
  self->reply_all_button = header_button ("mail-reply-all-symbolic", "Reply All (Ctrl+Shift+R)", "win.reply-all");
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), self->reply_all_button);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header),
                             header_button ("mail-forward-symbolic", "Forward (Ctrl+L)", "win.forward"));

  adw_header_bar_pack_end (ADW_HEADER_BAR (header),
                           header_button ("user-trash-symbolic", "Move to Trash (Delete)", "win.trash"));
  adw_header_bar_pack_end (ADW_HEADER_BAR (header),
                           header_button ("folder-documents-symbolic", "Archive (A)", "win.archive"));
  self->star_button = header_button ("non-starred-symbolic", "Star (S)", "win.toggle-star");
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), self->star_button);
  self->read_button = header_button ("mail-unread-symbolic", "Mark as Unread (U)", "win.toggle-read");
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), self->read_button);
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), header);

  GtkWidget *empty = adw_status_page_new ();
  adw_status_page_set_icon_name (ADW_STATUS_PAGE (empty), "mail-unread-symbolic");
  adw_status_page_set_title (ADW_STATUS_PAGE (empty), "No Message Selected");
  adw_status_page_set_description (ADW_STATUS_PAGE (empty), "Select a message to read it here");

  GtkWidget *spinner = gtk_spinner_new ();
  gtk_spinner_set_spinning (GTK_SPINNER (spinner), TRUE);
  gtk_widget_set_size_request (spinner, 32, 32);
  gtk_widget_set_halign (spinner, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (spinner, GTK_ALIGN_CENTER);

  self->viewer = smail_message_view_new ();
  self->viewer_stack = gtk_stack_new ();
  gtk_stack_add_named (GTK_STACK (self->viewer_stack), empty, "empty");
  gtk_stack_add_named (GTK_STACK (self->viewer_stack), spinner, "loading");
  gtk_stack_add_named (GTK_STACK (self->viewer_stack), self->viewer, "message");
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), self->viewer_stack);

  self->viewer_page = GTK_WIDGET (adw_navigation_page_new (toolbar, "Message"));
  return self->viewer_page;
}

static GtkWidget *
build_welcome (SMailWindow *self)
{
  GtkWidget *toolbar = adw_toolbar_view_new ();
  GtkWidget *header = adw_header_bar_new ();
  adw_header_bar_set_title_widget (ADW_HEADER_BAR (header), gtk_label_new (NULL));
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), header);

  GtkWidget *status = adw_status_page_new ();
  adw_status_page_set_icon_name (ADW_STATUS_PAGE (status), "mail-send-symbolic");
  adw_status_page_set_title (ADW_STATUS_PAGE (status), "Welcome to SMail");
  adw_status_page_set_description (ADW_STATUS_PAGE (status),
                                   "Connect an email account to read and send mail");

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_halign (box, GTK_ALIGN_CENTER);
  GtkWidget *add = gtk_button_new_with_mnemonic ("_Add Account");
  gtk_actionable_set_action_name (GTK_ACTIONABLE (add), "win.add-account");
  gtk_widget_add_css_class (add, "pill");
  gtk_widget_add_css_class (add, "suggested-action");
  GtkWidget *demo = gtk_button_new_with_mnemonic ("_Try the Demo");
  gtk_actionable_set_action_name (GTK_ACTIONABLE (demo), "win.try-demo");
  gtk_widget_add_css_class (demo, "pill");
  GtkWidget *skip = gtk_button_new_with_mnemonic ("Use Calendar, Tasks & _Chat Without Email");
  gtk_actionable_set_action_name (GTK_ACTIONABLE (skip), "win.skip-welcome");
  gtk_widget_add_css_class (skip, "flat");
  gtk_box_append (GTK_BOX (box), add);
  gtk_box_append (GTK_BOX (box), demo);
  gtk_box_append (GTK_BOX (box), skip);
  adw_status_page_set_child (ADW_STATUS_PAGE (status), box);

  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), status);
  return toolbar;
}

static void
smail_window_dispose (GObject *object)
{
  SMailWindow *self = SMAIL_WINDOW (object);
  g_clear_handle_id (&self->refresh_source, g_source_remove);
  g_clear_handle_id (&self->reminder_source, g_source_remove);
  if (self->lan)
    smail_lan_stop (self->lan);
  g_cancellable_cancel (self->list_cancellable);
  g_cancellable_cancel (self->fetch_cancellable);
  G_OBJECT_CLASS (smail_window_parent_class)->dispose (object);
}

static void
smail_window_finalize (GObject *object)
{
  SMailWindow *self = SMAIL_WINDOW (object);
  g_ptr_array_unref (self->ctxs);
  g_ptr_array_unref (self->accounts);
  g_clear_object (&self->messages);
  g_clear_object (&self->filter);
  g_clear_object (&self->current_message);
  g_clear_object (&self->list_cancellable);
  g_clear_object (&self->fetch_cancellable);
  g_free (self->current_folder);
  g_free (self->current_tool);
  g_clear_object (&self->lan);
  smail_store_close (self->store);
  G_OBJECT_CLASS (smail_window_parent_class)->finalize (object);
}

static void
smail_window_class_init (SMailWindowClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = smail_window_dispose;
  G_OBJECT_CLASS (klass)->finalize = smail_window_finalize;
}

static void
smail_window_init (SMailWindow *self)
{
  self->ctxs = g_ptr_array_new_with_free_func ((GDestroyNotify) account_ctx_free);
  self->messages = g_list_store_new (SMAIL_TYPE_MESSAGE);
  self->accounts = smail_accounts_load ();

  g_autoptr (GError) store_error = NULL;
  self->store = smail_store_open (NULL, &store_error);
  if (!self->store) {
    g_warning ("%s — calendar, tasks and logs will not be saved this session", store_error->message);
    self->store = smail_store_open (":memory:", NULL);
  }
  g_autofree char *skip = smail_store_get_setting (self->store, "skip-welcome");
  self->skip_welcome = g_strcmp0 (skip, "1") == 0;

  gtk_window_set_title (GTK_WINDOW (self), "SMail");
  gtk_window_set_default_size (GTK_WINDOW (self), 1180, 760);
  gtk_widget_set_size_request (GTK_WIDGET (self), 360, 300);
  g_action_map_add_action_entries (G_ACTION_MAP (self), win_actions, G_N_ELEMENTS (win_actions), self);

  /* folders | (list | viewer) — libadwaita's triple pane pattern */
  self->inner_split = adw_navigation_split_view_new ();
  adw_navigation_split_view_set_sidebar (ADW_NAVIGATION_SPLIT_VIEW (self->inner_split),
                                         ADW_NAVIGATION_PAGE (build_list_page (self)));
  adw_navigation_split_view_set_content (ADW_NAVIGATION_SPLIT_VIEW (self->inner_split),
                                         ADW_NAVIGATION_PAGE (build_viewer_page (self)));
  adw_navigation_split_view_set_min_sidebar_width (ADW_NAVIGATION_SPLIT_VIEW (self->inner_split), 300);
  adw_navigation_split_view_set_max_sidebar_width (ADW_NAVIGATION_SPLIT_VIEW (self->inner_split), 420);
  adw_navigation_split_view_set_sidebar_width_fraction (ADW_NAVIGATION_SPLIT_VIEW (self->inner_split), 0.4);

  self->outer_split = adw_navigation_split_view_new ();
  adw_navigation_split_view_set_sidebar (ADW_NAVIGATION_SPLIT_VIEW (self->outer_split),
                                         ADW_NAVIGATION_PAGE (build_sidebar_page (self)));
  /* the content area shows the mail panes or one of the organizer pages */
  g_autofree char *lan_error = start_lan (self);
  self->calendar_page = smail_calendar_page_new (self->store);
  self->tasks_page = smail_tasks_page_new (self->store);
  self->reminders_page = smail_reminders_page_new (self->store);
  self->chat_page = smail_chat_page_new (self->store, self->lan, lan_error);
  smail_chat_page_set_unread_func (self->chat_page, on_chat_unread, self);
  self->log_page = smail_mail_log_page_new (self->store);
  self->filters_page = smail_filters_page_new (self->store, run_filters_now, self);

  self->content_stack = gtk_stack_new ();
  gtk_stack_add_named (GTK_STACK (self->content_stack), self->inner_split, "mail");
  gtk_stack_add_named (GTK_STACK (self->content_stack), self->calendar_page, "calendar");
  gtk_stack_add_named (GTK_STACK (self->content_stack), self->tasks_page, "tasks");
  gtk_stack_add_named (GTK_STACK (self->content_stack), self->reminders_page, "reminders");
  gtk_stack_add_named (GTK_STACK (self->content_stack), self->chat_page, "chat");
  gtk_stack_add_named (GTK_STACK (self->content_stack), self->log_page, "log");
  gtk_stack_add_named (GTK_STACK (self->content_stack), self->filters_page, "filters");
  self->content_page = GTK_WIDGET (adw_navigation_page_new (self->content_stack, "Mail"));
  adw_navigation_split_view_set_content (ADW_NAVIGATION_SPLIT_VIEW (self->outer_split),
                                         ADW_NAVIGATION_PAGE (self->content_page));
  adw_navigation_split_view_set_min_sidebar_width (ADW_NAVIGATION_SPLIT_VIEW (self->outer_split), 200);
  adw_navigation_split_view_set_max_sidebar_width (ADW_NAVIGATION_SPLIT_VIEW (self->outer_split), 260);

  self->main_stack = gtk_stack_new ();
  gtk_stack_set_transition_type (GTK_STACK (self->main_stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  gtk_stack_add_named (GTK_STACK (self->main_stack), build_welcome (self), "welcome");
  gtk_stack_add_named (GTK_STACK (self->main_stack), self->outer_split, "mail");

  self->toast_overlay = adw_toast_overlay_new ();
  adw_toast_overlay_set_child (ADW_TOAST_OVERLAY (self->toast_overlay), self->main_stack);
  adw_application_window_set_content (ADW_APPLICATION_WINDOW (self), self->toast_overlay);

  /* adaptive breakpoints */
  AdwBreakpoint *bp = adw_breakpoint_new (adw_breakpoint_condition_parse ("max-width: 860sp"));
  adw_breakpoint_add_setters (bp,
                              G_OBJECT (self->outer_split), "collapsed", TRUE,
                              G_OBJECT (self->inner_split), "sidebar-width-fraction", 0.33,
                              NULL);
  adw_application_window_add_breakpoint (ADW_APPLICATION_WINDOW (self), bp);

  bp = adw_breakpoint_new (adw_breakpoint_condition_parse ("max-width: 500sp"));
  adw_breakpoint_add_setters (bp,
                              G_OBJECT (self->outer_split), "collapsed", TRUE,
                              G_OBJECT (self->inner_split), "collapsed", TRUE,
                              G_OBJECT (self->reply_all_button), "visible", FALSE,
                              G_OBJECT (self->read_button), "visible", FALSE,
                              G_OBJECT (smail_chat_page_get_split (self->chat_page)), "collapsed", TRUE,
                              NULL);
  adw_application_window_add_breakpoint (ADW_APPLICATION_WINDOW (self), bp);

  self->refresh_source = g_timeout_add_seconds (AUTO_REFRESH_SECONDS, auto_refresh, self);
  self->reminder_source = g_timeout_add_seconds (REMINDER_CHECK_SECONDS, check_reminders, self);
  g_idle_add_full (G_PRIORITY_LOW, check_reminders_once, g_object_ref (self), g_object_unref);
}

SMailWindow *
smail_window_new (AdwApplication *app, gboolean demo)
{
  SMailWindow *self = g_object_new (SMAIL_TYPE_WINDOW, "application", app, NULL);
  self->demo = demo;
  sync_theme_from_store (self);
  if (demo) /* the demo mailbox is new every time: record it from scratch */
    smail_store_set_last_uid (self->store, "demo", "INBOX", 0);
  reload_accounts (self);
  return self;
}
