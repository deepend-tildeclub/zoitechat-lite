#include "chat_page.h"

#include <stdarg.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <pango/pango.h>

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

/* -------------------------------------------------------------------------
 * ANSI SGR (\x1b[...m) support for chat windows.
 *
 * Why? Because servers/bouncers/scripts sometimes send colored output and
 * “plain text only” is a choice best left to 1988.
 *
 * Scope: SGR 'm' codes (basic/bright 8 colors, 256-color, truecolor),
 * plus bold + underline + reverse.
 * Unknown sequences are skipped (not rendered verbatim).
 * ------------------------------------------------------------------------- */

typedef struct {
  gboolean bold;
  gboolean underline;
  gboolean reverse;

  gboolean fg_set;
  gboolean bg_set;
  GdkRGBA fg;
  GdkRGBA bg;
} ZclAnsiState;

static void
ansi_state_reset(ZclAnsiState *st) {
  st->bold = FALSE;
  st->underline = FALSE;
  st->reverse = FALSE;
  st->fg_set = FALSE;
  st->bg_set = FALSE;
  /* fg/bg values ignored unless *_set is TRUE */
}

static void
ansi_rgba_set_u8(GdkRGBA *c, gint r, gint g, gint b) {
  c->red = (gdouble)r / 255.0;
  c->green = (gdouble)g / 255.0;
  c->blue = (gdouble)b / 255.0;
  c->alpha = 1.0;
}

static void
ansi_color_from_8(gint idx, gboolean bright, GdkRGBA *out) {
  /* Close enough to common terminal palettes. */
  static const gint normal[8][3] = {
    {0, 0, 0},       /* black */
    {205, 49, 49},   /* red */
    {13, 188, 121},  /* green */
    {229, 229, 16},  /* yellow */
    {36, 114, 200},  /* blue */
    {188, 63, 188},  /* magenta */
    {17, 168, 205},  /* cyan */
    {229, 229, 229}, /* white */
  };
  static const gint brightc[8][3] = {
    {102, 102, 102},
    {241, 76, 76},
    {35, 209, 139},
    {245, 245, 67},
    {59, 142, 234},
    {214, 112, 214},
    {41, 184, 219},
    {255, 255, 255},
  };

  if (idx < 0) idx = 0;
  if (idx > 7) idx = 7;
  const gint (*pal)[3] = bright ? brightc : normal;
  ansi_rgba_set_u8(out, pal[idx][0], pal[idx][1], pal[idx][2]);
}

static void
ansi_color_from_256(gint n, GdkRGBA *out) {
  if (n < 0) n = 0;
  if (n > 255) n = 255;

  if (n < 8) {
    ansi_color_from_8(n, FALSE, out);
    return;
  }
  if (n < 16) {
    ansi_color_from_8(n - 8, TRUE, out);
    return;
  }

  if (n >= 16 && n <= 231) {
    gint idx = n - 16;
    gint r = idx / 36;
    gint g = (idx / 6) % 6;
    gint b = idx % 6;

    /* xterm cube: 0, 95, 135, 175, 215, 255 */
    gint rr = (r == 0) ? 0 : (r * 40 + 55);
    gint gg = (g == 0) ? 0 : (g * 40 + 55);
    gint bb = (b == 0) ? 0 : (b * 40 + 55);

    ansi_rgba_set_u8(out, rr, gg, bb);
    return;
  }

  /* grayscale 232..255 */
  gint g = 8 + (n - 232) * 10;
  ansi_rgba_set_u8(out, g, g, g);
}

static void
ansi_effective_state(const ZclAnsiState *in, ZclAnsiState *out_eff) {
  *out_eff = *in;

  if (out_eff->reverse) {
    /* Swap fg/bg semantics. If only one side is set, treat it as swapping
     * with “default”, so the set color becomes the opposite side. */
    const gboolean fg_set = out_eff->fg_set;
    const gboolean bg_set = out_eff->bg_set;
    const GdkRGBA fg = out_eff->fg;
    const GdkRGBA bg = out_eff->bg;

    out_eff->fg_set = bg_set;
    out_eff->bg_set = fg_set;
    out_eff->fg = bg;
    out_eff->bg = fg;
    out_eff->reverse = FALSE;
  }
}

static void
ansi_rgba_to_hex(const GdkRGBA *c, gchar out_hex[7]) {
  gint r = (gint)(c->red * 255.0 + 0.5);
  gint g = (gint)(c->green * 255.0 + 0.5);
  gint b = (gint)(c->blue * 255.0 + 0.5);

  r = CLAMP(r, 0, 255);
  g = CLAMP(g, 0, 255);
  b = CLAMP(b, 0, 255);

  g_snprintf(out_hex, 7, "%02X%02X%02X", r, g, b);
}

static gchar *

ansi_tag_name_for(const ZclAnsiState *st) {
  gchar fhex[7];
  gchar bhex[7];
  const gchar *f = "none";
  const gchar *b = "none";

  if (st->fg_set) { ansi_rgba_to_hex(&st->fg, fhex); f = fhex; }
  if (st->bg_set) { ansi_rgba_to_hex(&st->bg, bhex); b = bhex; }

  return g_strdup_printf("zc-ansi-b%d-u%d-f%s-bg%s", st->bold ? 1 : 0, st->underline ? 1 : 0, f, b);
}

static GtkTextTag *
ansi_ensure_tag(GtkTextBuffer *buf, const ZclAnsiState *st_eff) {
  gchar *name = ansi_tag_name_for(st_eff);

  GtkTextTagTable *tbl = gtk_text_buffer_get_tag_table(buf);
  GtkTextTag *tag = gtk_text_tag_table_lookup(tbl, name);
  if (tag) {
    g_free(name);
    return tag;
  }

  tag = gtk_text_buffer_create_tag(buf, name, NULL);

  if (st_eff->fg_set) {
    g_object_set(tag, "foreground-rgba", &st_eff->fg, NULL);
  }
  if (st_eff->bg_set) {
    g_object_set(tag, "background-rgba", &st_eff->bg, NULL);
  }
  if (st_eff->bold) {
    g_object_set(tag, "weight", PANGO_WEIGHT_BOLD, NULL);
  }
  if (st_eff->underline) {
    g_object_set(tag, "underline", PANGO_UNDERLINE_SINGLE, NULL);
  }

  g_free(name);
  return tag;
}

static void
ansi_apply_sgr(ZclAnsiState *st, const gint *params, gsize n_params) {
  if (n_params == 0) {
    ansi_state_reset(st);
    return;
  }

  for (gsize i = 0; i < n_params; i++) {
    const gint p = params[i];

    if (p == 0) {
      ansi_state_reset(st);
      continue;
    }

    switch (p) {
      case 1:  st->bold = TRUE; break;
      case 22: st->bold = FALSE; break;
      case 4:  st->underline = TRUE; break;
      case 24: st->underline = FALSE; break;
      case 7:  st->reverse = TRUE; break;
      case 27: st->reverse = FALSE; break;

      case 39: st->fg_set = FALSE; break;
      case 49: st->bg_set = FALSE; break;

      default:
        break;
    }

    /* Basic foreground/background */
    if (p >= 30 && p <= 37) {
      st->fg_set = TRUE;
      ansi_color_from_8(p - 30, FALSE, &st->fg);
      continue;
    }
    if (p >= 90 && p <= 97) {
      st->fg_set = TRUE;
      ansi_color_from_8(p - 90, TRUE, &st->fg);
      continue;
    }
    if (p >= 40 && p <= 47) {
      st->bg_set = TRUE;
      ansi_color_from_8(p - 40, FALSE, &st->bg);
      continue;
    }
    if (p >= 100 && p <= 107) {
      st->bg_set = TRUE;
      ansi_color_from_8(p - 100, TRUE, &st->bg);
      continue;
    }

    /* 256-color / truecolor */
    if (p == 38 || p == 48) {
      const gboolean is_fg = (p == 38);

      if (i + 1 >= n_params) continue;
      const gint mode = params[i + 1];

      if (mode == 5) {
        if (i + 2 >= n_params) { i += 1; continue; }
        const gint idx = params[i + 2];
        if (is_fg) {
          st->fg_set = TRUE;
          ansi_color_from_256(idx, &st->fg);
        } else {
          st->bg_set = TRUE;
          ansi_color_from_256(idx, &st->bg);
        }
        i += 2;
        continue;
      }

      if (mode == 2) {
        if (i + 4 >= n_params) { i += 1; continue; }
        const gint r = params[i + 2];
        const gint g = params[i + 3];
        const gint b = params[i + 4];
        if (is_fg) {
          st->fg_set = TRUE;
          ansi_rgba_set_u8(&st->fg, CLAMP(r, 0, 255), CLAMP(g, 0, 255), CLAMP(b, 0, 255));
        } else {
          st->bg_set = TRUE;
          ansi_rgba_set_u8(&st->bg, CLAMP(r, 0, 255), CLAMP(g, 0, 255), CLAMP(b, 0, 255));
        }
        i += 4;
        continue;
      }

      /* Unknown 38/48 mode, skip it. */
    }
  }
}

static void
buffer_insert_run(GtkTextBuffer *buf, GtkTextIter *iter, const gchar *text, gssize len, const ZclAnsiState *st) {
  ZclAnsiState eff;
  ansi_effective_state(st, &eff);

  const gboolean styled = eff.bold || eff.underline || eff.fg_set || eff.bg_set;
  if (!styled) {
    gtk_text_buffer_insert(buf, iter, text, len);
    return;
  }

  GtkTextTag *tag = ansi_ensure_tag(buf, &eff);
  gtk_text_buffer_insert_with_tags(buf, iter, text, len, tag, NULL);
}

static gboolean
mirc_color_to_rgba(gint idx, GdkRGBA *out) {
  /* mIRC 0-15 palette (HexChat/XChat-like). */
  static const gchar *hex[16] = {
    "#FFFFFF", /* 0 white */
    "#000000", /* 1 black */
    "#00007F", /* 2 navy */
    "#009300", /* 3 green */
    "#FF0000", /* 4 red */
    "#7F0000", /* 5 maroon */
    "#9C009C", /* 6 purple */
    "#FC7F00", /* 7 orange */
    "#FFFF00", /* 8 yellow */
    "#00FC00", /* 9 light green */
    "#009393", /* 10 teal */
    "#00FFFF", /* 11 light cyan */
    "#0000FC", /* 12 light blue */
    "#FF00FF", /* 13 pink */
    "#7F7F7F", /* 14 grey */
    "#D2D2D2", /* 15 light grey */
  };

  if (!out) return FALSE;
  if (idx < 0 || idx > 15) return FALSE;
  return gdk_rgba_parse(out, hex[idx]);
}

static gboolean
mirc_read_1or2_digits(const gchar *p, gint *out_val, gint *out_used) {
  if (!p || !out_val || !out_used) return FALSE;
  if (!g_ascii_isdigit((guchar)p[0])) return FALSE;

  gint val = (gint)(p[0] - '0');
  gint used = 1;

  if (g_ascii_isdigit((guchar)p[1])) {
    val = (val * 10) + (gint)(p[1] - '0');
    used = 2;
  }

  *out_val = val;
  *out_used = used;
  return TRUE;
}


static gsize
ansi_parse_params(const gchar *start, const gchar *end, gint *out, gsize out_cap) {
  if (!start || !end || start >= end || !out || out_cap == 0) return 0;

  gsize n = 0;
  const gchar *p = start;

  while (p < end) {
    const gchar *seg_end = p;
    while (seg_end < end && *seg_end != ';') seg_end++;

    gint val = 0;
    gboolean neg = FALSE;
    gboolean have_digit = FALSE;

    const gchar *t = p;
    if (t < seg_end && *t == '-') { neg = TRUE; t++; }

    for (; t < seg_end; t++) {
      if (g_ascii_isdigit((guchar)*t)) {
        have_digit = TRUE;
        val = (val * 10) + (gint)(*t - '0');
        /* avoid silly overflow on malicious input */
        if (val > 1000000) break;
      }
    }

    if (!have_digit) val = 0;
    if (neg) val = -val;

    if (n < out_cap) out[n++] = val;

    p = seg_end;
    if (p < end && *p == ';') p++;
  }

  return n;
}

static void
buffer_insert_ansi(GtkTextBuffer *buf, GtkTextIter *iter, const gchar *s) {
  /* Supports both:
   *  - ANSI SGR:   ESC [ ... m
   *  - IRC/mIRC:   ^B (0x02) bold, ^_ (0x1F) underline, ^V (0x16) reverse,
   *               ^O (0x0F) reset, ^C (0x03) colors (fg[,bg])
   */
  ZclAnsiState st;
  ansi_state_reset(&st);

  const gchar *p = s;
  const gchar *run = p;

  while (*p) {
    const guchar c = (guchar)p[0];

    /* ANSI CSI */
    if (c == 0x1b && p[1] == '[') {
      if (p > run) buffer_insert_run(buf, iter, run, (gssize)(p - run), &st);

      const gchar *q = p + 2;
      const gchar *final = q;
      while (*final && !((guchar)*final >= 0x40 && (guchar)*final <= 0x7E)) final++;

      if (!*final) {
        buffer_insert_run(buf, iter, p, -1, &st);
        return;
      }

      const gchar fin = *final;
      if (fin == 'm') {
        gint params[64];
        const gsize n_params = ansi_parse_params(q, final, params, G_N_ELEMENTS(params));
        ansi_apply_sgr(&st, params, n_params);
      } else if (fin == 'K') {
        /* Erase-in-line: ignore safely. */
      }
      p = final + 1;
      run = p;
      continue;
    }

    /* IRC/mIRC formatting controls */
    if (c == 0x02 /* bold */ ||
        c == 0x1F /* underline */ ||
        c == 0x16 /* reverse */ ||
        c == 0x0F /* reset */ ||
        c == 0x03 /* color */) {

      if (p > run) buffer_insert_run(buf, iter, run, (gssize)(p - run), &st);

      if (c == 0x02) {
        st.bold = !st.bold;
        p++;
      } else if (c == 0x1F) {
        st.underline = !st.underline;
        p++;
      } else if (c == 0x16) {
        st.reverse = !st.reverse;
        p++;
      } else if (c == 0x0F) {
        ansi_state_reset(&st);
        p++;
      } else if (c == 0x03) {
        /* ^C [fg] [,bg]  (fg/bg are 1-2 digits). If no digits, reset colors. */
        p++;

        gint fg = -1, bg = -1;
        gint used = 0;

        gint val = 0, n = 0;
        gboolean has_fg = mirc_read_1or2_digits(p, &val, &n);
        if (!has_fg) {
          st.fg_set = FALSE;
          st.bg_set = FALSE;
        } else {
          fg = val;
          used += n;
          p += n;

          GdkRGBA rgba;
          if (mirc_color_to_rgba(fg, &rgba)) {
            st.fg = rgba;
            st.fg_set = TRUE;
          } else {
            st.fg_set = FALSE;
          }

          if (*p == ',') {
            p++;
            used++;

            gboolean has_bg = mirc_read_1or2_digits(p, &val, &n);
            if (has_bg) {
              bg = val;
              used += n;
              p += n;

              if (mirc_color_to_rgba(bg, &rgba)) {
                st.bg = rgba;
                st.bg_set = TRUE;
              } else {
                st.bg_set = FALSE;
              }
            } else {
              /* Comma but no digits: clear bg */
              st.bg_set = FALSE;
            }
          }
        }
      }

      run = p;
      continue;
    }

    p++;
  }

  if (p > run) buffer_insert_run(buf, iter, run, (gssize)(p - run), &st);
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

  /* ZCL_PAGE_WEAKPTR_V1: prevent stale cached tabs from crashing/blackholing output */
  g_object_add_weak_pointer(G_OBJECT(p->root),    (gpointer *)&p->root);
  g_object_add_weak_pointer(G_OBJECT(p->scroller),(gpointer *)&p->scroller);
  g_object_add_weak_pointer(G_OBJECT(p->textview),(gpointer *)&p->textview);
  g_object_add_weak_pointer(G_OBJECT(p->buffer),  (gpointer *)&p->buffer);

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
  /* ZCL_PAGE_WEAKPTR_V1: track entry lifetime too (created later than buffer). */
  g_object_add_weak_pointer(G_OBJECT(p->entry), (gpointer *)&p->entry);
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
  /* Clean up weak pointers if still alive.
   * Important: remove ALL weak pointers before freeing ChatPage, otherwise GTK
   * can later write NULL into freed memory when widgets finalize.
   */
  if (p->buffer)   g_object_remove_weak_pointer(G_OBJECT(p->buffer),   (gpointer *)&p->buffer);
  if (p->entry)    g_object_remove_weak_pointer(G_OBJECT(p->entry),    (gpointer *)&p->entry);
  if (p->textview) g_object_remove_weak_pointer(G_OBJECT(p->textview), (gpointer *)&p->textview);
  if (p->scroller) g_object_remove_weak_pointer(G_OBJECT(p->scroller), (gpointer *)&p->scroller);
  if (p->root)     g_object_remove_weak_pointer(G_OBJECT(p->root),     (gpointer *)&p->root);
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

  if (!p->buffer || !p->scroller) return;

  GtkTextIter end;
  gtk_text_buffer_get_end_iter(p->buffer, &end);

  gchar *ts = timestamp_now();
  gchar *prefix = g_strdup_printf("[%s] ", ts);

  /* Timestamp prefix is always plain; message supports ANSI colors. */
  gtk_text_buffer_insert(p->buffer, &end, prefix, -1);
  buffer_insert_ansi(p->buffer, &end, line);
  gtk_text_buffer_insert(p->buffer, &end, "\n", 1);

  g_free(prefix);
  g_free(ts);

  /* Auto-scroll to bottom without fighting GTK layout too hard. */
  GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(p->scroller));
  const gdouble upper = gtk_adjustment_get_upper(vadj);
  const gdouble page = gtk_adjustment_get_page_size(vadj);
  gdouble value = upper - page;
  if (value < 0) value = 0;
  gtk_adjustment_set_value(vadj, value);
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
