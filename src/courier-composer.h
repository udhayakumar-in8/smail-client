/* courier-composer.h — new message / reply / forward window */
#pragma once

#include <adwaita.h>
#include "courier-backend.h"

G_BEGIN_DECLS

typedef enum {
  COURIER_COMPOSE_NEW,
  COURIER_COMPOSE_REPLY,
  COURIER_COMPOSE_REPLY_ALL,
  COURIER_COMPOSE_FORWARD,
} CourierComposeMode;

#define COURIER_TYPE_COMPOSER (courier_composer_get_type ())
G_DECLARE_FINAL_TYPE (CourierComposer, courier_composer, COURIER, COMPOSER, AdwWindow)

/* Emits "sent" after the message has been handed to the server. */
CourierComposer *courier_composer_new (GtkWindow                  *parent,
                                       CourierBackend             *backend,
                                       CourierComposeMode          mode,
                                       const CourierMessageDetail *original);

G_END_DECLS
