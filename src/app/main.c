#include <gtk/gtk.h>

#include "ui.h"

#define APP_ID "net.zoite.ZoiteChatLite"


static void
on_startup(GtkApplication *app, gpointer user_data) {
  (void)app;
  (void)user_data;

  /* Plasma matches windows to .desktop via WM_CLASS; make it deterministic. */
  gdk_set_program_class(APP_ID);

  /* Default app icon for GTK windows/dialogs. */
  gtk_window_set_default_icon_name(APP_ID);
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
