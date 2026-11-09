/* smail-composer.h — new message / reply / forward window */
#pragma once

#include <adwaita.h>
#include "smail-backend.h"
#include "smail-store.h"

G_BEGIN_DECLS

typedef enum {
  SMAIL_COMPOSE_NEW,
  SMAIL_COMPOSE_REPLY,
  SMAIL_COMPOSE_REPLY_ALL,
  SMAIL_COMPOSE_FORWARD,
  SMAIL_COMPOSE_DRAFT,     /* continue a saved draft */
} SMailComposeMode;

#define SMAIL_TYPE_COMPOSER (smail_composer_get_type ())
G_DECLARE_FINAL_TYPE (SMailComposer, smail_composer, SMAIL, COMPOSER, AdwWindow)

/* Emits "sent" (GBytes *message) after the message has been handed to the server. */
SMailComposer *smail_composer_new (GtkWindow                  *parent,
                                       SMailBackend             *backend,
                                       SMailComposeMode          mode,
                                       const SMailMessageDetail *original);

SMailBackend  *smail_composer_get_backend (SMailComposer *self);
/* Suggest addresses from the address book in To/Cc/Bcc. */
void            smail_composer_set_address_book (SMailComposer *self, SMailStore *store);
void            smail_composer_set_to           (SMailComposer *self, const char *to);

G_END_DECLS
