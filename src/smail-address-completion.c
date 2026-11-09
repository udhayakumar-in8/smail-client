/* smail-address-completion.c — contact suggestions under an address field */
#include "smail-address-completion.h"
#include "smail-contacts-page.h"

#include <adwaita.h>
#include <string.h>

#define MAX_SUGGESTIONS 6

typedef struct {
  GtkWidget  *editable;
  GtkWidget  *popover;
  GtkWidget  *list;
  SMailStore *store;
  gboolean    updating;  /* we're changing the text ourselves */
  guint       hide_id;
} Completion;

/* The address being typed: everything after the last comma. */
static const char *
current_token (const char *text, gsize *prefix_len)
{
  const char *comma = strrchr (text, ',');
  const char *tok = comma ? comma + 1 : text;
  *prefix_len = tok - text;
  while (*tok == ' ')
    tok++;
  return tok;
}

static void
hide (Completion *c)
{
  gtk_popover_popdown (GTK_POPOVER (c->popover));
}

static void
accept_row (Completion *c, GtkListBoxRow *row)
{
  const char *addr = row ? g_object_get_data (G_OBJECT (row), "address") : NULL;
  if (!addr)
    return;
  const char *text = gtk_editable_get_text (GTK_EDITABLE (c->editable));
  gsize prefix_len;
  current_token (text, &prefix_len);
  g_autofree char *prefix = g_strndup (text, prefix_len);
  g_strchomp (prefix);
  g_autofree char *new_text = g_strdup_printf ("%s%s%s, ", prefix, *prefix ? " " : "", addr);
  c->updating = TRUE;
  gtk_editable_set_text (GTK_EDITABLE (c->editable), new_text);
  gtk_editable_set_position (GTK_EDITABLE (c->editable), -1);
  c->updating = FALSE;
  hide (c);
}

static void
on_row_activated (GtkListBox *box, GtkListBoxRow *row, Completion *c)
{
  accept_row (c, row);
  gtk_widget_grab_focus (c->editable);
  gtk_editable_set_position (GTK_EDITABLE (c->editable), -1);
}

static void
on_changed (GtkEditable *editable, Completion *c)
{
  if (c->updating || !c->store)
    return;
  gsize prefix_len;
  const char *tok = current_token (gtk_editable_get_text (editable), &prefix_len);
  if (strlen (tok) < 1) {
    hide (c);
    return;
  }

  g_autoptr (GPtrArray) matches = smail_store_list_contacts (c->store, tok, MAX_SUGGESTIONS);
  gtk_list_box_remove_all (GTK_LIST_BOX (c->list));
  for (guint i = 0; i < matches->len; i++) {
    SMailContact *ct = g_ptr_array_index (matches, i);
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
    const char *title = ct->name && *ct->name ? ct->name : ct->email;
    gtk_box_append (GTK_BOX (box), adw_avatar_new (28, title, TRUE));
    GtkWidget *labels = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *name = gtk_label_new (title);
    gtk_label_set_xalign (GTK_LABEL (name), 0);
    gtk_label_set_ellipsize (GTK_LABEL (name), PANGO_ELLIPSIZE_END);
    gtk_box_append (GTK_BOX (labels), name);
    if (ct->name && *ct->name) {
      GtkWidget *mail = gtk_label_new (ct->email);
      gtk_label_set_xalign (GTK_LABEL (mail), 0);
      gtk_label_set_ellipsize (GTK_LABEL (mail), PANGO_ELLIPSIZE_END);
      gtk_widget_add_css_class (mail, "caption");
      gtk_widget_add_css_class (mail, "dim-label");
      gtk_box_append (GTK_BOX (labels), mail);
    }
    gtk_box_append (GTK_BOX (box), labels);
    if (ct->favorite)
      gtk_box_append (GTK_BOX (box), gtk_image_new_from_icon_name ("starred-symbolic"));
    GtkWidget *row = gtk_list_box_row_new ();
    gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);
    g_object_set_data_full (G_OBJECT (row), "address", smail_contact_format_address (ct->name, ct->email), g_free);
    gtk_list_box_append (GTK_LIST_BOX (c->list), row);
  }

  if (matches->len == 0) {
    hide (c);
    return;
  }
  gtk_list_box_select_row (GTK_LIST_BOX (c->list), gtk_list_box_get_row_at_index (GTK_LIST_BOX (c->list), 0));
  gtk_widget_set_size_request (c->popover, MAX (gtk_widget_get_width (c->editable), 280), -1);
  if (gtk_widget_get_visible (c->popover))
    gtk_popover_present (GTK_POPOVER (c->popover)); /* re-measure: the list may have shrunk */
  else
    gtk_popover_popup (GTK_POPOVER (c->popover));
}

static gboolean
on_key (GtkEventControllerKey *ctl, guint keyval, guint keycode, GdkModifierType state, Completion *c)
{
  if (!gtk_widget_get_visible (c->popover))
    return FALSE;
  GtkListBoxRow *sel = gtk_list_box_get_selected_row (GTK_LIST_BOX (c->list));
  int idx = sel ? gtk_list_box_row_get_index (sel) : -1;
  switch (keyval) {
  case GDK_KEY_Down:
  case GDK_KEY_Up: {
    int next = idx + (keyval == GDK_KEY_Down ? 1 : -1);
    GtkListBoxRow *r = gtk_list_box_get_row_at_index (GTK_LIST_BOX (c->list), MAX (next, 0));
    if (r)
      gtk_list_box_select_row (GTK_LIST_BOX (c->list), r);
    return TRUE;
  }
  case GDK_KEY_Return:
  case GDK_KEY_KP_Enter:
  case GDK_KEY_Tab:
  case GDK_KEY_ISO_Enter:
    if (state & (GDK_CONTROL_MASK | GDK_ALT_MASK))
      return FALSE; /* Ctrl+Enter still sends */
    accept_row (c, sel);
    return TRUE;
  case GDK_KEY_Escape:
    hide (c);
    return TRUE;
  default:
    return FALSE;
  }
}

static gboolean
hide_later (gpointer data)
{
  Completion *c = data;
  c->hide_id = 0;
  hide (c);
  return G_SOURCE_REMOVE;
}

static void
on_focus_leave (GtkEventControllerFocus *ctl, Completion *c)
{
  /* give a click on a suggestion time to land first */
  if (!c->hide_id)
    c->hide_id = g_timeout_add (200, hide_later, c);
}

static void
on_destroy (GtkWidget *editable, Completion *c)
{
  g_clear_handle_id (&c->hide_id, g_source_remove);
  if (c->popover) {
    gtk_widget_unparent (c->popover);
    c->popover = NULL;
  }
}

void
smail_address_completion_attach (GtkWidget *editable, SMailStore *store)
{
  Completion *c = g_new0 (Completion, 1);
  c->editable = editable;
  c->store = store;

  c->list = gtk_list_box_new ();
  gtk_widget_add_css_class (c->list, "navigation-sidebar");
  gtk_widget_add_css_class (c->list, "address-suggestions");
  g_signal_connect (c->list, "row-activated", G_CALLBACK (on_row_activated), c);
  c->popover = gtk_popover_new ();
  gtk_popover_set_child (GTK_POPOVER (c->popover), c->list);
  gtk_popover_set_autohide (GTK_POPOVER (c->popover), FALSE); /* keep typing in the field */
  gtk_popover_set_has_arrow (GTK_POPOVER (c->popover), FALSE);
  gtk_popover_set_position (GTK_POPOVER (c->popover), GTK_POS_BOTTOM);
  gtk_widget_set_halign (c->popover, GTK_ALIGN_START);
  gtk_widget_set_parent (c->popover, editable);

  g_signal_connect (editable, "changed", G_CALLBACK (on_changed), c);
  GtkEventController *keys = gtk_event_controller_key_new ();
  gtk_event_controller_set_propagation_phase (keys, GTK_PHASE_CAPTURE);
  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_key), c);
  gtk_widget_add_controller (editable, keys);
  GtkEventController *focus = gtk_event_controller_focus_new ();
  g_signal_connect (focus, "leave", G_CALLBACK (on_focus_leave), c);
  gtk_widget_add_controller (editable, focus);
  g_signal_connect (editable, "destroy", G_CALLBACK (on_destroy), c);
  g_object_set_data_full (G_OBJECT (editable), "smail-completion", c, g_free);
}
