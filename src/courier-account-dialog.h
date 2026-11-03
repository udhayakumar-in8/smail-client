/* courier-account-dialog.h — add / edit an IMAP+SMTP account */
#pragma once

#include <adwaita.h>
#include "courier-types.h"

G_BEGIN_DECLS

/* Called after the server accepted the login. `account` is owned by the dialog. */
typedef void (*CourierAccountSavedFunc) (const CourierAccount *account, gpointer user_data);

void courier_account_dialog_present (GtkWidget               *parent,
                                     const CourierAccount    *existing, /* NULL to add */
                                     CourierAccountSavedFunc  callback,
                                     gpointer                 user_data);

G_END_DECLS
