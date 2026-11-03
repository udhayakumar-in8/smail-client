#!/usr/bin/env bash
# Start a throwaway Dovecot IMAP server (port 11430, user test@local / secret)
# and an SMTP sink (port 2525) for tests/test-imap.c, then print the
# environment variables the test reads. Needs dovecot-imapd and root or sudo.
#   eval "$(tests/live-servers.sh)" && ./build/test-imap
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
DIR=/var/tmp/smail-live
SUDO=""
[ "$(id -u)" -eq 0 ] || SUDO=sudo

$SUDO useradd -M -s /usr/sbin/nologin smailtest 2>/dev/null || true
$SUDO rm -rf "$DIR"
$SUDO mkdir -p "$DIR/run" "$DIR/mail"
$SUDO chown smailtest:smailtest "$DIR/mail"
$SUDO chmod 755 "$DIR"

$SUDO tee "$DIR/dovecot.conf" >/dev/null <<CONF
protocols = imap
listen = 127.0.0.1
base_dir = $DIR/run
ssl = no
disable_plaintext_auth = no
auth_mechanisms = plain login
mail_location = maildir:$DIR/mail/%u
log_path = $DIR/dovecot.log
service imap-login {
  inet_listener imap {
    port = 11430
  }
  inet_listener imaps {
    port = 0
  }
}
passdb {
  driver = static
  args = password=secret
}
userdb {
  driver = static
  args = uid=smailtest gid=smailtest home=$DIR/mail/%u
}
namespace inbox {
  inbox = yes
  separator = /
  mailbox Sent {
    special_use = \\Sent
    auto = create
  }
  mailbox Trash {
    special_use = \\Trash
    auto = create
  }
  mailbox Archive {
    special_use = \\Archive
    auto = create
  }
  mailbox "Café Ünïcode" {
    auto = create
  }
}
CONF

$SUDO dovecot -c "$DIR/dovecot.conf" >&2
nohup python3 "$HERE/smtp-sink.py" 2525 >"$DIR/smtp.log" 2>&1 &

# wait until both ports answer
for _ in $(seq 1 50); do
  if (exec 3<>/dev/tcp/127.0.0.1/11430) 2>/dev/null && (exec 3<>/dev/tcp/127.0.0.1/2525) 2>/dev/null; then
    break
  fi
  sleep 0.2
done

echo "export SMAIL_TEST_IMAP=127.0.0.1:11430 SMAIL_TEST_USER=test@local SMAIL_TEST_PASS=secret SMAIL_TEST_SMTP=127.0.0.1:2525"
