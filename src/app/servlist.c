#include "servlist.h"

#include <glib/gstdio.h>
#include <string.h>

struct _ZcServlistNetwork {
  gchar *name;
  GPtrArray *servers;
  gchar *default_channel;
  gchar *auto_join;
  gchar *charset;
  gboolean default_tls;
};

typedef struct {
  const gchar *network;
  const gchar *host;
  const gchar *channel;
  const gchar *charset;
  gboolean ssl;
} ZcServlistDefault;

static const ZcServlistDefault zc_servlist_defaults[] = {
  {"2600net", NULL, NULL, NULL, FALSE},
  {NULL, "irc.2600.net", NULL, NULL, FALSE},

  {"AfterNET", NULL, NULL, NULL, TRUE},
  {NULL, "irc.afternet.org", NULL, NULL, FALSE},

  {"Aitvaras", NULL, NULL, NULL, FALSE},
  {NULL, "irc.data.lt/+6668", NULL, NULL, FALSE},
  {NULL, "irc.omicron.lt/+6668", NULL, NULL, FALSE},
  {NULL, "irc.vub.lt/+6668", NULL, NULL, FALSE},
  {NULL, "irc.data.lt", NULL, NULL, FALSE},
  {NULL, "irc.omicron.lt", NULL, NULL, FALSE},
  {NULL, "irc.vub.lt", NULL, NULL, FALSE},

  {"Anthrochat", NULL, NULL, NULL, TRUE},
  {NULL, "irc.anthrochat.net", NULL, NULL, FALSE},

  {"ARCNet", NULL, NULL, NULL, FALSE},
  {NULL, "arcnet-irc.org", NULL, NULL, FALSE},

  {"AustNet", NULL, NULL, NULL, FALSE},
  {NULL, "irc.austnet.org", NULL, NULL, FALSE},

  {"AzzurraNet", NULL, NULL, NULL, FALSE},
  {NULL, "irc.azzurra.org", NULL, NULL, FALSE},

  {"Canternet", NULL, NULL, NULL, TRUE},
  {NULL, "irc.canternet.org", NULL, NULL, FALSE},

  {"Chat4all", NULL, NULL, NULL, TRUE},
  {NULL, "irc.chat4all.org", NULL, NULL, FALSE},

  {"ChatJunkies", NULL, NULL, NULL, FALSE},
  {NULL, "irc.chatjunkies.org", NULL, NULL, FALSE},

  {"chatpat", NULL, NULL, "CP1251", FALSE},
  {NULL, "irc.unibg.net", NULL, NULL, FALSE},
  {NULL, "irc.chatpat.bg", NULL, NULL, FALSE},

  {"ChatSpike", NULL, NULL, NULL, FALSE},
  {NULL, "irc.chatspike.net", NULL, NULL, FALSE},

  {"DaIRC", NULL, NULL, NULL, FALSE},
  {NULL, "irc.dairc.net", NULL, NULL, FALSE},

  {"DALnet", NULL, NULL, NULL, FALSE},
  {NULL, "us.dal.net", NULL, NULL, FALSE},

  {"DarkMyst", NULL, NULL, NULL, TRUE},
  {NULL, "irc.darkmyst.org", NULL, NULL, FALSE},

  {"darkscience", NULL, NULL, NULL, TRUE},
  {NULL, "irc.darkscience.net", NULL, NULL, FALSE},
  {NULL, "irc.drk.sc", NULL, NULL, FALSE},
  {NULL, "irc.darkscience.ws", NULL, NULL, FALSE},

  {"Dark-Tou-Net", NULL, NULL, NULL, FALSE},
  {NULL, "irc.d-t-net.de", NULL, NULL, FALSE},

  {"DigitalIRC", NULL, NULL, NULL, TRUE},
  {NULL, "irc.digitalirc.org", NULL, NULL, FALSE},

  {"DosersNET", NULL, NULL, NULL, TRUE},
  {NULL, "irc.dosers.net/+6697", NULL, NULL, FALSE},

  {"EFnet", NULL, NULL, NULL, FALSE},
  {NULL, "irc.choopa.net", NULL, NULL, FALSE},
  {NULL, "efnet.port80.se", NULL, NULL, FALSE},
  {NULL, "irc.underworld.no", NULL, NULL, FALSE},
  {NULL, "efnet.deic.eu", NULL, NULL, FALSE},

  {"EnterTheGame", NULL, NULL, NULL, FALSE},
  {NULL, "irc.enterthegame.com", NULL, NULL, FALSE},

  {"EntropyNet", NULL, NULL, NULL, TRUE},
  {NULL, "irc.entropynet.net", NULL, NULL, FALSE},

  {"EsperNet", NULL, NULL, NULL, TRUE},
  {NULL, "irc.esper.net", NULL, NULL, FALSE},

  {"EUIrc", NULL, NULL, NULL, FALSE},
  {NULL, "irc.euirc.net", NULL, NULL, FALSE},

  {"EuropNet", NULL, NULL, NULL, FALSE},
  {NULL, "irc.europnet.org", NULL, NULL, FALSE},

  {"FDFNet", NULL, NULL, NULL, FALSE},
  {NULL, "irc.fdfnet.net", NULL, NULL, FALSE},

  {"GameSurge", NULL, NULL, NULL, FALSE},
  {NULL, "irc.gamesurge.net", NULL, NULL, FALSE},

  {"GeekShed", NULL, NULL, NULL, TRUE},
  {NULL, "irc.geekshed.net", NULL, NULL, FALSE},

  {"German-Elite", NULL, NULL, "CP1252", FALSE},
  {NULL, "irc.german-elite.net", NULL, NULL, FALSE},

  {"GIMPNet", NULL, NULL, NULL, FALSE},
  {NULL, "irc.gimp.org", NULL, NULL, FALSE},
  {NULL, "irc.gnome.org", NULL, NULL, FALSE},

  {"GlobalGamers", NULL, NULL, NULL, FALSE},
  {NULL, "irc.globalgamers.net/+6660", NULL, NULL, FALSE},
  {NULL, "irc.globalgamers.net", NULL, NULL, FALSE},

  {"hackint", NULL, NULL, NULL, TRUE},
  {NULL, "irc.hackint.org", NULL, NULL, FALSE},
  {NULL, "irc.eu.hackint.org", NULL, NULL, FALSE},

  {"Hashmark", NULL, NULL, NULL, FALSE},
  {NULL, "irc.hashmark.net", NULL, NULL, FALSE},

  {"ICQ-Chat", NULL, NULL, NULL, TRUE},
  {NULL, "irc.icq-chat.com", NULL, NULL, FALSE},

  {"Interlinked", NULL, NULL, NULL, TRUE},
  {NULL, "irc.interlinked.me", NULL, NULL, FALSE},

  {"Irc-Nerds", NULL, NULL, NULL, TRUE},
  {NULL, "irc.irc-nerds.net", NULL, NULL, FALSE},

  {"IRC4Fun", NULL, NULL, NULL, TRUE},
  {NULL, "irc.irc4fun.net", NULL, NULL, FALSE},

  {"IRCNet", NULL, NULL, NULL, FALSE},
  {NULL, "open.ircnet.net", NULL, NULL, FALSE},

  {"IRCtoo", NULL, NULL, NULL, FALSE},
  {NULL, "irc.irctoo.net", NULL, NULL, FALSE},

  {"Keyboard-Failure", NULL, NULL, NULL, FALSE},
  {NULL, "irc.kbfail.net", NULL, NULL, FALSE},

  {"Libera.Chat", NULL, NULL, NULL, TRUE},
  {NULL, "irc.libera.chat", NULL, NULL, FALSE},

  {"LibertaCasa", NULL, NULL, NULL, TRUE},
  {NULL, "irc.liberta.casa", NULL, NULL, FALSE},

  {"LibraIRC", NULL, NULL, NULL, FALSE},
  {NULL, "irc.librairc.net", NULL, NULL, FALSE},

  {"LinkNet", NULL, NULL, NULL, FALSE},
  {NULL, "irc.link-net.org/+7000", NULL, NULL, FALSE},

  {"MindForge", NULL, NULL, NULL, FALSE},
  {NULL, "irc.mindforge.org", NULL, NULL, FALSE},

  {"MIXXnet", NULL, NULL, NULL, FALSE},
  {NULL, "irc.mixxnet.net", NULL, NULL, FALSE},

  {"Newnet", NULL, NULL, NULL, TRUE},
  {NULL, "irc.newnet.net", NULL, NULL, FALSE},
  {NULL, "australia-au.newnet.net", NULL, NULL, FALSE},
  {NULL, "beauharnois-ca.newnet.net", NULL, NULL, FALSE},
  {NULL, "vancouver-ca.newnet.net", NULL, NULL, FALSE},
  {NULL, "gravelines-fr.newnet.net", NULL, NULL, FALSE},
  {NULL, "sao-paulo.newnet.net", NULL, NULL, FALSE},

  {"Oceanius", NULL, NULL, NULL, FALSE},
  {NULL, "irc.oceanius.com", NULL, NULL, FALSE},

  {"OFTC", NULL, NULL, NULL, TRUE},
  {NULL, "irc.oftc.net", NULL, NULL, FALSE},

  {"OtherNet", NULL, NULL, NULL, FALSE},
  {NULL, "irc.othernet.org", NULL, NULL, FALSE},

  {"OzOrg", NULL, NULL, NULL, FALSE},
  {NULL, "irc.oz.org", NULL, NULL, FALSE},

  {"PIK", NULL, NULL, NULL, FALSE},
  {NULL, "irc.krstarica.com", NULL, NULL, FALSE},

  {"pirc.pl", NULL, NULL, NULL, TRUE},
  {NULL, "irc.pirc.pl", NULL, NULL, FALSE},

  {"PTNet", NULL, NULL, NULL, FALSE},
  {NULL, "irc.ptnet.org", NULL, NULL, FALSE},
  {NULL, "uevora.ptnet.org", NULL, NULL, FALSE},
  {NULL, "claranet.ptnet.org", NULL, NULL, FALSE},
  {NULL, "sonaquela.ptnet.org", NULL, NULL, FALSE},
  {NULL, "uc.ptnet.org", NULL, NULL, FALSE},
  {NULL, "ipg.ptnet.org", NULL, NULL, FALSE},

  {"QuakeNet", NULL, NULL, NULL, FALSE},
  {NULL, "irc.quakenet.org", NULL, NULL, FALSE},

  {"Rizon", NULL, NULL, NULL, TRUE},
  {NULL, "irc.rizon.net", NULL, NULL, FALSE},

  {"RusNet", NULL, NULL, "KOI8-R (Cyrillic)", FALSE},
  {NULL, "irc.tomsk.net", NULL, NULL, FALSE},
  {NULL, "irc.run.net", NULL, NULL, FALSE},
  {NULL, "irc.ru", NULL, NULL, FALSE},
  {NULL, "irc.lucky.net", NULL, NULL, FALSE},

  {"Serenity-IRC", NULL, NULL, NULL, FALSE},
  {NULL, "irc.serenity-irc.net", NULL, NULL, FALSE},

  {"SimosNap", NULL, NULL, NULL, TRUE},
  {NULL, "irc.simosnap.com", NULL, NULL, FALSE},

  {"SlashNET", NULL, NULL, NULL, FALSE},
  {NULL, "irc.slashnet.org", NULL, NULL, FALSE},

  {"Snoonet", NULL, NULL, NULL, TRUE},
  {NULL, "irc.snoonet.org", NULL, NULL, FALSE},

  {"Sohbet.Net", NULL, NULL, "CP1254", FALSE},
  {NULL, "irc.sohbet.net", NULL, NULL, FALSE},

  {"SorceryNet", NULL, NULL, NULL, FALSE},
  {NULL, "irc.sorcery.net", NULL, NULL, FALSE},

  {"SpotChat", NULL, NULL, NULL, TRUE},
  {NULL, "irc.spotchat.org", NULL, NULL, FALSE},

  {"Station51", NULL, NULL, NULL, FALSE},
  {NULL, "irc.station51.net", NULL, NULL, FALSE},

  {"StormBit", NULL, NULL, NULL, TRUE},
  {NULL, "irc.stormbit.net", NULL, NULL, FALSE},

  {"SwiftIRC", NULL, NULL, NULL, FALSE},
  {NULL, "irc.swiftirc.net", NULL, NULL, FALSE},

  {"synIRC", NULL, NULL, NULL, FALSE},
  {NULL, "irc.synirc.net", NULL, NULL, FALSE},

  {"Techtronix", NULL, NULL, NULL, TRUE},
  {NULL, "irc.techtronix.net", NULL, NULL, FALSE},

  {"tilde.chat", NULL, NULL, NULL, TRUE},
  {NULL, "irc.tilde.chat", NULL, NULL, FALSE},

  {"TURLINet", NULL, NULL, NULL, TRUE},
  {NULL, "irc.servx.org", NULL, NULL, FALSE},
  {NULL, "i.valware.uk", NULL, NULL, FALSE},

  {"TripSit", NULL, NULL, NULL, TRUE},
  {NULL, "irc.tripsit.me", NULL, NULL, FALSE},
  {NULL, "newirc.tripsit.me", NULL, NULL, FALSE},
  {NULL, "coconut.tripsit.me", NULL, NULL, FALSE},
  {NULL, "innsbruck.tripsit.me", NULL, NULL, FALSE},

  {"UnderNet", NULL, NULL, NULL, FALSE},
  {NULL, "us.undernet.org", NULL, NULL, FALSE},

  {"Xertion", NULL, NULL, NULL, TRUE},
  {NULL, "irc.xertion.org", NULL, NULL, FALSE},

  {"Zoite", NULL, NULL, NULL, TRUE},
  {NULL, "irc.zoite.net", NULL, NULL, FALSE},
  {NULL, "penumbra.newnet.net", NULL, NULL, FALSE},
  {NULL, "hedy.newnet.net", NULL, NULL, FALSE},

  {NULL, NULL, NULL, NULL, FALSE}
};

static GPtrArray *zc_servlist_networks = NULL;
static gboolean zc_servlist_loaded = FALSE;

static void
zc_servlist_network_free(ZcServlistNetwork *net) {
  if (!net) return;
  g_free(net->name);
  if (net->servers) {
    g_ptr_array_free(net->servers, TRUE);
  }
  g_free(net->default_channel);
  g_free(net->auto_join);
  g_free(net->charset);
  g_free(net);
}

static void
zc_servlist_init_defaults(void) {
  if (!zc_servlist_networks) {
    zc_servlist_networks = g_ptr_array_new_with_free_func((GDestroyNotify)zc_servlist_network_free);
  }

  ZcServlistNetwork *current = NULL;
  for (gsize i = 0; zc_servlist_defaults[i].network || zc_servlist_defaults[i].host; i++) {
    const ZcServlistDefault *entry = &zc_servlist_defaults[i];
    if (entry->network) {
      current = g_new0(ZcServlistNetwork, 1);
      current->name = g_strdup(entry->network);
      current->servers = g_ptr_array_new_with_free_func(g_free);
      if (entry->channel) current->default_channel = g_strdup(entry->channel);
      if (entry->charset) current->charset = g_strdup(entry->charset);
      current->default_tls = entry->ssl;
      g_ptr_array_add(zc_servlist_networks, current);
    } else if (current && entry->host) {
      g_ptr_array_add(current->servers, g_strdup(entry->host));
    }
  }
}

static gchar *
zc_servlist_path(void) {
  const gchar *base = g_get_user_config_dir();
  gchar *dir = g_build_filename(base, "zoitechat-lite", NULL);
  g_mkdir_with_parents(dir, 0700);
  gchar *path = g_build_filename(dir, "servlist.ini", NULL);
  g_free(dir);
  return path;
}

static void
zc_servlist_clear(void) {
  if (!zc_servlist_networks) return;
  g_ptr_array_set_size(zc_servlist_networks, 0);
}

static void
zc_servlist_load(void) {
  if (zc_servlist_loaded) return;
  zc_servlist_loaded = TRUE;

  if (!zc_servlist_networks) {
    zc_servlist_networks = g_ptr_array_new_with_free_func((GDestroyNotify)zc_servlist_network_free);
  }

  gchar *path = zc_servlist_path();
  GKeyFile *kf = g_key_file_new();
  GError *error = NULL;
  if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, &error)) {
    g_clear_error(&error);
    g_key_file_free(kf);
    g_free(path);
    zc_servlist_init_defaults();
    return;
  }

  gsize group_count = 0;
  gchar **groups = g_key_file_get_groups(kf, &group_count);
  for (gsize i = 0; i < group_count; i++) {
    const gchar *group = groups[i];
    if (g_str_has_prefix(group, "network:")) {
      const gchar *name = group + strlen("network:");
      if (!name || !*name) continue;
      ZcServlistNetwork *net = g_new0(ZcServlistNetwork, 1);
      net->name = g_strdup(name);
      net->servers = g_ptr_array_new_with_free_func(g_free);

      if (g_key_file_has_key(kf, group, "default_channel", NULL)) {
        net->default_channel = g_key_file_get_string(kf, group, "default_channel", NULL);
      }
      if (g_key_file_has_key(kf, group, "auto_join", NULL)) {
        net->auto_join = g_key_file_get_string(kf, group, "auto_join", NULL);
      }
      if (g_key_file_has_key(kf, group, "charset", NULL)) {
        net->charset = g_key_file_get_string(kf, group, "charset", NULL);
      }
      if (g_key_file_has_key(kf, group, "tls", NULL)) {
        net->default_tls = g_key_file_get_boolean(kf, group, "tls", NULL);
      }

      gsize server_count = 0;
      gchar **servers = g_key_file_get_string_list(kf, group, "servers", &server_count, NULL);
      if (servers) {
        for (gsize s = 0; s < server_count; s++) {
          if (servers[s] && *servers[s]) {
            g_ptr_array_add(net->servers, g_strdup(servers[s]));
          }
        }
        g_strfreev(servers);
      }
      g_ptr_array_add(zc_servlist_networks, net);
    }
  }

  g_strfreev(groups);
  g_key_file_free(kf);
  g_free(path);

  if (zc_servlist_networks->len == 0) {
    zc_servlist_clear();
    zc_servlist_init_defaults();
  }
}

const ZcServlistNetwork *const *
zc_servlist_get_networks(gsize *out_len) {
  zc_servlist_load();
  if (out_len) *out_len = zc_servlist_networks ? zc_servlist_networks->len : 0;
  if (!zc_servlist_networks || zc_servlist_networks->len == 0) return NULL;
  return (const ZcServlistNetwork *const *)zc_servlist_networks->pdata;
}

const gchar *
zc_servlist_network_get_name(const ZcServlistNetwork *net) {
  return net ? net->name : NULL;
}

const gchar *
zc_servlist_network_get_default_server(const ZcServlistNetwork *net) {
  if (!net || !net->servers || net->servers->len == 0) return NULL;
  return (const gchar *)g_ptr_array_index(net->servers, 0);
}

const gchar *
zc_servlist_network_get_default_channel(const ZcServlistNetwork *net) {
  return net ? net->default_channel : NULL;
}

const gchar *
zc_servlist_network_get_auto_join(const ZcServlistNetwork *net) {
  return net ? net->auto_join : NULL;
}

const gchar *const *
zc_servlist_network_get_servers(const ZcServlistNetwork *net, gsize *out_len) {
  if (out_len) *out_len = net && net->servers ? net->servers->len : 0;
  if (!net || !net->servers || net->servers->len == 0) return NULL;
  return (const gchar *const *)net->servers->pdata;
}

static void
zc_servlist_split_host(const gchar *spec, gchar *out_host, gsize host_len) {
  const gchar *slash = strchr(spec, '/');
  gsize len = slash ? (gsize)(slash - spec) : strlen(spec);
  if (len >= host_len) len = host_len - 1;
  memcpy(out_host, spec, len);
  out_host[len] = '\0';
}

void
zc_servlist_parse_server(const ZcServlistNetwork *net,
                          const gchar *spec,
                          gchar **out_host,
                          guint16 *out_port,
                          gboolean *out_tls) {
  if (!spec) return;

  gboolean tls = net ? net->default_tls : FALSE;
  guint16 port = tls ? 6697 : 6667;

  const gchar *slash = strchr(spec, '/');
  if (slash) {
    if (slash[1] == '+') {
      tls = TRUE;
      port = (guint16)g_ascii_strtoll(slash + 2, NULL, 10);
    } else {
      port = (guint16)g_ascii_strtoll(slash + 1, NULL, 10);
    }
  }

  if (out_host) {
    gchar *host = g_strdup(spec);
    gchar *sep = strchr(host, '/');
    if (sep) *sep = '\0';
    *out_host = host;
  }
  if (out_port) *out_port = port;
  if (out_tls) *out_tls = tls;
}

const ZcServlistNetwork *
zc_servlist_find_network_by_host(const gchar *host) {
  if (!host || !*host) return NULL;
  zc_servlist_load();

  gchar lower_host[256];
  g_strlcpy(lower_host, host, sizeof(lower_host));
  g_ascii_strdown(lower_host, -1);

  for (guint i = 0; i < zc_servlist_networks->len; i++) {
    ZcServlistNetwork *net = g_ptr_array_index(zc_servlist_networks, i);
    if (!net || !net->servers) continue;
    for (guint s = 0; s < net->servers->len; s++) {
      const gchar *spec = g_ptr_array_index(net->servers, s);
      gchar spec_host[256];
      zc_servlist_split_host(spec, spec_host, sizeof(spec_host));
      g_ascii_strdown(spec_host, -1);
      if (g_strcmp0(spec_host, lower_host) == 0) return net;
    }
  }

  return NULL;
}

ZcServlistNetwork *
zc_servlist_add_network(const gchar *name) {
  if (!name || !*name) return NULL;
  zc_servlist_load();
  ZcServlistNetwork *net = g_new0(ZcServlistNetwork, 1);
  net->name = g_strdup(name);
  net->servers = g_ptr_array_new_with_free_func(g_free);
  g_ptr_array_add(zc_servlist_networks, net);
  return net;
}

gboolean
zc_servlist_remove_network(ZcServlistNetwork *net) {
  if (!net || !zc_servlist_networks) return FALSE;
  return g_ptr_array_remove(zc_servlist_networks, net);
}

gboolean
zc_servlist_network_set_name(ZcServlistNetwork *net, const gchar *name) {
  if (!net || !name || !*name) return FALSE;
  g_free(net->name);
  net->name = g_strdup(name);
  return TRUE;
}

gboolean
zc_servlist_network_add_server(ZcServlistNetwork *net, const gchar *spec) {
  if (!net || !net->servers || !spec || !*spec) return FALSE;
  g_ptr_array_add(net->servers, g_strdup(spec));
  return TRUE;
}

gboolean
zc_servlist_network_update_server(ZcServlistNetwork *net, gsize idx, const gchar *spec) {
  if (!net || !net->servers || !spec || !*spec) return FALSE;
  if (idx >= net->servers->len) return FALSE;
  g_free(g_ptr_array_index(net->servers, idx));
  g_ptr_array_index(net->servers, idx) = g_strdup(spec);
  return TRUE;
}

gboolean
zc_servlist_network_remove_server(ZcServlistNetwork *net, gsize idx) {
  if (!net || !net->servers) return FALSE;
  if (idx >= net->servers->len) return FALSE;
  g_ptr_array_remove_index(net->servers, (guint)idx);
  return TRUE;
}

gboolean
zc_servlist_network_set_auto_join(ZcServlistNetwork *net, const gchar *auto_join) {
  if (!net) return FALSE;
  g_free(net->auto_join);
  net->auto_join = (auto_join && *auto_join) ? g_strdup(auto_join) : NULL;
  return TRUE;
}

gboolean
zc_servlist_save(void) {
  zc_servlist_load();
  if (!zc_servlist_networks) return FALSE;

  GKeyFile *kf = g_key_file_new();
  for (guint i = 0; i < zc_servlist_networks->len; i++) {
    ZcServlistNetwork *net = g_ptr_array_index(zc_servlist_networks, i);
    if (!net || !net->name || !net->servers) continue;
    gchar *group = g_strdup_printf("network:%s", net->name);
    gsize server_count = net->servers->len;
    const gchar **servers = (const gchar **)net->servers->pdata;
    g_key_file_set_string_list(kf, group, "servers", servers, server_count);
    if (net->default_channel)
      g_key_file_set_string(kf, group, "default_channel", net->default_channel);
    if (net->auto_join)
      g_key_file_set_string(kf, group, "auto_join", net->auto_join);
    if (net->charset)
      g_key_file_set_string(kf, group, "charset", net->charset);
    g_key_file_set_boolean(kf, group, "tls", net->default_tls);
    g_free(group);
  }

  gsize len = 0;
  gchar *data = g_key_file_to_data(kf, &len, NULL);
  gchar *path = zc_servlist_path();
  gboolean ok = g_file_set_contents(path, data, (gssize)len, NULL);
  g_free(path);
  g_free(data);
  g_key_file_free(kf);
  return ok;
}
