/* smail-composer.h — new message / reply / forward window */
#pragma once

#include <adwaita.h>
#include "smail-backend.h"

G_BEGIN_DECLS

typedef enum {
  SMAIL_COMPOSE_NEW,
  SMAIL_COMPOSE_REPLY,
  SMAIL_COMPOSE_REPLY_ALL,
  SMAIL_COMPOSE_FORWARD,
} SMailComposeMode;

#define SMAIL_TYPE_COMPOSER (smail_composer_get_type ())
G_DECLARE_FINAL_TYPE (SMailComposer, smail_composer, SMAIL, COMPOSER, AdwWindow)

/* Emits "sent" (GBytes *message) after the message has been handed to the server. */
SMailComposer *smail_composer_new (GtkWindow                  *parent,
                                       SMailBackend             *backend,
                                       SMailComposeMode          mode,
                                       const SMailMessageDetail *original);

SMailBackend  *smail_composer_get_backend (SMailComposer *self);

G_END_DECLS
