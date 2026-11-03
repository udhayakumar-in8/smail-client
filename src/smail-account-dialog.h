/* smail-account-dialog.h — add / edit an IMAP+SMTP account */
#pragma once

#include <adwaita.h>
#include "smail-types.h"

G_BEGIN_DECLS

/* Called after the server accepted the login. `account` is owned by the dialog. */
typedef void (*SMailAccountSavedFunc) (const SMailAccount *account, gpointer user_data);

void smail_account_dialog_present (GtkWidget               *parent,
                                     const SMailAccount    *existing, /* NULL to add */
                                     SMailAccountSavedFunc  callback,
                                     gpointer                 user_data);

G_END_DECLS
