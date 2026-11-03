/* smail-theme.h — "System", "Matte White" and "Matte Black" styles */
#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define SMAIL_THEME_SYSTEM      "system"
#define SMAIL_THEME_MATTE_WHITE "matte-white"
#define SMAIL_THEME_MATTE_BLACK "matte-black"

/* Apply a style to the whole application. Unknown names fall back to "system". */
void        smail_theme_apply   (const char *name);
const char *smail_theme_current (void);
gboolean    smail_theme_is_valid (const char *name);

G_END_DECLS
