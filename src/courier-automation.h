/* courier-automation.h — mail filter rules and the automatic mail log */
#pragma once

#include "courier-types.h"
#include "courier-store.h"

G_BEGIN_DECLS

/* Does the rule match this message? Matching ignores case. */
gboolean courier_filter_matches  (const CourierFilter *filter, CourierMessage *message);
/* "If From contains “bank” → Move to “Finance”" */
char    *courier_filter_describe (const CourierFilter *filter);
/* move/trash end processing of a message; read/star let later rules run too */
gboolean courier_filter_is_final (const CourierFilter *filter);

/* Labels for the UI, NULL-terminated, index-aligned with the stored keys. */
extern const char * const courier_filter_fields[];
extern const char * const courier_filter_field_labels[];
extern const char * const courier_filter_ops[];
extern const char * const courier_filter_op_labels[];
extern const char * const courier_filter_actions[];
extern const char * const courier_filter_action_labels[];
guint courier_filter_index_of (const char * const *keys, const char *key);

/* Mail log entries */
CourierMailLogEntry *courier_mail_log_from_message (CourierMessage *message, const char *account,
                                                    const char *folder);
/* Parses an outgoing message, keeps a full .eml copy in `eml_dir` (may be NULL). */
CourierMailLogEntry *courier_mail_log_from_sent    (GBytes *raw, const char *account, const char *eml_dir,
                                                    GError **error);

G_END_DECLS
