#include "zoitechat/zoitechat.h"

#include <string.h>

struct _ZcClient {
  GObject parent_instance;

  gchar *nick;
  gchar *user;
  gchar *realname;

  GSocketClient *sock_client;
  GSocketConnection *connection;
  GDataInputStream *din;
  GOutputStream *out;
  GCancellable *cancellable;

  gboolean connected;
  GMutex write_lock;
};

G_DEFINE_TYPE(ZcClient, zc_client, G_TYPE_OBJECT)

enum {
  SIG_CONNECTED,
  SIG_DISCONNECTED,
  SIG_RAW_LINE,
  SIG_IRC_MESSAGE,
  N_SIGNALS
};

static guint signals[N_SIGNALS] = {0};

static void zc_client_start_read_loop(ZcClient *self);

static void
zc_client_dispose(GObject *object) {
  ZcClient *self = ZC_CLIENT(object);

  if (self->cancellable) {
    g_cancellable_cancel(self->cancellable);
    g_clear_object(&self->cancellable);
  }

  if (self->connection) {
    GIOStream *s = G_IO_STREAM(self->connection);
    g_io_stream_close(s, NULL, NULL);
  }

  g_clear_object(&self->din);
  g_clear_object(&self->out);
  g_clear_object(&self->connection);
  g_clear_object(&self->sock_client);

  G_OBJECT_CLASS(zc_client_parent_class)->dispose(object);
}

static void
zc_client_finalize(GObject *object) {
  ZcClient *self = ZC_CLIENT(object);

  g_free(self->nick);
  g_free(self->user);
  g_free(self->realname);
  g_mutex_clear(&self->write_lock);

  G_OBJECT_CLASS(zc_client_parent_class)->finalize(object);
}

static void
zc_client_class_init(ZcClientClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = zc_client_dispose;
  object_class->finalize = zc_client_finalize;

  signals[SIG_CONNECTED] = g_signal_new(
    "connected",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0,
    NULL, NULL,
    NULL,
    G_TYPE_NONE,
    0
  );

  signals[SIG_DISCONNECTED] = g_signal_new(
    "disconnected",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0,
    NULL, NULL,
    NULL,
    G_TYPE_NONE,
    2,
    G_TYPE_INT,
    G_TYPE_STRING
  );

  signals[SIG_RAW_LINE] = g_signal_new(
    "raw-line",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0,
    NULL, NULL,
    NULL,
    G_TYPE_NONE,
    1,
    G_TYPE_STRING
  );

  signals[SIG_IRC_MESSAGE] = g_signal_new(
    "irc-message",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0,
    NULL, NULL,
    NULL,
    G_TYPE_NONE,
    1,
    ZC_TYPE_IRC_MESSAGE
  );
}

static void
zc_client_init(ZcClient *self) {
  self->sock_client = g_socket_client_new();
  self->connected = FALSE;
  g_mutex_init(&self->write_lock);
}

ZcClient *
zc_client_new(void) {
  return g_object_new(ZC_TYPE_CLIENT, NULL);
}

void
zc_client_set_identity(ZcClient *self, const gchar *nick, const gchar *user, const gchar *realname) {
  g_return_if_fail(ZC_IS_CLIENT(self));
  g_free(self->nick);
  g_free(self->user);
  g_free(self->realname);

  self->nick = g_strdup(nick ? nick : "zoite");
  self->user = g_strdup(user ? user : "zoite");
  self->realname = g_strdup(realname ? realname : "ZoiteChat Lite");
}

const gchar *
zc_client_get_nick(ZcClient *self) {
  g_return_val_if_fail(ZC_IS_CLIENT(self), NULL);
  return self->nick;
}

gboolean
zc_client_is_connected(ZcClient *self) {
  g_return_val_if_fail(ZC_IS_CLIENT(self), FALSE);
  return self->connected;
}

static void
emit_disconnected(ZcClient *self, gint code, const gchar *message) {
  self->connected = FALSE;
  g_signal_emit(self, signals[SIG_DISCONNECTED], 0, code, message ? message : "");
}

static gboolean
write_line(ZcClient *self, const gchar *line, GError **error) {
  g_return_val_if_fail(line != NULL, FALSE);
  if (!self->out) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_CLOSED, "Not connected");
    return FALSE;
  }

  gchar *payload = g_strdup_printf("%s\r\n", line);

  g_mutex_lock(&self->write_lock);
  gboolean ok = g_output_stream_write_all(self->out, payload, strlen(payload), NULL, self->cancellable, error);
  if (ok) ok = g_output_stream_flush(self->out, self->cancellable, error);
  g_mutex_unlock(&self->write_lock);

  g_free(payload);
  return ok;
}

gboolean
zc_client_send_raw(ZcClient *self, const gchar *line, GError **error) {
  g_return_val_if_fail(ZC_IS_CLIENT(self), FALSE);
  g_return_val_if_fail(line != NULL, FALSE);
  return write_line(self, line, error);
}

gboolean
zc_client_login(ZcClient *self, GError **error) {
  g_return_val_if_fail(ZC_IS_CLIENT(self), FALSE);
  if (!self->nick || !self->user || !self->realname) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Identity not set");
    return FALSE;
  }

  gchar *nick_line = g_strdup_printf("NICK %s", self->nick);
  gboolean ok = write_line(self, nick_line, error);
  g_free(nick_line);
  if (!ok) return FALSE;

  gchar *user_line = g_strdup_printf("USER %s 0 * :%s", self->user, self->realname);
  ok = write_line(self, user_line, error);
  g_free(user_line);
  return ok;
}

gboolean
zc_client_join(ZcClient *self, const gchar *channel, GError **error) {
  g_return_val_if_fail(ZC_IS_CLIENT(self), FALSE);
  g_return_val_if_fail(channel != NULL, FALSE);
  gchar *line = g_strdup_printf("JOIN %s", channel);
  gboolean ok = write_line(self, line, error);
  g_free(line);
  return ok;
}

gboolean
zc_client_privmsg(ZcClient *self, const gchar *target, const gchar *text, GError **error) {
  g_return_val_if_fail(ZC_IS_CLIENT(self), FALSE);
  g_return_val_if_fail(target != NULL, FALSE);
  g_return_val_if_fail(text != NULL, FALSE);
  gchar *line = g_strdup_printf("PRIVMSG %s :%s", target, text);
  gboolean ok = write_line(self, line, error);
  g_free(line);
  return ok;
}

gboolean
zc_client_quit(ZcClient *self, const gchar *message, GError **error) {
  g_return_val_if_fail(ZC_IS_CLIENT(self), FALSE);
  const gchar *msg = message ? message : "Client exiting";
  gchar *line = g_strdup_printf("QUIT :%s", msg);
  gboolean ok = write_line(self, line, error);
  g_free(line);
  return ok;
}

void
zc_client_disconnect(ZcClient *self) {
  g_return_if_fail(ZC_IS_CLIENT(self));

  if (self->cancellable) g_cancellable_cancel(self->cancellable);

  if (self->connection) {
    GIOStream *s = G_IO_STREAM(self->connection);
    g_io_stream_close(s, NULL, NULL);
  }

  g_clear_object(&self->din);
  g_clear_object(&self->out);
  g_clear_object(&self->connection);

  if (self->connected) emit_disconnected(self, 0, "Disconnected");
}

static void
on_read_line(GObject *source, GAsyncResult *res, gpointer user_data) {
  ZcClient *self = ZC_CLIENT(user_data);
  GDataInputStream *din = G_DATA_INPUT_STREAM(source);
  GError *error = NULL;

  gsize length = 0;
  gchar *line = g_data_input_stream_read_line_finish(din, res, &length, &error);

  if (error) {
    /* Normal during disconnect/shutdown. Don’t spam signals or explode. */
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED) ||
        g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CLOSED)) {
      g_clear_error(&error);
      g_object_unref(self);
      return;
    }

    emit_disconnected(self, error->code, error->message);
    g_clear_error(&error);
    g_object_unref(self);
    return;
  }

  if (!line) {
    emit_disconnected(self, 0, "EOF");
    g_object_unref(self);
    return;
  }

  /* Strip CR if present */
  if (length > 0 && line[length - 1] == '\r') {
    line[length - 1] = '\0';
  }

  g_signal_emit(self, signals[SIG_RAW_LINE], 0, line);

  ZcIrcMessage *msg = zc_irc_message_parse_line(line);
  if (msg) {
    g_signal_emit(self, signals[SIG_IRC_MESSAGE], 0, msg);

    /* Auto PING/PONG */
    if (g_strcmp0(msg->command, "PING") == 0) {
      const gchar *pong = msg->trailing ? msg->trailing : zc_irc_message_param(msg, 0);
      if (pong) {
        gchar *pong_line = g_strdup_printf("PONG :%s", pong);
        (void)write_line(self, pong_line, NULL);
        g_free(pong_line);
      }
    }

    zc_irc_message_free(msg);
  }

  g_free(line);

  /* Only continue if we’re still connected and this read belongs to the current stream. */
  if (self->connected && self->din == din &&
      self->cancellable && !g_cancellable_is_cancelled(self->cancellable)) {
    zc_client_start_read_loop(self);
  }

  g_object_unref(self);
}

static void
zc_client_start_read_loop(ZcClient *self) {
  if (!self->din) return;
  /* Hold a ref to self for the lifetime of the async read. */
  g_data_input_stream_read_line_async(
    self->din,
    G_PRIORITY_DEFAULT,
    self->cancellable,
    on_read_line,
    g_object_ref(self)
  );
}
static void
on_connected(GObject *source, GAsyncResult *res, gpointer user_data) {
  (void)source;
  GTask *task = G_TASK(user_data);
  ZcClient *self = ZC_CLIENT(g_task_get_source_object(task));
  GError *error = NULL;

  GSocketConnection *conn = g_socket_client_connect_to_host_finish(self->sock_client, res, &error);
  if (!conn) {
    g_task_return_error(task, error);
    g_object_unref(task);
    return;
  }

  self->connection = conn;
  /* Disable 30s socket I/O timeout (it disconnects idle IRC sessions).
   * Keep the connect timeout on the GSocketClient, but once connected,
   * make the socket non-timeout and enable keepalive.
   */
  GSocket *sock = g_socket_connection_get_socket(conn);
  if (sock) {
    g_socket_set_timeout(sock, 0);
    g_socket_set_keepalive(sock, TRUE);
  }
  GIOStream *s = G_IO_STREAM(conn);
  self->out = g_io_stream_get_output_stream(s);
  g_object_ref(self->out);

  GInputStream *in = g_io_stream_get_input_stream(s);
  self->din = g_data_input_stream_new(in);
  g_data_input_stream_set_newline_type(self->din, G_DATA_STREAM_NEWLINE_TYPE_CR_LF);

  self->connected = TRUE;
  g_signal_emit(self, signals[SIG_CONNECTED], 0);
  zc_client_start_read_loop(self);

  g_task_return_boolean(task, TRUE);
  g_object_unref(task);
}

void
zc_client_connect_async(
  ZcClient *self,
  const gchar *host,
  guint16 port,
  gboolean use_tls,
  GCancellable *cancellable,
  GAsyncReadyCallback callback,
  gpointer user_data
) {
  g_return_if_fail(ZC_IS_CLIENT(self));
  g_return_if_fail(host != NULL);

  zc_client_disconnect(self);

  if (self->cancellable) {
    g_cancellable_cancel(self->cancellable);
    g_clear_object(&self->cancellable);
  }
  self->cancellable = cancellable ? g_object_ref(cancellable) : g_cancellable_new();

  g_socket_client_set_tls(self->sock_client, use_tls);
  g_socket_client_set_timeout(self->sock_client, 30);

  GTask *task = g_task_new(self, self->cancellable, callback, user_data);
  g_socket_client_connect_to_host_async(self->sock_client, host, port, self->cancellable, on_connected, task);
}

gboolean
zc_client_connect_finish(ZcClient *self, GAsyncResult *res, GError **error) {
  g_return_val_if_fail(ZC_IS_CLIENT(self), FALSE);
  return g_task_propagate_boolean(G_TASK(res), error);
}
