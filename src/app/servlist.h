#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct _ZcServlistNetwork ZcServlistNetwork;

const ZcServlistNetwork *const *zc_servlist_get_networks(gsize *out_len);
const ZcServlistNetwork *zc_servlist_find_network_by_host(const gchar *host);

const gchar *zc_servlist_network_get_name(const ZcServlistNetwork *net);
const gchar *zc_servlist_network_get_default_server(const ZcServlistNetwork *net);
const gchar *zc_servlist_network_get_default_channel(const ZcServlistNetwork *net);
const gchar *zc_servlist_network_get_auto_join(const ZcServlistNetwork *net);
const gchar *const *zc_servlist_network_get_servers(const ZcServlistNetwork *net, gsize *out_len);

void zc_servlist_parse_server(const ZcServlistNetwork *net,
                              const gchar *spec,
                              gchar **out_host,
                              guint16 *out_port,
                              gboolean *out_tls);

ZcServlistNetwork *zc_servlist_add_network(const gchar *name);
gboolean zc_servlist_remove_network(ZcServlistNetwork *net);
gboolean zc_servlist_network_set_name(ZcServlistNetwork *net, const gchar *name);
gboolean zc_servlist_network_add_server(ZcServlistNetwork *net, const gchar *spec);
gboolean zc_servlist_network_update_server(ZcServlistNetwork *net, gsize idx, const gchar *spec);
gboolean zc_servlist_network_remove_server(ZcServlistNetwork *net, gsize idx);
gboolean zc_servlist_network_set_auto_join(ZcServlistNetwork *net, const gchar *auto_join);

gboolean zc_servlist_save(void);

G_END_DECLS
