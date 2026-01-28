#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

typedef struct _ZcIrcMessage ZcIrcMessage;

/**
 * ZcIrcMessage:
 * @prefix: Optional message prefix (nick!user@host or server name), may be %NULL
 * @command: IRC command or numeric (e.g. "PRIVMSG", "001"), never %NULL
 * @params: Array of parameters (strings). For commands with a trailing parameter,
 *          @trailing contains that value and it is not duplicated in @params.
 * @trailing: Trailing parameter (after ':'), may be %NULL
 */
struct _ZcIrcMessage {
  gchar *prefix;
  gchar *command;
  GPtrArray *params; /* char* */
  gchar *trailing;
};

#define ZC_TYPE_IRC_MESSAGE (zc_irc_message_get_type())

GType zc_irc_message_get_type(void);

ZcIrcMessage *zc_irc_message_new(void);
ZcIrcMessage *zc_irc_message_parse_line(const gchar *line);
ZcIrcMessage *zc_irc_message_copy(const ZcIrcMessage *msg);
void zc_irc_message_free(ZcIrcMessage *msg);

const gchar *zc_irc_message_param(const ZcIrcMessage *msg, guint idx);

gchar *zc_irc_extract_nick(const gchar *prefix);

G_END_DECLS
