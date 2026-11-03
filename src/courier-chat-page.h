/* courier-chat-page.h — Local network chat page */
#pragma once

#include <adwaita.h>
#include "courier-store.h"
#include "courier-lan.h"

G_BEGIN_DECLS

typedef void (*CourierUnreadFunc) (int unread, gpointer user_data);

/* `lan_error` is shown in a banner when the service could not start (may be NULL). */
GtkWidget *courier_chat_page_new       (CourierStore *store, CourierLan *lan, const char *lan_error);
void       courier_chat_page_set_unread_func (GtkWidget *page, CourierUnreadFunc func, gpointer user_data);
/* Tell the page whether it is on screen (unread counting, notifications). */
void       courier_chat_page_set_active (GtkWidget *page, gboolean active);
GtkWidget *courier_chat_page_get_split (GtkWidget *page);

G_END_DECLS
