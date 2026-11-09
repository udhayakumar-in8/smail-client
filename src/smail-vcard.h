/* smail-vcard.h — import and export the address book (vCard and CSV) */
#pragma once

#include "smail-store.h"

G_BEGIN_DECLS

/* vCard 3.0, one card per contact. */
char      *smail_contacts_to_vcard (GPtrArray *contacts);

/* Reads vCard (2.1/3.0/4.0, as written by Thunderbird, phones, Apple and Google)
 * or CSV with a header row (Thunderbird, Google and Outlook exports).
 * A card with several addresses becomes one contact per address.
 * Returns SMailContact* (id 0), or NULL with `error` set. */
GPtrArray *smail_contacts_parse    (const char *data, gsize len, GError **error);

G_END_DECLS
