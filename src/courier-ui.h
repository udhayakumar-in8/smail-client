/* courier-ui.h — small widgets and helpers shared by the organizer pages */
#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

/* Show a toast in the nearest AdwToastOverlay above `widget`. */
void       courier_toast (GtkWidget *widget, const char *text);

/* AdwToolbarView with a header bar titled `title`; returns the toolbar view. */
GtkWidget *courier_page_new        (const char *title, GtkWidget *content, GtkWidget **header_out);
GtkWidget *courier_icon_button     (const char *icon, const char *tooltip);
GtkWidget *courier_boxed_list      (const char *placeholder);
/* Call after filling a boxed list: adds the placeholder row if it is empty. */
void       courier_boxed_list_done (GtkWidget *list);
GtkWidget *courier_section_label   (const char *text);
/* Scrolled, centred column for page content. */
GtkWidget *courier_clamped_column  (GtkWidget **box_out);

char      *courier_format_unix       (gint64 t);   /* "Tue 3 Nov, 14:00" (year added if not this year) */
char      *courier_format_unix_time  (gint64 t);   /* "14:00" */
gint64     courier_unix_now          (void);

/* Date + time picker: a calendar popover button and hour/minute spinners. */
#define COURIER_TYPE_DATE_TIME (courier_date_time_get_type ())
G_DECLARE_FINAL_TYPE (CourierDateTime, courier_date_time, COURIER, DATE_TIME, GtkBox)

GtkWidget *courier_date_time_new      (gint64 unix_time);
gint64     courier_date_time_get_unix (CourierDateTime *self);
void       courier_date_time_set_unix (CourierDateTime *self, gint64 unix_time);

/* Round up to the next quarter hour, as a sensible default start time. */
gint64     courier_next_quarter_hour  (gint64 unix_time);

G_END_DECLS
