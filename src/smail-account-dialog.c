/* smail-account-dialog.c — account form with provider autodetection and a login test */
#include "smail-account-dialog.h"
#include "smail-backend.h"

#include <string.h>

typedef struct {
  AdwDialog *dialog;
  GtkWidget *banner;
  GtkWidget *save;
  GtkWidget *spinner;
  GtkWidget *name, *email, *password;
  GtkWidget *imap_host, *imap_port, *imap_security, *username;
  GtkWidget *smtp_host, *smtp_port, *smtp_security;

  char *existing_id;
  char *auto_imap, *auto_smtp, *auto_user; /* last values we filled in automatically */
  SMailAccount *pending;
  SMailAccountSavedFunc callback;
  gpointer user_data;
  gboolean busy;
} Form;

static void
form_free (Form *f)
{
  g_free (f->existing_id);
  g_free (f->auto_imap);
  g_free (f->auto_smtp);
  g_free (f->auto_user);
  smail_account_free (f->pending);
  g_free (f);
}

/* ---------------- provider presets ---------------- */

typedef struct {
  const char *domains;   /* space separated */
  const char *imap; guint imap_port; SMailSecurity imap_sec;
  const char *smtp; guint smtp_port; SMailSecurity smtp_sec;
} Provider;

static const Provider providers[] = {
  { "gmail.com googlemail.com",
    "imap.gmail.com", 993, SMAIL_SECURITY_SSL, "smtp.gmail.com", 465, SMAIL_SECURITY_SSL },
  { "outlook.com hotmail.com live.com msn.com",
    "outlook.office365.com", 993, SMAIL_SECURITY_SSL, "smtp-mail.outlook.com", 587, SMAIL_SECURITY_STARTTLS },
  { "yahoo.com ymail.com",
    "imap.mail.yahoo.com", 993, SMAIL_SECURITY_SSL, "smtp.mail.yahoo.com", 465, SMAIL_SECURITY_SSL },
  { "icloud.com me.com mac.com",
    "imap.mail.me.com", 993, SMAIL_SECURITY_SSL, "smtp.mail.me.com", 587, SMAIL_SECURITY_STARTTLS },
  { "fastmail.com fastmail.fm",
    "imap.fastmail.com", 993, SMAIL_SECURITY_SSL, "smtp.fastmail.com", 465, SMAIL_SECURITY_SSL },
  { "proton.me protonmail.com", /* via Proton Mail Bridge */
    "127.0.0.1", 1143, SMAIL_SECURITY_STARTTLS, "127.0.0.1", 1025, SMAIL_SECURITY_STARTTLS },
  { "zoho.com",
    "imap.zoho.com", 993, SMAIL_SECURITY_SSL, "smtp.zoho.com", 465, SMAIL_SECURITY_SSL },
};

static const char *
row_text (GtkWidget *row)
{
  return gtk_editable_get_text (GTK_EDITABLE (row));
}

static void
set_auto (GtkWidget *row, char **last, const char *value)
{
  /* only overwrite what the user hasn't edited */
  const char *cur = row_text (row);
  if (*cur && g_strcmp0 (cur, *last) != 0)
    return;
  gtk_editable_set_text (GTK_EDITABLE (row), value);
  g_free (*last);
  *last = g_strdup (value);
}

static void
on_email_changed (GtkEditable *editable, Form *f)
{
  const char *email = row_text (f->email);
  const char *at = strchr (email, '@');
  set_auto (f->username, &f->auto_user, email);
  if (!at || !at[1] || !strchr (at, '.'))
    return;
  g_autofree char *domain = g_ascii_strdown (at + 1, -1);

  const Provider *p = NULL;
  for (gsize i = 0; i < G_N_ELEMENTS (providers) && !p; i++) {
    g_auto (GStrv) ds = g_strsplit (providers[i].domains, " ", -1);
    if (g_strv_contains ((const char * const *) ds, domain))
      p = &providers[i];
  }

  g_autofree char *imap = p ? g_strdup (p->imap) : g_strdup_printf ("imap.%s", domain);
  g_autofree char *smtp = p ? g_strdup (p->smtp) : g_strdup_printf ("smtp.%s", domain);
  gboolean imap_auto = !*row_text (f->imap_host) || g_strcmp0 (row_text (f->imap_host), f->auto_imap) == 0;
  gboolean smtp_auto = !*row_text (f->smtp_host) || g_strcmp0 (row_text (f->smtp_host), f->auto_smtp) == 0;
  set_auto (f->imap_host, &f->auto_imap, imap);
  set_auto (f->smtp_host, &f->auto_smtp, smtp);
  if (imap_auto) {
    adw_combo_row_set_selected (ADW_COMBO_ROW (f->imap_security), p ? p->imap_sec : SMAIL_SECURITY_SSL);
    adw_spin_row_set_value (ADW_SPIN_ROW (f->imap_port), p ? p->imap_port : 993);
  }
  if (smtp_auto) {
    adw_combo_row_set_selected (ADW_COMBO_ROW (f->smtp_security), p ? p->smtp_sec : SMAIL_SECURITY_SSL);
    adw_spin_row_set_value (ADW_SPIN_ROW (f->smtp_port), p ? p->smtp_port : 465);
  }
}

static void
on_security_changed (AdwComboRow *row, GParamSpec *pspec, GtkWidget *port)
{
  /* suggest the standard port for the chosen security mode */
  gboolean smtp = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "smtp"));
  guint sel = adw_combo_row_get_selected (row);
  guint ports_imap[] = { 993, 143, 143 };
  guint ports_smtp[] = { 465, 587, 25 };
  if (sel < 3)
    adw_spin_row_set_value (ADW_SPIN_ROW (port), smtp ? ports_smtp[sel] : ports_imap[sel]);
}

/* ---------------- saving ---------------- */

static void
set_busy (Form *f, gboolean busy)
{
  f->busy = busy;
  gtk_widget_set_sensitive (f->save, !busy);
  gtk_widget_set_visible (f->spinner, busy);
  gtk_spinner_set_spinning (GTK_SPINNER (f->spinner), busy);
  adw_dialog_set_can_close (f->dialog, !busy);
}

static void
test_thread (GTask *task, gpointer source, gpointer data, GCancellable *c)
{
  GError *error = NULL;
  if (smail_backend_test_account (data, &error))
    g_task_return_boolean (task, TRUE);
  else
    g_task_return_error (task, error);
}

static void
on_tested (GObject *source, GAsyncResult *res, gpointer data)
{
  Form *f = data;
  g_autoptr (GError) error = NULL;
  set_busy (f, FALSE);
  if (!g_task_propagate_boolean (G_TASK (res), &error)) {
    g_autofree char *msg = g_strdup_printf ("Couldn't sign in: %s", error->message);
    adw_banner_set_title (ADW_BANNER (f->banner), msg);
    adw_banner_set_revealed (ADW_BANNER (f->banner), TRUE);
    g_object_unref (f->dialog);
    return;
  }
  f->callback (f->pending, f->user_data);
  adw_dialog_force_close (f->dialog);
  g_object_unref (f->dialog);
}

static char *
stripped (GtkWidget *row)
{
  return g_strstrip (g_strdup (row_text (row)));
}

static void
on_save (GtkButton *button, Form *f)
{
  if (f->busy)
    return;
  g_autoptr (SMailAccount) a = smail_account_new ();
  a->id = f->existing_id ? g_strdup (f->existing_id) : g_uuid_string_random ();
  a->name = stripped (f->name);
  a->email = stripped (f->email);
  a->password = g_strdup (row_text (f->password));
  a->username = stripped (f->username);
  a->imap_host = stripped (f->imap_host);
  a->imap_port = (guint) adw_spin_row_get_value (ADW_SPIN_ROW (f->imap_port));
  a->imap_security = adw_combo_row_get_selected (ADW_COMBO_ROW (f->imap_security));
  a->smtp_host = stripped (f->smtp_host);
  a->smtp_port = (guint) adw_spin_row_get_value (ADW_SPIN_ROW (f->smtp_port));
  a->smtp_security = adw_combo_row_get_selected (ADW_COMBO_ROW (f->smtp_security));
  if (!*a->username) {
    g_free (a->username);
    a->username = g_strdup (a->email);
  }

  const char *problem = NULL;
  if (!*a->email || !strchr (a->email, '@'))
    problem = "Enter a valid email address";
  else if (!*a->password)
    problem = "Enter your password";
  else if (!*a->imap_host || !*a->smtp_host)
    problem = "Enter the incoming and outgoing server names";
  if (problem) {
    adw_banner_set_title (ADW_BANNER (f->banner), problem);
    adw_banner_set_revealed (ADW_BANNER (f->banner), TRUE);
    return;
  }
  if (!*a->name) {
    g_free (a->name);
    a->name = g_strndup (a->email, strchr (a->email, '@') - a->email);
  }

  adw_banner_set_revealed (ADW_BANNER (f->banner), FALSE);
  smail_account_free (f->pending);
  f->pending = g_steal_pointer (&a);
  set_busy (f, TRUE);

  g_autoptr (GTask) task = g_task_new (NULL, NULL, on_tested, f);
  g_object_ref (f->dialog); /* keep the form alive while testing */
  g_task_set_task_data (task, smail_account_copy (f->pending), (GDestroyNotify) smail_account_free);
  g_task_run_in_thread (task, test_thread);
}

/* ---------------- building the form ---------------- */

static GtkWidget *
entry_row (AdwPreferencesGroup *group, const char *title, const char *value, GtkInputPurpose purpose)
{
  GtkWidget *row = adw_entry_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
  gtk_editable_set_text (GTK_EDITABLE (row), value ? value : "");
  adw_entry_row_set_input_purpose (ADW_ENTRY_ROW (row), purpose);
  adw_preferences_group_add (group, row);
  return row;
}

static GtkWidget *
security_row (AdwPreferencesGroup *group, SMailSecurity value, GtkWidget *port, gboolean smtp)
{
  const char *items[] = { "SSL/TLS", "STARTTLS", "None (insecure)", NULL };
  GtkWidget *row = adw_combo_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), "Security");
  adw_combo_row_set_model (ADW_COMBO_ROW (row), G_LIST_MODEL (gtk_string_list_new (items)));
  adw_combo_row_set_selected (ADW_COMBO_ROW (row), value);
  g_object_set_data (G_OBJECT (row), "smtp", GINT_TO_POINTER (smtp));
  g_signal_connect (row, "notify::selected", G_CALLBACK (on_security_changed), port);
  adw_preferences_group_add (group, row);
  return row;
}

static GtkWidget *
port_row (AdwPreferencesGroup *group, guint value)
{
  GtkWidget *row = adw_spin_row_new_with_range (1, 65535, 1);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), "Port");
  adw_spin_row_set_value (ADW_SPIN_ROW (row), value);
  return row; /* added after the security row is created */
}

void
smail_account_dialog_present (GtkWidget *parent, const SMailAccount *existing,
                                SMailAccountSavedFunc callback, gpointer user_data)
{
  Form *f = g_new0 (Form, 1);
  f->callback = callback;
  f->user_data = user_data;
  f->existing_id = existing ? g_strdup (existing->id) : NULL;

  f->dialog = adw_dialog_new ();
  adw_dialog_set_title (f->dialog, existing ? "Account Settings" : "Add Account");
  adw_dialog_set_content_width (f->dialog, 520);
  adw_dialog_set_content_height (f->dialog, 680);
  g_object_set_data_full (G_OBJECT (f->dialog), "form", f, (GDestroyNotify) form_free);

  GtkWidget *toolbar = adw_toolbar_view_new ();
  GtkWidget *header = adw_header_bar_new ();
  adw_header_bar_set_show_end_title_buttons (ADW_HEADER_BAR (header), FALSE);
  adw_header_bar_set_show_start_title_buttons (ADW_HEADER_BAR (header), FALSE);

  GtkWidget *cancel = gtk_button_new_with_mnemonic ("_Cancel");
  g_signal_connect_swapped (cancel, "clicked", G_CALLBACK (adw_dialog_close), f->dialog);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), cancel);

  f->save = gtk_button_new_with_mnemonic (existing ? "_Save" : "_Sign In");
  gtk_widget_add_css_class (f->save, "suggested-action");
  g_signal_connect (f->save, "clicked", G_CALLBACK (on_save), f);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), f->save);
  f->spinner = gtk_spinner_new ();
  gtk_widget_set_visible (f->spinner, FALSE);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), f->spinner);

  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), header);
  f->banner = adw_banner_new ("");
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), f->banner);

  GtkWidget *page = adw_preferences_page_new ();

  /* identity */
  AdwPreferencesGroup *id = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_description (id,
    "Gmail, Outlook, Yahoo and iCloud need an app password when two-factor authentication is on.");
  f->name = entry_row (id, "Your Name", existing ? existing->name : g_get_real_name (), GTK_INPUT_PURPOSE_NAME);
  f->email = entry_row (id, "Email Address", existing ? existing->email : NULL, GTK_INPUT_PURPOSE_EMAIL);
  f->password = adw_password_entry_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (f->password), "Password");
  if (existing && existing->password)
    gtk_editable_set_text (GTK_EDITABLE (f->password), existing->password);
  adw_preferences_group_add (id, f->password);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), id);

  /* incoming */
  AdwPreferencesGroup *in = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (in, "Incoming Mail (IMAP)");
  f->imap_host = entry_row (in, "Server", existing ? existing->imap_host : NULL, GTK_INPUT_PURPOSE_URL);
  f->imap_port = port_row (in, existing ? existing->imap_port : 993);
  f->imap_security = security_row (in, existing ? existing->imap_security : SMAIL_SECURITY_SSL, f->imap_port, FALSE);
  adw_preferences_group_add (in, f->imap_port);
  f->username = entry_row (in, "Username", existing ? existing->username : NULL, GTK_INPUT_PURPOSE_FREE_FORM);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), in);

  /* outgoing */
  AdwPreferencesGroup *out = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (out, "Outgoing Mail (SMTP)");
  adw_preferences_group_set_description (out, "Uses the same username and password.");
  f->smtp_host = entry_row (out, "Server", existing ? existing->smtp_host : NULL, GTK_INPUT_PURPOSE_URL);
  f->smtp_port = port_row (out, existing ? existing->smtp_port : 465);
  f->smtp_security = security_row (out, existing ? existing->smtp_security : SMAIL_SECURITY_SSL, f->smtp_port, TRUE);
  adw_preferences_group_add (out, f->smtp_port);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), out);

  if (existing) {
    /* existing values count as "user edited": don't autofill over them */
    f->auto_user = g_strdup ("\x01");
    f->auto_imap = g_strdup ("\x01");
    f->auto_smtp = g_strdup ("\x01");
  }
  g_signal_connect (f->email, "changed", G_CALLBACK (on_email_changed), f);
  g_signal_connect (f->password, "entry-activated", G_CALLBACK (on_save), f);

  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), page);
  adw_dialog_set_child (f->dialog, toolbar);
  adw_dialog_set_default_widget (f->dialog, f->save);
  adw_dialog_present (f->dialog, parent);
  gtk_widget_grab_focus (existing ? f->password : f->email);
}
