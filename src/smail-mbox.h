/* smail-mbox.h — read and write mbox files (the format Thunderbird stores folders in) */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* Split an mbox file — or a single .eml message — into messages (GBytes*,
 * CRLF line endings, ready for IMAP APPEND). ">From " quoting is undone. */
GPtrArray *smail_mbox_split  (const char *data, gsize len);

/* Append one message to an mbox being built (mboxrd quoting, LF endings). */
void       smail_mbox_append (GString *mbox, GBytes *message, const char *sender, GDateTime *date);

G_END_DECLS
