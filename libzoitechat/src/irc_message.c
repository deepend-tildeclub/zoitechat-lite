#include "zoitechat/irc_message.h"

#include <string.h>

G_DEFINE_BOXED_TYPE(ZcIrcMessage, zc_irc_message, zc_irc_message_copy, zc_irc_message_free)

static void
zc_irc_message_init_arrays(ZcIrcMessage *msg) {
  msg->params = g_ptr_array_new_with_free_func(g_free);
}

ZcIrcMessage *
zc_irc_message_new(void) {
  ZcIrcMessage *msg = g_new0(ZcIrcMessage, 1);
  zc_irc_message_init_arrays(msg);
  return msg;
}

void
zc_irc_message_free(ZcIrcMessage *msg) {
  if (!msg) return;
  g_free(msg->prefix);
  g_free(msg->command);
  if (msg->params) g_ptr_array_free(msg->params, TRUE);
  g_free(msg->trailing);
  g_free(msg);
}

ZcIrcMessage *
zc_irc_message_copy(const ZcIrcMessage *src) {
  if (!src) return NULL;
  ZcIrcMessage *dst = zc_irc_message_new();
  dst->prefix = g_strdup(src->prefix);
  dst->command = g_strdup(src->command);
  for (guint i = 0; i < src->params->len; i++) {
    g_ptr_array_add(dst->params, g_strdup((const gchar *)g_ptr_array_index(src->params, i)));
  }
  dst->trailing = g_strdup(src->trailing);
  return dst;
}

const gchar *
zc_irc_message_param(const ZcIrcMessage *msg, guint idx) {
  if (!msg || !msg->params) return NULL;
  if (idx >= msg->params->len) return NULL;
  return (const gchar *)g_ptr_array_index(msg->params, idx);
}

static gboolean
is_space(gchar c) {
  return c == ' ' || c == '\t';
}

/* RFC1459-ish line parsing, good enough for a starter */
ZcIrcMessage *
zc_irc_message_parse_line(const gchar *line) {
  if (!line) return NULL;

  const gchar *p = line;
  ZcIrcMessage *msg = zc_irc_message_new();

  /* Prefix */
  if (*p == ':') {
    p++;
    const gchar *start = p;
    while (*p && !is_space(*p)) p++;
    msg->prefix = g_strndup(start, (gsize)(p - start));
    while (*p && is_space(*p)) p++;
  }

  /* Command */
  if (!*p) {
    zc_irc_message_free(msg);
    return NULL;
  }
  {
    const gchar *start = p;
    while (*p && !is_space(*p)) p++;
    msg->command = g_ascii_strup(start, (gsize)(p - start));
    while (*p && is_space(*p)) p++;
  }

  /* Params + trailing */
  while (*p) {
    if (*p == ':') {
      p++;
      msg->trailing = g_strdup(p);
      break;
    }
    const gchar *start = p;
    while (*p && !is_space(*p)) p++;
    if (p > start) {
      g_ptr_array_add(msg->params, g_strndup(start, (gsize)(p - start)));
    }
    while (*p && is_space(*p)) p++;
  }

  return msg;
}

/* Extract "nick" from "nick!user@host" */
gchar *
zc_irc_extract_nick(const gchar *prefix) {
  if (!prefix) return NULL;
  const gchar *bang = strchr(prefix, '!');
  if (!bang) return g_strdup(prefix);
  return g_strndup(prefix, (gsize)(bang - prefix));
}
