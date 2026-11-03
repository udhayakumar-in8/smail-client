/* courier-smtp.c — send mail through SMTP using libcurl */
#include "courier-backend.h"

#include <curl/curl.h>
#include <string.h>

typedef struct {
  const char *data;
  gsize       len;
  gsize       pos;
} Upload;

static size_t
read_cb (char *buffer, size_t size, size_t nitems, void *userp)
{
  Upload *up = userp;
  size_t room = size * nitems;
  size_t left = up->len - up->pos;
  size_t n = MIN (room, left);
  memcpy (buffer, up->data + up->pos, n);
  up->pos += n;
  return n;
}

gboolean
courier_smtp_send (const CourierAccount *account, GBytes *message, const char *from,
                   const char * const *recipients, GError **error)
{
  static gsize curl_initialized = 0;
  if (g_once_init_enter (&curl_initialized)) {
    curl_global_init (CURL_GLOBAL_DEFAULT);
    g_once_init_leave (&curl_initialized, 1);
  }

  if (!recipients || !recipients[0]) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "No recipients");
    return FALSE;
  }

  CURL *curl = curl_easy_init ();
  if (!curl) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not initialise libcurl");
    return FALSE;
  }

  const char *scheme = account->smtp_security == COURIER_SECURITY_SSL ? "smtps" : "smtp";
  g_autofree char *url = g_strdup_printf ("%s://%s:%u", scheme, account->smtp_host, account->smtp_port);
  g_autofree char *mail_from = g_strdup_printf ("<%s>", from);
  char errbuf[CURL_ERROR_SIZE] = { 0 };

  struct curl_slist *rcpt = NULL;
  for (int i = 0; recipients[i]; i++) {
    g_autofree char *r = g_strdup_printf ("<%s>", recipients[i]);
    rcpt = curl_slist_append (rcpt, r);
  }

  Upload up = { 0 };
  up.data = g_bytes_get_data (message, &up.len);

  curl_easy_setopt (curl, CURLOPT_URL, url);
  if (account->smtp_security == COURIER_SECURITY_STARTTLS)
    curl_easy_setopt (curl, CURLOPT_USE_SSL, (long) CURLUSESSL_ALL);
  curl_easy_setopt (curl, CURLOPT_USERNAME, account->username ? account->username : account->email);
  curl_easy_setopt (curl, CURLOPT_PASSWORD, account->password ? account->password : "");
  curl_easy_setopt (curl, CURLOPT_MAIL_FROM, mail_from);
  curl_easy_setopt (curl, CURLOPT_MAIL_RCPT, rcpt);
  curl_easy_setopt (curl, CURLOPT_READFUNCTION, read_cb);
  curl_easy_setopt (curl, CURLOPT_READDATA, &up);
  curl_easy_setopt (curl, CURLOPT_UPLOAD, 1L);
  curl_easy_setopt (curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t) up.len);
  curl_easy_setopt (curl, CURLOPT_ERRORBUFFER, errbuf);
  curl_easy_setopt (curl, CURLOPT_CONNECTTIMEOUT, 30L);
  curl_easy_setopt (curl, CURLOPT_NOSIGNAL, 1L);

  CURLcode res = curl_easy_perform (curl);

  curl_slist_free_all (rcpt);
  curl_easy_cleanup (curl);

  if (res != CURLE_OK) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Sending failed: %s",
                 *errbuf ? errbuf : curl_easy_strerror (res));
    return FALSE;
  }
  return TRUE;
}
