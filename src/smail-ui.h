/* smail-ui.h — small widgets and helpers shared by the organizer pages */
#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

/* Show a toast in the nearest AdwToastOverlay above `widget`. */
void       smail_toast (GtkWidget *widget, const char *text);

/* AdwToolbarView with a header bar titled `title`; returns the toolbar view. */
GtkWidget *smail_page_new        (const char *title, GtkWidget *content, GtkWidget **header_out);
GtkWidget *smail_icon_button     (const char *icon, const char *tooltip);
GtkWidget *smail_boxed_list      (const char *placeholder);
/* Call after filling a boxed list: adds the placeholder row if it is empty. */
void       smail_boxed_list_done (GtkWidget *list);
GtkWidget *smail_section_label   (const char *text);
/* Scrolled, centred column for page content. */
GtkWidget *smail_clamped_column  (GtkWidget **box_out);

char      *smail_format_unix       (gint64 t);   /* "Tue 3 Nov, 14:00" (year added if not this year) */
char      *smail_format_unix_time  (gint64 t);   /* "14:00" */
gint64     smail_unix_now          (void);

/* Date + time picker: a calendar popover button and hour/minute spinners. */
#define SMAIL_TYPE_DATE_TIME (smail_date_time_get_type ())
G_DECLARE_FINAL_TYPE (SMailDateTime, smail_date_time, SMAIL, DATE_TIME, GtkBox)

GtkWidget *smail_date_time_new      (gint64 unix_time);
gint64     smail_date_time_get_unix (SMailDateTime *self);
void       smail_date_time_set_unix (SMailDateTime *self, gint64 unix_time);

/* gtk_calendar_select_day() is deprecated since GTK 4.20 */
static inline void
smail_calendar_set_date (GtkCalendar *calendar, GDateTime *date)
{
#if GTK_CHECK_VERSION (4, 20, 0)
  gtk_calendar_set_date (calendar, date);
#else
  gtk_calendar_select_day (calendar, date);
#endif
}

/* Round up to the next quarter hour, as a sensible default start time. */
gint64     smail_next_quarter_hour  (gint64 unix_time);

G_END_DECLS
