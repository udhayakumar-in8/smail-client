/* smail-backup-dialog.h — "Backup & Restore" dialog */
#pragma once

#include <adwaita.h>
#include "smail-store.h"

G_BEGIN_DECLS

/* Called after a successful restore with the restored accounts (SMailAccount*). */
typedef void (*SMailRestoredFunc) (GPtrArray *accounts, gboolean had_passwords, gpointer user_data);

void smail_backup_dialog_present (GtkWidget         *parent,
                                  SMailStore        *store,
                                  GPtrArray         *accounts,   /* SMailAccount*, with passwords */
                                  SMailRestoredFunc  on_restored,
                                  gpointer           user_data);

G_END_DECLS
