/* smail-backup.h — back up and restore everything SMail keeps on this computer.
 *
 * A backup is a single SQLite file (*.smailbackup) holding a copy of the local
 * database (filters, calendar, tasks, reminders, mail log, chat history,
 * settings), the account settings and, optionally, every downloaded message
 * and sent-mail copy plus the account passwords. */
#pragma once

#include "smail-types.h"
#include "smail-store.h"

G_BEGIN_DECLS

#define SMAIL_BACKUP_EXTENSION ".smailbackup"

typedef struct {
  gint64 accounts;
  gint64 filters;
  gint64 events;
  gint64 todos;
  gint64 reminders;
  gint64 log_entries;
  gint64 chat_messages;
  gint64 contacts;
  gint64 emails;       /* stored .eml files */
  gint64 bytes;        /* size of the stored files */
  gboolean passwords;
  char  *created;      /* ISO 8601 */
} SMailBackupSummary;

void     smail_backup_summary_clear (SMailBackupSummary *summary);

/* `accounts`: SMailAccount* (passwords are only written if include_passwords). */
gboolean smail_backup_create  (SMailStore *store, GPtrArray *accounts, const char *path,
                               gboolean include_emails, gboolean include_passwords,
                               SMailBackupSummary *summary, GError **error);

/* Reads the summary without changing anything. */
gboolean smail_backup_inspect (const char *path, SMailBackupSummary *summary, GError **error);

/* Replaces the local data with the backup's. `accounts_out` receives the
 * backed-up accounts (SMailAccount*); the caller saves them. The local
 * chat identity is kept so two restored computers don't clash. */
gboolean smail_backup_restore (SMailStore *store, const char *path, GPtrArray **accounts_out,
                               SMailBackupSummary *summary, GError **error);

/* Path for an automatic safety copy: <data>/backups/<prefix>-<timestamp>.smailbackup */
char    *smail_backup_auto_path (const char *prefix);

G_END_DECLS
