/* smail-address-completion.h — suggest contacts while typing in To/Cc/Bcc */
#pragma once

#include <gtk/gtk.h>
#include "smail-store.h"

G_BEGIN_DECLS

/* Attach address suggestions to an editable (GtkText or GtkEntry) holding a
 * comma separated address list. `store` must outlive the widget. */
void smail_address_completion_attach (GtkWidget *editable, SMailStore *store);

G_END_DECLS
