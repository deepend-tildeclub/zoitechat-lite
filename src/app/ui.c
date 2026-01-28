#include "ui.h"
#include <stdint.h>
#include "chat_page.h"
#include "settings.h"

static void on_connect_clicked(GtkButton *btn, gpointer user_data);
static void on_disconnect_clicked(GtkButton *btn, gpointer user_data);

#include <gio/gio.h>
#include <string.h>

#include "zoitechat/zoitechat.h"
#include "zoitechat/irc_message.h"

typedef struct {
  GtkApplication *app;
  GtkWidget *win;

  GtkWidget *notebook;
  GtkWidget *status_label;

  GtkWidget *menu_btn;
  ZcClient *client;

  /* current connection settings */
  gchar *host;
  guint16 port;
  gboolean tls;
  gchar *nick;
  gchar *user;
  gchar *realname;
  gchar *auto_join;

  gboolean autojoin_pending;
  /* map target -> ChatPage* */
  GHashTable *pages;

  GtkWidget *conn_toggle_btn;
  /* channel -> (nick -> prefix string) */
  GHashTable *chan_users;

  /* persisted settings */
  ZcSettings *settings;
} UiState;




/* ZCL_DM_TABS_PATCH_V1 */
// Tab close buttons + DM/query helpers.
static void zcl_notebook_apply_close_button(UiState *st, GtkWidget *child);
static void zcl_on_tab_close_clicked(GtkButton *btn, gpointer user_data);
static void zcl_ui_open_query(UiState *st, const gchar *nick);
static void zcl_ui_close_target(UiState *st, const gchar *target, gboolean send_part);
static const gchar *zcl_target_for_child(UiState *st, GtkWidget *child);
static ChatPage *zcl_page_for_child(UiState *st, GtkWidget *child);

// Userlist interactions (only used if a userlist TreeView exists on the page).
static void zcl_userlist_row_activated(GtkTreeView *tv, GtkTreePath *path, GtkTreeViewColumn *col, gpointer user_data);
static gboolean zcl_userlist_button_press(GtkWidget *w, GdkEventButton *ev, gpointer user_data);
static gchar *zcl_userlist_get_nick_at_path(GtkTreeView *tv, GtkTreePath *path);
static gchar *zcl_userlist_normalize_nick(const gchar *s);
static void zcl_userlist_menu_send_dm(GtkMenuItem *mi, gpointer user_data);
static void zcl_userlist_menu_whois(GtkMenuItem *mi, gpointer user_data);
static void zcl_userlist_menu_copy(GtkMenuItem *mi, gpointer user_data);

static gboolean zcl_userlist_button_press(GtkWidget *w, GdkEventButton *ev, gpointer user_data);
static void on_userlist_menu_send_dm(GtkMenuItem *mi, gpointer user_data);
static void on_userlist_menu_whois(GtkMenuItem *mi, gpointer user_data);
static void on_userlist_menu_copy(GtkMenuItem *mi, gpointer user_data);

static void
ui_update_connect_toggle_button(UiState *st) {
  if (!st || !st->conn_toggle_btn) return;
  const gboolean connected = zc_client_is_connected(st->client);
  gtk_button_set_label(GTK_BUTTON(st->conn_toggle_btn), connected ? "Disconnect" : "Connect");
}

static void
on_connect_toggle_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  UiState *st = (UiState *)user_data;
  if (!st) return;

  if (zc_client_is_connected(st->client)) {
    on_disconnect_clicked(NULL, st);
  } else {
    on_connect_clicked(NULL, st);
  }

  ui_update_connect_toggle_button(st);
}

static gboolean
is_channel_name(const gchar *s) {
  return s && (*s == '#' || *s == '&' || *s == '!' || *s == '+');
}


static void on_entry_activate(GtkEntry *entry, gpointer user_data);
static void
zcl_userlist_row_activated(GtkTreeView *tv, GtkTreePath *path, GtkTreeViewColumn *col, gpointer user_data);


static ChatPage *
get_or_create_page(UiState *st, const gchar *target) {
  if (!target || !*target) target = "status";
  ChatPage *page = g_hash_table_lookup(st->pages, target);
  if (page) return page;

  page = chat_page_new(target);
  GtkWidget *root = chat_page_get_root(page);

  if (!g_object_get_data(G_OBJECT(root), "zcl-target")) {
    g_object_set_data_full(G_OBJECT(root), "zcl-target", g_strdup(target), g_free);
  }
  GtkWidget *tab = gtk_label_new(target);
  gtk_widget_set_halign(tab, GTK_ALIGN_START);

  gint idx = gtk_notebook_append_page(GTK_NOTEBOOK(st->notebook), root, tab);
  gtk_widget_show_all(root);
  gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(st->notebook), root, TRUE);
  gtk_notebook_set_current_page(GTK_NOTEBOOK(st->notebook), idx);

  /* connect entry handler */
  GtkEntry *entry = GTK_ENTRY(chat_page_get_entry(page));
  g_object_set_data(G_OBJECT(entry), "zc-target", (gpointer)chat_page_get_target(page));
  g_object_set_data(G_OBJECT(entry), "zc-state", st);
  g_signal_connect(entry, "activate", G_CALLBACK(on_entry_activate), NULL);


  /* userlist: right-click context menu (only if this page has a userlist view) */
  GtkWidget *uv = chat_page_get_userlist_view(page);
/* ZCL_USERLIST_SIGNALS_V1 */
  // Hook userlist interactions only for channel pages.
  // (DM/status tabs can have no userlist or should not DM-open on click.)
  if (uv && GTK_IS_TREE_VIEW(uv)) {
    const gchar *tgt = chat_page_get_target(page);
    gboolean is_chan = (tgt && (tgt[0] == '#' || tgt[0] == '&' || tgt[0] == '!' || tgt[0] == '+'));
    if (is_chan) {
      gtk_widget_add_events(uv, GDK_BUTTON_PRESS_MASK);
      g_signal_connect(uv, "row-activated", G_CALLBACK(zcl_userlist_row_activated), st);
      g_signal_connect(uv, "button-press-event", G_CALLBACK(zcl_userlist_button_press), st);
    }
  }

  if (uv && !g_object_get_data(G_OBJECT(uv), "zc-userlist-menu-hook")) {
    gtk_widget_add_events(uv, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(uv, "button-press-event", G_CALLBACK(zcl_userlist_button_press), st);
    g_object_set_data(G_OBJECT(uv), "zc-userlist-menu-hook", GINT_TO_POINTER(1));
  }

  uv = chat_page_get_userlist_view(page);
  if (uv) {
    g_signal_connect(uv, "row-activated", G_CALLBACK(zcl_userlist_row_activated), st);
  }


  g_hash_table_insert(st->pages, g_strdup(target), page);
  return page;
}

static const gchar *
current_target(UiState *st) {
  gint page_num = gtk_notebook_get_current_page(GTK_NOTEBOOK(st->notebook));
  if (page_num < 0) return "status";

  GtkWidget *child = gtk_notebook_get_nth_page(GTK_NOTEBOOK(st->notebook), page_num);
  const gchar *target = "status";

  /* child layout is vbox: scroller + entry_box */
  GList *kids = gtk_container_get_children(GTK_CONTAINER(child));
  for (GList *l = kids; l; l = l->next) {
    if (GTK_IS_BOX(l->data)) {
      GList *ek = gtk_container_get_children(GTK_CONTAINER(l->data));
      for (GList *e = ek; e; e = e->next) {
        if (GTK_IS_ENTRY(e->data)) {
          target = (const gchar *)g_object_get_data(G_OBJECT(e->data), "zc-target");
        }
      }
      g_list_free(ek);
    }
  }
  g_list_free(kids);

  return target ? target : "status";
}


static void
ui_open_query(UiState *st, const gchar *nick) {
  if (!st || !nick || !*nick) return;
  ChatPage *page = get_or_create_page(st, nick);
  gtk_widget_grab_focus(GTK_WIDGET(chat_page_get_entry(page)));
}

static void
zcl_userlist_row_activated(GtkTreeView *tv, GtkTreePath *path, GtkTreeViewColumn *col, gpointer user_data) {
  (void)col;
  UiState *st = (UiState *)user_data;
  GtkTreeModel *model = gtk_tree_view_get_model(tv);
  GtkTreeIter iter;
  if (!gtk_tree_model_get_iter(model, &iter, path)) return;

  gchar *nick = NULL;
  gtk_tree_model_get(model, &iter, ZC_USERLIST_COL_NICK, &nick, -1);
  if (!nick || !*nick) {
    g_free(nick);
    return;
  }

  if (st->nick && g_ascii_strcasecmp(st->nick, nick) == 0) {
    g_free(nick);
    return;
  }

  ui_open_query(st, nick);
  g_free(nick);
}


static void
set_status(UiState *st, const gchar *text) {
  const gchar *t = text ? text : "";
  /* No status text in the hamburger menu. Keep it as a tooltip only. */
  if (st && st->menu_btn) {
    gtk_widget_set_tooltip_text(st->menu_btn, *t ? t : "Menu");
  }
  /* If some older UI element still exists, keep it in sync harmlessly. */
  if (st && st->status_label) {
    gtk_label_set_text(GTK_LABEL(st->status_label), t);
  }
}

static void
ui_apply_window_icon(GtkWindow *win) {
  if (!win) return;

  /* Prefer the icon theme name everywhere. */
  gtk_window_set_default_icon_name("net.zoite.ZoiteChatLite");
  gtk_window_set_icon_name(win, "net.zoite.ZoiteChatLite");

  /* Dev runs: make sure the repo icon is visible to the icon theme. */
  GtkIconTheme *theme = gtk_icon_theme_get_default();
  if (theme && g_file_test("data/icons", G_FILE_TEST_IS_DIR)) {
    gtk_icon_theme_append_search_path(theme, "data/icons");
  }

  /* Some WMs/shells ignore icon-name unless we also set a pixbuf. */
  if (theme) {
    GError *e = NULL;
    GdkPixbuf *pix = gtk_icon_theme_load_icon(theme, "net.zoite.ZoiteChatLite", 128, 0, &e);
    if (!pix && e) {
      g_clear_error(&e);
    }
    if (pix) {
      gtk_window_set_icon(win, pix);
      g_object_unref(pix);
    }
  }
}

static void
on_about_zoitechat(GtkButton *btn, gpointer user_data) {
  (void)btn;
  UiState *st = (UiState *)user_data;
  if (!st || !st->win) return;

  /* If launched from the popover, hide it so it doesn't sit there like a confused raccoon. */
  GtkWidget *pop = gtk_widget_get_ancestor(GTK_WIDGET(btn), GTK_TYPE_POPOVER);
  if (pop) gtk_widget_hide(pop);

  GtkWidget *dlg = gtk_dialog_new_with_buttons(
    "About ZoiteChat",
    GTK_WINDOW(st->win),
    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
    "_Close", GTK_RESPONSE_CLOSE,
    NULL
  );

  

  gtk_window_set_default_icon_name("net.zoite.ZoiteChatLite");
  gtk_window_set_icon_name(GTK_WINDOW(dlg), "net.zoite.ZoiteChatLite");

/* force about dialog window icon */
gtk_window_set_default_icon_name("net.zoite.ZoiteChatLite");
gtk_window_set_icon_name(GTK_WINDOW(dlg), "net.zoite.ZoiteChatLite");

/* Plasma can ignore icon-name for dialogs unless we also set an explicit pixbuf. */
GtkIconTheme *theme = gtk_icon_theme_get_default();
if (theme) {
  GError *e = NULL;
  GdkPixbuf *pix = gtk_icon_theme_load_icon(theme, "net.zoite.ZoiteChatLite", 128, 0, &e);
  if (pix) {
    gtk_window_set_icon(GTK_WINDOW(dlg), pix);
    g_object_unref(pix);
  }
  if (e) g_clear_error(&e);
}
ui_apply_window_icon(GTK_WINDOW(dlg));

  GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_container_set_border_width(GTK_CONTAINER(box), 14);
  gtk_container_add(GTK_CONTAINER(area), box);

  /* Logo (same icon as the app) */
GtkWidget *img = gtk_image_new_from_icon_name("net.zoite.ZoiteChatLite", GTK_ICON_SIZE_DIALOG);
gtk_image_set_pixel_size(GTK_IMAGE(img), 96);
gtk_widget_set_halign(img, GTK_ALIGN_CENTER);
gtk_box_pack_start(GTK_BOX(box), img, FALSE, FALSE, 0);GtkWidget *title = gtk_label_new(NULL);
  gtk_label_set_markup(GTK_LABEL(title), "<b>ZoiteChat Lite</b>");
  gtk_widget_set_halign(title, GTK_ALIGN_CENTER);
  gtk_box_pack_start(GTK_BOX(box), title, FALSE, FALSE, 0);

  GtkWidget *copy = gtk_label_new("Copyright © 2026 zoite.net");
  gtk_widget_set_halign(copy, GTK_ALIGN_CENTER);
  gtk_box_pack_start(GTK_BOX(box), copy, FALSE, FALSE, 0);

  GtkWidget *lic = gtk_link_button_new_with_label(
    "https://www.gnu.org/licenses/old-licenses/gpl-2.0.html",
    "GPL-2.0 License"
  );
  gtk_widget_set_halign(lic, GTK_ALIGN_CENTER);
  gtk_box_pack_start(GTK_BOX(box), lic, FALSE, FALSE, 0);

  gtk_widget_show_all(dlg);
  gtk_dialog_run(GTK_DIALOG(dlg));
  gtk_widget_destroy(dlg);
}

static gint
prefix_rank(gchar c) {
  switch (c) {
    case '~': return 5;
    case '&': return 4;
    case '@': return 3;
    case '%': return 2;
    case '+': return 1;
    default: return 0;
  }
}

static GHashTable *
users_for_channel(UiState *st, const gchar *chan) {
  if (!st->chan_users) {
    st->chan_users = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_hash_table_destroy);
  }

  GHashTable *map = g_hash_table_lookup(st->chan_users, chan);
  if (!map) {
    map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free); /* nick -> prefix string */
    g_hash_table_insert(st->chan_users, g_strdup(chan), map);
  }
  return map;
}

static void
user_add_token(UiState *st, const gchar *chan, const gchar *token) {
  if (!is_channel_name(chan) || !token || !*token) return;

  gchar pfx = 0;
  const gchar *nick = token;
  if (strchr("~&@%+", token[0])) {
    pfx = token[0];
    nick = token + 1;
  }
  if (!nick || !*nick) return;

  GHashTable *map = users_for_channel(st, chan);
  gchar *existing = g_hash_table_lookup(map, nick);

  if (!existing) {
    gchar prefbuf[2] = {0, 0};
    if (pfx) prefbuf[0] = pfx;
    g_hash_table_insert(map, g_strdup(nick), g_strdup(prefbuf));
    return;
  }

  if (pfx && prefix_rank(pfx) > prefix_rank(existing[0])) {
    gchar prefbuf[2] = {pfx, 0};
    g_hash_table_replace(map, g_strdup(nick), g_strdup(prefbuf));
  }
}


static void
user_remove(UiState *st, const gchar *chan, const gchar *nick) {
  if (!st->chan_users || !is_channel_name(chan) || !nick || !*nick) return;
  GHashTable *map = g_hash_table_lookup(st->chan_users, chan);
  if (map) g_hash_table_remove(map, nick);
}

static void
user_remove_everywhere(UiState *st, const gchar *nick) {
  if (!st->chan_users || !nick || !*nick) return;
  GHashTableIter it;
  gpointer ck, cv;
  g_hash_table_iter_init(&it, st->chan_users);
  while (g_hash_table_iter_next(&it, &ck, &cv)) {
    GHashTable *map = (GHashTable *)cv;
    g_hash_table_remove(map, nick);
  }
}

static void
user_rename_everywhere(UiState *st, const gchar *oldnick, const gchar *newnick) {
  if (!st->chan_users || !oldnick || !newnick || !*oldnick || !*newnick) return;

  GHashTableIter it;
  gpointer ck, cv;
  g_hash_table_iter_init(&it, st->chan_users);
  while (g_hash_table_iter_next(&it, &ck, &cv)) {
    GHashTable *map = (GHashTable *)cv;
    gchar *pref = g_hash_table_lookup(map, oldnick);
    if (!pref) continue;

    gchar *pref_copy = g_strdup(pref);
    g_hash_table_remove(map, oldnick);
    g_hash_table_insert(map, g_strdup(newnick), pref_copy);
  }
}
static gchar
user_prefix_from_value(gpointer v) {
  if (!v) return 0;

  /* Some code stores prefixes as small integers (eg GINT_TO_POINTER). */
  if ((guintptr)v < 0x100) {
    const gchar c = (gchar)(guintptr)v;
    if (c == '~' || c == '&' || c == '@' || c == '%' || c == '+') return c;
    return 0;
  }

  /* Otherwise treat as a string like "@", "+", "~", or "" */
  const gchar *sv = (const gchar *)v;
  if (!sv || !sv[0]) return 0;
  if (sv[0] == '~' || sv[0] == '&' || sv[0] == '@' || sv[0] == '%' || sv[0] == '+') return sv[0];
  return 0;
}

static void
userlist_refresh_channel(UiState *st, const gchar *chan) {
  if (!st || !chan || !*chan) return;

  ChatPage *page = g_hash_table_lookup(st->pages, chan);
  GHashTable *map = users_for_channel(st, chan);

  if (!page) return;
  if (!chat_page_get_userlist_view(page)) return;

  chat_page_userlist_clear(page);
  if (!map) return;

  GHashTableIter it;
  gpointer k = NULL, v = NULL;
  g_hash_table_iter_init(&it, map);
  while (g_hash_table_iter_next(&it, &k, &v)) {
    const gchar *nick = (const gchar *)k;
    if (!nick || !*nick) continue;
    const gchar px = user_prefix_from_value(v);
    chat_page_userlist_upsert(page, nick, px);
  }
}

static void
userlist_refresh_all(UiState *st) {
  if (!st->chan_users) return;
  GHashTableIter it;
  gpointer ck, cv;
  g_hash_table_iter_init(&it, st->chan_users);
  while (g_hash_table_iter_next(&it, &ck, &cv)) {
    userlist_refresh_channel(st, (const gchar *)ck);
  }
}


static void
apply_css(void) {
  GtkCssProvider *prov = gtk_css_provider_new();
  gtk_css_provider_load_from_resource(prov, "/net/zoite/ZoiteChatLite/style.css");
  gtk_style_context_add_provider_for_screen(
    gdk_screen_get_default(),
    GTK_STYLE_PROVIDER(prov),
    GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
  );
  g_object_unref(prov);
}

static void
on_client_connected(ZcClient *client, UiState *st) {
  (void)client;
  set_status(st, "Connected");

  ChatPage *status = get_or_create_page(st, "status");
  chat_page_append(status, "Connected.");

  zc_client_set_identity(st->client, st->nick, st->user, st->realname);

  GError *error = NULL;
  if (!zc_client_login(st->client, &error)) {
    chat_page_append_fmt(status, "Login failed: %s", error->message);
    g_clear_error(&error);
    return;
  }
/* Joining before registration completes is unreliable across servers.
 * Queue auto-join and perform it after we receive 001 / end-of-MOTD.
 */
st->autojoin_pending = TRUE;
if (st->auto_join && *st->auto_join) {
  chat_page_append_fmt(status, "Auto-join queued: %s", st->auto_join);
  ui_update_connect_toggle_button(st);

}
  }


static void
on_client_disconnected(ZcClient *client, gint code, gchar *message, UiState *st) {
  (void)client;
  gchar *status = g_strdup_printf("Disconnected (%d): %s", code, message ? message : "");
  const gboolean _plain = (code == 0) && (!message || !*message || g_strcmp0(message, "Disconnected") == 0);
  set_status(st, _plain ? "Disconnected" : status);
  ChatPage *page = get_or_create_page(st, "status");
  chat_page_append(page, _plain ? "Disconnected." : status);

  g_free(status);
  ui_update_connect_toggle_button(st);

}

static void
append_server_line(UiState *st, const gchar *line) {
  ChatPage *status = get_or_create_page(st, "status");
  chat_page_append_fmt(status, "← %s", line);
}

static void
append_to_target(UiState *st, const gchar *target, const gchar *line) {
  ChatPage *page = get_or_create_page(st, target);
  chat_page_append(page, line);
}

static gboolean
is_ctcp_action(const gchar *text) {
  if (!text) return FALSE;
  return g_str_has_prefix(text, "\001ACTION ") && g_str_has_suffix(text, "\001");
}

static gchar *
ctcp_action_text(const gchar *text) {
  if (!is_ctcp_action(text)) return NULL;
  gsize len = strlen(text);
  /* "\001ACTION " is 8 bytes; trailing \001 is 1 */
  return g_strndup(text + 8, len - 9);
}

static void
ui_try_autojoin(UiState *st) {
  if (!st->autojoin_pending) return;
  st->autojoin_pending = FALSE;

  if (!st->auto_join || !*st->auto_join) return;
  if (!zc_client_is_connected(st->client)) return;

  /* Accept "#a,#b" or "#a #b" */
  gchar **parts = g_strsplit_set(st->auto_join, ", ", -1);
  for (gint i = 0; parts && parts[i]; i++) {
    const gchar *chan = parts[i];
    if (!chan || !*chan) continue;
    if (!is_channel_name(chan)) continue;

    (void)get_or_create_page(st, chan);

    GError *error = NULL;
    gchar *raw = g_strdup_printf("JOIN %s", chan);
    if (!zc_client_send_raw(st->client, raw, &error)) {
      ChatPage *status = get_or_create_page(st, "status");
      chat_page_append_fmt(status, "Auto-join JOIN %s failed: %s", chan, error ? error->message : "unknown");
      if (error) g_clear_error(&error);
    } else {
      ChatPage *status = get_or_create_page(st, "status");
      chat_page_append_fmt(status, "Joining %s …", chan);
    }
    g_free(raw);

    /* NAMES kick so user list can populate */
    error = NULL;
    raw = g_strdup_printf("NAMES %s", chan);
    (void)zc_client_send_raw(st->client, raw, &error);
    if (error) g_clear_error(&error);
    g_free(raw);
  }
  if (parts) g_strfreev(parts);
}

static void
on_client_irc_message(ZcClient *client, ZcIrcMessage *msg, UiState *st) {
  (void)client;
  if (!msg || !msg->command) return;


  if (st->autojoin_pending && (g_strcmp0(msg->command, "001") == 0 || g_strcmp0(msg->command, "376") == 0 || g_strcmp0(msg->command, "422") == 0)) {
    ui_try_autojoin(st);
  }

  /* NAMES (353/366) -> user list */
  if (g_strcmp0(msg->command, "353") == 0) {
    const gchar *chan = NULL;
    if (msg->params && msg->params->len >= 3) chan = zc_irc_message_param(msg, 2);
    else if (msg->params && msg->params->len >= 1) chan = zc_irc_message_param(msg, (guint)(msg->params->len - 1));
    const gchar *names = msg->trailing ? msg->trailing : "";
    if (chan && is_channel_name(chan) && names && *names) {
      gchar **parts = g_strsplit(names, " ", -1);
      for (gint i = 0; parts[i]; i++) user_add_token(st, chan, parts[i]);
      g_strfreev(parts);
      userlist_refresh_channel(st, chan);
    }
    /* keep existing status output below */
  }
  if (g_strcmp0(msg->command, "366") == 0) {
    const gchar *chan = zc_irc_message_param(msg, 1);
    if (chan && is_channel_name(chan)) userlist_refresh_channel(st, chan);
  }

  if (g_strcmp0(msg->command, "NICK") == 0) {
    gchar *oldn = zc_irc_extract_nick(msg->prefix);
    const gchar *newn = msg->trailing ? msg->trailing : zc_irc_message_param(msg, 0);
    if (oldn && newn && *newn) {
      user_rename_everywhere(st, oldn, newn);
      userlist_refresh_all(st);
    }
    g_free(oldn);
  }

  if (g_strcmp0(msg->command, "PRIVMSG") == 0) {
    const gchar *to = zc_irc_message_param(msg, 0);
    const gchar *text = msg->trailing ? msg->trailing : "";
    gchar *from = zc_irc_extract_nick(msg->prefix);



/* CTCP handling:
 * - don't open query tabs for CTCP requests (VERSION/PING/TIME/etc)
 * - reply with NOTICE as expected
 */
if (text && text[0] == '') {
  gsize tlen = strlen(text);
  if (tlen >= 2 && text[tlen - 1] == '' && !is_ctcp_action(text)) {
    gchar *inner = g_strndup(text + 1, tlen - 2);
    gchar **ct = g_strsplit(inner, " ", 2);
    const gchar *ctcp_cmd = ct[0] ? ct[0] : "";
    const gchar *ctcp_arg = ct[1] ? ct[1] : "";

    ChatPage *status = get_or_create_page(st, "status");
    chat_page_append_fmt(status, "CTCP %s request from %s%s%s",
      ctcp_cmd, from ? from : "?", *ctcp_arg ? " " : "", ctcp_arg);

    if (from && *from) {
      gchar *reply = NULL;

      if (g_ascii_strcasecmp(ctcp_cmd, "VERSION") == 0) {
        reply = g_strdup_printf("NOTICE %s :VERSION ZoiteChat Lite (GTK3 + LibZoiteChat)", from);
      } else if (g_ascii_strcasecmp(ctcp_cmd, "PING") == 0) {
        reply = g_strdup_printf("NOTICE %s :PING %s", from, ctcp_arg);
      } else if (g_ascii_strcasecmp(ctcp_cmd, "TIME") == 0) {
        GDateTime *dt = g_date_time_new_now_local();
        gchar *ts = g_date_time_format(dt, "%Y-%m-%d %H:%M:%S %z");
        reply = g_strdup_printf("NOTICE %s :TIME %s", from, ts ? ts : "");
        g_free(ts);
        g_date_time_unref(dt);
      }

      if (reply) {
        GError *e = NULL;
        (void)zc_client_send_raw(st->client, reply, &e);
        if (e) g_clear_error(&e);
        g_free(reply);
      }
    }

    g_strfreev(ct);
    g_free(inner);
    g_free(from);
    return;
  }
}

    const gchar *target = to ? to : "status";
    /* private message: target becomes sender nick */
    if (to && st->nick && g_ascii_strcasecmp(to, st->nick) == 0) target = from ? from : "status";

    if (is_ctcp_action(text)) {
      gchar *act = ctcp_action_text(text);
      gchar *line = g_strdup_printf("* %s %s", from ? from : "?", act ? act : "");
      append_to_target(st, target, line);
      g_free(line);
      g_free(act);
    } else {
      gchar *line = g_strdup_printf("<%s> %s", from ? from : "?", text);
      append_to_target(st, target, line);
      g_free(line);
    }

    g_free(from);
    return;
  }

  if (g_strcmp0(msg->command, "JOIN") == 0) {
    gchar *nick = zc_irc_extract_nick(msg->prefix);
    const gchar *chan = msg->trailing ? msg->trailing : zc_irc_message_param(msg, 0);
    if (chan) {
      gchar *line = g_strdup_printf("• %s joined", nick ? nick : "?");
      append_to_target(st, chan, line);
      g_free(line);
    }
        if (chan && is_channel_name(chan)) {
      user_add_token(st, chan, nick ? nick : "");
      userlist_refresh_channel(st, chan);
      if (nick && st->nick && g_ascii_strcasecmp(nick, st->nick) == 0) {
        GError *e = NULL;
        gchar *raw = g_strdup_printf("NAMES %s", chan);
        (void)zc_client_send_raw(st->client, raw, &e);
        g_free(raw);
        if (e) g_clear_error(&e);
      }
    }
    g_free(nick);
    return;
  }

  if (g_strcmp0(msg->command, "PART") == 0) {
    gchar *nick = zc_irc_extract_nick(msg->prefix);
    const gchar *chan = zc_irc_message_param(msg, 0);
    const gchar *why = msg->trailing;

    if (chan) {
      gchar *line = g_strdup_printf("• %s left%s%s%s",
        nick ? nick : "?",
        why ? " (" : "",
        why ? why : "",
        why ? ")" : ""
      );
      append_to_target(st, chan, line);
      g_free(line);
    }

        if (chan && is_channel_name(chan) && nick) {
      user_remove(st, chan, nick);
      userlist_refresh_channel(st, chan);
    }
    g_free(nick);
    return;
  }

  if (g_strcmp0(msg->command, "QUIT") == 0) {
    gchar *nick = zc_irc_extract_nick(msg->prefix);
    const gchar *why = msg->trailing;

    gchar *line = g_strdup_printf("• %s quit%s%s%s",
      nick ? nick : "?",
      why ? " (" : "",
      why ? why : "",
      why ? ")" : ""
    );
    append_server_line(st, line);
    g_free(line);

        if (nick) {
      user_remove_everywhere(st, nick);
      userlist_refresh_all(st);
    }
    g_free(nick);
    return;
  }

  /* Numerics and everything else go to status */
  {
    const gchar *p0 = (msg->params && msg->params->len > 0) ? (const gchar *)g_ptr_array_index(msg->params, 0) : "";
    gchar *line = g_strdup_printf("%s %s%s%s",
      msg->command,
      p0 ? p0 : "",
      msg->trailing ? " :" : "",
      msg->trailing ? msg->trailing : ""
    );
    append_server_line(st, line);
    g_free(line);
  }
}

static void
on_client_raw_line(ZcClient *client, gchar *line, UiState *st) {
  (void)client;
  if (!line) return;
  append_server_line(st, line);
}

static void
connect_async_cb(GObject *source, GAsyncResult *res, gpointer user_data) {
  (void)source;
  UiState *st = (UiState *)user_data;

  GError *error = NULL;
  if (!zc_client_connect_finish(st->client, res, &error)) {
    ChatPage *status = get_or_create_page(st, "status");
    chat_page_append_fmt(status, "Connect failed: %s", error->message);
    set_status(st, "Connect failed");
    g_clear_error(&error);
    return;
  }
}

static void
do_connect(UiState *st) {
  if (!st->host || !*st->host) return;

  ChatPage *status = get_or_create_page(st, "status");
  chat_page_append_fmt(status, "Connecting to %s:%u (%s)…",
    st->host, (guint)st->port, st->tls ? "TLS" : "plain");

  set_status(st, "Connecting…");

  zc_client_connect_async(st->client, st->host, st->port, st->tls, NULL, connect_async_cb, st);
}

static void
run_command(UiState *st, const gchar *target, const gchar *line) {
  if (!line || !*line) return;

  const gchar *effective_target = target ? target : "status";
  ChatPage *page = get_or_create_page(st, effective_target);

  if (line[0] != '/') {
    if (!zc_client_is_connected(st->client)) {
      chat_page_append(page, "Not connected.");
      return;
    }
    GError *error = NULL;
    if (!zc_client_privmsg(st->client, effective_target, line, &error)) {
      chat_page_append_fmt(page, "Send failed: %s", error->message);
      g_clear_error(&error);
    } else {
      chat_page_append_fmt(page, "<%s> %s", st->nick ? st->nick : "me", line);
    }
    return;
  }

  /* Slash commands */
  gchar **parts = g_strsplit(line + 1, " ", 2);
  const gchar *cmd = parts[0] ? parts[0] : "";
  const gchar *rest = parts[1] ? parts[1] : "";

  if (g_ascii_strcasecmp(cmd, "join") == 0) {
    if (!zc_client_is_connected(st->client)) {
      chat_page_append(page, "Not connected.");
    } else if (*rest) {
      GError *error = NULL;
      if (!zc_client_join(st->client, rest, &error)) {
        chat_page_append_fmt(page, "JOIN failed: %s", error->message);
        g_clear_error(&error);
      } else {
        get_or_create_page(st, rest);
      }
    }
  } else if (g_ascii_strcasecmp(cmd, "nick") == 0) {
    if (*rest) {
      g_free(st->nick);
      st->nick = g_strdup(rest);
      if (zc_client_is_connected(st->client)) {
        GError *error = NULL;
        gchar *raw = g_strdup_printf("NICK %s", rest);
        (void)zc_client_send_raw(st->client, raw, &error);
        if (error) {
          chat_page_append_fmt(page, "NICK failed: %s", error->message);
          g_clear_error(&error);
        }
        g_free(raw);
      }
    }
  } else if (g_ascii_strcasecmp(cmd, "me") == 0) {
    if (!zc_client_is_connected(st->client)) {
      chat_page_append(page, "Not connected.");
    } else if (*rest) {
      gchar *ctcp = g_strdup_printf("\001ACTION %s\001", rest);
      GError *error = NULL;
      if (!zc_client_privmsg(st->client, effective_target, ctcp, &error)) {
        chat_page_append_fmt(page, "ACTION failed: %s", error->message);
        g_clear_error(&error);
      } else {
        chat_page_append_fmt(page, "* %s %s", st->nick ? st->nick : "me", rest);
      }
      g_free(ctcp);
    }
  } else if (g_ascii_strcasecmp(cmd, "query") == 0) {
    gchar **p2 = g_strsplit(rest, " ", 2);
    const gchar *who = (p2 && p2[0]) ? p2[0] : "";
    const gchar *msg = (p2 && p2[1]) ? p2[1] : "";

    if (!who || !*who) {
      chat_page_append(page, "Usage: /query <nick> [message]");
    } else {
      ChatPage *pp = get_or_create_page(st, who);
      gtk_widget_grab_focus(GTK_WIDGET(chat_page_get_entry(pp)));

      if (*msg) {
        if (!zc_client_is_connected(st->client)) {
          chat_page_append(pp, "Not connected.");
        } else {
          GError *error = NULL;
          if (!zc_client_privmsg(st->client, who, msg, &error)) {
            chat_page_append_fmt(pp, "MSG failed: %s", error->message);
            g_clear_error(&error);
          } else {
            chat_page_append_fmt(pp, "<%s> %s", st->nick ? st->nick : "me", msg);
          }
        }
      }
    }

    g_strfreev(p2);
  } else if (g_ascii_strcasecmp(cmd, "msg") == 0) {
    if (!zc_client_is_connected(st->client)) {
      chat_page_append(page, "Not connected.");
    } else {
      gchar **p2 = g_strsplit(rest, " ", 2);
      const gchar *to = p2[0] ? p2[0] : "";
      const gchar *msg = p2[1] ? p2[1] : "";
      if (*to && *msg) {
        GError *error = NULL;
        if (!zc_client_privmsg(st->client, to, msg, &error)) {
          chat_page_append_fmt(page, "MSG failed: %s", error->message);
          g_clear_error(&error);
        } else {
          ChatPage *pp = get_or_create_page(st, to);
          chat_page_append_fmt(pp, "<%s> %s", st->nick ? st->nick : "me", msg);
        }
      }
      g_strfreev(p2);
    }
  } else if (g_ascii_strcasecmp(cmd, "raw") == 0) {
    if (!zc_client_is_connected(st->client)) {
      chat_page_append(page, "Not connected.");
    } else if (*rest) {
      GError *error = NULL;
      if (!zc_client_send_raw(st->client, rest, &error)) {
        chat_page_append_fmt(page, "RAW failed: %s", error->message);
        g_clear_error(&error);
      } else {
        chat_page_append_fmt(page, "→ %s", rest);
      }
    }
  } else if (g_ascii_strcasecmp(cmd, "quit") == 0) {
    if (zc_client_is_connected(st->client)) {
      GError *error = NULL;
      (void)zc_client_quit(st->client, *rest ? rest : NULL, &error);
      if (error) g_clear_error(&error);
    }
    zc_client_disconnect(st->client);
  } else if (g_strcmp0(cmd, "query") == 0) {
  // /query <nick> [message...]  (opens DM tab, optionally sends message)
  gchar **parts = g_strsplit(rest, " ", 2);
  const gchar *nick = parts[0] ? g_strstrip(parts[0]) : "";
  if (nick[0]) {
    zcl_ui_open_query(st, nick);
    if (parts[1] && parts[1][0] && st->client && zc_client_is_connected(st->client)) {
      gchar *line = g_strdup_printf("PRIVMSG %s :%s", nick, parts[1]);
      zc_client_send_raw(st->client, line, NULL);
      g_free(line);
    }
  } else {
    chat_page_append_fmt(page, "Usage: /query <nick> [message]");
  }
  g_strfreev(parts);
} else if (g_strcmp0(cmd, "whois") == 0) {
  gchar *nick = g_strdup(rest);
  g_strstrip(nick);
  ChatPage *status = get_or_create_page(st, "status");
  if (!nick[0]) {
    chat_page_append_fmt(page, "Usage: /whois <nick>");
  } else if (!st->client || !zc_client_is_connected(st->client)) {
    chat_page_append_fmt(status, "Not connected.");
  } else {
    gchar *line = g_strdup_printf("WHOIS %s", nick);
    zc_client_send_raw(st->client, line, NULL);
    g_free(line);
    chat_page_append_fmt(status, "→ WHOIS %s", nick);
  }
} else if (g_strcmp0(cmd, "close") == 0) {
  // /close [target]  (closes current tab if no target)
  gchar *t = g_strdup(rest);
  g_strstrip(t);
  if (!t[0]) {
    GtkNotebook *nb = GTK_NOTEBOOK(st->notebook);
    GtkWidget *child = gtk_notebook_get_nth_page(nb, gtk_notebook_get_current_page(nb));
    const gchar *cur = zcl_target_for_child(st, child);
    if (cur && g_strcmp0(cur, "status") != 0) zcl_ui_close_target(st, cur, TRUE);
  g_free(t);
  } else {
    zcl_ui_close_target(st, t, TRUE);
  }
} else {
  chat_page_append_fmt(page, "Unknown command: /%s", cmd);
}

  g_strfreev(parts);
}

static void
on_entry_activate(GtkEntry *entry, gpointer user_data) {
  (void)user_data;
  UiState *st = (UiState *)g_object_get_data(G_OBJECT(entry), "zc-state");
  const gchar *target = (const gchar *)g_object_get_data(G_OBJECT(entry), "zc-target");
  const gchar *text = gtk_entry_get_text(entry);
  if (!st || !text) return;

  run_command(st, target ? target : current_target(st), text);
  gtk_entry_set_text(entry, "");
}

static GtkWidget *
connect_dialog(UiState *st) {
  GtkWidget *dlg = gtk_dialog_new_with_buttons(
    "Connect",
    GTK_WINDOW(st->win),
    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
    "_Cancel", GTK_RESPONSE_CANCEL,
    "_Connect", GTK_RESPONSE_OK,
    NULL
  );

  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
  gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
  gtk_container_set_border_width(GTK_CONTAINER(grid), 12);
  gtk_container_add(GTK_CONTAINER(content), grid);

  GtkWidget *host = gtk_entry_new();
  gtk_entry_set_text(GTK_ENTRY(host), st->host ? st->host : "irc.libera.chat");

  GtkWidget *port = gtk_spin_button_new_with_range(1, 65535, 1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(port), st->port ? st->port : 6697);

  GtkWidget *tls = gtk_check_button_new_with_label("Use TLS");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(tls), st->tls);

  GtkWidget *nick = gtk_entry_new();
  gtk_entry_set_text(GTK_ENTRY(nick), st->nick ? st->nick : "zoiteguest");

  GtkWidget *user = gtk_entry_new();
  gtk_entry_set_text(GTK_ENTRY(user), st->user ? st->user : "zoite");

  GtkWidget *real = gtk_entry_new();
  gtk_entry_set_text(GTK_ENTRY(real), st->realname ? st->realname : "ZoiteChat Lite");

  GtkWidget *join = gtk_entry_new();
  gtk_entry_set_text(GTK_ENTRY(join), st->auto_join ? st->auto_join : "#zoite");

  int r = 0;
  gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Host"), 0, r, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), host, 1, r++, 1, 1);

  gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Port"), 0, r, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), port, 1, r++, 1, 1);

  gtk_grid_attach(GTK_GRID(grid), tls, 1, r++, 1, 1);

  gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Nick"), 0, r, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), nick, 1, r++, 1, 1);

  gtk_grid_attach(GTK_GRID(grid), gtk_label_new("User"), 0, r, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), user, 1, r++, 1, 1);

  gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Real name"), 0, r, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), real, 1, r++, 1, 1);

  gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Auto-join"), 0, r, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), join, 1, r++, 1, 1);

  gtk_widget_show_all(dlg);

  gint resp = gtk_dialog_run(GTK_DIALOG(dlg));
  if (resp == GTK_RESPONSE_OK) {
    g_free(st->host);
    g_free(st->nick);
    g_free(st->user);
    g_free(st->realname);
    g_free(st->auto_join);

    st->host = g_strdup(gtk_entry_get_text(GTK_ENTRY(host)));
    st->port = (guint16)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(port));
    st->tls = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(tls));
    st->nick = g_strdup(gtk_entry_get_text(GTK_ENTRY(nick)));
    st->user = g_strdup(gtk_entry_get_text(GTK_ENTRY(user)));
    st->realname = g_strdup(gtk_entry_get_text(GTK_ENTRY(real)));
    st->auto_join = g_strdup(gtk_entry_get_text(GTK_ENTRY(join)));

    if (st->settings) {
      g_free(st->settings->host); st->settings->host = g_strdup(st->host);
      st->settings->port = st->port;
      st->settings->tls = st->tls;
      g_free(st->settings->nick); st->settings->nick = g_strdup(st->nick);
      g_free(st->settings->user); st->settings->user = g_strdup(st->user);
      g_free(st->settings->realname); st->settings->realname = g_strdup(st->realname);
      g_free(st->settings->auto_join); st->settings->auto_join = g_strdup(st->auto_join);
      GError *se = NULL;
      (void)zc_settings_save(st->settings, &se);
      if (se) g_clear_error(&se);
    }
    do_connect(st);
  }

  gtk_widget_destroy(dlg);
  return NULL;
}

static void
on_connect_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  UiState *st = (UiState *)user_data;
  connect_dialog(st);
}

static void
on_disconnect_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  UiState *st = (UiState *)user_data;
  zc_client_disconnect(st->client);
}

static void
on_page_added(GtkNotebook *nb, GtkWidget *child, guint page_num, gpointer user_data) {
  (void)nb; (void)child;
  UiState *st = (UiState *)user_data;

  GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(st->notebook), (gint)page_num);
  GList *kids = gtk_container_get_children(GTK_CONTAINER(page));
  for (GList *l = kids; l; l = l->next) {
    if (GTK_IS_BOX(l->data)) {
      GList *ek = gtk_container_get_children(GTK_CONTAINER(l->data));
      for (GList *e = ek; e; e = e->next) {
        if (GTK_IS_ENTRY(e->data)) {
          g_signal_connect(e->data, "activate", G_CALLBACK(on_entry_activate), NULL);
        }
      }
      g_list_free(ek);
    }
  }
  zcl_notebook_apply_close_button(st, child);

  g_list_free(kids);
}

static void
ui_state_free(UiState *st) {
  if (!st) return;

  if (st->settings && st->win && GTK_IS_WINDOW(st->win)) {
    gint w = 0, h = 0;
    gtk_window_get_size(GTK_WINDOW(st->win), &w, &h);
    st->settings->win_w = w;
    st->settings->win_h = h;
    g_free(st->settings->host); st->settings->host = g_strdup(st->host ? st->host : "");
    st->settings->port = st->port;
    st->settings->tls = st->tls;
    g_free(st->settings->nick); st->settings->nick = g_strdup(st->nick ? st->nick : "");
    g_free(st->settings->user); st->settings->user = g_strdup(st->user ? st->user : "");
    g_free(st->settings->realname); st->settings->realname = g_strdup(st->realname ? st->realname : "");
    g_free(st->settings->auto_join); st->settings->auto_join = g_strdup(st->auto_join ? st->auto_join : "");
    GError *se = NULL;
    (void)zc_settings_save(st->settings, &se);
    if (se) g_clear_error(&se);
  }

  g_free(st->host);
  g_free(st->nick);
  g_free(st->user);
  g_free(st->realname);
  g_free(st->auto_join);

  if (st->chan_users) {
    g_hash_table_destroy(st->chan_users);
    st->chan_users = NULL;
  }

  if (st->settings) {
    zc_settings_free(st->settings);
    st->settings = NULL;
  }

  if (st->pages) {
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, st->pages);
    while (g_hash_table_iter_next(&it, &k, &v)) {
      chat_page_free((ChatPage *)v);
    }
    g_hash_table_destroy(st->pages);
  }

  g_clear_object(&st->client);
  g_free(st);
}

/* userlist context menu */

static gchar *
userlist_extract_nick(GtkTreeModel *model, GtkTreeIter *iter) {
  if (!model || !iter) return NULL;

  const gint ncols = gtk_tree_model_get_n_columns(model);
  for (gint col = 0; col < ncols; col++) {
    GValue v = G_VALUE_INIT;
    gtk_tree_model_get_value(model, iter, col, &v);

    if (G_VALUE_HOLDS_STRING(&v)) {
      const gchar *sv = g_value_get_string(&v);
      if (sv && sv[0]) {
        /* Ignore pure prefix columns like "@", "+", etc. */
        if (!(sv[1] == '\0' && (sv[0] == '~' || sv[0] == '&' || sv[0] == '@' || sv[0] == '%' || sv[0] == '+'))) {
          gchar *out = g_strdup(sv);
          g_value_unset(&v);
          return out;
        }
      }
    }

    g_value_unset(&v);
  }

  return NULL;
}

static void
userlist_popup_menu(UiState *st, const gchar *nick, GdkEventButton *ev) {
  if (!st || !nick || !*nick) return;

  GtkWidget *menu = gtk_menu_new();

  GtkWidget *mi_dm = gtk_menu_item_new_with_label("Send DM");
  g_object_set_data_full(G_OBJECT(mi_dm), "zc-nick", g_strdup(nick), g_free);
  g_signal_connect(mi_dm, "activate", G_CALLBACK(zcl_userlist_menu_send_dm), st);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi_dm);

  GtkWidget *mi_whois = gtk_menu_item_new_with_label("WHOIS");
  g_object_set_data_full(G_OBJECT(mi_whois), "zc-nick", g_strdup(nick), g_free);
  g_signal_connect(mi_whois, "activate", G_CALLBACK(zcl_userlist_menu_whois), st);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi_whois);

  GtkWidget *sep = gtk_separator_menu_item_new();
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), sep);

  GtkWidget *mi_copy = gtk_menu_item_new_with_label("Copy Nick");
  g_object_set_data_full(G_OBJECT(mi_copy), "zc-nick", g_strdup(nick), g_free);
  g_signal_connect(mi_copy, "activate", G_CALLBACK(zcl_userlist_menu_copy), st);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi_copy);

  gtk_widget_show_all(menu);

  g_signal_connect_swapped(menu, "deactivate", G_CALLBACK(gtk_widget_destroy), menu);

#if GTK_CHECK_VERSION(3,22,0)
  gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
#else
  gtk_menu_popup(GTK_MENU(menu), NULL, NULL, NULL, NULL, ev ? ev->button : 0, ev ? ev->time : gtk_get_current_event_time());
#endif
}

static gboolean
zcl_userlist_button_press(GtkWidget *w, GdkEventButton *ev, gpointer user_data) {
  UiState *st = (UiState *)user_data;
  if (!GTK_IS_TREE_VIEW(w) || !ev) return FALSE;

  if (ev->type == GDK_BUTTON_PRESS && ev->button == 3) {
    GtkTreeView *tv = GTK_TREE_VIEW(w);
    GtkTreeModel *model = gtk_tree_view_get_model(tv);
    GtkTreePath *path = NULL;

    if (gtk_tree_view_get_path_at_pos(tv, (gint)ev->x, (gint)ev->y, &path, NULL, NULL, NULL) && path) {
      GtkTreeSelection *sel = gtk_tree_view_get_selection(tv);
      gtk_tree_selection_unselect_all(sel);
      gtk_tree_selection_select_path(sel, path);
      gtk_tree_view_set_cursor(tv, path, NULL, FALSE);

      GtkTreeIter iter;
      if (model && gtk_tree_model_get_iter(model, &iter, path)) {
        gchar *nick = userlist_extract_nick(model, &iter);
        if (nick) {
          userlist_popup_menu(st, nick, ev);
          g_free(nick);
          gtk_tree_path_free(path);
          return TRUE;
        }
      }

      gtk_tree_path_free(path);
    }

    return TRUE; /* we handled the click even if we didn't resolve a nick */
  }

  return FALSE;
}

static void
on_userlist_menu_send_dm(GtkMenuItem *mi, gpointer user_data)
{
  /* Old handler name kept as a wrapper for compatibility. */
  zcl_userlist_menu_send_dm(mi, user_data);
}



static void
on_userlist_menu_whois(GtkMenuItem *mi, gpointer user_data)
{
  /* Old handler name kept as a wrapper for compatibility. */
  zcl_userlist_menu_whois(mi, user_data);
}



static void
on_userlist_menu_copy(GtkMenuItem *mi, gpointer user_data)
{
  /* Old handler name kept as a wrapper for compatibility. */
  zcl_userlist_menu_copy(mi, user_data);
}




/* ZCL_DM_TABS_IMPL_V1 */

static ChatPage *
zcl_page_for_child(UiState *st, GtkWidget *child) {
  if (!st || !child) return NULL;
  GHashTableIter it;
  gpointer k = NULL, v = NULL;
  g_hash_table_iter_init(&it, st->pages);
  while (g_hash_table_iter_next(&it, &k, &v)) {
    ChatPage *p = (ChatPage *)v;
    if (chat_page_get_root(p) == child) return p;
  }
  return NULL;
}

static const gchar *
zcl_target_for_child(UiState *st, GtkWidget *child) {
  const gchar *t = g_object_get_data(G_OBJECT(child), "zcl-target");
  if (t && t[0]) return t;

  ChatPage *p = zcl_page_for_child(st, child);
  return p ? chat_page_get_target(p) : NULL;
}

static void
zcl_notebook_apply_close_button(UiState *st, GtkWidget *child) {
  if (!st || !child) return;
  GtkNotebook *nb = GTK_NOTEBOOK(st->notebook);

  const gchar *target = zcl_target_for_child(st, child);
  if (!target || !target[0] || g_strcmp0(target, "status") == 0) return;
// don't close status

  GtkWidget *existing = gtk_notebook_get_tab_label(nb, child);
  if (existing && g_object_get_data(G_OBJECT(existing), "zcl-tab") != NULL) return;

  const gchar *label_text = NULL;
  if (existing && GTK_IS_LABEL(existing)) {
    label_text = gtk_label_get_text(GTK_LABEL(existing));
  }
  if (!label_text) label_text = target ? target : "tab";

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  g_object_set_data(G_OBJECT(box), "zcl-tab", (gpointer)0x1);

  GtkWidget *lbl = gtk_label_new(label_text);
  gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
  gtk_widget_set_halign(lbl, GTK_ALIGN_START);

  gtk_widget_set_hexpand(lbl, TRUE);
  gtk_label_set_width_chars(GTK_LABEL(lbl), 12);
  gtk_label_set_max_width_chars(GTK_LABEL(lbl), 24);
  GtkWidget *btn = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_MENU);
  if (!btn) btn = gtk_button_new_with_label("×");
  gtk_button_set_relief(GTK_BUTTON(btn), GTK_RELIEF_NONE);
  gtk_widget_set_focus_on_click(btn, FALSE);
  gtk_widget_set_tooltip_text(btn, "Close tab");
  g_object_set_data(G_OBJECT(btn), "zcl-child", child);

  g_signal_connect(btn, "clicked", G_CALLBACK(zcl_on_tab_close_clicked), st);

  gtk_box_pack_start(GTK_BOX(box), lbl, TRUE, TRUE, 0);
  gtk_box_pack_end(GTK_BOX(box), btn, FALSE, FALSE, 0);

  gtk_widget_show_all(box);
  gtk_notebook_set_tab_label(nb, child, box);
}

static void
zcl_ui_close_target(UiState *st, const gchar *target, gboolean send_part) {
  if (!st || !target || !*target) return;
  if (g_strcmp0(target, "status") == 0) return;

  ChatPage *page = (ChatPage *)g_hash_table_lookup(st->pages, target);
  if (!page) return;

  if (send_part && (target[0] == '#' || target[0] == '&' || target[0] == '!' || target[0] == '+')) {
    if (st->client && zc_client_is_connected(st->client)) {
      gchar *line = g_strdup_printf("PART %s :Closed", target);
      zc_client_send_raw(st->client, line, NULL);
      g_free(line);
    }
  }

  GtkWidget *child = chat_page_get_root(page);
  gint idx = gtk_notebook_page_num(GTK_NOTEBOOK(st->notebook), child);
  if (idx >= 0) gtk_notebook_remove_page(GTK_NOTEBOOK(st->notebook), idx);

  g_hash_table_remove(st->pages, target);
  chat_page_free(page);
}

static void
zcl_on_tab_close_clicked(GtkButton *btn, gpointer user_data) {
  UiState *st = (UiState *)user_data;
  GtkWidget *child = (GtkWidget *)g_object_get_data(G_OBJECT(btn), "zcl-child");
  const gchar *target = zcl_target_for_child(st, child);
  if (target) zcl_ui_close_target(st, target, TRUE);
}

static void
zcl_ui_open_query(UiState *st, const gchar *nick) {
  if (!st || !nick || !*nick) return;
  ChatPage *page = get_or_create_page(st, nick);
  GtkWidget *child = chat_page_get_root(page);
  gint idx = gtk_notebook_page_num(GTK_NOTEBOOK(st->notebook), child);
  if (idx >= 0) gtk_notebook_set_current_page(GTK_NOTEBOOK(st->notebook), idx);
  GtkWidget *entry = GTK_WIDGET(chat_page_get_entry(page));
  if (entry) gtk_widget_grab_focus(entry);
}

static gchar *
zcl_userlist_normalize_nick(const gchar *s) {
  if (!s) return NULL;
  while (*s == ' ' || *s == '\t') s++;
  if (*s == '\0') return NULL;
  if ((*s == '@' || *s == '+' || *s == '%' || *s == '~' || *s == '&') && s[1] != '\0') s++;
  return g_strdup(s);
}

static gchar *
zcl_userlist_get_nick_at_path(GtkTreeView *tv, GtkTreePath *path) {
  if (!tv || !path) return NULL;
  GtkTreeModel *model = gtk_tree_view_get_model(tv);
  if (!model) return NULL;

  GtkTreeIter iter;
  if (!gtk_tree_model_get_iter(model, &iter, path)) return NULL;

  gchar *s0 = NULL;
  gtk_tree_model_get(model, &iter, 0, &s0, -1);
  if (s0 && *s0) {
    gchar *n = zcl_userlist_normalize_nick(s0);
    g_free(s0);
    return n;
  }
  g_free(s0);

  gchar *s1 = NULL;
  gtk_tree_model_get(model, &iter, 1, &s1, -1);
  if (s1 && *s1) {
    gchar *n = zcl_userlist_normalize_nick(s1);
    g_free(s1);
    return n;
  }
  g_free(s1);
  return NULL;
}

static void
zcl_userlist_menu_send_dm(GtkMenuItem *mi, gpointer user_data) {
  UiState *st = (UiState *)user_data;
  const gchar *nick = (const gchar *)g_object_get_data(G_OBJECT(mi), "zcl-nick");
  if (nick && *nick) zcl_ui_open_query(st, nick);
}

static void
zcl_userlist_menu_whois(GtkMenuItem *mi, gpointer user_data) {
  UiState *st = (UiState *)user_data;
  const gchar *nick = (const gchar *)g_object_get_data(G_OBJECT(mi), "zcl-nick");
  if (!nick || !*nick) return;

  ChatPage *status = get_or_create_page(st, "status");
  if (!st->client || !zc_client_is_connected(st->client)) {
    chat_page_append_fmt(status, "Not connected.");
    return;
  }

  gchar *line = g_strdup_printf("WHOIS %s", nick);
  zc_client_send_raw(st->client, line, NULL);
  g_free(line);
  chat_page_append_fmt(status, "→ WHOIS %s", nick);
}

static void
zcl_userlist_menu_copy(GtkMenuItem *mi, gpointer user_data) {
  (void)user_data;
  const gchar *nick = (const gchar *)g_object_get_data(G_OBJECT(mi), "zcl-nick");
  if (!nick || !*nick) return;

  GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
  gtk_clipboard_set_text(cb, nick, -1);
}




GtkWidget *
zc_ui_create_main_window(GtkApplication *app) {
  apply_css();

  UiState *st = g_new0(UiState, 1);
  st->app = app;
  st->client = zc_client_new();

  st->settings = zc_settings_load();

  st->host = g_strdup(st->settings->host);
  st->port = st->settings->port;
  st->tls = st->settings->tls;
  st->nick = g_strdup(st->settings->nick);
  st->user = g_strdup(st->settings->user);
  st->realname = g_strdup(st->settings->realname);
  st->auto_join = g_strdup(st->settings->auto_join);


  st->pages = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

  st->win = gtk_application_window_new(app);
/* Icons: use the one true icon everywhere (dev + installed). */
  ui_apply_window_icon(GTK_WINDOW(st->win));

GtkIconTheme *theme = gtk_icon_theme_get_default();
if (theme && g_file_test("data/icons", G_FILE_TEST_IS_DIR)) {
  /* So gtk_image_new_from_icon_name("net.zoite.ZoiteChatLite", ...) resolves in dev runs. */
  gtk_icon_theme_append_search_path(theme, "data/icons");
}

gtk_window_set_default_icon_name("net.zoite.ZoiteChatLite");
gtk_window_set_icon_name(GTK_WINDOW(st->win), "net.zoite.ZoiteChatLite");

/* Some shells ignore icon-name unless the app is installed. Set the window icon directly from the SVG in dev runs. */
if (g_file_test("data/icons/hicolor/scalable/apps/net.zoite.ZoiteChatLite.svg", G_FILE_TEST_EXISTS)) {
  GError *e = NULL;
  GdkPixbuf *pix = gdk_pixbuf_new_from_file_at_scale("data/icons/hicolor/scalable/apps/net.zoite.ZoiteChatLite.svg", 128, 128, TRUE, &e);
  if (pix) {
    gtk_window_set_icon(GTK_WINDOW(st->win), pix);
    g_object_unref(pix);
  } else {
    g_clear_error(&e);
  }
}
  gtk_window_set_default_size(GTK_WINDOW(st->win), st->settings ? st->settings->win_w : 980, st->settings ? st->settings->win_h : 640);
  gtk_window_set_title(GTK_WINDOW(st->win), "ZoiteChat Lite");
  gtk_window_set_position(GTK_WINDOW(st->win), GTK_WIN_POS_CENTER);

  GtkWidget *hb = gtk_header_bar_new();
  gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(hb), TRUE);
  gtk_header_bar_set_title(GTK_HEADER_BAR(hb), "ZoiteChat Lite");
  gtk_header_bar_set_subtitle(GTK_HEADER_BAR(hb), "GTK3 frontend + LibZoiteChat backend");
  gtk_window_set_titlebar(GTK_WINDOW(st->win), hb);

  GtkWidget *btn_connect = gtk_button_new_with_label("Connect");
  st->conn_toggle_btn = btn_connect;
  ui_update_connect_toggle_button(st);
  gtk_header_bar_pack_start(GTK_HEADER_BAR(hb), btn_connect);

/* Right-side hamburger menu (replaces the old status label). */
GtkWidget *menu_btn = gtk_menu_button_new();
GtkWidget *menu_img = gtk_image_new_from_icon_name("open-menu-symbolic", GTK_ICON_SIZE_BUTTON);
gtk_button_set_image(GTK_BUTTON(menu_btn), menu_img);
gtk_widget_set_tooltip_text(menu_btn, "Menu");
gtk_header_bar_pack_end(GTK_HEADER_BAR(hb), menu_btn);
st->menu_btn = menu_btn;

GtkWidget *popover = gtk_popover_new(menu_btn);
GtkWidget *vb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
gtk_container_set_border_width(GTK_CONTAINER(vb), 10);
GtkWidget *about_btn = gtk_model_button_new();
gtk_button_set_label(GTK_BUTTON(about_btn), "About ZoiteChat");
gtk_widget_set_halign(about_btn, GTK_ALIGN_START);
g_signal_connect(about_btn, "clicked", G_CALLBACK(on_about_zoitechat), st);
gtk_box_pack_start(GTK_BOX(vb), about_btn, FALSE, FALSE, 0);

gtk_container_add(GTK_CONTAINER(popover), vb);
gtk_widget_show_all(popover);
gtk_menu_button_set_popover(GTK_MENU_BUTTON(menu_btn), popover);

set_status(st, zc_client_is_connected(st->client) ? "Connected" : "Disconnected");
  GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_container_add(GTK_CONTAINER(st->win), root);

  st->notebook = gtk_notebook_new();
  gtk_notebook_set_scrollable(GTK_NOTEBOOK(st->notebook), TRUE);
  gtk_box_pack_start(GTK_BOX(root), st->notebook, TRUE, TRUE, 0);

  g_signal_connect(btn_connect, "clicked", G_CALLBACK(on_connect_toggle_clicked), st);
  g_signal_connect(st->notebook, "page-added", G_CALLBACK(on_page_added), st);

  /* status page */
  get_or_create_page(st, "status");

  /* connect to backend signals */
  g_signal_connect(st->client, "connected", G_CALLBACK(on_client_connected), st);
  g_signal_connect(st->client, "disconnected", G_CALLBACK(on_client_disconnected), st);
  g_signal_connect(st->client, "raw-line", G_CALLBACK(on_client_raw_line), st);
  g_signal_connect(st->client, "irc-message", G_CALLBACK(on_client_irc_message), st);

  g_object_set_data_full(G_OBJECT(st->win), "zc-state", st, (GDestroyNotify)ui_state_free);

  gtk_widget_show_all(st->win);
  return st->win;
}
