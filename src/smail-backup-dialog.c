/* smail-backup-dialog.c — create backups and restore them */
#include "smail-backup-dialog.h"
#include "smail-backup.h"

typedef struct {
  AdwDialog         *dialog;
  GtkWidget         *toasts;
  GtkWidget         *spinner;
  GtkWidget         *emails;
  GtkWidget         *passwords;
  GtkWidget         *page;
  SMailStore        *store;
  GPtrArray         *accounts;   /* copies */
  SMailRestoredFunc  on_restored;
  gpointer           user_data;
  char              *restore_path;
  gboolean           busy;
} BackupUI;

static void
ui_free (BackupUI *ui)
{
  g_ptr_array_unref (ui->accounts);
  g_free (ui->restore_path);
  g_free (ui);
}

static void
toast (BackupUI *ui, const char *text)
{
  adw_toast_overlay_add_toast (ADW_TOAST_OVERLAY (ui->toasts), adw_toast_new (text));
}

static void
set_busy (BackupUI *ui, gboolean busy)
{
  ui->busy = busy;
  gtk_widget_set_sensitive (ui->page, !busy);
  gtk_widget_set_visible (ui->spinner, busy);
  gtk_spinner_set_spinning (GTK_SPINNER (ui->spinner), busy);
  adw_dialog_set_can_close (ui->dialog, !busy);
}

static char *
describe (const SMailBackupSummary *s)
{
  g_autofree char *size = g_format_size (s->bytes);
  GString *out = g_string_new (NULL);
  g_string_append_printf (out, "%" G_GINT64_FORMAT " account%s, %" G_GINT64_FORMAT " filter%s, "
                          "%" G_GINT64_FORMAT " event%s, %" G_GINT64_FORMAT " task%s, "
                          "%" G_GINT64_FORMAT " mail log entr%s, %" G_GINT64_FORMAT " chat message%s",
                          s->accounts, s->accounts == 1 ? "" : "s",
                          s->filters, s->filters == 1 ? "" : "s",
                          s->events, s->events == 1 ? "" : "s",
                          s->todos, s->todos == 1 ? "" : "s",
                          s->log_entries, s->log_entries == 1 ? "y" : "ies",
                          s->chat_messages, s->chat_messages == 1 ? "" : "s");
  if (s->emails)
    g_string_append_printf (out, " and %" G_GINT64_FORMAT " email%s (%s)", s->emails, s->emails == 1 ? "" : "s", size);
  g_string_append (out, s->passwords ? ". Passwords included." : ". Passwords not included.");
  return g_string_free (out, FALSE);
}

/* ---------------- create ---------------- */

typedef struct {
  BackupUI *ui;
  char     *path;
  gboolean  emails;
  gboolean  passwords;
  SMailBackupSummary summary;
} CreateJob;

static void
create_job_free (CreateJob *j)
{
  g_free (j->path);
  smail_backup_summary_clear (&j->summary);
  g_free (j);
}

static void
create_thread (GTask *task, gpointer src, gpointer data, GCancellable *c)
{
  CreateJob *j = data;
  GError *error = NULL;
  if (smail_backup_create (j->ui->store, j->ui->accounts, j->path, j->emails, j->passwords, &j->summary, &error))
    g_task_return_boolean (task, TRUE);
  else
    g_task_return_error (task, error);
}

static void
on_created (GObject *src, GAsyncResult *res, gpointer data)
{
  CreateJob *j = g_task_get_task_data (G_TASK (res));
  BackupUI *ui = j->ui;
  g_autoptr (GError) error = NULL;
  set_busy (ui, FALSE);
  if (g_task_propagate_boolean (G_TASK (res), &error)) {
    g_autofree char *what = describe (&j->summary);
    g_autofree char *base = g_path_get_basename (j->path);
    g_autofree char *body = g_strdup_printf ("Saved to “%s”.\n\n%s", base, what);
    AdwDialog *done = adw_alert_dialog_new ("Backup Created", body);
    adw_alert_dialog_add_response (ADW_ALERT_DIALOG (done), "ok", "_OK");
    adw_dialog_present (done, GTK_WIDGET (ui->dialog));
  } else {
    g_autofree char *msg = g_strdup_printf ("Backup failed: %s", error->message);
    toast (ui, msg);
  }
  g_object_unref (ui->dialog);
}

static void
on_save_chosen (GObject *src, GAsyncResult *res, gpointer data)
{
  BackupUI *ui = data;
  g_autoptr (GFile) file = gtk_file_dialog_save_finish (GTK_FILE_DIALOG (src), res, NULL);
  if (!file) {
    g_object_unref (ui->dialog);
    return;
  }
  CreateJob *j = g_new0 (CreateJob, 1);
  j->ui = ui;
  j->path = g_file_get_path (file);
  if (!g_str_has_suffix (j->path, SMAIL_BACKUP_EXTENSION)) {
    char *p = g_strconcat (j->path, SMAIL_BACKUP_EXTENSION, NULL);
    g_free (j->path);
    j->path = p;
  }
  j->emails = adw_switch_row_get_active (ADW_SWITCH_ROW (ui->emails));
  j->passwords = adw_switch_row_get_active (ADW_SWITCH_ROW (ui->passwords));
  set_busy (ui, TRUE);
  g_autoptr (GTask) task = g_task_new (NULL, NULL, on_created, NULL);
  g_task_set_task_data (task, j, (GDestroyNotify) create_job_free);
  g_task_run_in_thread (task, create_thread);
}

static GtkFileFilter *
backup_filter (void)
{
  GtkFileFilter *f = gtk_file_filter_new ();
  gtk_file_filter_set_name (f, "SMail Backups");
  gtk_file_filter_add_pattern (f, "*" SMAIL_BACKUP_EXTENSION);
  return f;
}

static void
on_create_clicked (AdwActionRow *row, BackupUI *ui)
{
  if (ui->busy)
    return;
  g_autoptr (GtkFileDialog) fd = gtk_file_dialog_new ();
  gtk_file_dialog_set_title (fd, "Save Backup");
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  g_autofree char *stamp = g_date_time_format (now, "%Y-%m-%d");
  g_autofree char *name = g_strdup_printf ("smail-backup-%s" SMAIL_BACKUP_EXTENSION, stamp);
  gtk_file_dialog_set_initial_name (fd, name);
  g_autoptr (GtkFileFilter) filter = backup_filter ();
  gtk_file_dialog_set_default_filter (fd, filter);
  g_object_ref (ui->dialog);
  gtk_file_dialog_save (fd, GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (ui->dialog))), NULL, on_save_chosen, ui);
}

/* ---------------- restore ---------------- */

static void
on_restore_confirmed (AdwAlertDialog *alert, const char *response, BackupUI *ui)
{
  if (!g_str_equal (response, "restore"))
    return;

  set_busy (ui, TRUE);
  g_autoptr (GError) error = NULL;

  /* safety copy of what's here now */
  g_autofree char *safety = smail_backup_auto_path ("before-restore");
  if (!smail_backup_create (ui->store, ui->accounts, safety, TRUE, FALSE, NULL, &error)) {
    set_busy (ui, FALSE);
    g_autofree char *msg = g_strdup_printf ("Restore cancelled, couldn't save a safety copy first: %s", error->message);
    toast (ui, msg);
    return;
  }

  g_autoptr (GPtrArray) accounts = NULL;
  SMailBackupSummary summary = { 0 };
  gboolean ok = smail_backup_restore (ui->store, ui->restore_path, &accounts, &summary, &error);
  set_busy (ui, FALSE);
  if (!ok) {
    g_autofree char *msg = g_strdup_printf ("Restore failed: %s", error->message);
    toast (ui, msg);
    smail_backup_summary_clear (&summary);
    return;
  }
  ui->on_restored (accounts, summary.passwords, ui->user_data);
  smail_backup_summary_clear (&summary);
  adw_dialog_force_close (ui->dialog);
}

static void
on_open_chosen (GObject *src, GAsyncResult *res, gpointer data)
{
  BackupUI *ui = data;
  g_autoptr (GFile) file = gtk_file_dialog_open_finish (GTK_FILE_DIALOG (src), res, NULL);
  if (file) {
    g_free (ui->restore_path);
    ui->restore_path = g_file_get_path (file);
    SMailBackupSummary s = { 0 };
    g_autoptr (GError) error = NULL;
    if (!smail_backup_inspect (ui->restore_path, &s, &error)) {
      toast (ui, error->message);
    } else {
      g_autofree char *what = describe (&s);
      g_autoptr (GDateTime) when = s.created ? g_date_time_new_from_iso8601 (s.created, NULL) : NULL;
      g_autofree char *date = when ? g_date_time_format (when, "%-d %B %Y at %H:%M") : g_strdup ("an unknown date");
      g_autofree char *body = g_strdup_printf (
        "Backup from %s: %s\n\nThis replaces your current accounts, filters, calendar, tasks, reminders, "
        "mail log, chat history and settings. A safety copy of your current data is saved first.",
        date, what);
      AdwDialog *alert = adw_alert_dialog_new ("Restore This Backup?", body);
      adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (alert), "cancel", "_Cancel", "restore", "_Restore", NULL);
      adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (alert), "restore", ADW_RESPONSE_DESTRUCTIVE);
      adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (alert), "cancel");
      g_signal_connect (alert, "response", G_CALLBACK (on_restore_confirmed), ui);
      adw_dialog_present (alert, GTK_WIDGET (ui->dialog));
    }
    smail_backup_summary_clear (&s);
  }
  g_object_unref (ui->dialog);
}

static void
on_restore_clicked (AdwActionRow *row, BackupUI *ui)
{
  if (ui->busy)
    return;
  g_autoptr (GtkFileDialog) fd = gtk_file_dialog_new ();
  gtk_file_dialog_set_title (fd, "Choose a Backup");
  g_autoptr (GtkFileFilter) filter = backup_filter ();
  gtk_file_dialog_set_default_filter (fd, filter);
  g_object_ref (ui->dialog);
  gtk_file_dialog_open (fd, GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (ui->dialog))), NULL, on_open_chosen, ui);
}

/* ---------------- dialog ---------------- */

static GtkWidget *
button_row (const char *title, const char *subtitle, const char *icon)
{
  GtkWidget *row = adw_action_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
  if (subtitle)
    adw_action_row_set_subtitle (ADW_ACTION_ROW (row), subtitle);
  adw_action_row_add_suffix (ADW_ACTION_ROW (row), gtk_image_new_from_icon_name (icon));
  gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), TRUE);
  return row;
}

void
smail_backup_dialog_present (GtkWidget *parent, SMailStore *store, GPtrArray *accounts,
                             SMailRestoredFunc on_restored, gpointer user_data)
{
  BackupUI *ui = g_new0 (BackupUI, 1);
  ui->store = store;
  ui->on_restored = on_restored;
  ui->user_data = user_data;
  ui->accounts = g_ptr_array_new_with_free_func ((GDestroyNotify) smail_account_free);
  for (guint i = 0; accounts && i < accounts->len; i++)
    g_ptr_array_add (ui->accounts, smail_account_copy (g_ptr_array_index (accounts, i)));

  ui->dialog = adw_dialog_new ();
  adw_dialog_set_title (ui->dialog, "Backup & Restore");
  adw_dialog_set_content_width (ui->dialog, 520);
  g_object_set_data_full (G_OBJECT (ui->dialog), "ui", ui, (GDestroyNotify) ui_free);

  GtkWidget *toolbar = adw_toolbar_view_new ();
  GtkWidget *header = adw_header_bar_new ();
  ui->spinner = gtk_spinner_new ();
  gtk_widget_set_visible (ui->spinner, FALSE);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), ui->spinner);
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), header);

  ui->page = adw_preferences_page_new ();
  AdwPreferencesGroup *g = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (g, "Back Up");
  adw_preferences_group_set_description (g, "Save your accounts, mail filters, calendar, tasks, reminders, "
                                            "mail log, local chat history and settings to a single file.");
  gint64 count = 0, bytes = 0;
  smail_store_message_stats (store, &count, &bytes);
  g_autofree char *size = g_format_size (bytes);
  g_autofree char *email_sub = g_strdup_printf ("%" G_GINT64_FORMAT " downloaded email%s, %s",
                                                count, count == 1 ? "" : "s", size);
  ui->emails = adw_switch_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (ui->emails), "Include Downloaded Emails");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (ui->emails), email_sub);
  adw_switch_row_set_active (ADW_SWITCH_ROW (ui->emails), TRUE);
  adw_preferences_group_add (g, ui->emails);
  ui->passwords = adw_switch_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (ui->passwords), "Include Passwords");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (ui->passwords),
                               "Stored without encryption, so keep the backup file somewhere safe");
  adw_preferences_group_add (g, ui->passwords);
  GtkWidget *create = button_row ("Create Backup…", NULL, "document-save-symbolic");
  gtk_widget_add_css_class (create, "suggested-row");
  g_signal_connect (create, "activated", G_CALLBACK (on_create_clicked), ui);
  adw_preferences_group_add (g, create);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (ui->page), g);

  g = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (g, "Restore");
  adw_preferences_group_set_description (g, "Replace the data on this computer with a backup. "
                                            "A safety copy of your current data is saved first in "
                                            "~/.local/share/smail/backups.");
  GtkWidget *restore = button_row ("Restore from Backup…", NULL, "document-open-symbolic");
  g_signal_connect (restore, "activated", G_CALLBACK (on_restore_clicked), ui);
  adw_preferences_group_add (g, restore);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (ui->page), g);

  ui->toasts = adw_toast_overlay_new ();
  adw_toast_overlay_set_child (ADW_TOAST_OVERLAY (ui->toasts), ui->page);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), ui->toasts);
  adw_dialog_set_child (ui->dialog, toolbar);
  adw_dialog_present (ui->dialog, parent);
}
