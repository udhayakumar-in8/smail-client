/* courier-mail-tools.h — Filters and Mail Log pages */
#pragma once

#include <adwaita.h>
#include "courier-store.h"

G_BEGIN_DECLS

typedef void (*CourierRunFiltersFunc) (gpointer user_data);

GtkWidget *courier_filters_page_new         (CourierStore *store, CourierRunFiltersFunc run_now, gpointer user_data);
void       courier_filters_page_refresh     (GtkWidget *page);
/* Folder names offered for "Move to folder" (NULL-terminated, copied). */
void       courier_filters_page_set_folders (GtkWidget *page, const char * const *folders);

GtkWidget *courier_mail_log_page_new        (CourierStore *store);
void       courier_mail_log_page_refresh    (GtkWidget *page);

G_END_DECLS
