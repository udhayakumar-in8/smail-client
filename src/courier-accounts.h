/* courier-accounts.h — persist accounts (key file) and passwords (libsecret) */
#pragma once

#include "courier-types.h"

G_BEGIN_DECLS

GPtrArray *courier_accounts_load            (void);                       /* CourierAccount* */
gboolean   courier_accounts_save            (GPtrArray *accounts, GError **error);
gboolean   courier_accounts_store_password  (const CourierAccount *account, GError **error);
void       courier_accounts_clear_password  (const char *account_id);

G_END_DECLS
