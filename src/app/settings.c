#include "settings.h"
#include <glib/gstdio.h>

static gchar *settings_path(void) {
  const gchar *base = g_get_user_config_dir();
  gchar *dir = g_build_filename(base, "zoitechat-lite", NULL);
  g_mkdir_with_parents(dir, 0700);
  gchar *path = g_build_filename(dir, "settings.ini", NULL);
  g_free(dir);
  return path;
}

static void set_defaults(ZcSettings *s) {
  s->host = g_strdup("irc.zoite.net");
  s->port = 6697;
  s->tls = TRUE;
  s->nick = g_strdup("zoiteguest");
  s->user = g_strdup("zoite");
  s->realname = g_strdup("ZoiteChat Lite");
  s->auto_join = g_strdup("#zoite");
  s->win_w = 980;
  s->win_h = 640;
}

ZcSettings *zc_settings_load(void) {
  ZcSettings *s = g_new0(ZcSettings, 1);
  set_defaults(s);

  gchar *path = settings_path();
  GKeyFile *kf = g_key_file_new();
  GError *error = NULL;

  if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, &error)) {
    g_clear_error(&error);
    g_key_file_free(kf);
    g_free(path);
    return s;
  }

  #define GETSTR(sec,key,field) \
    if (g_key_file_has_key(kf, sec, key, NULL)) { g_free(s->field); s->field = g_key_file_get_string(kf, sec, key, NULL); }

  GETSTR("connection","host",host)
  GETSTR("connection","nick",nick)
  GETSTR("connection","user",user)
  GETSTR("connection","realname",realname)
  GETSTR("connection","auto_join",auto_join)

  if (g_key_file_has_key(kf, "connection", "port", NULL)) {
    const gint port_i = g_key_file_get_integer(kf, "connection", "port", NULL);
    if (port_i > 0 && port_i <= 65535) s->port = (guint16)port_i;
  }
  if (g_key_file_has_key(kf, "connection", "tls", NULL))
    s->tls = g_key_file_get_boolean(kf, "connection", "tls", NULL);

  if (g_key_file_has_key(kf, "window", "width", NULL)) {
    const gint w = g_key_file_get_integer(kf, "window", "width", NULL);
    if (w > 0) s->win_w = w;
  }
  if (g_key_file_has_key(kf, "window", "height", NULL)) {
    const gint h = g_key_file_get_integer(kf, "window", "height", NULL);
    if (h > 0) s->win_h = h;
  }

  g_key_file_free(kf);
  g_free(path);
  return s;
}

gboolean zc_settings_save(const ZcSettings *s, GError **error) {
  if (!s) return FALSE;

  gchar *path = settings_path();
  GKeyFile *kf = g_key_file_new();

  g_key_file_set_string(kf, "connection", "host", s->host ? s->host : "");
  g_key_file_set_integer(kf, "connection", "port", (gint)s->port);
  g_key_file_set_boolean(kf, "connection", "tls", s->tls);

  g_key_file_set_string(kf, "connection", "nick", s->nick ? s->nick : "");
  g_key_file_set_string(kf, "connection", "user", s->user ? s->user : "");
  g_key_file_set_string(kf, "connection", "realname", s->realname ? s->realname : "");
  g_key_file_set_string(kf, "connection", "auto_join", s->auto_join ? s->auto_join : "");

  if (s->win_w > 0) g_key_file_set_integer(kf, "window", "width", s->win_w);
  if (s->win_h > 0) g_key_file_set_integer(kf, "window", "height", s->win_h);

  gsize len = 0;
  gchar *data = g_key_file_to_data(kf, &len, NULL);
  gboolean ok = g_file_set_contents(path, data, (gssize)len, error);

  g_free(data);
  g_key_file_free(kf);
  g_free(path);
  return ok;
}

void zc_settings_free(ZcSettings *s) {
  if (!s) return;
  g_free(s->host);
  g_free(s->nick);
  g_free(s->user);
  g_free(s->realname);
  g_free(s->auto_join);
  g_free(s);
}
