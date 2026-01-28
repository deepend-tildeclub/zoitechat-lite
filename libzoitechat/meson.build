glib_dep = dependency('glib-2.0', version: '>=2.74')
gio_dep = dependency('gio-2.0', version: '>=2.74')
gobject_dep = dependency('gobject-2.0', version: '>=2.74')

inc = include_directories('include')

libzoitechat_sources = files(
  'src/zoitechat.c',
  'src/irc_message.c',
)

libzoitechat = library(
  'zoitechat',
  libzoitechat_sources,
  include_directories: inc,
  dependencies: [glib_dep, gio_dep, gobject_dep],
  install: true,
  version: '0.1.0',
  soversion: '0',
)

libzoitechat_dep = declare_dependency(
  link_with: libzoitechat,
  include_directories: inc,
  dependencies: [glib_dep, gio_dep, gobject_dep],
)

install_headers(
  'include/zoitechat/zoitechat.h',
  'include/zoitechat/irc_message.h',
  subdir: 'zoitechat'
)

pkg = import('pkgconfig')
pkg.generate(
  libzoitechat,
  name: 'libzoitechat',
  description: 'LibZoiteChat IRC backend (GIO/GObject)',
  subdirs: 'zoitechat',
  requires: ['glib-2.0', 'gio-2.0', 'gobject-2.0'],
)
