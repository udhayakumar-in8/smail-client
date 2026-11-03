/* smail-organizer.h — Calendar, Tasks and Reminders pages */
#pragma once

#include <adwaita.h>
#include "smail-store.h"

G_BEGIN_DECLS

GtkWidget *smail_calendar_page_new      (SMailStore *store);
void       smail_calendar_page_refresh  (GtkWidget *page);

GtkWidget *smail_tasks_page_new         (SMailStore *store);
void       smail_tasks_page_refresh     (GtkWidget *page);

GtkWidget *smail_reminders_page_new     (SMailStore *store);
void       smail_reminders_page_refresh (GtkWidget *page);

G_END_DECLS
