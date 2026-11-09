/* smail-contacts-page.c — the Address Book: your contacts and collected addresses */
#include "smail-contacts-page.h"
#include "smail-gmime.h"
#include "smail-ui.h"
#include "smail-vcard.h"

#include <string.h>

typedef struct {
  SMailStore         *store;
  GtkWidget          *page;
  GtkWidget          *search;
  GtkWidget          *contacts;
  GtkWidget          *collected;
  GtkWidget          *collected_label;
  GtkWidget          *count;
  SMailComposeToFunc  compose_to;
  gpointer            compose_data;
} ContactsPage;

static ContactsPage *
contacts_page (GtkWidget *page)
{
  return g_object_get_data (G_OBJECT (page), "contacts-page");
}

char *
smail_contact_format_address (const char *name, const char *email)
{
  g_autoptr (InternetAddress) ia = internet_address_mailbox_new (name && *name ? name : NULL, email);
  return internet_address_to_string (ia, NULL, FALSE);
}

/* ---------------- edit dialog ---------------- */

typedef struct {
  GtkWidget    *page;
  AdwDialog    *dialog;
  GtkWidget    *banner, *name, *email, *phone, *notes, *favorite;
  SMailContact *contact;
} ContactForm;

static void
contact_form_free (ContactForm *f)
{
  smail_contact_free (f->contact);
  g_free (f);
}

static char *
row_text (GtkWidget *row)
{
  return g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (row))));
}

static void
on_contact_save (GtkButton *b, ContactForm *f)
{
  SMailContact *c = f->contact;
  g_free (c->name);  c->name = row_text (f->name);
  g_free (c->email); c->email = row_text (f->email);
  g_free (c->phone); c->phone = row_text (f->phone);
  g_free (c->notes); c->notes = row_text (f->notes);
  c->favorite = adw_switch_row_get_active (ADW_SWITCH_ROW (f->favorite));
  c->collected = FALSE; /* edited by you: it's a real contact now */
  g_autoptr (GError) error = NULL;
  if (!smail_store_save_contact (contacts_page (f->page)->store, c, &error)) {
    adw_banner_set_title (ADW_BANNER (f->banner), error->message);
    adw_banner_set_revealed (ADW_BANNER (f->banner), TRUE);
    return;
  }
  smail_contacts_page_refresh (f->page);
  adw_dialog_force_close (f->dialog);
}

static void
on_contact_delete (GtkButton *b, ContactForm *f)
{
  smail_store_delete_contact (contacts_page (f->page)->store, f->contact->id, NULL);
  smail_contacts_page_refresh (f->page);
  smail_toast (f->page, "Contact deleted");
  adw_dialog_force_close (f->dialog);
}

static GtkWidget *
entry_row (AdwPreferencesGroup *g, const char *title, const char *value, GtkInputPurpose purpose)
{
  GtkWidget *row = adw_entry_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
  gtk_editable_set_text (GTK_EDITABLE (row), value ? value : "");
  adw_entry_row_set_input_purpose (ADW_ENTRY_ROW (row), purpose);
  adw_preferences_group_add (g, row);
  return row;
}

static void
open_contact_dialog (GtkWidget *page, SMailContact *contact)
{
  ContactForm *f = g_new0 (ContactForm, 1);
  f->page = page;
  f->contact = contact;
  gboolean editing = contact->id != 0;

  f->dialog = adw_dialog_new ();
  adw_dialog_set_title (f->dialog, editing ? "Edit Contact" : "New Contact");
  adw_dialog_set_content_width (f->dialog, 460);
  g_object_set_data_full (G_OBJECT (f->dialog), "form", f, (GDestroyNotify) contact_form_free);

  GtkWidget *toolbar = adw_toolbar_view_new ();
  GtkWidget *header = adw_header_bar_new ();
  adw_header_bar_set_show_start_title_buttons (ADW_HEADER_BAR (header), FALSE);
  adw_header_bar_set_show_end_title_buttons (ADW_HEADER_BAR (header), FALSE);
  GtkWidget *cancel = gtk_button_new_with_mnemonic ("_Cancel");
  g_signal_connect_swapped (cancel, "clicked", G_CALLBACK (adw_dialog_close), f->dialog);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), cancel);
  GtkWidget *save = gtk_button_new_with_mnemonic (editing ? "_Save" : "_Add");
  gtk_widget_add_css_class (save, "suggested-action");
  g_signal_connect (save, "clicked", G_CALLBACK (on_contact_save), f);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), save);
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), header);
  f->banner = adw_banner_new ("");
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), f->banner);

  GtkWidget *prefs = adw_preferences_page_new ();
  AdwPreferencesGroup *g = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  f->name = entry_row (g, "Name", contact->name, GTK_INPUT_PURPOSE_NAME);
  f->email = entry_row (g, "Email Address", contact->email, GTK_INPUT_PURPOSE_EMAIL);
  f->phone = entry_row (g, "Phone", contact->phone, GTK_INPUT_PURPOSE_PHONE);
  f->notes = entry_row (g, "Notes", contact->notes, GTK_INPUT_PURPOSE_FREE_FORM);
  f->favorite = adw_switch_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (f->favorite), "Favorite");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (f->favorite), "Shown first and suggested first while typing");
  adw_switch_row_set_active (ADW_SWITCH_ROW (f->favorite), contact->favorite);
  adw_preferences_group_add (g, f->favorite);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (prefs), g);

  if (editing) {
    AdwPreferencesGroup *dg = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
    GtkWidget *del = gtk_button_new_with_label ("Delete Contact");
    gtk_widget_add_css_class (del, "destructive-action");
    gtk_widget_add_css_class (del, "pill");
    gtk_widget_set_halign (del, GTK_ALIGN_CENTER);
    g_signal_connect (del, "clicked", G_CALLBACK (on_contact_delete), f);
    adw_preferences_group_add (dg, del);
    adw_preferences_page_add (ADW_PREFERENCES_PAGE (prefs), dg);
  }

  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), prefs);
  adw_dialog_set_child (f->dialog, toolbar);
  adw_dialog_set_default_widget (f->dialog, save);
  adw_dialog_present (f->dialog, page);
  gtk_widget_grab_focus (editing ? f->name : f->name);
}

/* ---------------- list ---------------- */

static void
on_row_activated (AdwActionRow *row, GtkWidget *page)
{
  open_contact_dialog (page, smail_contact_copy (g_object_get_data (G_OBJECT (row), "contact")));
}

static void
on_write_clicked (GtkButton *b, GtkWidget *page)
{
  ContactsPage *p = contacts_page (page);
  SMailContact *c = g_object_get_data (G_OBJECT (b), "contact");
  g_autofree char *to = smail_contact_format_address (c->name, c->email);
  if (p->compose_to)
    p->compose_to (to, p->compose_data);
}

static void
on_star_clicked (GtkButton *b, GtkWidget *page)
{
  SMailContact *c = smail_contact_copy (g_object_get_data (G_OBJECT (b), "contact"));
  c->favorite = !c->favorite;
  c->collected = FALSE;
  smail_store_save_contact (contacts_page (page)->store, c, NULL);
  smail_contact_free (c);
  smail_contacts_page_refresh (page);
}

static GtkWidget *
flat_button (const char *icon, const char *tooltip)
{
  GtkWidget *b = smail_icon_button (icon, tooltip);
  gtk_widget_add_css_class (b, "flat");
  gtk_widget_set_valign (b, GTK_ALIGN_CENTER);
  return b;
}

static GtkWidget *
contact_row (GtkWidget *page, SMailContact *c)
{
  SMailContact *copy = smail_contact_copy (c);
  GtkWidget *row = adw_action_row_new ();
  g_object_set_data_full (G_OBJECT (row), "contact", copy, (GDestroyNotify) smail_contact_free);
  const char *title = c->name && *c->name ? c->name : c->email;
  g_autofree char *t = g_markup_escape_text (title, -1);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), t);
  g_autofree char *sub = c->phone && *c->phone ? g_markup_printf_escaped ("%s · %s", c->email, c->phone)
                                               : g_markup_escape_text (c->email, -1);
  adw_action_row_set_subtitle (ADW_ACTION_ROW (row), sub);
  adw_action_row_add_prefix (ADW_ACTION_ROW (row), adw_avatar_new (34, title, TRUE));
  gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), TRUE);
  g_signal_connect (row, "activated", G_CALLBACK (on_row_activated), page);

  GtkWidget *star = flat_button (c->favorite ? "starred-symbolic" : "non-starred-symbolic",
                                 c->favorite ? "Remove from Favorites" : "Add to Favorites");
  if (c->favorite)
    gtk_widget_add_css_class (star, "favorite-star");
  g_object_set_data (G_OBJECT (star), "contact", copy);
  g_signal_connect (star, "clicked", G_CALLBACK (on_star_clicked), page);
  adw_action_row_add_suffix (ADW_ACTION_ROW (row), star);

  GtkWidget *write = flat_button ("mail-message-new-symbolic", "Write Email");
  g_object_set_data (G_OBJECT (write), "contact", copy);
  g_signal_connect (write, "clicked", G_CALLBACK (on_write_clicked), page);
  adw_action_row_add_suffix (ADW_ACTION_ROW (row), write);
  return row;
}

void
smail_contacts_page_refresh (GtkWidget *page)
{
  ContactsPage *p = contacts_page (page);
  gtk_list_box_remove_all (GTK_LIST_BOX (p->contacts));
  gtk_list_box_remove_all (GTK_LIST_BOX (p->collected));
  const char *q = gtk_editable_get_text (GTK_EDITABLE (p->search));
  g_autoptr (GPtrArray) all = smail_store_list_contacts (p->store, q && *q ? q : NULL, 500);
  guint n_contacts = 0, n_collected = 0;
  for (guint i = 0; i < all->len; i++) {
    SMailContact *c = g_ptr_array_index (all, i);
    if (c->collected) {
      if (n_collected++ < 200)
        gtk_list_box_append (GTK_LIST_BOX (p->collected), contact_row (page, c));
    } else {
      n_contacts++;
      gtk_list_box_append (GTK_LIST_BOX (p->contacts), contact_row (page, c));
    }
  }
  smail_boxed_list_done (p->contacts);
  gtk_widget_set_visible (p->collected, n_collected > 0);
  gtk_widget_set_visible (p->collected_label, n_collected > 0);
  g_autofree char *count = g_strdup_printf ("%u contact%s, %u collected address%s", n_contacts,
                                            n_contacts == 1 ? "" : "s", n_collected, n_collected == 1 ? "" : "es");
  gtk_label_set_text (GTK_LABEL (p->count), count);
}

/* ---------------- import / export ---------------- */

static void
on_import_chosen (GObject *src, GAsyncResult *res, gpointer data)
{
  GtkWidget *page = data;
  g_autoptr (GFile) file = gtk_file_dialog_open_finish (GTK_FILE_DIALOG (src), res, NULL);
  if (file) {
    g_autoptr (GError) error = NULL;
    g_autoptr (GBytes) bytes = g_file_load_bytes (file, NULL, NULL, &error);
    gsize len = 0;
    const char *d = bytes ? g_bytes_get_data (bytes, &len) : NULL;
    g_autoptr (GPtrArray) contacts = d ? smail_contacts_parse (d, len, &error) : NULL;
    if (!contacts) {
      g_autofree char *msg = g_strdup_printf ("Couldn't import: %s", error->message);
      smail_toast (page, msg);
    } else {
      guint ok = 0;
      for (guint i = 0; i < contacts->len; i++)
        if (smail_store_save_contact (contacts_page (page)->store, g_ptr_array_index (contacts, i), NULL))
          ok++;
      g_autofree char *msg = g_strdup_printf (ok == 1 ? "Imported 1 contact" : "Imported %u contacts", ok);
      smail_toast (page, msg);
      smail_contacts_page_refresh (page);
    }
  }
  g_object_unref (page);
}

static GtkFileFilter *
contacts_filter (void)
{
  GtkFileFilter *f = gtk_file_filter_new ();
  gtk_file_filter_set_name (f, "Contacts (vCard, CSV)");
  gtk_file_filter_add_pattern (f, "*.vcf");
  gtk_file_filter_add_pattern (f, "*.vcard");
  gtk_file_filter_add_pattern (f, "*.csv");
  gtk_file_filter_add_mime_type (f, "text/vcard");
  gtk_file_filter_add_mime_type (f, "text/csv");
  return f;
}

static void
on_import (GtkButton *b, GtkWidget *page)
{
  g_autoptr (GtkFileDialog) fd = gtk_file_dialog_new ();
  gtk_file_dialog_set_title (fd, "Import Contacts");
  g_autoptr (GtkFileFilter) filter = contacts_filter ();
  gtk_file_dialog_set_default_filter (fd, filter);
  gtk_file_dialog_open (fd, GTK_WINDOW (gtk_widget_get_root (page)), NULL, on_import_chosen, g_object_ref (page));
}

static void
on_export_chosen (GObject *src, GAsyncResult *res, gpointer data)
{
  GtkWidget *page = data;
  g_autoptr (GFile) file = gtk_file_dialog_save_finish (GTK_FILE_DIALOG (src), res, NULL);
  if (file) {
    g_autoptr (GPtrArray) all = smail_store_list_contacts (contacts_page (page)->store, NULL, -1);
    g_autoptr (GPtrArray) mine = g_ptr_array_new ();
    for (guint i = 0; i < all->len; i++)
      if (!((SMailContact *) g_ptr_array_index (all, i))->collected)
        g_ptr_array_add (mine, g_ptr_array_index (all, i));
    g_autofree char *vcf = smail_contacts_to_vcard (mine);
    g_autoptr (GError) error = NULL;
    if (g_file_replace_contents (file, vcf, strlen (vcf), NULL, FALSE, G_FILE_CREATE_NONE, NULL, NULL, &error)) {
      g_autofree char *msg = g_strdup_printf ("Exported %u contacts", mine->len);
      smail_toast (page, msg);
    } else {
      smail_toast (page, error->message);
    }
  }
  g_object_unref (page);
}

static void
on_export (GtkButton *b, GtkWidget *page)
{
  g_autoptr (GtkFileDialog) fd = gtk_file_dialog_new ();
  gtk_file_dialog_set_title (fd, "Export Contacts");
  gtk_file_dialog_set_initial_name (fd, "contacts.vcf");
  gtk_file_dialog_save (fd, GTK_WINDOW (gtk_widget_get_root (page)), NULL, on_export_chosen, g_object_ref (page));
}

static void
on_new_contact (GtkButton *b, GtkWidget *page)
{
  open_contact_dialog (page, g_new0 (SMailContact, 1));
}

GtkWidget *
smail_contacts_page_new (SMailStore *store, SMailComposeToFunc compose_to, gpointer user_data)
{
  ContactsPage *p = g_new0 (ContactsPage, 1);
  p->store = store;
  p->compose_to = compose_to;
  p->compose_data = user_data;

  GtkWidget *box;
  GtkWidget *scroll = smail_clamped_column (&box);
  p->search = gtk_search_entry_new ();
  gtk_search_entry_set_placeholder_text (GTK_SEARCH_ENTRY (p->search), "Search name or address");
  gtk_box_append (GTK_BOX (box), p->search);
  p->count = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (p->count), 0);
  gtk_widget_add_css_class (p->count, "caption");
  gtk_widget_add_css_class (p->count, "dim-label");
  gtk_box_append (GTK_BOX (box), p->count);

  gtk_box_append (GTK_BOX (box), smail_section_label ("Contacts"));
  p->contacts = smail_boxed_list ("No contacts yet. Add one with +, import a vCard or CSV file, "
                                  "or star an address below.");
  gtk_box_append (GTK_BOX (box), p->contacts);
  p->collected_label = smail_section_label ("Collected Addresses");
  gtk_widget_set_tooltip_text (p->collected_label, "People you've exchanged mail with, remembered automatically");
  gtk_box_append (GTK_BOX (box), p->collected_label);
  p->collected = smail_boxed_list (NULL);
  gtk_box_append (GTK_BOX (box), p->collected);

  GtkWidget *header;
  GtkWidget *page = smail_page_new ("Address Book", scroll, &header);
  p->page = page;
  g_object_set_data_full (G_OBJECT (page), "contacts-page", p, g_free);
  g_signal_connect_swapped (p->search, "search-changed", G_CALLBACK (smail_contacts_page_refresh), page);

  GtkWidget *add = smail_icon_button ("contact-new-symbolic", "New Contact");
  g_signal_connect (add, "clicked", G_CALLBACK (on_new_contact), page);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), add);
  GtkWidget *exp = smail_icon_button ("document-save-symbolic", "Export Contacts (vCard)");
  g_signal_connect (exp, "clicked", G_CALLBACK (on_export), page);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), exp);
  GtkWidget *imp = smail_icon_button ("document-open-symbolic", "Import Contacts (vCard or CSV)");
  g_signal_connect (imp, "clicked", G_CALLBACK (on_import), page);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), imp);

  smail_contacts_page_refresh (page);
  return page;
}
