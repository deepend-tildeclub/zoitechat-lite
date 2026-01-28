gtk_dep = dependency('gtk+-3.0', version: '>=3.24')
glib_dep = dependency('glib-2.0', version: '>=2.74')

gnome = import('gnome')

resources = gnome.compile_resources(
  'zoitechat-lite-resources',
  'app/resources/resources.gresource.xml',
  source_dir: 'app/resources',
  c_name: 'zoitechat_lite',
)

app_sources = files(
  'app/main.c',
  'app/ui.c',
  'app/ui.h',
  'app/chat_page.c',
  'app/chat_page.h',
  'app/settings.c',
  'app/settings.h',
)

executable(
  'zoitechat-lite',
  app_sources + resources,
  dependencies: [gtk_dep, glib_dep, libzoitechat_dep],
  install_rpath: join_paths(get_option('prefix'), get_option('libdir')),

  install: true,
)

install_data(
  files('../data/net.zoite.ZoiteChatLite.desktop'),
  install_dir: join_paths(get_option('datadir'), 'applications')
)

install_data(
  files('../data/icons/hicolor/scalable/apps/net.zoite.ZoiteChatLite.svg'),
  install_dir: join_paths(get_option('datadir'), 'icons', 'hicolor', 'scalable', 'apps')
)

install_subdir(
  '../data/icons/hicolor',
  install_dir: join_paths(get_option('datadir'), 'icons')
)
