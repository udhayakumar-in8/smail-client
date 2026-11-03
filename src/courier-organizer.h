/* courier-organizer.h — Calendar, Tasks and Reminders pages */
#pragma once

#include <adwaita.h>
#include "courier-store.h"

G_BEGIN_DECLS

GtkWidget *courier_calendar_page_new      (CourierStore *store);
void       courier_calendar_page_refresh  (GtkWidget *page);

GtkWidget *courier_tasks_page_new         (CourierStore *store);
void       courier_tasks_page_refresh     (GtkWidget *page);

GtkWidget *courier_reminders_page_new     (CourierStore *store);
void       courier_reminders_page_refresh (GtkWidget *page);

G_END_DECLS
