/* courier-window.h — main three-pane window */
#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define COURIER_TYPE_WINDOW (courier_window_get_type ())
G_DECLARE_FINAL_TYPE (CourierWindow, courier_window, COURIER, WINDOW, AdwApplicationWindow)

CourierWindow *courier_window_new (AdwApplication *app, gboolean demo);

G_END_DECLS
