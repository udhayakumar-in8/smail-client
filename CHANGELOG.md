# Changelog

## 0.2.0

Thunderbird-style mail tools.

### Address book
- Address Book page with your contacts and automatically collected addresses
- Autocomplete in To, Cc and Bcc, ranked by favorites and how often you write to someone
- Import contacts from vCard (2.1/3.0/4.0) and CSV (Thunderbird, Google, Outlook); export to vCard

### Writing
- Save drafts to the server (Ctrl+S, or when closing an unfinished message) and continue them later
  with Edit Draft (Ctrl+E); saving again replaces the earlier copy, sending removes it
- A signature for each account

### Reading and organising
- Search the whole folder on the server, including message text (press Enter in the search box)
- Load older messages beyond the newest 100
- Right-click menu on messages: Move To any folder, Mark as Junk / Not Junk, Save As File (.eml),
  View Source, Print, and the usual reply/star/read/archive/delete actions
- Folder actions: Mark All as Read, New Folder (also inside another folder), Rename, Delete
- Import messages from .eml and mbox files (including Thunderbird folders) and export a folder as mbox
- Open attachments with their usual app, or save them
- Sub-folders are shown indented under their parent

### Settings
- Preferences: check for new mail every 1–60 minutes, new-mail notifications, and loading images
  automatically from people you know
- Backups now include contacts and signatures (older backups still restore)


## 0.1.1

### Arch Linux support
- Arch Linux package (`.pkg.tar.zst`) and `PKGBUILD` with every release
- Builds cleanly with GTK 4.24 and libadwaita 1.10: the calendar uses `gtk_calendar_set_date`
  on GTK 4.20+, and the keyboard shortcuts open in the new `AdwShortcutsDialog` on
  libadwaita 1.8+
- The Matte White and Matte Black styles also set the CSS variables that libadwaita 1.6+
  uses, so they look the same on Arch as on Ubuntu

### Testing
- CI builds, tests and packages SMail on both Ubuntu 24.04 and Arch Linux
- A GUI smoke test starts the app in every style on a virtual display, fails on any GTK or
  libadwaita warning, and checks from a screenshot that each style is applied
- The `.deb` file name no longer contains `~`, so it matches `SHA256SUMS`

## 0.1.0

The first release of SMail.

### Mail
- Three-pane layout (folders, message list, reading pane) in the style of elementary OS Mail,
  adapting to narrow windows and phones
- IMAP (SSL/TLS, STARTTLS) and SMTP, with presets for Gmail, Outlook, Yahoo, iCloud, Fastmail,
  Zoho and Proton Bridge; passwords kept in the system keyring
- HTML mail shown with JavaScript off and remote images blocked until you allow them
- Compose, reply, reply all and forward, with attachments, Cc/Bcc and threading headers
- Star, mark read/unread, archive and trash; search; keyboard shortcuts
- Every message you open is kept as a local `.eml` copy and opens offline afterwards

### Organizer
- Calendar with reminders, Tasks with due dates, and standalone Reminders with snooze
- Mail filters that run automatically on new Inbox mail (move, mark read, star, trash)
- Mail Log: details of every received and sent message are saved automatically,
  sent mail with a full `.eml` copy; searchable and exportable to CSV
- Local Chat with other SMail users on the same network, no internet needed

### Everything else
- Backup & Restore of accounts, filters, calendar, tasks, reminders, mail log, chat history,
  settings and (optionally) downloaded emails and passwords, in one `.smailbackup` file
- Matte White and Matte Black styles in the elementary OS look, plus the System style
- Demo mode (`smail --demo`) to try it without an account
