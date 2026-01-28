#pragma once
#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  gchar *host;
  guint16 port;
  gboolean tls;

  gchar *nick;
  gchar *user;
  gchar *realname;
  gchar *auto_join;

  gint win_w;
  gint win_h;
} ZcSettings;

ZcSettings *zc_settings_load(void);
gboolean zc_settings_save(const ZcSettings *s, GError **error);
void zc_settings_free(ZcSettings *s);

G_END_DECLS
