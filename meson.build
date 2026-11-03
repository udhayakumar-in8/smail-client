project('courier', 'c',
  version: '0.1.0',
  license: 'GPL-3.0-or-later',
  meson_version: '>= 0.62',
  default_options: ['c_std=gnu11', 'warning_level=2', 'buildtype=debugoptimized'],
)

cc = meson.get_compiler('c')
add_project_arguments(
  '-DPACKAGE_VERSION="@0@"'.format(meson.project_version()),
  '-DG_LOG_DOMAIN="Courier"',
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

core = static_library('courier-core',
  'src/courier-types.c',
  'src/courier-imap.c',
  'src/courier-backend.c',
  'src/courier-demo.c',
  'src/courier-smtp.c',
  'src/courier-util.c',
  'src/courier-store.c',
  'src/courier-automation.c',
  'src/courier-lan.c',
  dependencies: deps,
)

executable('courier',
  'src/main.c',
  'src/courier-accounts.c',
  'src/courier-message-view.c',
  'src/courier-composer.c',
  'src/courier-account-dialog.c',
  'src/courier-window.c',
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

install_data('data/dev.courier.Mail.desktop',
  install_dir: get_option('datadir') / 'applications')
