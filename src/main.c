/* main.c — application entry point */
#include <adwaita.h>
#include <gmime/gmime.h>

#include "smail-window.h"

#define APP_ID "io.github.udhayakumar_in8.SMail"

static gboolean opt_demo = FALSE;

static const char *app_css =
  /* sidebar */
  ".account-header { font-weight: 700; font-size: 0.8em; opacity: 0.55; margin: 12px 6px 2px 6px; }"
  ".sidebar-status { margin: 0 6px 6px 6px; font-size: 0.85em; }"
  ".unread-badge { font-size: 0.8em; font-weight: 700; min-width: 12px; padding: 1px 7px;"
  "                border-radius: 99px; background: alpha(currentColor, 0.12); }"
  /* message list */
  ".message-list { background: transparent; }"
  ".message-row { padding: 9px 12px 9px 6px; }"
  ".message-row .subject { opacity: 0.7; }"
  ".message-row.unread .sender { font-weight: 700; }"
  ".message-row.unread .subject { opacity: 1; }"
  ".message-row .star { color: #e5a50a; -gtk-icon-size: 14px; }"
  ".unread-dot { min-width: 8px; min-height: 8px; border-radius: 99px; background: @accent_bg_color; opacity: 0; }"
  ".message-row.unread .unread-dot { opacity: 1; }"
  /* reading pane */
  ".message-header { padding: 18px 24px 14px 24px; }"
  "button.attachment { padding: 6px 12px; }"
  /* composer */
  ".composer-field { padding: 4px 12px; min-height: 38px; }"
  ".composer-attachments { padding: 6px 12px; }"
  ".attachment-chip { padding: 2px 4px 2px 10px; border-radius: 8px; background: alpha(currentColor, 0.08); }"
  ".composer-body { font-size: 1.05em; }"
  /* organizer pages */
  ".page-column { margin: 18px 12px 24px 12px; }"
  ".section-label { margin-top: 12px; }"
  ".list-placeholder { padding: 18px; }"
  "row.task-done .title { text-decoration: line-through; opacity: 0.6; }"
  "row.overdue .subtitle { color: @error_color; opacity: 1; }"
  "calendar.card { padding: 6px; }"
  /* local chat */
  ".chat-me { margin: 6px 12px; }"
  ".chat-messages { background: transparent; padding: 12px; }"
  ".chat-messages row { background: transparent; padding: 3px 6px; }"
  ".bubble { padding: 8px 12px; border-radius: 16px; background: alpha(currentColor, 0.08); }"
  ".bubble.outgoing { background: @accent_bg_color; color: @accent_fg_color; }"
  ".chat-compose { padding: 6px 12px; }";

static void
about_activated (GSimpleAction *a, GVariant *p, gpointer app)
{
  const char *developers[] = { "SMail contributors", NULL };
  AdwDialog *about = adw_about_dialog_new ();
  adw_about_dialog_set_application_name (ADW_ABOUT_DIALOG (about), "SMail");
  adw_about_dialog_set_application_icon (ADW_ABOUT_DIALOG (about), "mail-send-symbolic");
  adw_about_dialog_set_version (ADW_ABOUT_DIALOG (about), PACKAGE_VERSION);
  adw_about_dialog_set_comments (ADW_ABOUT_DIALOG (about),
                                 "A simple, fast email client built with GTK 4 and libadwaita");
  adw_about_dialog_set_developers (ADW_ABOUT_DIALOG (about), developers);
  adw_about_dialog_set_license_type (ADW_ABOUT_DIALOG (about), GTK_LICENSE_GPL_3_0);
  adw_dialog_present (about, GTK_WIDGET (gtk_application_get_active_window (app)));
}

#if !ADW_CHECK_VERSION (1, 8, 0)
static const char *shortcuts_ui =
  "<interface><object class='GtkShortcutsWindow' id='w'><property name='modal'>1</property>"
  "<child><object class='GtkShortcutsSection'><property name='section-name'>main</property>"
  "<child><object class='GtkShortcutsGroup'><property name='title'>Messages</property>"
  "<child><object class='GtkShortcutsShortcut'><property name='title'>Compose</property><property name='accelerator'>&lt;Control&gt;n</property></object></child>"
  "<child><object class='GtkShortcutsShortcut'><property name='title'>Reply</property><property name='accelerator'>&lt;Control&gt;r</property></object></child>"
  "<child><object class='GtkShortcutsShortcut'><property name='title'>Reply All</property><property name='accelerator'>&lt;Control&gt;&lt;Shift&gt;r</property></object></child>"
  "<child><object class='GtkShortcutsShortcut'><property name='title'>Forward</property><property name='accelerator'>&lt;Control&gt;l</property></object></child>"
  "<child><object class='GtkShortcutsShortcut'><property name='title'>Send (in composer)</property><property name='accelerator'>&lt;Control&gt;Return</property></object></child>"
  "</object></child>"
  "<child><object class='GtkShortcutsGroup'><property name='title'>Organize</property>"
  "<child><object class='GtkShortcutsShortcut'><property name='title'>Archive</property><property name='accelerator'>a</property></object></child>"
  "<child><object class='GtkShortcutsShortcut'><property name='title'>Move to Trash</property><property name='accelerator'>Delete</property></object></child>"
  "<child><object class='GtkShortcutsShortcut'><property name='title'>Star / Unstar</property><property name='accelerator'>s</property></object></child>"
  "<child><object class='GtkShortcutsShortcut'><property name='title'>Mark Read / Unread</property><property name='accelerator'>u</property></object></child>"
  "</object></child>"
  "<child><object class='GtkShortcutsGroup'><property name='title'>General</property>"
  "<child><object class='GtkShortcutsShortcut'><property name='title'>Search</property><property name='accelerator'>&lt;Control&gt;f</property></object></child>"
  "<child><object class='GtkShortcutsShortcut'><property name='title'>Refresh</property><property name='accelerator'>F5</property></object></child>"
  "<child><object class='GtkShortcutsShortcut'><property name='title'>Keyboard Shortcuts</property><property name='accelerator'>&lt;Control&gt;question</property></object></child>"
  "<child><object class='GtkShortcutsShortcut'><property name='title'>Quit</property><property name='accelerator'>&lt;Control&gt;q</property></object></child>"
  "</object></child></object></child></object></interface>";
#endif

#if ADW_CHECK_VERSION (1, 8, 0)
/* libadwaita 1.8+: GtkShortcutsWindow is deprecated, use AdwShortcutsDialog */
static void
shortcuts_activated (GSimpleAction *a, GVariant *p, gpointer app)
{
  static const struct { const char *section, *title, *accel; } items[] = {
    { "Messages", "Compose", "<Control>n" },
    { "Messages", "Reply", "<Control>r" },
    { "Messages", "Reply All", "<Control><Shift>r" },
    { "Messages", "Forward", "<Control>l" },
    { "Messages", "Send (in composer)", "<Control>Return" },
    { "Organize", "Archive", "a" },
    { "Organize", "Move to Trash", "Delete" },
    { "Organize", "Star / Unstar", "s" },
    { "Organize", "Mark Read / Unread", "u" },
    { "General", "Search", "<Control>f" },
    { "General", "Refresh", "F5" },
    { "General", "Keyboard Shortcuts", "<Control>question" },
    { "General", "Quit", "<Control>q" },
  };
  AdwDialog *dialog = adw_shortcuts_dialog_new ();
  AdwShortcutsSection *section = NULL;
  const char *current = NULL;
  for (gsize i = 0; i < G_N_ELEMENTS (items); i++) {
    if (g_strcmp0 (current, items[i].section) != 0) {
      current = items[i].section;
      section = adw_shortcuts_section_new (current);
      adw_shortcuts_dialog_add (ADW_SHORTCUTS_DIALOG (dialog), section);
    }
    adw_shortcuts_section_add (section, adw_shortcuts_item_new (items[i].title, items[i].accel));
  }
  adw_dialog_present (dialog, GTK_WIDGET (gtk_application_get_active_window (app)));
}
#else
static void
shortcuts_activated (GSimpleAction *a, GVariant *p, gpointer app)
{
  g_autoptr (GtkBuilder) builder = gtk_builder_new_from_string (shortcuts_ui, -1);
  GtkWindow *win = GTK_WINDOW (gtk_builder_get_object (builder, "w"));
  gtk_window_set_transient_for (win, gtk_application_get_active_window (app));
  gtk_window_present (win);
}
#endif

static void
quit_activated (GSimpleAction *a, GVariant *p, gpointer app)
{
  g_application_quit (G_APPLICATION (app));
}

/* Notification buttons can only target app actions: forward to the window. */
static void
snooze_reminder_activated (GSimpleAction *a, GVariant *p, gpointer app)
{
  GtkWindow *win = gtk_application_get_active_window (app);
  if (!win) {
    GList *wins = gtk_application_get_windows (app);
    win = wins ? wins->data : NULL;
  }
  if (win)
    g_action_group_activate_action (G_ACTION_GROUP (win), "snooze-reminder", p);
}

static void
present_activated (GSimpleAction *a, GVariant *p, gpointer app)
{
  g_application_activate (G_APPLICATION (app));
}

static const GActionEntry app_actions[] = {
  { "activate",  present_activated },
  { "snooze-reminder", snooze_reminder_activated, "x" },
  { "about",     about_activated },
  { "shortcuts", shortcuts_activated },
  { "quit",      quit_activated },
};

static void
on_startup (GApplication *app)
{
  g_mime_init ();

  g_autoptr (GtkCssProvider) css = gtk_css_provider_new ();
  gtk_css_provider_load_from_string (css, app_css);
  gtk_style_context_add_provider_for_display (gdk_display_get_default (), GTK_STYLE_PROVIDER (css),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

  g_action_map_add_action_entries (G_ACTION_MAP (app), app_actions, G_N_ELEMENTS (app_actions), app);

  struct { const char *action; const char *accels[3]; } accels[] = {
    { "app.quit",            { "<Control>q", NULL } },
    { "app.shortcuts",       { "<Control>question", NULL } },
    { "win.compose",         { "<Control>n", NULL } },
    { "win.reply",           { "<Control>r", NULL } },
    { "win.reply-all",       { "<Control><Shift>r", NULL } },
    { "win.forward",         { "<Control>l", NULL } },
    { "win.archive",         { "a", NULL } },
    { "win.trash",           { "Delete", NULL } },
    { "win.toggle-star",     { "s", NULL } },
    { "win.toggle-read",     { "u", NULL } },
    { "win.refresh",         { "F5", "<Control>F5", NULL } },
    { "win.search",          { "<Control>f", NULL } },
  };
  for (gsize i = 0; i < G_N_ELEMENTS (accels); i++)
    gtk_application_set_accels_for_action (GTK_APPLICATION (app), accels[i].action, accels[i].accels);
}

static void
on_activate (GApplication *app)
{
  GtkWindow *win = gtk_application_get_active_window (GTK_APPLICATION (app));
  if (!win)
    win = GTK_WINDOW (smail_window_new (ADW_APPLICATION (app), opt_demo));
  gtk_window_present (win);
}

static void
on_shutdown (GApplication *app)
{
  g_mime_shutdown ();
}

int
main (int argc, char *argv[])
{
  g_autoptr (AdwApplication) app = adw_application_new (APP_ID, G_APPLICATION_DEFAULT_FLAGS);
  g_set_application_name ("SMail");

  GOptionEntry options[] = {
    { "demo", 0, 0, G_OPTION_ARG_NONE, &opt_demo, "Explore the app with sample messages", NULL },
    { NULL }
  };
  g_application_add_main_option_entries (G_APPLICATION (app), options);

  g_signal_connect (app, "startup", G_CALLBACK (on_startup), NULL);
  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
  g_signal_connect (app, "shutdown", G_CALLBACK (on_shutdown), NULL);
  return g_application_run (G_APPLICATION (app), argc, argv);
}
