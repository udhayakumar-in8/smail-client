/* smail-message-view.h — the reading pane */
#pragma once

#include <adwaita.h>
#include "smail-types.h"

G_BEGIN_DECLS

#define SMAIL_TYPE_MESSAGE_VIEW (smail_message_view_get_type ())
G_DECLARE_FINAL_TYPE (SMailMessageView, smail_message_view, SMAIL, MESSAGE_VIEW, GtkBox)

GtkWidget                  *smail_message_view_new        (void);
/* takes ownership of `detail` */
void                        smail_message_view_set_detail (SMailMessageView *self, SMailMessageDetail *detail);
const SMailMessageDetail *smail_message_view_get_detail (SMailMessageView *self);

G_END_DECLS
