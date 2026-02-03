#pragma once

#include <glib.h>
#include <gtk/gtk.h>

G_BEGIN_DECLS

gchar *zcl_ansi_to_markup(const gchar *text);
gchar *zcl_strip_ansi(const gchar *text);
void zcl_buffer_insert_ansi(GtkTextBuffer *buf, GtkTextIter *iter, const gchar *text);

G_END_DECLS
