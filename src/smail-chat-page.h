/* smail-chat-page.h — Local network chat page */
#pragma once

#include <adwaita.h>
#include "smail-store.h"
#include "smail-lan.h"

G_BEGIN_DECLS

typedef void (*SMailUnreadFunc) (int unread, gpointer user_data);

/* `lan_error` is shown in a banner when the service could not start (may be NULL). */
GtkWidget *smail_chat_page_new       (SMailStore *store, SMailLan *lan, const char *lan_error);
void       smail_chat_page_set_unread_func (GtkWidget *page, SMailUnreadFunc func, gpointer user_data);
/* Tell the page whether it is on screen (unread counting, notifications). */
void       smail_chat_page_set_active (GtkWidget *page, gboolean active);
GtkWidget *smail_chat_page_get_split (GtkWidget *page);

G_END_DECLS
