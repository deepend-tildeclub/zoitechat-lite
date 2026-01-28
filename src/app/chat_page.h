#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef struct _ChatPage ChatPage;

/* User list model columns (only present for channel pages). */
enum {
  ZC_USERLIST_COL_NICK = 0,
  ZC_USERLIST_COL_DISPLAY = 1,
  ZC_USERLIST_COL_SORTKEY = 2,
  ZC_USERLIST_N_COLS
};

ChatPage *chat_page_new(const gchar *target);
void chat_page_free(ChatPage *page);

GtkWidget *chat_page_get_root(ChatPage *page);
const gchar *chat_page_get_target(ChatPage *page);

void chat_page_append(ChatPage *page, const gchar *line);
void chat_page_append_fmt(ChatPage *page, const gchar *fmt, ...) G_GNUC_PRINTF(2, 3);

GtkEntry *chat_page_get_entry(ChatPage *page);
GtkTextBuffer *chat_page_get_buffer(ChatPage *page);

/* Channel-only: returns the user list view (GtkTreeView). Returns NULL for
 * status/query pages.
 */
GtkWidget *chat_page_get_userlist_view(ChatPage *page);

/* Channel-only helpers. No-ops for status/query pages. */
void chat_page_userlist_clear(ChatPage *page);
void chat_page_userlist_upsert(ChatPage *page, const gchar *nick, gchar prefix);
void chat_page_userlist_remove(ChatPage *page, const gchar *nick);
void chat_page_userlist_rename(ChatPage *page, const gchar *oldnick, const gchar *newnick);

G_END_DECLS
