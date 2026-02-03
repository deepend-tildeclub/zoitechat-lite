#include "ansi_format.h"

#include <gdk/gdk.h>
#include <gtk/gtk.h>
#include <pango/pango.h>
#include <string.h>

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
  static const gint normal[8][3] = {
    {0, 0, 0},
    {205, 49, 49},
    {13, 188, 121},
    {229, 229, 16},
    {36, 114, 200},
    {188, 63, 188},
    {17, 168, 205},
    {229, 229, 229},
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

    gint rr = (r == 0) ? 0 : (r * 40 + 55);
    gint gg = (g == 0) ? 0 : (g * 40 + 55);
    gint bb = (b == 0) ? 0 : (b * 40 + 55);

    ansi_rgba_set_u8(out, rr, gg, bb);
    return;
  }

  gint g = 8 + (n - 232) * 10;
  ansi_rgba_set_u8(out, g, g, g);
}

static void
ansi_effective_state(const ZclAnsiState *in, ZclAnsiState *out_eff) {
  *out_eff = *in;

  if (out_eff->reverse) {
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
    }
  }
}

static gboolean
mirc_color_to_rgba(gint idx, GdkRGBA *out) {
  static const gchar *hex[16] = {
    "#FFFFFF",
    "#000000",
    "#00007F",
    "#009300",
    "#FF0000",
    "#7F0000",
    "#9C009C",
    "#FC7F00",
    "#FFFF00",
    "#00FC00",
    "#009393",
    "#00FFFF",
    "#0000FC",
    "#FF00FF",
    "#7F7F7F",
    "#D2D2D2",
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

typedef void (*ZclAnsiRunFunc)(GString *out, const gchar *text, gssize len, const ZclAnsiState *st);

static gchar *
ansi_parse_runs(const gchar *s, ZclAnsiRunFunc run_cb) {
  if (!s) return g_strdup("");

  ZclAnsiState st;
  ansi_state_reset(&st);

  GString *out = g_string_new("");
  const gchar *p = s;
  const gchar *run = p;

  while (*p) {
    const guchar c = (guchar)p[0];

    if (c == 0x1b && p[1] == '[') {
      if (p > run) run_cb(out, run, (gssize)(p - run), &st);

      const gchar *q = p + 2;
      const gchar *final = q;
      while (*final && !((guchar)*final >= 0x40 && (guchar)*final <= 0x7E)) final++;

      if (!*final) {
        run_cb(out, p, -1, &st);
        return g_string_free(out, FALSE);
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

    if (c == 0x02 ||
        c == 0x1F ||
        c == 0x16 ||
        c == 0x0F ||
        c == 0x03) {

      if (p > run) run_cb(out, run, (gssize)(p - run), &st);

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
        p++;

        gint val = 0, n = 0;
        gboolean has_fg = mirc_read_1or2_digits(p, &val, &n);
        if (!has_fg) {
          st.fg_set = FALSE;
          st.bg_set = FALSE;
        } else {
          const gint fg = val;
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

            gboolean has_bg = mirc_read_1or2_digits(p, &val, &n);
            if (has_bg) {
              const gint bg = val;
              p += n;

              if (mirc_color_to_rgba(bg, &rgba)) {
                st.bg = rgba;
                st.bg_set = TRUE;
              } else {
                st.bg_set = FALSE;
              }
            } else {
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

  if (p > run) run_cb(out, run, (gssize)(p - run), &st);

  return g_string_free(out, FALSE);
}

static void
append_run_plain(GString *out, const gchar *text, gssize len, const ZclAnsiState *st) {
  (void)st;
  if (!text) return;
  if (len < 0) {
    g_string_append(out, text);
  } else {
    g_string_append_len(out, text, (gsize)len);
  }
}

static void
append_run_markup(GString *out, const gchar *text, gssize len, const ZclAnsiState *st) {
  if (!text) return;

  ZclAnsiState eff;
  ansi_effective_state(st, &eff);

  gchar *escaped = g_markup_escape_text(text, len);
  const gboolean styled = eff.bold || eff.underline || eff.fg_set || eff.bg_set;
  if (!styled) {
    g_string_append(out, escaped);
    g_free(escaped);
    return;
  }

  gchar fhex[7];
  gchar bhex[7];
  GString *span = g_string_new("<span");

  if (eff.fg_set) {
    ansi_rgba_to_hex(&eff.fg, fhex);
    g_string_append_printf(span, " foreground=\"#%s\"", fhex);
  }
  if (eff.bg_set) {
    ansi_rgba_to_hex(&eff.bg, bhex);
    g_string_append_printf(span, " background=\"#%s\"", bhex);
  }
  if (eff.bold) {
    g_string_append(span, " weight=\"bold\"");
  }
  if (eff.underline) {
    g_string_append(span, " underline=\"single\"");
  }

  g_string_append(span, ">");
  g_string_append(span, escaped);
  g_string_append(span, "</span>");

  g_string_append(out, span->str);
  g_string_free(span, TRUE);
  g_free(escaped);
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

gchar *
zcl_ansi_to_markup(const gchar *text) {
  return ansi_parse_runs(text, append_run_markup);
}

gchar *
zcl_strip_ansi(const gchar *text) {
  return ansi_parse_runs(text, append_run_plain);
}

void
zcl_buffer_insert_ansi(GtkTextBuffer *buf, GtkTextIter *iter, const gchar *text) {
  if (!buf || !iter || !text) return;

  ZclAnsiState st;
  ansi_state_reset(&st);

  const gchar *p = text;
  const gchar *run = p;

  while (*p) {
    const guchar c = (guchar)p[0];

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

    if (c == 0x02 ||
        c == 0x1F ||
        c == 0x16 ||
        c == 0x0F ||
        c == 0x03) {

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
        p++;

        gint val = 0;
        gint n = 0;
        gboolean has_fg = mirc_read_1or2_digits(p, &val, &n);
        if (!has_fg) {
          st.fg_set = FALSE;
          st.bg_set = FALSE;
        } else {
          const gint fg = val;
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

            gboolean has_bg = mirc_read_1or2_digits(p, &val, &n);
            if (has_bg) {
              const gint bg = val;
              p += n;

              if (mirc_color_to_rgba(bg, &rgba)) {
                st.bg = rgba;
                st.bg_set = TRUE;
              } else {
                st.bg_set = FALSE;
              }
            } else {
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
