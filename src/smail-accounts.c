/* smail-accounts.c — accounts in ~/.config/smail/accounts.ini, passwords in the keyring */
#include "smail-accounts.h"

#include <libsecret/secret.h>

static const SecretSchema *
password_schema (void)
{
  static const SecretSchema schema = {
    "io.github.udhayakumar_in8.SMail.Account", SECRET_SCHEMA_NONE,
    {
      { "account-id", SECRET_SCHEMA_ATTRIBUTE_STRING },
      { NULL, 0 },
    }
  };
  return &schema;
}

static char *
config_path (void)
{
  return g_build_filename (g_get_user_config_dir (), "smail", "accounts.ini", NULL);
}

static const char *
security_to_string (SMailSecurity s)
{
  switch (s) {
  case SMAIL_SECURITY_STARTTLS: return "starttls";
  case SMAIL_SECURITY_NONE:     return "none";
  default:                        return "ssl";
  }
}

static SMailSecurity
security_from_string (const char *s)
{
  if (g_strcmp0 (s, "starttls") == 0) return SMAIL_SECURITY_STARTTLS;
  if (g_strcmp0 (s, "none") == 0)     return SMAIL_SECURITY_NONE;
  return SMAIL_SECURITY_SSL;
}

GPtrArray *
smail_accounts_load (void)
{
  GPtrArray *accounts = g_ptr_array_new_with_free_func ((GDestroyNotify) smail_account_free);
  g_autofree char *path = config_path ();
  g_autoptr (GKeyFile) kf = g_key_file_new ();
  if (!g_key_file_load_from_file (kf, path, G_KEY_FILE_NONE, NULL))
    return accounts;

  g_auto (GStrv) groups = g_key_file_get_groups (kf, NULL);
  for (int i = 0; groups[i]; i++) {
    const char *g = groups[i];
    SMailAccount *a = smail_account_new ();
    a->id = g_strdup (g);
    a->name = g_key_file_get_string (kf, g, "name", NULL);
    a->email = g_key_file_get_string (kf, g, "email", NULL);
    a->username = g_key_file_get_string (kf, g, "username", NULL);
    a->imap_host = g_key_file_get_string (kf, g, "imap-host", NULL);
    a->imap_port = g_key_file_get_integer (kf, g, "imap-port", NULL);
    g_autofree char *is = g_key_file_get_string (kf, g, "imap-security", NULL);
    a->imap_security = security_from_string (is);
    a->smtp_host = g_key_file_get_string (kf, g, "smtp-host", NULL);
    a->smtp_port = g_key_file_get_integer (kf, g, "smtp-port", NULL);
    g_autofree char *ss = g_key_file_get_string (kf, g, "smtp-security", NULL);
    a->smtp_security = security_from_string (ss);

    g_autoptr (GError) error = NULL;
    a->password = secret_password_lookup_sync (password_schema (), NULL, &error, "account-id", a->id, NULL);
    if (error)
      g_message ("Could not read password for %s from the keyring: %s", a->email, error->message);

    g_ptr_array_add (accounts, a);
  }
  return accounts;
}

gboolean
smail_accounts_save (GPtrArray *accounts, GError **error)
{
  g_autoptr (GKeyFile) kf = g_key_file_new ();
  for (guint i = 0; i < accounts->len; i++) {
    SMailAccount *a = g_ptr_array_index (accounts, i);
    const char *g = a->id;
    g_key_file_set_string (kf, g, "name", a->name ? a->name : "");
    g_key_file_set_string (kf, g, "email", a->email ? a->email : "");
    g_key_file_set_string (kf, g, "username", a->username ? a->username : "");
    g_key_file_set_string (kf, g, "imap-host", a->imap_host ? a->imap_host : "");
    g_key_file_set_integer (kf, g, "imap-port", a->imap_port);
    g_key_file_set_string (kf, g, "imap-security", security_to_string (a->imap_security));
    g_key_file_set_string (kf, g, "smtp-host", a->smtp_host ? a->smtp_host : "");
    g_key_file_set_integer (kf, g, "smtp-port", a->smtp_port);
    g_key_file_set_string (kf, g, "smtp-security", security_to_string (a->smtp_security));
  }
  g_autofree char *path = config_path ();
  g_autofree char *dir = g_path_get_dirname (path);
  g_mkdir_with_parents (dir, 0700);
  return g_key_file_save_to_file (kf, path, error);
}

gboolean
smail_accounts_store_password (const SMailAccount *a, GError **error)
{
  g_autofree char *label = g_strdup_printf ("SMail mail password for %s", a->email);
  return secret_password_store_sync (password_schema (), SECRET_COLLECTION_DEFAULT, label,
                                     a->password ? a->password : "", NULL, error,
                                     "account-id", a->id, NULL);
}

void
smail_accounts_clear_password (const char *account_id)
{
  secret_password_clear_sync (password_schema (), NULL, NULL, "account-id", account_id, NULL);
}
