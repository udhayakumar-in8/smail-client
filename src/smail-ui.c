/* smail-ui.c — shared UI helpers and the date-time picker */
#include "smail-ui.h"

void
smail_toast (GtkWidget *widget, const char *text)
{
  GtkWidget *overlay = gtk_widget_get_ancestor (widget, ADW_TYPE_TOAST_OVERLAY);
  if (overlay)
    adw_toast_overlay_add_toast (ADW_TOAST_OVERLAY (overlay), adw_toast_new (text));
  else
    g_message ("%s", text);
}

GtkWidget *
smail_page_new (const char *title, GtkWidget *content, GtkWidget **header_out)
{
  GtkWidget *toolbar = adw_toolbar_view_new ();
  GtkWidget *header = adw_header_bar_new ();
  adw_header_bar_set_title_widget (ADW_HEADER_BAR (header), adw_window_title_new (title, NULL));
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), header);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), content);
  if (header_out)
    *header_out = header;
  return toolbar;
}

GtkWidget *
smail_icon_button (const char *icon, const char *tooltip)
{
  GtkWidget *b = gtk_button_new_from_icon_name (icon);
  gtk_widget_set_tooltip_text (b, tooltip);
  gtk_accessible_update_property (GTK_ACCESSIBLE (b), GTK_ACCESSIBLE_PROPERTY_LABEL, tooltip, -1);
  return b;
}

GtkWidget *
smail_boxed_list (const char *placeholder)
{
  GtkWidget *list = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class (list, "boxed-list");
  if (placeholder)
    g_object_set_data_full (G_OBJECT (list), "empty-text", g_strdup (placeholder), g_free);
  return list;
}

void
smail_boxed_list_done (GtkWidget *list)
{
  const char *text = g_object_get_data (G_OBJECT (list), "empty-text");
  if (!text || gtk_widget_get_first_child (list))
    return;
  GtkWidget *label = gtk_label_new (text);
  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_widget_add_css_class (label, "dim-label");
  gtk_widget_add_css_class (label, "list-placeholder");
  GtkWidget *row = gtk_list_box_row_new ();
  gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), FALSE);
  gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), label);
  gtk_list_box_append (GTK_LIST_BOX (list), row);
}

GtkWidget *
smail_section_label (const char *text)
{
  GtkWidget *l = gtk_label_new (text);
  gtk_label_set_xalign (GTK_LABEL (l), 0);
  gtk_widget_add_css_class (l, "heading");
  gtk_widget_add_css_class (l, "section-label");
  return l;
}

GtkWidget *
smail_clamped_column (GtkWidget **box_out)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_add_css_class (box, "page-column");
  GtkWidget *clamp = adw_clamp_new ();
  adw_clamp_set_maximum_size (ADW_CLAMP (clamp), 720);
  adw_clamp_set_child (ADW_CLAMP (clamp), box);
  GtkWidget *scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), clamp);
  gtk_widget_set_vexpand (scroll, TRUE);
  *box_out = box;
  return scroll;
}

gint64
smail_unix_now (void)
{
  return g_get_real_time () / G_USEC_PER_SEC;
}

char *
smail_format_unix (gint64 t)
{
  g_autoptr (GDateTime) dt = g_date_time_new_from_unix_local (t);
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  if (g_date_time_get_year (dt) == g_date_time_get_year (now))
    return g_date_time_format (dt, "%a %-d %b, %H:%M");
  return g_date_time_format (dt, "%a %-d %b %Y, %H:%M");
}

char *
smail_format_unix_time (gint64 t)
{
  g_autoptr (GDateTime) dt = g_date_time_new_from_unix_local (t);
  return g_date_time_format (dt, "%H:%M");
}

gint64
smail_next_quarter_hour (gint64 t)
{
  gint64 q = 15 * 60;
  return ((t + q - 1) / q) * q;
}

/* ---------------- SMailDateTime ---------------- */

struct _SMailDateTime {
  GtkBox     parent_instance;
  GtkWidget *date_button;
  GtkWidget *calendar;
  GtkWidget *hour;
  GtkWidget *minute;
};

G_DEFINE_FINAL_TYPE (SMailDateTime, smail_date_time, GTK_TYPE_BOX)

static void
update_date_label (SMailDateTime *self)
{
  g_autoptr (GDateTime) d = gtk_calendar_get_date (GTK_CALENDAR (self->calendar));
  g_autofree char *label = g_date_time_format (d, "%a %-d %b %Y");
  gtk_menu_button_set_label (GTK_MENU_BUTTON (self->date_button), label);
}

static void
on_day_selected (GtkCalendar *calendar, SMailDateTime *self)
{
  update_date_label (self);
  gtk_menu_button_popdown (GTK_MENU_BUTTON (self->date_button));
}

static gboolean
on_spin_output (GtkSpinButton *spin, gpointer data)
{
  g_autofree char *text = g_strdup_printf ("%02d", gtk_spin_button_get_value_as_int (spin));
  gtk_editable_set_text (GTK_EDITABLE (spin), text);
  return TRUE;
}

static GtkWidget *
spin (int max, int step, const char *label)
{
  GtkWidget *s = gtk_spin_button_new_with_range (0, max, step);
  gtk_spin_button_set_wrap (GTK_SPIN_BUTTON (s), TRUE);
  gtk_widget_set_valign (s, GTK_ALIGN_CENTER);
  gtk_accessible_update_property (GTK_ACCESSIBLE (s), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
  g_signal_connect (s, "output", G_CALLBACK (on_spin_output), NULL);
  return s;
}

static void
smail_date_time_class_init (SMailDateTimeClass *klass)
{
}

static void
smail_date_time_init (SMailDateTime *self)
{
  gtk_box_set_spacing (GTK_BOX (self), 6);
  gtk_widget_set_valign (GTK_WIDGET (self), GTK_ALIGN_CENTER);

  self->calendar = gtk_calendar_new ();
  g_signal_connect (self->calendar, "day-selected", G_CALLBACK (on_day_selected), self);
  GtkWidget *popover = gtk_popover_new ();
  gtk_popover_set_child (GTK_POPOVER (popover), self->calendar);
  self->date_button = gtk_menu_button_new ();
  gtk_menu_button_set_popover (GTK_MENU_BUTTON (self->date_button), popover);
  gtk_widget_set_valign (self->date_button, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (self), self->date_button);

  self->hour = spin (23, 1, "Hour");
  self->minute = spin (59, 5, "Minute");
  gtk_box_append (GTK_BOX (self), self->hour);
  gtk_box_append (GTK_BOX (self), gtk_label_new (":"));
  gtk_box_append (GTK_BOX (self), self->minute);
}

void
smail_date_time_set_unix (SMailDateTime *self, gint64 t)
{
  g_autoptr (GDateTime) dt = g_date_time_new_from_unix_local (t);
  smail_calendar_set_date (GTK_CALENDAR (self->calendar), dt);
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (self->hour), g_date_time_get_hour (dt));
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (self->minute), g_date_time_get_minute (dt));
  update_date_label (self);
}

gint64
smail_date_time_get_unix (SMailDateTime *self)
{
  g_autoptr (GDateTime) d = gtk_calendar_get_date (GTK_CALENDAR (self->calendar));
  g_autoptr (GTimeZone) tz = g_time_zone_new_local ();
  g_autoptr (GDateTime) dt = g_date_time_new (tz, g_date_time_get_year (d), g_date_time_get_month (d),
                                              g_date_time_get_day_of_month (d),
                                              gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (self->hour)),
                                              gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (self->minute)), 0);
  return g_date_time_to_unix (dt);
}

GtkWidget *
smail_date_time_new (gint64 t)
{
  SMailDateTime *self = g_object_new (SMAIL_TYPE_DATE_TIME, NULL);
  smail_date_time_set_unix (self, t);
  return GTK_WIDGET (self);
}
