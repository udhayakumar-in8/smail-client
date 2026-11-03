/* smail-automation.h — mail filter rules and the automatic mail log */
#pragma once

#include "smail-types.h"
#include "smail-store.h"

G_BEGIN_DECLS

/* Does the rule match this message? Matching ignores case. */
gboolean smail_filter_matches  (const SMailFilter *filter, SMailMessage *message);
/* "If From contains “bank” → Move to “Finance”" */
char    *smail_filter_describe (const SMailFilter *filter);
/* move/trash end processing of a message; read/star let later rules run too */
gboolean smail_filter_is_final (const SMailFilter *filter);

/* Labels for the UI, NULL-terminated, index-aligned with the stored keys. */
extern const char * const smail_filter_fields[];
extern const char * const smail_filter_field_labels[];
extern const char * const smail_filter_ops[];
extern const char * const smail_filter_op_labels[];
extern const char * const smail_filter_actions[];
extern const char * const smail_filter_action_labels[];
guint smail_filter_index_of (const char * const *keys, const char *key);

/* Mail log entries */
SMailMailLogEntry *smail_mail_log_from_message (SMailMessage *message, const char *account,
                                                    const char *folder);
/* Parses an outgoing message, keeps a full .eml copy in `eml_dir` (may be NULL). */
SMailMailLogEntry *smail_mail_log_from_sent    (GBytes *raw, const char *account, const char *eml_dir,
                                                    GError **error);

G_END_DECLS
