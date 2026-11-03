/* courier-util.h — formatting helpers */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

char *courier_format_date_short (GDateTime *date);   /* "14:05", "Yesterday", "3 Nov", "03/11/2024" */
char *courier_format_date_long  (GDateTime *date);   /* "Tuesday, 3 November 2026 at 14:05"        */
char *courier_html_to_text      (const char *html);
char *courier_text_to_html      (const char *text);  /* escaped, linkified, quotes styled */
char *courier_quote_text        (const char *text);  /* prefix every line with "> " */

G_END_DECLS
