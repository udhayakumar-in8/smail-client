/* smail-mail-tools.h — Filters and Mail Log pages */
#pragma once

#include <adwaita.h>
#include "smail-store.h"

G_BEGIN_DECLS

typedef void (*SMailRunFiltersFunc) (gpointer user_data);

GtkWidget *smail_filters_page_new         (SMailStore *store, SMailRunFiltersFunc run_now, gpointer user_data);
void       smail_filters_page_refresh     (GtkWidget *page);
/* Folder names offered for "Move to folder" (NULL-terminated, copied). */
void       smail_filters_page_set_folders (GtkWidget *page, const char * const *folders);

GtkWidget *smail_mail_log_page_new        (SMailStore *store);
void       smail_mail_log_page_refresh    (GtkWidget *page);

G_END_DECLS
