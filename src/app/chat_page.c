#include "chat_page.h"

#include <stdarg.h>
#include <time.h>

struct _ChatPage {
  gchar *target;
  GtkWidget *root;
  GtkWidget *top_row;
  GtkWidget *scroller;
  GtkWidget *textview;
  GtkTextBuffer *buffer;
  GtkWidget *entry;

  /* Channel-only user list (NULL for status/query pages) */
  GtkWidget *user_scroller;
  GtkWidget *user_view;
  GtkListStore *user_store;
};

static gboolean
is_channel_target(const gchar *s) {
  if (!s || !*s) return FALSE;
  return s[0] == '#' || s[0] == '&' || s[0] == '+' || s[0] == '!';
}

static gint
prefix_rank(gchar prefix) {
  /* Common prefix ordering: owner/admin/op/halfop/voice/none */
  switch (prefix) {
    case '~': return 0;
    case '&': return 1;
    case '@': return 2;
    case '%': return 3;
    case '+': return 4;
    default:  return 5;
  }
}

static gboolean
user_store_find(ChatPage *p, const gchar *nick, GtkTreeIter *out_iter) {
  if (!p || !p->user_store || !nick || !*nick) return FALSE;

  GtkTreeIter iter;
  gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(p->user_store), &iter);
  while (valid) {
    gchar *cur = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(p->user_store), &iter, ZC_USERLIST_COL_NICK, &cur, -1);
    gboolean match = (cur && g_ascii_strcasecmp(cur, nick) == 0);
    g_free(cur);
    if (match) {
      if (out_iter) *out_iter = iter;
      return TRUE;
    }
    valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(p->user_store), &iter);
  }
  return FALSE;
}

static gchar *
timestamp_now(void) {
  time_t t = time(NULL);
  struct tm lt;
#if defined(_WIN32)
  localtime_s(&lt, &t);
#else
  localtime_r(&t, &lt);
#endif
  gchar buf[16];
  strftime(buf, sizeof(buf), "%H:%M", &lt);
  return g_strdup(buf);
}

ChatPage *
chat_page_new(const gchar *target) {
  ChatPage *p = g_new0(ChatPage, 1);
  p->target = g_strdup(target ? target : "status");
  const gboolean is_chan = is_channel_target(p->target);

  p->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_hexpand(p->root, TRUE);
  gtk_widget_set_vexpand(p->root, TRUE);
  gtk_style_context_add_class(gtk_widget_get_style_context(p->root), "zc-chatview");

  p->scroller = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(p->scroller), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_hexpand(p->scroller, TRUE);
  gtk_widget_set_vexpand(p->scroller, TRUE);

  p->textview = gtk_text_view_new();
  gtk_text_view_set_editable(GTK_TEXT_VIEW(p->textview), FALSE);
  gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(p->textview), FALSE);
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(p->textview), GTK_WRAP_WORD_CHAR);
  gtk_widget_set_vexpand(p->textview, TRUE);
  p->buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(p->textview));

  gtk_container_add(GTK_CONTAINER(p->scroller), p->textview);

  /* Top row holds chat view (and channel user list, if applicable) */
  p->top_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_hexpand(p->top_row, TRUE);
  gtk_widget_set_vexpand(p->top_row, TRUE);
  gtk_box_pack_start(GTK_BOX(p->top_row), p->scroller, TRUE, TRUE, 0);

  if (is_chan) {
    p->user_store = gtk_list_store_new(ZC_USERLIST_N_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    GtkTreeSortable *sortable = GTK_TREE_SORTABLE(p->user_store);
    gtk_tree_sortable_set_sort_column_id(sortable, ZC_USERLIST_COL_SORTKEY, GTK_SORT_ASCENDING);

    p->user_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(p->user_store));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(p->user_view), FALSE);
    gtk_tree_view_set_enable_search(GTK_TREE_VIEW(p->user_view), TRUE);
    gtk_widget_set_vexpand(p->user_view, TRUE);
    gtk_widget_set_hexpand(p->user_view, FALSE);
    gtk_widget_set_name(p->user_view, "zc-userlist");

    GtkCellRenderer *r = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes("Users", r, "text", ZC_USERLIST_COL_DISPLAY, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(p->user_view), c);

    p->user_scroller = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(p->user_scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(p->user_scroller, 190, -1);
    gtk_widget_set_hexpand(p->user_scroller, FALSE);
    gtk_widget_set_vexpand(p->user_scroller, TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(p->user_scroller), "zc-userlist");
    gtk_container_add(GTK_CONTAINER(p->user_scroller), p->user_view);
    gtk_box_pack_start(GTK_BOX(p->top_row), p->user_scroller, FALSE, TRUE, 0);
  }

  GtkWidget *entry_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_style_context_add_class(gtk_widget_get_style_context(entry_box), "zc-entry");

  p->entry = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(p->entry), "Type a message… (/join, /nick, /me, /msg, /query, /raw, /quit)");
  gtk_widget_set_hexpand(p->entry, TRUE);

  gtk_box_pack_start(GTK_BOX(entry_box), p->entry, TRUE, TRUE, 0);

  gtk_box_pack_start(GTK_BOX(p->root), p->top_row, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(p->root), entry_box, FALSE, FALSE, 0);

  return p;
}

void
chat_page_free(ChatPage *p) {
  if (!p) return;
  g_free(p->target);
  g_free(p);
}

GtkWidget *
chat_page_get_root(ChatPage *p) {
  return p ? p->root : NULL;
}

const gchar *
chat_page_get_target(ChatPage *p) {
  return p ? p->target : NULL;
}

GtkEntry *
chat_page_get_entry(ChatPage *p) {
  return p ? GTK_ENTRY(p->entry) : NULL;
}

GtkTextBuffer *
chat_page_get_buffer(ChatPage *p) {
  return p ? p->buffer : NULL;
}

GtkWidget *
chat_page_get_userlist_view(ChatPage *p) {
  return (p && p->user_view) ? p->user_view : NULL;
}

void
chat_page_userlist_clear(ChatPage *p) {
  if (!p || !p->user_store) return;
  gtk_list_store_clear(p->user_store);
}

void
chat_page_userlist_upsert(ChatPage *p, const gchar *nick, gchar prefix) {
  if (!p || !p->user_store || !nick || !*nick) return;

  const gchar px = prefix ? prefix : '\0';
  gchar *disp = px ? g_strdup_printf("%c%s", px, nick) : g_strdup(nick);
  gchar *fold = g_ascii_strdown(nick, -1);
  gchar *sort = g_strdup_printf("%d %s", prefix_rank(px), fold);
  g_free(fold);

  GtkTreeIter iter;
  if (user_store_find(p, nick, &iter)) {
    gtk_list_store_set(p->user_store, &iter,
      ZC_USERLIST_COL_NICK, nick,
      ZC_USERLIST_COL_DISPLAY, disp,
      ZC_USERLIST_COL_SORTKEY, sort,
      -1);
  } else {
    gtk_list_store_append(p->user_store, &iter);
    gtk_list_store_set(p->user_store, &iter,
      ZC_USERLIST_COL_NICK, nick,
      ZC_USERLIST_COL_DISPLAY, disp,
      ZC_USERLIST_COL_SORTKEY, sort,
      -1);
  }

  g_free(disp);
  g_free(sort);
}

void
chat_page_userlist_remove(ChatPage *p, const gchar *nick) {
  if (!p || !p->user_store || !nick || !*nick) return;
  GtkTreeIter iter;
  if (user_store_find(p, nick, &iter)) {
    gtk_list_store_remove(p->user_store, &iter);
  }
}

void
chat_page_userlist_rename(ChatPage *p, const gchar *oldnick, const gchar *newnick) {
  if (!p || !p->user_store || !oldnick || !*oldnick || !newnick || !*newnick) return;

  GtkTreeIter iter;
  if (!user_store_find(p, oldnick, &iter)) return;

  gchar *disp = NULL;
  gtk_tree_model_get(GTK_TREE_MODEL(p->user_store), &iter, ZC_USERLIST_COL_DISPLAY, &disp, -1);
  gchar px = '\0';
  if (disp && (disp[0] == '~' || disp[0] == '&' || disp[0] == '@' || disp[0] == '%' || disp[0] == '+')) {
    px = disp[0];
  }
  g_free(disp);

  GtkTreeIter dup;
  if (user_store_find(p, newnick, &dup)) {
    gtk_list_store_remove(p->user_store, &dup);
  }
  chat_page_userlist_upsert(p, newnick, px);
  if (user_store_find(p, oldnick, &iter)) {
    gtk_list_store_remove(p->user_store, &iter);
  }
}

void
chat_page_append(ChatPage *p, const gchar *line) {
  if (!p || !line) return;

  GtkTextIter end;
  gtk_text_buffer_get_end_iter(p->buffer, &end);

  gchar *ts = timestamp_now();
  gchar *full = g_strdup_printf("[%s] %s\n", ts, line);
  gtk_text_buffer_insert(p->buffer, &end, full, -1);
  g_free(ts);
  g_free(full);

  GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(p->scroller));
  gtk_adjustment_set_value(vadj, gtk_adjustment_get_upper(vadj));
}

void
chat_page_append_fmt(ChatPage *p, const gchar *fmt, ...) {
  if (!p || !fmt) return;
  va_list ap;
  va_start(ap, fmt);
  gchar *line = g_strdup_vprintf(fmt, ap);
  va_end(ap);
  chat_page_append(p, line);
  g_free(line);
}
