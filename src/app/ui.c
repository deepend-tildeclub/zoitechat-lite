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
static gchar *zcl_userlist_normalize_nick(const gchar *s);

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

  /* Ensure tab-building callbacks can always resolve the page/target. */
  g_hash_table_insert(st->pages, g_strdup(target), page);
  g_object_set_data_full(G_OBJECT(root), "zcl-target", g_strdup(target), g_free);

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


static void zcl_whois_clear(void);

static void
on_client_disconnected(ZcClient *client, gint code, gchar *message, UiState *st) {
  zcl_whois_clear();
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

/* ZCL_WHOIS_DIALOG_V1
 * Collect WHOIS numerics and show them in a dialog instead of spamming status.
 */
typedef struct {
  gchar *nick;
  gchar *userhost;
  gchar *realname;
  gchar *server;
  gchar *server_info;
  gchar *account;
  gchar *away;
  gboolean is_oper;
  gboolean is_secure;
  guint idle_secs;
  gint64 signon_ts;
  gchar *channels_raw;
} ZclWhoisPending;

static ZclWhoisPending *zcl_whois = NULL;

static void
zcl_whois_clear(void) {
  if (!zcl_whois) return;
  g_free(zcl_whois->nick);
  g_free(zcl_whois->userhost);
  g_free(zcl_whois->realname);
  g_free(zcl_whois->server);
  g_free(zcl_whois->server_info);
  g_free(zcl_whois->account);
  g_free(zcl_whois->away);
  g_free(zcl_whois->channels_raw);
  g_free(zcl_whois);
  zcl_whois = NULL;
}

static void
zcl_whois_begin(const gchar *nick) {
  zcl_whois_clear();
  zcl_whois = g_new0(ZclWhoisPending, 1);
  zcl_whois->nick = g_strdup(nick ? nick : "");
}

static gchar *
zcl_format_duration(guint secs) {
  guint h = secs / 3600;
  guint m = (secs % 3600) / 60;
  guint s = secs % 60;
  if (h) return g_strdup_printf("%uh %um %us", h, m, s);
  if (m) return g_strdup_printf("%um %us", m, s);
  return g_strdup_printf("%us", s);
}

static gchar *
zcl_wrap_words(const gchar *s, gint width, const gchar *indent) {
  if (!s || !*s) return g_strdup("");
  if (width < 10) width = 78;
  if (!indent) indent = "";

  gchar **toks = g_strsplit(s, " ", -1);
  GString *out = g_string_new(NULL);

  gint col = 0;
  for (gchar **t = toks; t && *t; t++) {
    if (!**t) continue;
    gint need = (gint)strlen(*t) + 1;
    if (col == 0) {
      g_string_append(out, indent);
      col = (gint)strlen(indent);
    }
    if (col + need > width) {
      g_string_append(out, "\n");
      g_string_append(out, indent);
      col = (gint)strlen(indent);
    }
    g_string_append(out, *t);
    g_string_append_c(out, ' ');
    col += need;
  }

  g_strfreev(toks);

  if (out->len && out->str[out->len - 1] == ' ')
    g_string_truncate(out, out->len - 1);

  return g_string_free(out, FALSE);
}

static gchar *
zcl_whois_render_text(const ZclWhoisPending *w) {
  GString *out = g_string_new(NULL);

  g_string_append_printf(out, "WHOIS: %s\n\n", (w && w->nick && *w->nick) ? w->nick : "?");

  if (w && w->userhost && *w->userhost)
    g_string_append_printf(out, "User:      %s\n", w->userhost);

  if (w && w->realname && *w->realname)
    g_string_append_printf(out, "Real name: %s\n", w->realname);

  if (w && w->server && *w->server) {
    if (w->server_info && *w->server_info)
      g_string_append_printf(out, "Server:    %s (%s)\n", w->server, w->server_info);
    else
      g_string_append_printf(out, "Server:    %s\n", w->server);
  }

  if (w && w->account && *w->account)
    g_string_append_printf(out, "Account:   %s\n", w->account);

  if (w && w->away && *w->away)
    g_string_append_printf(out, "Away:      %s\n", w->away);

  if (w && w->idle_secs) {
    gchar *dur = zcl_format_duration(w->idle_secs);
    g_string_append_printf(out, "Idle:      %s\n", dur);
    g_free(dur);
  }

  if (w && w->signon_ts > 0) {
    GDateTime *dt = g_date_time_new_from_unix_local(w->signon_ts);
    if (dt) {
      gchar *ts = g_date_time_format(dt, "%Y-%m-%d %H:%M:%S");
      g_string_append_printf(out, "Signed on: %s\n", ts ? ts : "");
      g_free(ts);
      g_date_time_unref(dt);
    }
  }

  if (w && (w->is_oper || w->is_secure)) {
    g_string_append(out, "Flags:     ");
    gboolean first = TRUE;
    if (w->is_oper) {
      g_string_append(out, "IRC operator");
      first = FALSE;
    }
    if (w->is_secure) {
      if (!first) g_string_append(out, ", ");
      g_string_append(out, "secure connection");
    }
    g_string_append_c(out, '\n');
  }

  if (w && w->channels_raw && *w->channels_raw) {
    gint n = 0;
    gchar **toks = g_strsplit(w->channels_raw, " ", -1);
    for (gchar **t = toks; t && *t; t++) {
      if (**t) n++;
    }
    g_strfreev(toks);
    g_string_append_printf(out, "Channels:  %d (see list)\n", n);
  }

  

  return g_string_free(out, FALSE);
}

static gchar *
zcl_whois_render_copy_text(const ZclWhoisPending *w) {
  gchar *body = zcl_whois_render_text(w);
  if (!w || !w->channels_raw || !*w->channels_raw) return body;

  GString *out = g_string_new(body ? body : "");
  g_free(body);

  g_string_append(out, "\n\nChannels:\n");
  gchar **toks = g_strsplit(w->channels_raw, " ", -1);
  for (gchar **t = toks; t && *t; t++) {
    if (!**t) continue;
    g_string_append_printf(out, "  %s\n", *t);
  }
  g_strfreev(toks);

  return g_string_free(out, FALSE);
}

static const gchar *
zcl_channel_name_no_prefix(const gchar *tok) {
  if (!tok) return "";
  while (*tok && (*tok == '~' || *tok == '&' || *tok == '@' || *tok == '%' || *tok == '+')) tok++;
  return tok;
}

static gint
zcl_channel_cmp(gconstpointer a, gconstpointer b) {
  const gchar *sa = *(const gchar * const *)a;
  const gchar *sb = *(const gchar * const *)b;
  const gchar *na = zcl_channel_name_no_prefix(sa);
  const gchar *nb = zcl_channel_name_no_prefix(sb);

  gint c = g_ascii_strcasecmp(na, nb);
  if (c != 0) return c;
  return g_ascii_strcasecmp(sa, sb);
}

static void
zcl_whois_fill_channels_list(GtkListBox *lb, const gchar *raw) {
  if (!lb) return;

  /* Clear existing rows. */
  GList *rows = gtk_container_get_children(GTK_CONTAINER(lb));
  for (GList *l = rows; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
  g_list_free(rows);

  if (!raw || !*raw) {
    GtkWidget *lbl = gtk_label_new("No channels");
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_list_box_insert(lb, lbl, -1);
    return;
  }

  GPtrArray *arr = g_ptr_array_new_with_free_func(g_free);
  gchar **toks = g_strsplit(raw, " ", -1);
  for (gchar **t = toks; t && *t; t++) {
    if (!**t) continue;
    g_ptr_array_add(arr, g_strdup(*t));
  }
  g_strfreev(toks);

  g_ptr_array_sort(arr, (GCompareFunc)zcl_channel_cmp);

  for (guint i = 0; i < arr->len; i++) {
    const gchar *tok = (const gchar *)g_ptr_array_index(arr, i);
    GtkWidget *lbl = gtk_label_new(tok);
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_list_box_insert(lb, lbl, -1);
  }

  g_ptr_array_free(arr, TRUE);
}

static GtkWindow *
zcl_parent_window(UiState *st) {
  if (st && st->win && GTK_IS_WINDOW(st->win)) return GTK_WINDOW(st->win);
  if (st && st->notebook) {
    GtkWidget *toplevel = gtk_widget_get_toplevel(st->notebook);
    if (GTK_IS_WINDOW(toplevel)) return GTK_WINDOW(toplevel);
  }
  return NULL;
}

static void
zcl_whois_show_dialog(UiState *st) {
  if (!zcl_whois || !zcl_whois->nick || !*zcl_whois->nick) return;

  gchar *body = zcl_whois_render_text(zcl_whois);

  GtkWidget *dlg = gtk_dialog_new_with_buttons(
      "WHOIS",
      zcl_parent_window(st),
      GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
      "_Copy",
      1,
      "_Close",
      GTK_RESPONSE_CLOSE,
      NULL);

  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
  gtk_container_set_border_width(GTK_CONTAINER(content), 12);

  GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_box_pack_start(GTK_BOX(content), paned, TRUE, TRUE, 0);

  /* Left: channels list (scrollable). */
  GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);

  GtkWidget *ch_hdr = gtk_label_new(NULL);
  gtk_label_set_markup(GTK_LABEL(ch_hdr), "<b>Channels</b>");
  gtk_widget_set_halign(ch_hdr, GTK_ALIGN_START);
  gtk_box_pack_start(GTK_BOX(left), ch_hdr, FALSE, FALSE, 0);

  GtkWidget *ch_sw = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(ch_sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_size_request(ch_sw, 240, 420);

  GtkWidget *ch_list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(ch_list), GTK_SELECTION_NONE);
  gtk_container_add(GTK_CONTAINER(ch_sw), ch_list);
  gtk_box_pack_start(GTK_BOX(left), ch_sw, TRUE, TRUE, 0);

  zcl_whois_fill_channels_list(GTK_LIST_BOX(ch_list), zcl_whois ? zcl_whois->channels_raw : NULL);

  /* Right: formatted details text. */
  GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_size_request(sw, 600, 420);

  GtkWidget *tv = gtk_text_view_new();
  gtk_text_view_set_editable(GTK_TEXT_VIEW(tv), FALSE);
  gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(tv), FALSE);
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tv), GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(tv), TRUE);

  GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));
  gtk_text_buffer_set_text(buf, body, -1);

  gtk_container_add(GTK_CONTAINER(sw), tv);

  gtk_paned_pack1(GTK_PANED(paned), left, FALSE, FALSE);
  gtk_paned_pack2(GTK_PANED(paned), sw, TRUE, FALSE);
  gtk_paned_set_position(GTK_PANED(paned), 260);

  gtk_widget_show_all(dlg);

  gint resp = gtk_dialog_run(GTK_DIALOG(dlg));
  if (resp == 1) {
    GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gchar *all = zcl_whois_render_copy_text(zcl_whois);
          gtk_clipboard_set_text(cb, all ? all : "", -1);
          g_free(all);
  }

  gtk_widget_destroy(dlg);
  g_free(body);
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
    const gboolean is_numeric =
      (msg->command &&
       strlen(msg->command) == 3 &&
       g_ascii_isdigit((guchar)msg->command[0]) &&
       g_ascii_isdigit((guchar)msg->command[1]) &&
       g_ascii_isdigit((guchar)msg->command[2]));

    if (is_numeric) {
      // WHOIS pretty-print (common numerics). This keeps status output readable.
      const gchar *wnick = zc_irc_message_param(msg, 1);
      
/* WHOIS dialog capture. If a /WHOIS is in progress, collect numerics for that nick
 * and show a formatted dialog at 318 (end of WHOIS). */
if (zcl_whois && zcl_whois->nick && *zcl_whois->nick && wnick &&
    g_ascii_strcasecmp(wnick, zcl_whois->nick) == 0) {

  /* 301 RPL_AWAY */
  if (g_strcmp0(msg->command, "301") == 0) {
    g_free(zcl_whois->away);
    zcl_whois->away = g_strdup(msg->trailing ? msg->trailing : "");
    return;
  }

  /* 311 RPL_WHOISUSER: <me> <nick> <user> <host> * :<realname> */
  if (g_strcmp0(msg->command, "311") == 0) {
    const gchar *user = zc_irc_message_param(msg, 2);
    const gchar *host = zc_irc_message_param(msg, 3);
    g_free(zcl_whois->userhost);
    zcl_whois->userhost = g_strdup_printf("%s@%s", user ? user : "?", host ? host : "?");
    g_free(zcl_whois->realname);
    zcl_whois->realname = g_strdup(msg->trailing ? msg->trailing : "");
    return;
  }

  /* 312 RPL_WHOISSERVER: <me> <nick> <server> :<info> */
  if (g_strcmp0(msg->command, "312") == 0) {
    g_free(zcl_whois->server);
    zcl_whois->server = g_strdup(zc_irc_message_param(msg, 2));
    g_free(zcl_whois->server_info);
    zcl_whois->server_info = g_strdup(msg->trailing ? msg->trailing : "");
    return;
  }

  /* 319 RPL_WHOISCHANNELS */
  if (g_strcmp0(msg->command, "319") == 0) {
    g_free(zcl_whois->channels_raw);
    zcl_whois->channels_raw = g_strdup(msg->trailing ? msg->trailing : "");
    return;
  }

  /* 317 RPL_WHOISIDLE: <me> <nick> <idle> <signon> :... */
  if (g_strcmp0(msg->command, "317") == 0) {
    const gchar *idle = zc_irc_message_param(msg, 2);
    const gchar *signon = zc_irc_message_param(msg, 3);
    zcl_whois->idle_secs = idle ? (guint)g_ascii_strtoull(idle, NULL, 10) : 0;
    zcl_whois->signon_ts = signon ? (gint64)g_ascii_strtoll(signon, NULL, 10) : 0;
    return;
  }

  /* 313 / 320: operator-ish lines */
  if (g_strcmp0(msg->command, "313") == 0 || g_strcmp0(msg->command, "320") == 0) {
    zcl_whois->is_oper = TRUE;
    return;
  }

  /* 671: secure connection */
  if (g_strcmp0(msg->command, "671") == 0) {
    zcl_whois->is_secure = TRUE;
    return;
  }

  /* 330: account */
  if (g_strcmp0(msg->command, "330") == 0) {
    g_free(zcl_whois->account);
    zcl_whois->account = g_strdup(zc_irc_message_param(msg, 2));
    return;
  }

  /* 318 RPL_ENDOFWHOIS */
  if (g_strcmp0(msg->command, "318") == 0) {
    zcl_whois_show_dialog(st);
    zcl_whois_clear();
    return;
  }

  /* Ignore other numerics for this WHOIS. */
  return;
}

if (!wnick) wnick = zc_irc_message_param(msg, 0);
      if (!wnick) wnick = "";

      if (g_strcmp0(msg->command, "311") == 0) {
        const gchar *user = zc_irc_message_param(msg, 2);
        const gchar *host = zc_irc_message_param(msg, 3);
        const gchar *real = msg->trailing ? msg->trailing : "";
        gchar *line = g_strdup_printf("WHOIS %s: %s@%s (%s)", wnick, user ? user : "?", host ? host : "?", real);
        append_server_line(st, line);
        g_free(line);
        return;
      }

      if (g_strcmp0(msg->command, "312") == 0) {
        const gchar *srv = zc_irc_message_param(msg, 2);
        const gchar *info = msg->trailing ? msg->trailing : "";
        gchar *line = g_strdup_printf("WHOIS %s: server %s (%s)", wnick, srv ? srv : "?", info);
        append_server_line(st, line);
        g_free(line);
        return;
      }

      if (g_strcmp0(msg->command, "313") == 0) {
        gchar *line = g_strdup_printf("WHOIS %s: IRC operator", wnick);
        append_server_line(st, line);
        g_free(line);
        return;
      }

      if (g_strcmp0(msg->command, "317") == 0) {
        const gchar *idle_s = zc_irc_message_param(msg, 2);
        const gchar *signon_s = zc_irc_message_param(msg, 3);
        gint64 signon = signon_s ? g_ascii_strtoll(signon_s, NULL, 10) : 0;
        GDateTime *dt = (signon > 0) ? g_date_time_new_from_unix_local(signon) : NULL;
        gchar *when = dt ? g_date_time_format(dt, "%Y-%m-%d %H:%M:%S") : NULL;
        gchar *line = g_strdup_printf("WHOIS %s: idle %ss, signon %s", wnick, idle_s ? idle_s : "?", when ? when : "?");
        append_server_line(st, line);
        g_free(line);
        g_free(when);
        if (dt) g_date_time_unref(dt);
        return;
      }

      if (g_strcmp0(msg->command, "319") == 0) {
        const gchar *chans = msg->trailing ? msg->trailing : "";
        gchar *line = g_strdup_printf("WHOIS %s: channels %s", wnick, chans);
        append_server_line(st, line);
        g_free(line);
        return;
      }

      if (g_strcmp0(msg->command, "330") == 0) {
        const gchar *acct = zc_irc_message_param(msg, 2);
        gchar *line = g_strdup_printf("WHOIS %s: account %s", wnick, acct ? acct : "?");
        append_server_line(st, line);
        g_free(line);
        return;
      }

      if (g_strcmp0(msg->command, "671") == 0) {
        gchar *line = g_strdup_printf("WHOIS %s: secure connection", wnick);
        append_server_line(st, line);
        g_free(line);
        return;
      }

      if (g_strcmp0(msg->command, "378") == 0) {
        const gchar *info = msg->trailing ? msg->trailing : "";
        gchar *line = g_strdup_printf("WHOIS %s: %s", wnick, info);
        append_server_line(st, line);
        g_free(line);
        return;
      }

      if (g_strcmp0(msg->command, "318") == 0) {
        gchar *line = g_strdup_printf("WHOIS %s: end", wnick);
        append_server_line(st, line);
        g_free(line);
        return;
      }

      // Cleaner default numeric output: prefer the human text.
      if (msg->trailing && *msg->trailing) {
        append_server_line(st, msg->trailing);
        return;
      }
    }

    // Fallback (non-numeric or no trailing)
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

static gboolean
zcl_send_raw_line(UiState *st, ChatPage *page, const gchar *raw) {
  if (!raw || !*raw) return FALSE;
  if (!st || !st->client) return FALSE;
  if (!zc_client_is_connected(st->client)) {
    if (page) chat_page_append(page, "Not connected.");
    return FALSE;
  }

  GError *err = NULL;
  gboolean ok = zc_client_send_raw(st->client, raw, &err);
  if (!ok && page) {
    chat_page_append_fmt(page, "Send failed: %s", err ? err->message : "unknown error");
  }
  g_clear_error(&err);
  return ok;
}

static gchar *
zcl_take_token(gchar **s) {
  if (!s || !*s) return NULL;
  gchar *p = *s;
  g_strstrip(p);
  if (!*p) return NULL;

  gchar *sp = strchr(p, ' ');
  if (sp) {
    *sp = '\0';
    sp++;
    g_strstrip(sp);
    *s = sp;
  } else {
    *s = p + strlen(p);
  }
  return p;
}

static void
run_command(UiState *st, const gchar *target, const gchar *line) {
  // Clean slash-command -> raw-line dispatch.
  if (!st || !line) return;

  const gchar *effective_target = target && *target ? target : "status";
  ChatPage *page = get_or_create_page(st, effective_target);

  // "//foo" sends literal "/foo" as a message (common IRC client behavior).
  if (line[0] == '/' && line[1] == '/') {
    const gchar *msg = line + 1;

    if (g_strcmp0(effective_target, "status") == 0) {
      zcl_send_raw_line(st, page, msg);
      return;
    }

	    if (!st->client || !zc_client_is_connected(st->client)) {
	      chat_page_append(page, "Not connected.");
      return;
    }

	    GError *err = NULL;
	    zc_client_privmsg(st->client, effective_target, msg, &err);
    if (err) {
      chat_page_append_fmt(page, "Send failed: %s", err->message);
      g_clear_error(&err);
    }
    return;
  }

  // Non-command text: message in channels/queries, raw in status.
  if (line[0] != '/') {
    if (g_strcmp0(effective_target, "status") == 0) {
      zcl_send_raw_line(st, page, line);
      return;
    }

	    if (!st->client || !zc_client_is_connected(st->client)) {
	      chat_page_append(page, "Not connected.");
      return;
    }

	    GError *err = NULL;
	    zc_client_privmsg(st->client, effective_target, line, &err);
    if (err) {
      chat_page_append_fmt(page, "Send failed: %s", err->message);
      g_clear_error(&err);
    }
    return;
  }

  // Parse "/cmd rest..."
  gchar *tmp = g_strdup(line + 1);
  g_strstrip(tmp);
  if (!tmp[0]) { g_free(tmp); return; }

  gchar *rest = strchr(tmp, ' ');
  if (rest) {
    *rest = '\0';
    rest++;
    g_strstrip(rest);
  } else {
    rest = (gchar *)"";
  }

  gchar *cmd = tmp;
  for (gchar *p = cmd; *p; p++) *p = (gchar)g_ascii_tolower(*p);

  typedef enum {
    ZCL_CMD_RAW_REST,
    ZCL_CMD_SIMPLE_REST,        // "CMD rest"
    ZCL_CMD_MSG,                // /msg nick text => PRIVMSG nick :text
    ZCL_CMD_NOTICE,             // /notice nick text
    ZCL_CMD_ACTION,             // /me text => CTCP ACTION to current target
    ZCL_CMD_JOIN,               // /join #chan1,#chan2 [keys]
    ZCL_CMD_PART,               // /part [chan] [reason]
    ZCL_CMD_TOPIC,              // /topic [chan] [topic]
    ZCL_CMD_WHOIS,              // /whois nick
    ZCL_CMD_NAMES,              // /names [chan]
    ZCL_CMD_WHO,                // /who [mask]
    ZCL_CMD_LIST,               // /list [args]
    ZCL_CMD_MODE,               // /mode ...
    ZCL_CMD_KICK,               // /kick [chan] nick [reason]
    ZCL_CMD_INVITE,             // /invite nick [chan]
    ZCL_CMD_AWAY,               // /away [msg]
    ZCL_CMD_QUERY_UI,           // /query nick
    ZCL_CMD_CLOSE_UI,           // /close
    ZCL_CMD_SAY_UI,             // /say text (send as message, not raw)
  } ZclCmdRule;

  typedef struct {
    const gchar *name;
    ZclCmdRule rule;
    const gchar *irc_cmd; // used by SIMPLE_REST
  } ZclCmdSpec;

  static const ZclCmdSpec table[] = {
    {"raw",    ZCL_CMD_RAW_REST,    NULL},
    {"quote",  ZCL_CMD_RAW_REST,    NULL},

    {"join",   ZCL_CMD_JOIN,        "JOIN"},
    {"j",      ZCL_CMD_JOIN,        "JOIN"},
    {"part",   ZCL_CMD_PART,        "PART"},
    {"leave",  ZCL_CMD_PART,        "PART"},
    {"nick",   ZCL_CMD_SIMPLE_REST, "NICK"},
    {"quit",   ZCL_CMD_SIMPLE_REST, "QUIT"},
    {"disconnect", ZCL_CMD_SIMPLE_REST, "QUIT"},
    {"away",   ZCL_CMD_AWAY,        "AWAY"},
    {"topic",  ZCL_CMD_TOPIC,       "TOPIC"},
    {"mode",   ZCL_CMD_MODE,        "MODE"},
    {"kick",   ZCL_CMD_KICK,        "KICK"},
    {"invite", ZCL_CMD_INVITE,      "INVITE"},

    {"msg",    ZCL_CMD_MSG,         NULL},
    {"m",      ZCL_CMD_MSG,         NULL},
    {"notice", ZCL_CMD_NOTICE,      NULL},
    {"me",     ZCL_CMD_ACTION,      NULL},
    {"action", ZCL_CMD_ACTION,      NULL},

    {"query",  ZCL_CMD_QUERY_UI,    NULL},
    {"q",      ZCL_CMD_QUERY_UI,    NULL},
    {"close",  ZCL_CMD_CLOSE_UI,    NULL},
    {"say",    ZCL_CMD_SAY_UI,      NULL},

    {"whois",  ZCL_CMD_WHOIS,       "WHOIS"},
    {"names",  ZCL_CMD_NAMES,       "NAMES"},
    {"who",    ZCL_CMD_WHO,         "WHO"},
    {"list",   ZCL_CMD_LIST,        "LIST"},
  };

  const ZclCmdSpec *spec = NULL;
  for (guint i = 0; i < G_N_ELEMENTS(table); i++) {
    if (g_strcmp0(cmd, table[i].name) == 0) { spec = &table[i]; break; }
  }

  // Unknown: pass through as raw "CMD rest" (uppercased).
  if (!spec) {
    gchar *uc = g_ascii_strup(cmd, -1);
    gchar *raw = (rest && *rest) ? g_strdup_printf("%s %s", uc, rest) : g_strdup(uc);
    zcl_send_raw_line(st, page, raw);
    g_free(raw);
    g_free(uc);
    g_free(tmp);
    return;
  }

  // UI-only commands can run even while disconnected.
  if (spec->rule == ZCL_CMD_QUERY_UI) {
    gchar *r = rest;
    gchar *nick = zcl_take_token(&r);
    if (nick && *nick) zcl_ui_open_query(st, nick);
    else chat_page_append(page, "Usage: /query <nick>");
    g_free(tmp);
    return;
  }

  if (spec->rule == ZCL_CMD_CLOSE_UI) {
    if (st->notebook) {
      GtkWidget *child = gtk_notebook_get_nth_page(GTK_NOTEBOOK(st->notebook),
        gtk_notebook_get_current_page(GTK_NOTEBOOK(st->notebook)));
      const gchar *cur = child ? zcl_target_for_child(st, child) : NULL;
      if (cur && g_strcmp0(cur, "status") != 0) zcl_ui_close_target(st, cur, TRUE);
    }
    g_free(tmp);
    return;
  }

  if (spec->rule == ZCL_CMD_SAY_UI) {
    if (!rest || !*rest) { g_free(tmp); return; }
    if (g_strcmp0(effective_target, "status") == 0) {
      chat_page_append(page, "No target here. Switch to a channel/query tab.");
      g_free(tmp);
      return;
    }
    if (!st->client || !zc_client_is_connected(st->client)) {
      chat_page_append(page, "Not connected.");
      g_free(tmp);
      return;
    }
    GError *err = NULL;
    zc_client_privmsg(st->client, effective_target, rest, &err);
    if (err) {
      chat_page_append_fmt(page, "Send failed: %s", err->message);
      g_clear_error(&err);
    }
    g_free(tmp);
    return;
  }

  // Everything below this line requires an IRC connection.
  if (!st->client || !zc_client_is_connected(st->client)) {
    chat_page_append(page, "Not connected.");
    g_free(tmp);
    return;
  }

  switch (spec->rule) {

    /* These are handled in the UI-only section above; keep them here to
     * satisfy -Wswitch when new enum values are added. */
    case ZCL_CMD_QUERY_UI:
    case ZCL_CMD_CLOSE_UI:
    case ZCL_CMD_SAY_UI:
      return;

    case ZCL_CMD_RAW_REST: {
      if (rest && *rest) zcl_send_raw_line(st, page, rest);
      break;
    }

    case ZCL_CMD_SIMPLE_REST: {
      if (rest && *rest) {
        gchar *raw = g_strdup_printf("%s %s", spec->irc_cmd, rest);
        zcl_send_raw_line(st, page, raw);
        g_free(raw);
      } else {
        zcl_send_raw_line(st, page, spec->irc_cmd);
      }
      break;
    }

    case ZCL_CMD_JOIN: {
      if (!rest || !*rest) { chat_page_append(page, "Usage: /join <#channel>[,<#channel>]"); break; }
      gchar *raw = g_strdup_printf("JOIN %s", rest);
      zcl_send_raw_line(st, page, raw);
      g_free(raw);
      break;
    }

    case ZCL_CMD_PART: {
      gchar *r = rest;
      gchar *a = zcl_take_token(&r);
      const gchar *chan = NULL;
      const gchar *reason = NULL;

      if (a && is_channel_name(a)) {
        chan = a;
        reason = (r && *r) ? r : NULL;
      } else {
        chan = effective_target;
        reason = (a && *a) ? rest : NULL;
      }

      if (!chan || g_strcmp0(chan, "status") == 0) {
        chat_page_append(page, "Usage: /part <#channel> [reason]");
        break;
      }

      gchar *raw = NULL;
      if (reason && *reason) raw = g_strdup_printf("PART %s :%s", chan, reason);
      else raw = g_strdup_printf("PART %s", chan);

      zcl_send_raw_line(st, page, raw);
      g_free(raw);
      break;
    }

    case ZCL_CMD_TOPIC: {
      gchar *r = rest;
      gchar *a = zcl_take_token(&r);
      const gchar *chan = NULL;
      const gchar *topic = NULL;

      if (a && is_channel_name(a)) {
        chan = a;
        topic = (r && *r) ? r : NULL;
      } else {
        chan = effective_target;
        topic = (rest && *rest) ? rest : NULL;
      }

      if (!chan || g_strcmp0(chan, "status") == 0) {
        chat_page_append(page, "Usage: /topic <#channel> [topic]");
        break;
      }

      gchar *raw = NULL;
      if (topic && *topic) raw = g_strdup_printf("TOPIC %s :%s", chan, topic);
      else raw = g_strdup_printf("TOPIC %s", chan);

      zcl_send_raw_line(st, page, raw);
      g_free(raw);
      break;
    }

    case ZCL_CMD_WHOIS: {
      gchar *r = rest;
      gchar *nick = zcl_take_token(&r);
      if (!nick || !*nick) { chat_page_append(page, "Usage: /whois <nick>"); break; }
      gchar *raw = g_strdup_printf("WHOIS %s", nick);
      gboolean ok = zcl_send_raw_line(st, page, raw);
      if (ok) zcl_whois_begin(nick);
      g_free(raw);
      break;
    }

    case ZCL_CMD_NAMES: {
      const gchar *chan = (rest && *rest) ? rest : effective_target;
      if (!chan || g_strcmp0(chan, "status") == 0) { chat_page_append(page, "Usage: /names <#channel>"); break; }
      gchar *raw = g_strdup_printf("NAMES %s", chan);
      zcl_send_raw_line(st, page, raw);
      g_free(raw);
      break;
    }

    case ZCL_CMD_WHO: {
      if (rest && *rest) {
        gchar *raw = g_strdup_printf("WHO %s", rest);
        zcl_send_raw_line(st, page, raw);
        g_free(raw);
      } else {
        const gchar *chan = effective_target;
        if (!chan || g_strcmp0(chan, "status") == 0) { chat_page_append(page, "Usage: /who <mask>"); break; }
        gchar *raw = g_strdup_printf("WHO %s", chan);
        zcl_send_raw_line(st, page, raw);
        g_free(raw);
      }
      break;
    }

    case ZCL_CMD_LIST: {
      if (rest && *rest) {
        gchar *raw = g_strdup_printf("LIST %s", rest);
        zcl_send_raw_line(st, page, raw);
        g_free(raw);
      } else {
        zcl_send_raw_line(st, page, "LIST");
      }
      break;
    }

    case ZCL_CMD_MODE: {
      if (!rest || !*rest) { chat_page_append(page, "Usage: /mode <target> [modes]"); break; }
      gchar *raw = g_strdup_printf("MODE %s", rest);
      zcl_send_raw_line(st, page, raw);
      g_free(raw);
      break;
    }

    case ZCL_CMD_KICK: {
      gchar *r = rest;
      gchar *a = zcl_take_token(&r);
      gchar *b = zcl_take_token(&r);

      const gchar *chan = NULL;
      const gchar *nick = NULL;
      const gchar *reason = (r && *r) ? r : NULL;

      if (a && is_channel_name(a)) { chan = a; nick = b; }
      else {
        chan = effective_target;
        nick = a;
        if (b && *b) reason = b;
        if (r && *r) reason = r;
      }

      if (!chan || g_strcmp0(chan, "status") == 0 || !nick || !*nick) {
        chat_page_append(page, "Usage: /kick [#channel] <nick> [reason]");
        break;
      }

      gchar *raw = NULL;
      if (reason && *reason) raw = g_strdup_printf("KICK %s %s :%s", chan, nick, reason);
      else raw = g_strdup_printf("KICK %s %s", chan, nick);

      zcl_send_raw_line(st, page, raw);
      g_free(raw);
      break;
    }

    case ZCL_CMD_INVITE: {
      gchar *r = rest;
      gchar *nick = zcl_take_token(&r);
      const gchar *chan = (r && *r) ? r : effective_target;

      if (!nick || !*nick || !chan || g_strcmp0(chan, "status") == 0) {
        chat_page_append(page, "Usage: /invite <nick> [#channel]");
        break;
      }

      gchar *raw = g_strdup_printf("INVITE %s %s", nick, chan);
      zcl_send_raw_line(st, page, raw);
      g_free(raw);
      break;
    }

    case ZCL_CMD_AWAY: {
      if (rest && *rest) {
        gchar *raw = g_strdup_printf("AWAY :%s", rest);
        zcl_send_raw_line(st, page, raw);
        g_free(raw);
      } else {
        zcl_send_raw_line(st, page, "AWAY");
      }
      break;
    }

    case ZCL_CMD_MSG:
    case ZCL_CMD_NOTICE: {
      gchar *r = rest;
      gchar *nick = zcl_take_token(&r);
      const gchar *msg = (r && *r) ? r : NULL;

      if (!nick || !*nick || !msg || !*msg) {
        chat_page_append(page, spec->rule == ZCL_CMD_NOTICE ? "Usage: /notice <nick> <text>" : "Usage: /msg <nick> <text>");
        break;
      }

      // Open a query tab for convenience when messaging someone directly.
      zcl_ui_open_query(st, nick);

      gchar *raw = NULL;
      if (spec->rule == ZCL_CMD_NOTICE) raw = g_strdup_printf("NOTICE %s :%s", nick, msg);
      else raw = g_strdup_printf("PRIVMSG %s :%s", nick, msg);

      zcl_send_raw_line(st, page, raw);
      g_free(raw);
      break;
    }

    case ZCL_CMD_ACTION: {
      if (!rest || !*rest) { chat_page_append(page, "Usage: /me <action>"); break; }
      if (g_strcmp0(effective_target, "status") == 0) { chat_page_append(page, "No target here. Switch to a channel/query tab."); break; }

      gchar *raw = g_strdup_printf("PRIVMSG %s :\001ACTION %s\001", effective_target, rest);
      zcl_send_raw_line(st, page, raw);
      g_free(raw);
      break;
    }
  }

  g_free(tmp);
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
  g_signal_connect(mi_dm, "activate", G_CALLBACK(on_userlist_menu_send_dm), st);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi_dm);

  GtkWidget *mi_whois = gtk_menu_item_new_with_label("WHOIS");
  g_object_set_data_full(G_OBJECT(mi_whois), "zc-nick", g_strdup(nick), g_free);
  g_signal_connect(mi_whois, "activate", G_CALLBACK(on_userlist_menu_whois), st);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi_whois);

  GtkWidget *sep = gtk_separator_menu_item_new();
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), sep);

  GtkWidget *mi_copy = gtk_menu_item_new_with_label("Copy Nick");
  g_object_set_data_full(G_OBJECT(mi_copy), "zc-nick", g_strdup(nick), g_free);
  g_signal_connect(mi_copy, "activate", G_CALLBACK(on_userlist_menu_copy), st);
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
  UiState *st = user_data;
  if (!st) return;

  const gchar *nick = g_object_get_data(G_OBJECT(mi), "zc-nick");
  if (!nick || !*nick) return;

  while (*nick && strchr("@+%&~", *nick)) nick++;
  if (!*nick) return;

  zcl_ui_open_query(st, nick);
}


static void
on_userlist_menu_whois(GtkMenuItem *mi, gpointer user_data)
{
  UiState *st = user_data;
  if (!st || !st->client) return;

  const gchar *nick = g_object_get_data(G_OBJECT(mi), "zc-nick");
  if (!nick || !*nick) return;

  /* Userlist rows can carry a mode prefix in the displayed text. WHOIS needs the bare nick. */
  while (*nick && strchr("@+%&~", *nick)) nick++;
  if (!*nick) return;

  ChatPage *status = get_or_create_page(st, "status");

  if (!zc_client_is_connected(st->client)) {
    chat_page_append_fmt(status, "Not connected.");
    return;
  }

  zcl_whois_begin(nick);

  gchar *line = g_strdup_printf("WHOIS %s", nick);
  GError *error = NULL;
  if (!zc_client_send_raw(st->client, line, &error)) {
    zcl_whois_clear();
    chat_page_append_fmt(status, "WHOIS failed: %s", error ? error->message : "unknown error");
    if (error) g_error_free(error);
    g_free(line);
    return;
  }

  chat_page_append_fmt(status, "→ WHOIS %s", nick);
  g_free(line);
}


static void
on_userlist_menu_copy(GtkMenuItem *mi, gpointer user_data)
{
  UiState *st = user_data;
  if (!st) return;

  const gchar *nick = g_object_get_data(G_OBJECT(mi), "zc-nick");
  if (!nick || !*nick) return;

  while (*nick && strchr("@+%&~", *nick)) nick++;
  if (!*nick) return;

  GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
  gtk_clipboard_set_text(cb, nick, -1);

  ChatPage *status = get_or_create_page(st, "status");
  chat_page_append_fmt(status, "Copied: %s", nick);
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
  const gchar *direct = g_object_get_data(G_OBJECT(child), "zcl-target");
  if (direct && *direct) return direct;
  ChatPage *p = zcl_page_for_child(st, child);
  return p ? chat_page_get_target(p) : NULL;
}

static void
zcl_notebook_apply_close_button(UiState *st, GtkWidget *child) {
  if (!st || !child) return;
  GtkNotebook *nb = GTK_NOTEBOOK(st->notebook);

  const gchar *target = zcl_target_for_child(st, child);
  if (!target || g_strcmp0(target, "status") == 0) return; // don't close status

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
  gtk_label_set_width_chars(GTK_LABEL(lbl), 12);
  gtk_widget_set_halign(lbl, GTK_ALIGN_START);

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
  /* Raw protocol spam makes /WHOIS (and everything else) unreadable. Keep it off. */
  /* g_signal_connect(st->client, "raw-line", G_CALLBACK(on_client_raw_line), st); */
  g_signal_connect(st->client, "irc-message", G_CALLBACK(on_client_irc_message), st);

  g_object_set_data_full(G_OBJECT(st->win), "zc-state", st, (GDestroyNotify)ui_state_free);

  gtk_widget_show_all(st->win);
  return st->win;
}

