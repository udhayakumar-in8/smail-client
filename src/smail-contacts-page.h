/* smail-contacts-page.h — Address Book page */
#pragma once

#include <adwaita.h>
#include "smail-store.h"

G_BEGIN_DECLS

/* Called with "Name <address>" when the user wants to write to a contact. */
typedef void (*SMailComposeToFunc) (const char *recipient, gpointer user_data);

GtkWidget *smail_contacts_page_new     (SMailStore *store, SMailComposeToFunc compose_to, gpointer user_data);
void       smail_contacts_page_refresh (GtkWidget *page);

/* "Name <address>" with proper quoting, for To/Cc fields. */
char      *smail_contact_format_address (const char *name, const char *email);

G_END_DECLS
