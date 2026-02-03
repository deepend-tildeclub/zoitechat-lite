#include <gtk/gtk.h>

#include "ui.h"

#define APP_ID "net.zoite.ZoiteChatLite"

static void
register_app_icons(void) {
  GtkIconTheme *theme = gtk_icon_theme_get_default();
  if (theme && g_file_test("data/icons", G_FILE_TEST_IS_DIR)) {
    gtk_icon_theme_append_search_path(theme, "data/icons");
  }

  gtk_window_set_default_icon_name(APP_ID);

  const gint sizes[] = {16, 22, 24, 32, 48, 64, 128, 256, 512};
  GList *icons = NULL;

  for (gsize i = 0; i < G_N_ELEMENTS(sizes); i++) {
    gchar *path = g_strdup_printf(
      "data/icons/hicolor/%dx%d/apps/net.zoite.ZoiteChatLite.png",
      sizes[i],
      sizes[i]
    );
    if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
      g_free(path);
      continue;
    }

    GError *error = NULL;
    GdkPixbuf *pix = gdk_pixbuf_new_from_file(path, &error);
    g_free(path);
    if (error) {
      g_clear_error(&error);
      continue;
    }
    if (pix) {
      icons = g_list_append(icons, pix);
    }
  }

  if (icons) {
    gtk_window_set_default_icon_list(icons);
    g_list_free_full(icons, g_object_unref);
  } else if (g_file_test("data/icons/hicolor/scalable/apps/net.zoite.ZoiteChatLite.svg", G_FILE_TEST_EXISTS)) {
    GError *error = NULL;
    gtk_window_set_default_icon_from_file(
      "data/icons/hicolor/scalable/apps/net.zoite.ZoiteChatLite.svg",
      &error
    );
    if (error) {
      g_clear_error(&error);
    }
  }
}


static void
on_startup(GtkApplication *app, gpointer user_data) {
  (void)app;
  (void)user_data;

  /* Plasma matches windows to .desktop via WM_CLASS; make it deterministic. */
  gdk_set_program_class(APP_ID);
  g_set_prgname(APP_ID);

  /* Default app icon for GTK windows/dialogs and Wayland shells. */
  register_app_icons();
}
static void
on_activate(GtkApplication *app, gpointer user_data) {
  (void)user_data;
  zc_ui_create_main_window(app);
}

int
main(int argc, char **argv) {
  GtkApplication *app = gtk_application_new(APP_ID, G_APPLICATION_DEFAULT_FLAGS);

  g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);

  
  g_signal_connect(app, "startup", G_CALLBACK(on_startup), NULL);

  int status = g_application_run(G_APPLICATION(app), argc, argv);
  g_object_unref(app);
  return status;
}
