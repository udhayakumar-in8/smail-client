/* smail-window.h — main three-pane window */
#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define SMAIL_TYPE_WINDOW (smail_window_get_type ())
G_DECLARE_FINAL_TYPE (SMailWindow, smail_window, SMAIL, WINDOW, AdwApplicationWindow)

SMailWindow *smail_window_new (AdwApplication *app, gboolean demo);

G_END_DECLS
