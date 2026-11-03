/* courier-mail-tools.c — Filters and Mail Log pages */
#include "courier-mail-tools.h"
#include "courier-automation.h"
#include "courier-ui.h"

#include <string.h>

/* =====================================================================
 *  Filters
 * ===================================================================== */

typedef struct {
  CourierStore          *store;
  GtkWidget             *list;
  GStrv                  folders;
  CourierRunFiltersFunc  run_now;
  gpointer               run_data;
} FiltersPage;

static void open_filter_dialog (GtkWidget *page, CourierFilter *filter);

static FiltersPage *
filters_page (GtkWidget *page)
{
  return g_object_get_data (G_OBJECT (page), "filters-page");
}

static void
on_filter_switched (GtkSwitch *sw, GParamSpec *pspec, GtkWidget *page)
{
  CourierFilter *f = g_object_get_data (G_OBJECT (sw), "filter");
  f->enabled = gtk_switch_get_active (sw);
  g_autoptr (GError) error = NULL;
  if (!courier_store_save_filter (filters_page (page)->store, f, &error))
    courier_toast (page, error->message);
}

static void
on_filter_edit (AdwActionRow *row, GtkWidget *page)
{
  open_filter_dialog (page, courier_filter_copy (g_object_get_data (G_OBJECT (row), "filter")));
}

static void
on_filter_delete (GtkButton *b, GtkWidget *page)
{
  CourierFilter *f = g_object_get_data (G_OBJECT (b), "filter");
  courier_store_delete_filter (filters_page (page)->store, f->id, NULL);
  courier_filters_page_refresh (page);
  courier_toast (page, "Filter deleted");
}

void
courier_filters_page_refresh (GtkWidget *page)
{
  FiltersPage *p = filters_page (page);
  gtk_list_box_remove_all (GTK_LIST_BOX (p->list));
  g_autoptr (GPtrArray) filters = courier_store_list_filters (p->store);
  for (guint i = 0; i < filters->len; i++) {
    CourierFilter *f = courier_filter_copy (g_ptr_array_index (filters, i));
    GtkWidget *row = adw_action_row_new ();
    g_object_set_data_full (G_OBJECT (row), "filter", f, (GDestroyNotify) courier_filter_free);
    g_autofree char *name = g_markup_escape_text (f->name, -1);
    g_autofree char *desc = courier_filter_describe (f);
    g_autofree char *desc_m = g_markup_escape_text (desc, -1);
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), name);
    adw_action_row_set_subtitle (ADW_ACTION_ROW (row), desc_m);
    gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), TRUE);
    g_signal_connect (row, "activated", G_CALLBACK (on_filter_edit), page);

    GtkWidget *sw = gtk_switch_new ();
    gtk_switch_set_active (GTK_SWITCH (sw), f->enabled);
    gtk_widget_set_valign (sw, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text (sw, "Enabled");
    g_object_set_data (G_OBJECT (sw), "filter", f);
    g_signal_connect (sw, "notify::active", G_CALLBACK (on_filter_switched), page);
    adw_action_row_add_prefix (ADW_ACTION_ROW (row), sw);

    GtkWidget *del = courier_icon_button ("user-trash-symbolic", "Delete Filter");
    gtk_widget_add_css_class (del, "flat");
    gtk_widget_set_valign (del, GTK_ALIGN_CENTER);
    g_object_set_data (G_OBJECT (del), "filter", f);
    g_signal_connect (del, "clicked", G_CALLBACK (on_filter_delete), page);
    adw_action_row_add_suffix (ADW_ACTION_ROW (row), del);
    gtk_list_box_append (GTK_LIST_BOX (p->list), row);
  }
  courier_boxed_list_done (p->list);
}

void
courier_filters_page_set_folders (GtkWidget *page, const char * const *folders)
{
  FiltersPage *p = filters_page (page);
  g_strfreev (p->folders);
  p->folders = g_strdupv ((char **) folders);
}

/* --- dialog --- */

typedef struct {
  GtkWidget     *page;
  AdwDialog     *dialog;
  CourierFilter *filter;
  GtkWidget     *banner, *name, *field, *op, *value, *action, *target_combo, *target_entry;
} FilterForm;

static void
filter_form_free (FilterForm *f)
{
  courier_filter_free (f->filter);
  g_free (f);
}

static GtkWidget *
combo (const char *title, const char * const *labels, guint selected)
{
  GtkWidget *row = adw_combo_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
  adw_combo_row_set_model (ADW_COMBO_ROW (row), G_LIST_MODEL (gtk_string_list_new (labels)));
  adw_combo_row_set_selected (ADW_COMBO_ROW (row), selected);
  return row;
}

static void
update_target_visibility (FilterForm *f)
{
  gboolean move = adw_combo_row_get_selected (ADW_COMBO_ROW (f->action)) == 0;
  gboolean have_folders = f->target_combo != NULL;
  if (f->target_combo)
    gtk_widget_set_visible (f->target_combo, move && have_folders);
  gtk_widget_set_visible (f->target_entry, move && !have_folders);
}

static void
on_filter_save (GtkButton *b, FilterForm *f)
{
  CourierFilter *flt = f->filter;
  g_free (flt->name);   flt->name = g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (f->name))));
  g_free (flt->value);  flt->value = g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (f->value))));
  g_free (flt->field);  flt->field = g_strdup (courier_filter_fields[adw_combo_row_get_selected (ADW_COMBO_ROW (f->field))]);
  g_free (flt->op);     flt->op = g_strdup (courier_filter_ops[adw_combo_row_get_selected (ADW_COMBO_ROW (f->op))]);
  g_free (flt->action); flt->action = g_strdup (courier_filter_actions[adw_combo_row_get_selected (ADW_COMBO_ROW (f->action))]);
  g_free (flt->target);
  flt->target = NULL;
  if (g_str_equal (flt->action, "move")) {
    if (f->target_combo) {
      GObject *item = adw_combo_row_get_selected_item (ADW_COMBO_ROW (f->target_combo));
      flt->target = item ? g_strdup (gtk_string_object_get_string (GTK_STRING_OBJECT (item))) : NULL;
    } else {
      flt->target = g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (f->target_entry))));
    }
  }

  g_autoptr (GError) error = NULL;
  if (!courier_store_save_filter (filters_page (f->page)->store, flt, &error)) {
    adw_banner_set_title (ADW_BANNER (f->banner), error->message);
    adw_banner_set_revealed (ADW_BANNER (f->banner), TRUE);
    return;
  }
  courier_filters_page_refresh (f->page);
  adw_dialog_force_close (f->dialog);
}

static void
open_filter_dialog (GtkWidget *page, CourierFilter *flt)
{
  FiltersPage *p = filters_page (page);
  FilterForm *f = g_new0 (FilterForm, 1);
  f->page = page;
  f->filter = flt;
  f->dialog = adw_dialog_new ();
  adw_dialog_set_title (f->dialog, flt->id ? "Edit Filter" : "New Filter");
  adw_dialog_set_content_width (f->dialog, 480);
  g_object_set_data_full (G_OBJECT (f->dialog), "form", f, (GDestroyNotify) filter_form_free);

  GtkWidget *toolbar = adw_toolbar_view_new ();
  GtkWidget *header = adw_header_bar_new ();
  adw_header_bar_set_show_start_title_buttons (ADW_HEADER_BAR (header), FALSE);
  adw_header_bar_set_show_end_title_buttons (ADW_HEADER_BAR (header), FALSE);
  GtkWidget *cancel = gtk_button_new_with_mnemonic ("_Cancel");
  g_signal_connect_swapped (cancel, "clicked", G_CALLBACK (adw_dialog_close), f->dialog);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), cancel);
  GtkWidget *save = gtk_button_new_with_mnemonic ("_Save");
  gtk_widget_add_css_class (save, "suggested-action");
  g_signal_connect (save, "clicked", G_CALLBACK (on_filter_save), f);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), save);
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), header);
  f->banner = adw_banner_new ("");
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), f->banner);

  GtkWidget *prefs = adw_preferences_page_new ();
  AdwPreferencesGroup *g = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  f->name = adw_entry_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (f->name), "Name (optional)");
  gtk_editable_set_text (GTK_EDITABLE (f->name), flt->name ? flt->name : "");
  adw_preferences_group_add (g, f->name);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (prefs), g);

  g = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (g, "When a new message arrives in the Inbox and");
  f->field = combo ("Field", courier_filter_field_labels, courier_filter_index_of (courier_filter_fields, flt->field));
  f->op = combo ("Condition", courier_filter_op_labels, courier_filter_index_of (courier_filter_ops, flt->op));
  f->value = adw_entry_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (f->value), "Text");
  gtk_editable_set_text (GTK_EDITABLE (f->value), flt->value ? flt->value : "");
  adw_preferences_group_add (g, f->field);
  adw_preferences_group_add (g, f->op);
  adw_preferences_group_add (g, f->value);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (prefs), g);

  g = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (g, "Then");
  f->action = combo ("Action", courier_filter_action_labels,
                     courier_filter_index_of (courier_filter_actions, flt->action));
  adw_preferences_group_add (g, f->action);
  if (p->folders && p->folders[0]) {
    f->target_combo = combo ("Folder", (const char * const *) p->folders, 0);
    for (guint i = 0; p->folders[i]; i++)
      if (g_strcmp0 (p->folders[i], flt->target) == 0)
        adw_combo_row_set_selected (ADW_COMBO_ROW (f->target_combo), i);
    adw_preferences_group_add (g, f->target_combo);
  }
  f->target_entry = adw_entry_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (f->target_entry), "Folder");
  gtk_editable_set_text (GTK_EDITABLE (f->target_entry), flt->target ? flt->target : "");
  adw_preferences_group_add (g, f->target_entry);
  g_signal_connect_swapped (f->action, "notify::selected", G_CALLBACK (update_target_visibility), f);
  update_target_visibility (f);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (prefs), g);

  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), prefs);
  adw_dialog_set_child (f->dialog, toolbar);
  adw_dialog_present (f->dialog, page);
  gtk_widget_grab_focus (f->value);
}

static void
on_new_filter (GtkButton *b, GtkWidget *page)
{
  CourierFilter *f = g_new0 (CourierFilter, 1);
  f->enabled = TRUE;
  f->field = g_strdup ("from");
  f->op = g_strdup ("contains");
  f->action = g_strdup ("move");
  open_filter_dialog (page, f);
}

static void
on_run_now (GtkButton *b, GtkWidget *page)
{
  FiltersPage *p = filters_page (page);
  if (p->run_now)
    p->run_now (p->run_data);
}

static void
filters_page_free (FiltersPage *p)
{
  g_strfreev (p->folders);
  g_free (p);
}

GtkWidget *
courier_filters_page_new (CourierStore *store, CourierRunFiltersFunc run_now, gpointer user_data)
{
  FiltersPage *p = g_new0 (FiltersPage, 1);
  p->store = store;
  p->run_now = run_now;
  p->run_data = user_data;

  GtkWidget *box;
  GtkWidget *scroll = courier_clamped_column (&box);
  GtkWidget *hint = gtk_label_new ("Filters run automatically on new mail in your Inbox, top to bottom. "
                                   "Moving or trashing a message stops further filters for it.");
  gtk_label_set_wrap (GTK_LABEL (hint), TRUE);
  gtk_label_set_xalign (GTK_LABEL (hint), 0);
  gtk_widget_add_css_class (hint, "dim-label");
  gtk_box_append (GTK_BOX (box), hint);
  p->list = courier_boxed_list ("No filters yet. Use + to add one.");
  gtk_box_append (GTK_BOX (box), p->list);

  GtkWidget *header;
  GtkWidget *page = courier_page_new ("Filters", scroll, &header);
  g_object_set_data_full (G_OBJECT (page), "filters-page", p, (GDestroyNotify) filters_page_free);

  GtkWidget *add = courier_icon_button ("list-add-symbolic", "New Filter");
  g_signal_connect (add, "clicked", G_CALLBACK (on_new_filter), page);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), add);
  GtkWidget *run = gtk_button_new_with_label ("Run on Inbox");
  gtk_widget_set_tooltip_text (run, "Apply the filters to the messages in the current account's Inbox now");
  g_signal_connect (run, "clicked", G_CALLBACK (on_run_now), page);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), run);

  courier_filters_page_refresh (page);
  return page;
}

/* =====================================================================
 *  Mail Log
 * ===================================================================== */

typedef struct {
  CourierStore *store;
  GtkWidget    *search;
  GtkWidget    *direction;
  GtkWidget    *list;
  GtkWidget    *count;
} MailLogPage;

static const char *direction_keys[] = { NULL, "received", "sent" };

static void
on_open_eml (GtkButton *b, GtkWidget *page)
{
  const char *path = g_object_get_data (G_OBJECT (b), "path");
  g_autoptr (GFile) file = g_file_new_for_path (path);
  g_autoptr (GtkFileLauncher) launcher = gtk_file_launcher_new (file);
  gtk_file_launcher_launch (launcher, GTK_WINDOW (gtk_widget_get_root (page)), NULL, NULL, NULL);
}

void
courier_mail_log_page_refresh (GtkWidget *page)
{
  MailLogPage *p = g_object_get_data (G_OBJECT (page), "log-page");
  gtk_list_box_remove_all (GTK_LIST_BOX (p->list));
  guint dir = gtk_drop_down_get_selected (GTK_DROP_DOWN (p->direction));
  const char *query = gtk_editable_get_text (GTK_EDITABLE (p->search));
  g_autoptr (GPtrArray) entries = courier_store_list_mail_log (p->store, direction_keys[MIN (dir, 2)], query, 500);

  for (guint i = 0; i < entries->len; i++) {
    CourierMailLogEntry *e = g_ptr_array_index (entries, i);
    gboolean sent = g_strcmp0 (e->direction, "sent") == 0;
    GtkWidget *row = adw_action_row_new ();
    g_autofree char *subject = g_markup_escape_text (e->subject && *e->subject ? e->subject : "(No Subject)", -1);
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), subject);
    g_autofree char *when = courier_format_unix (e->date);
    g_autofree char *sub = sent
      ? g_markup_printf_escaped ("To %s · %s", e->recipients ? e->recipients : "?", when)
      : g_markup_printf_escaped ("From %s · %s · %s", e->sender ? e->sender : "?", when,
                                 e->folder ? e->folder : "");
    adw_action_row_set_subtitle (ADW_ACTION_ROW (row), sub);
    adw_action_row_set_subtitle_lines (ADW_ACTION_ROW (row), 2);
    GtkWidget *icon = gtk_image_new_from_icon_name (sent ? "mail-send-symbolic" : "mail-unread-symbolic");
    gtk_widget_set_tooltip_text (icon, sent ? "Sent" : "Received");
    adw_action_row_add_prefix (ADW_ACTION_ROW (row), icon);
    if (e->preview && *e->preview)
      gtk_widget_set_tooltip_text (row, e->preview);
    if (e->eml_path && g_file_test (e->eml_path, G_FILE_TEST_EXISTS)) {
      GtkWidget *open = courier_icon_button ("document-open-symbolic", "Open saved copy (.eml)");
      gtk_widget_add_css_class (open, "flat");
      gtk_widget_set_valign (open, GTK_ALIGN_CENTER);
      g_object_set_data_full (G_OBJECT (open), "path", g_strdup (e->eml_path), g_free);
      g_signal_connect (open, "clicked", G_CALLBACK (on_open_eml), page);
      adw_action_row_add_suffix (ADW_ACTION_ROW (row), open);
    }
    gtk_list_box_append (GTK_LIST_BOX (p->list), row);
  }
  courier_boxed_list_done (p->list);
  g_autofree char *count = g_strdup_printf (entries->len == 1 ? "%u message" : "%u messages", entries->len);
  gtk_label_set_text (GTK_LABEL (p->count), count);
}

static char *
csv_field (const char *s)
{
  g_autoptr (GString) out = g_string_new ("\"");
  for (const char *c = s ? s : ""; *c; c++) {
    if (*c == '"')
      g_string_append_c (out, '"');
    g_string_append_c (out, *c);
  }
  g_string_append_c (out, '"');
  return g_string_free (g_steal_pointer (&out), FALSE);
}

static void
on_export_chosen (GObject *source, GAsyncResult *res, gpointer data)
{
  GtkWidget *page = data;
  g_autoptr (GFile) file = gtk_file_dialog_save_finish (GTK_FILE_DIALOG (source), res, NULL);
  if (!file)
    return;
  MailLogPage *p = g_object_get_data (G_OBJECT (page), "log-page");
  g_autoptr (GPtrArray) entries = courier_store_list_mail_log (p->store, NULL, NULL, -1);
  GString *csv = g_string_new ("direction,date,account,folder,from,to,subject,message_id,saved_copy\n");
  for (guint i = 0; i < entries->len; i++) {
    CourierMailLogEntry *e = g_ptr_array_index (entries, i);
    g_autoptr (GDateTime) dt = g_date_time_new_from_unix_local (e->date);
    g_autofree char *date = g_date_time_format_iso8601 (dt);
    const char *fields[] = { e->direction, date, e->account, e->folder, e->sender, e->recipients,
                             e->subject, e->message_id, e->eml_path };
    for (gsize j = 0; j < G_N_ELEMENTS (fields); j++) {
      g_autofree char *f = csv_field (fields[j]);
      g_string_append_printf (csv, "%s%s", j ? "," : "", f);
    }
    g_string_append_c (csv, '\n');
  }
  g_autoptr (GError) error = NULL;
  if (g_file_replace_contents (file, csv->str, csv->len, NULL, FALSE, G_FILE_CREATE_NONE, NULL, NULL, &error))
    courier_toast (page, "Mail log exported");
  else
    courier_toast (page, error->message);
  g_string_free (csv, TRUE);
}

static void
on_export (GtkButton *b, GtkWidget *page)
{
  g_autoptr (GtkFileDialog) dialog = gtk_file_dialog_new ();
  gtk_file_dialog_set_title (dialog, "Export Mail Log");
  gtk_file_dialog_set_initial_name (dialog, "mail-log.csv");
  gtk_file_dialog_save (dialog, GTK_WINDOW (gtk_widget_get_root (page)), NULL, on_export_chosen, page);
}

GtkWidget *
courier_mail_log_page_new (CourierStore *store)
{
  MailLogPage *p = g_new0 (MailLogPage, 1);
  p->store = store;

  GtkWidget *box;
  GtkWidget *scroll = courier_clamped_column (&box);
  GtkWidget *hint = gtk_label_new ("Every message that arrives in your Inbox and every message you send is "
                                   "recorded here automatically. Sent messages also keep a full .eml copy.");
  gtk_label_set_wrap (GTK_LABEL (hint), TRUE);
  gtk_label_set_xalign (GTK_LABEL (hint), 0);
  gtk_widget_add_css_class (hint, "dim-label");
  gtk_box_append (GTK_BOX (box), hint);

  GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  p->search = gtk_search_entry_new ();
  gtk_search_entry_set_placeholder_text (GTK_SEARCH_ENTRY (p->search), "Search sender, recipient or subject");
  gtk_widget_set_hexpand (p->search, TRUE);
  const char *dirs[] = { "All", "Received", "Sent", NULL };
  p->direction = gtk_drop_down_new_from_strings (dirs);
  gtk_box_append (GTK_BOX (bar), p->search);
  gtk_box_append (GTK_BOX (bar), p->direction);
  gtk_box_append (GTK_BOX (box), bar);

  p->count = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (p->count), 0);
  gtk_widget_add_css_class (p->count, "caption");
  gtk_widget_add_css_class (p->count, "dim-label");
  gtk_box_append (GTK_BOX (box), p->count);
  p->list = courier_boxed_list ("Nothing logged yet");
  gtk_box_append (GTK_BOX (box), p->list);

  GtkWidget *header;
  GtkWidget *page = courier_page_new ("Mail Log", scroll, &header);
  g_object_set_data_full (G_OBJECT (page), "log-page", p, g_free);
  g_signal_connect_swapped (p->search, "search-changed", G_CALLBACK (courier_mail_log_page_refresh), page);
  g_signal_connect_swapped (p->direction, "notify::selected", G_CALLBACK (courier_mail_log_page_refresh), page);

  GtkWidget *export = courier_icon_button ("document-save-symbolic", "Export as CSV");
  g_signal_connect (export, "clicked", G_CALLBACK (on_export), page);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), export);

  courier_mail_log_page_refresh (page);
  return page;
}
