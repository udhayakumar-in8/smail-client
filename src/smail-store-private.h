/* smail-store-private.h — raw database access for smail-backup.c only */
#pragma once

#include <sqlite3.h>
#include "smail-store.h"

sqlite3 *smail_store_get_db (SMailStore *store);
