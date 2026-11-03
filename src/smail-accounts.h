/* smail-accounts.h — persist accounts (key file) and passwords (libsecret) */
#pragma once

#include "smail-types.h"

G_BEGIN_DECLS

GPtrArray *smail_accounts_load            (void);                       /* SMailAccount* */
gboolean   smail_accounts_save            (GPtrArray *accounts, GError **error);
gboolean   smail_accounts_store_password  (const SMailAccount *account, GError **error);
void       smail_accounts_clear_password  (const char *account_id);

G_END_DECLS
