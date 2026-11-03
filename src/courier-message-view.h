/* courier-message-view.h — the reading pane */
#pragma once

#include <adwaita.h>
#include "courier-types.h"

G_BEGIN_DECLS

#define COURIER_TYPE_MESSAGE_VIEW (courier_message_view_get_type ())
G_DECLARE_FINAL_TYPE (CourierMessageView, courier_message_view, COURIER, MESSAGE_VIEW, GtkBox)

GtkWidget                  *courier_message_view_new        (void);
/* takes ownership of `detail` */
void                        courier_message_view_set_detail (CourierMessageView *self, CourierMessageDetail *detail);
const CourierMessageDetail *courier_message_view_get_detail (CourierMessageView *self);

G_END_DECLS
