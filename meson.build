project('smail-client', 'c',
  version: '0.1.1',
  license: 'GPL-3.0-or-later',
  meson_version: '>= 0.62',
  default_options: ['c_std=gnu11', 'warning_level=2', 'buildtype=debugoptimized'],
)

cc = meson.get_compiler('c')
add_project_arguments(
  '-DPACKAGE_VERSION="@0@"'.format(meson.project_version()),
  '-DG_LOG_DOMAIN="SMail"',
  cc.get_supported_arguments(['-Wno-unused-parameter', '-Wno-missing-field-initializers']),
  language: 'c',
)

deps = [
  dependency('gtk4', version: '>= 4.12'),
  dependency('libadwaita-1', version: '>= 1.5'),
  dependency('webkitgtk-6.0'),
  dependency('gmime-3.0'),
  dependency('libsecret-1'),
  dependency('libcurl'),
  dependency('sqlite3'),
  dependency('json-glib-1.0', version: '>= 1.6'),
]

core = static_library('smail-core',
  'src/smail-types.c',
  'src/smail-imap.c',
  'src/smail-backend.c',
  'src/smail-demo.c',
  'src/smail-smtp.c',
  'src/smail-util.c',
  'src/smail-store.c',
  'src/smail-automation.c',
  'src/smail-lan.c',
  'src/smail-backup.c',
  dependencies: deps,
)

executable('smail',
  'src/main.c',
  'src/smail-accounts.c',
  'src/smail-message-view.c',
  'src/smail-composer.c',
  'src/smail-account-dialog.c',
  'src/smail-window.c',
  'src/smail-ui.c',
  'src/smail-organizer.c',
  'src/smail-mail-tools.c',
  'src/smail-chat-page.c',
  'src/smail-theme.c',
  'src/smail-backup-dialog.c',
  link_with: core,
  dependencies: deps,
  install: true,
)

test_imap = executable('test-imap', 'tests/test-imap.c',
  include_directories: include_directories('src'),
  link_with: core,
  dependencies: deps,
)
test('imap', test_imap, timeout: 60)

test_local = executable('test-local', 'tests/test-local.c',
  include_directories: include_directories('src'),
  link_with: core,
  dependencies: deps,
)
test('local', test_local, timeout: 60)

install_data('data/io.github.udhayakumar_in8.SMail.desktop',
  install_dir: get_option('datadir') / 'applications')

test_backup = executable('test-backup', 'tests/test-backup.c',
  include_directories: include_directories('src'),
  link_with: core,
  dependencies: deps,
)
test('backup', test_backup, timeout: 60)
