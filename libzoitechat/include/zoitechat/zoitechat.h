#pragma once

#include <gio/gio.h>
#include "irc_message.h"

G_BEGIN_DECLS

#define ZC_TYPE_CLIENT (zc_client_get_type())
G_DECLARE_FINAL_TYPE(ZcClient, zc_client, ZC, CLIENT, GObject)

/**
 * ZcClient:
 * A small IRC client backend using GIO streams.
 *
 * Signals:
 * - "connected" (): emitted after TCP/TLS connect succeeded
 * - "disconnected" (gint code, gchar* message): emitted on disconnect or fatal IO error
 * - "raw-line" (gchar* line): emitted for each raw IRC line read
 * - "irc-message" (ZcIrcMessage* msg): emitted for each parsed IRC message
 */
ZcClient *zc_client_new(void);

void zc_client_set_identity(ZcClient *self, const gchar *nick, const gchar *user, const gchar *realname);

void zc_client_connect_async(
  ZcClient *self,
  const gchar *host,
  guint16 port,
  gboolean use_tls,
  GCancellable *cancellable,
  GAsyncReadyCallback callback,
  gpointer user_data
);

gboolean zc_client_connect_finish(ZcClient *self, GAsyncResult *res, GError **error);

gboolean zc_client_is_connected(ZcClient *self);

gboolean zc_client_send_raw(ZcClient *self, const gchar *line, GError **error);

gboolean zc_client_login(ZcClient *self, GError **error);
gboolean zc_client_join(ZcClient *self, const gchar *channel, GError **error);
gboolean zc_client_privmsg(ZcClient *self, const gchar *target, const gchar *text, GError **error);
gboolean zc_client_quit(ZcClient *self, const gchar *message, GError **error);

void zc_client_disconnect(ZcClient *self);

const gchar *zc_client_get_nick(ZcClient *self);

G_END_DECLS
