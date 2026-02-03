#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

GtkWidget *zc_ui_create_main_window(GtkApplication *app);

void zc_ui_register_menu_action(const gchar *label, GCallback callback);
void zc_ui_register_menu_separator(void);

G_END_DECLS
