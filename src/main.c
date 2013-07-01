/*
 * Copyright (C) 2012-2013 Paul Ionkin <paul.ionkin@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */
#include "global.h"

/*{{{ structs */
typedef struct {
    ConfData *conf;
    gchar *conf_path;

    struct event_base *evbase;
    struct evdns_base *dns_base;
    struct evhttp *httpd;

    GHashTable *h_torrents;
} TrackerApp;

typedef enum {
    PS_leecher = 0,
    PS_seeder = 1,
} PeerStatus;

typedef struct {
    gchar *peer_id;
    struct in_addr addr;
    gint port;
    PeerStatus status;
    time_t access_time;

    gint64 uploaded;
    gint64 downloaded;
    gint64 left;
} Peer;

typedef struct {
    gchar *info_hash;

    GHashTable *h_peers;
} Torrent;

typedef enum {
    AE_started = 0,
    AE_stopped = 1,
    AE_completed = 2,
    AE_update = 3,
} AnnounceEvent;

#define APP_LOG "main"
/*}}}*/

/*{{{ Peer */
static Peer *peer_create (const gchar *peer_id, const gchar *remote_addr, gint port)
{
    Peer *peer;

    peer = g_new0 (Peer, 1);
    peer->peer_id = g_strdup (peer_id);
    
    evutil_inet_pton (AF_INET, remote_addr, &peer->addr);
    peer->port = g_htons (port);
    peer->status = PS_leecher;
    peer->access_time = time (NULL);

    LOG_debug (APP_LOG, "Peer added, id: %s", peer->peer_id);

    return peer;
}

static void peer_destroy (Peer *peer)
{
    LOG_debug (APP_LOG, "Peer removed, id: %s", peer->peer_id);
    
    g_free (peer->peer_id);
    g_free (peer);
}

static uint8_t *peer_list_to_compact_val (GList *l_peers, size_t *len)
{
    GList *l;
    uint8_t *out, *tmp;;

    *len = g_list_length (l_peers) * 6;

    out = g_new0 (uint8_t, *len);
    tmp = out;

    for (l = g_list_first (l_peers); l; l = g_list_next (l)) {
        Peer *peer = (Peer *) l->data;

        memcpy (tmp, &peer->addr, 4); tmp += 4;
        memcpy (tmp, &peer->port, 2); tmp += 2;
    }

    return out;
}

/*
// XXX:
gchar *peer_list_to_dict_str (GList *l_peers)
{
    GList *l;
    gchar *out;
    size_t len;
    struct in_addr *addr;

    len = g_list_length (l_peers) * (20 ) + 1;

    for (l = g_list_first (l_peers); l; l = g_list_next (l)) {
        Peer *peer = (Peer *) l->data;
    }

}
*/

void peer_update (Peer *peer, gint64 uploaded, gint64 downloaded, gint64 left, AnnounceEvent ev)
{
    peer->uploaded = uploaded;
    peer->downloaded = downloaded;
    peer->left = left;

    if (ev == AE_completed)
        peer->status = PS_seeder;
}
/*}}}*/

/*{{{ Torrent */
static Torrent *torrent_create (const gchar *info_hash)
{
    Torrent *torrent;

    torrent = g_new0 (Torrent, 1);
    torrent->info_hash = g_strdup (info_hash);
    torrent->h_peers = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, (GDestroyNotify) peer_destroy);

    LOG_debug (APP_LOG, "Torrent added, info_hash: %s", torrent->info_hash);

    return torrent;
}

static void torrent_destroy (Torrent *torrent)
{
    LOG_debug (APP_LOG, "Torrent removed, info_hash: %s", torrent->info_hash);

    g_hash_table_destroy (torrent->h_peers);
    g_free (torrent->info_hash);
    g_free (torrent);
}

static Peer *torrent_get_peer (Torrent *torrent, const gchar *peer_id)
{
    return (Peer *) g_hash_table_lookup (torrent->h_peers, peer_id);
}

static Peer *torrent_add_peer (Torrent *torrent, const gchar *peer_id, const gchar *remote_addr, gint port)
{
    Peer *peer;

    peer = peer_create (peer_id, remote_addr, port);
    g_hash_table_insert (torrent->h_peers, peer->peer_id, peer);

    return peer;
}

static GList *torrent_get_list_of_peers (Torrent *torrent, gint numwant)
{
    GList *l = NULL;
    gint peers = 0;
    GHashTableIter iter;
    Peer *peer;

    g_hash_table_iter_init (&iter, torrent->h_peers);
    while (g_hash_table_iter_next (&iter, NULL, (gpointer *)&peer)) {
        if (peers > numwant)
            break;

        l = g_list_prepend (l, peer);

        peers++;
    }

    l = g_list_reverse (l);
    
    return l;
}

static Torrent *tracker_get_torrent (TrackerApp *app, const gchar *info_hash)
{
    return (Torrent *) g_hash_table_lookup (app->h_torrents, info_hash);
}

static Torrent *tracker_add_torrent (TrackerApp *app, const gchar *info_hash)
{
    Torrent *torrent;

    torrent = torrent_create (info_hash);
    g_hash_table_insert (app->h_torrents, torrent->info_hash, torrent);

    return torrent;
}

static void torrent_remove_peer (Torrent *torrent, const gchar *peer_id)
{
    g_hash_table_remove (torrent->h_peers, peer_id);
}

/*}}}*/

/*{{{ Announce*/
static void tracker_app_on_announce_cb (struct evhttp_request *req, void *ctx)
{
    TrackerApp *app = (TrackerApp *) ctx;
    struct evbuffer *evb = NULL;
    const gchar *query;
    struct evkeyvalq q_params;
    const char *info_hash, *peer_id, *compact, *event;
    AnnounceEvent ev;
    const char *tmp;
    gint64 uploaded, downloaded, left;
    gint port, numwant;
    char hinfo[SHA_DIGEST_LENGTH*3 + 1];
    Torrent *torrent;
    Peer *peer = NULL;
    GList *l;
    uint8_t *peer_list_val;
    size_t len;

    if (!req) {
        LOG_err (APP_LOG, "req == NULL !");
        return;
    }

    LOG_debug (APP_LOG, "[%s:%d] URL: %s", req->remote_host, req->remote_port, req->uri);

    query = evhttp_uri_get_query (evhttp_request_get_evhttp_uri (req));
    if (!query) {
        evhttp_send_reply (req, HTTP_NOCONTENT, "Not found", NULL);
        return;
    }

    TAILQ_INIT (&q_params);
    evhttp_parse_query_str (query, &q_params);

    port = numwant = 0;
    uploaded = downloaded = left = 0;

    info_hash = http_find_header (&q_params, "info_hash");
    peer_id = http_find_header (&q_params, "peer_id");
    tmp = http_find_header (&q_params, "port");
    if (tmp)
        port = atoi (tmp);
    tmp = http_find_header (&q_params, "uploaded");
    if (tmp)
        uploaded = g_ascii_strtoll (tmp, NULL, 10);
    tmp = http_find_header (&q_params, "downloaded");
    if (tmp)
        downloaded = g_ascii_strtoll (tmp, NULL, 10);
    tmp = http_find_header (&q_params, "left");
    if (tmp)
        left = g_ascii_strtoll (tmp, NULL, 10);
    tmp = http_find_header (&q_params, "numwant");
    if (tmp)
        numwant = atoi (tmp);
    if (!numwant)
        numwant = conf_get_int (app->conf, "tracker.default_numwant");
    compact = http_find_header (&q_params, "compact");
    event = http_find_header (&q_params, "event");

    // sanity check
    if (!info_hash || !peer_id) {
        evhttp_send_reply (req, HTTP_NOCONTENT, "Not Found", NULL);
        evhttp_clear_headers (&q_params);
        return;
    }

    if (event) {
        if (!strncmp (event, "started", 7)) {
            ev = AE_started;
        } else if (!strncmp (event, "stopped", 7)) {
            ev = AE_stopped;
        } else if (!strncmp (event, "completed", 9)) {
            ev = AE_completed;
        } else {
            ev = AE_update;
        }
    } else 
        ev = AE_update;

    sha1_to_hexstr (hinfo, (const uint8_t *)info_hash);

    LOG_debug (APP_LOG, "%s => peer_id: %s, port: %d, uploaded: %"G_GINT64_FORMAT", downloaded: %"G_GINT64_FORMAT", left: %"G_GINT64_FORMAT", numwant: %d, compact: %s, event: %s", 
        hinfo, peer_id, port, uploaded, downloaded, left, numwant, compact, event);

    torrent = tracker_get_torrent (app, hinfo);
    if (!torrent) {
        torrent = tracker_add_torrent (app, hinfo);
    }

    if (ev != AE_stopped) {
        if (!(peer = torrent_get_peer (torrent, peer_id))) {
            peer = torrent_add_peer (torrent, peer_id, req->remote_host, port);
        }
        
        peer_update (peer, uploaded, downloaded, left, ev);

    // remove peer
    } else {
        torrent_remove_peer (torrent, peer_id);
    }

    l = torrent_get_list_of_peers (torrent, numwant);
    peer_list_val = peer_list_to_compact_val (l, &len);

    LOG_debug (APP_LOG, "Sending list of peers (items: %d, len: %zd) for torrent: %s for peer: %s Total peers: %d (%d)", 
        g_list_length (l), len,
        torrent->info_hash, 
        peer ? peer->peer_id : "none",
        g_hash_table_size (torrent->h_peers), g_list_length (l)
    );
    
    evb = evbuffer_new ();
    evbuffer_add_printf (evb, "d8:intervali3600e5:peers%zd:", len);
    evbuffer_add (evb, peer_list_val, len);
    evbuffer_add_printf (evb, "e");

    g_list_free (l);
    g_free (peer_list_val);
    evhttp_send_reply (req, HTTP_OK, "OK", evb);
    evbuffer_free (evb);
    
    evhttp_clear_headers (&q_params);
}

static void tracker_app_on_http_gen_cb (struct evhttp_request *req, G_GNUC_UNUSED void *ctx)
{
    const gchar *query = NULL;
    //TrackerApp *app = (TrackerApp *) ctx;

    if (!req) {
        LOG_err (APP_LOG, "req == NULL !");
        return;
    }

    LOG_debug (APP_LOG, "Unknown request [%s:%d] URL: %s", req->remote_host, req->remote_port, req->uri);

    evhttp_send_reply (req, HTTP_NOCONTENT, "Not Found", NULL);
}
/*}}}*/

/*{{{ Application */
static void application_destroy (TrackerApp *app)
{
    if (app->httpd)
        evhttp_free (app->httpd);
    if (app->dns_base)
        evdns_base_free (app->dns_base, 0);
    if (app->evbase)
        event_base_free (app->evbase);
    if (app->h_torrents)
        g_hash_table_destroy (app->h_torrents);
    if (app->conf)
        conf_destroy (app->conf);
    if (app->conf_path)
        g_free (app->conf_path);
    g_free (app);
}

int main (G_GNUC_UNUSED int argc, G_GNUC_UNUSED char *argv[])
{
    TrackerApp *app;
    GOptionContext *context;
    GError *error = NULL;
    gchar **s_config = NULL;
    gchar **s_address = NULL;
    gint port = 6969;
    gboolean foreground = FALSE;
    gchar conf_str[1023];
    gboolean verbose = FALSE;
    gboolean version = FALSE;

    app = g_new0 (TrackerApp, 1);
    app->conf_path = g_build_filename (SYSCONFDIR, "tbfs_tracker.conf", NULL);
    g_snprintf (conf_str, sizeof (conf_str), "Path to configuration file. Default: %s", app->conf_path);

    GOptionEntry entries[] = {
        { "address", 'a', 0, G_OPTION_ARG_STRING_ARRAY, &s_address, "Address to bind Tracker server to, use \"0.0.0.0\" to bind on all available local addresses", NULL},
        { "port", 'p', 0, G_OPTION_ARG_INT, &port, "Port to bind Tracker server to. Default is \"6969\"", NULL },
        { "config", 'c', 0, G_OPTION_ARG_FILENAME_ARRAY, &s_config, conf_str, NULL},
        { "foreground", 'f', 0, G_OPTION_ARG_NONE, &foreground, "Flag. Do not daemonize process.", NULL },
        { "verbose", 'v', 0, G_OPTION_ARG_NONE, &verbose, "Verbose output.", NULL },
        { "version", 'V', 0, G_OPTION_ARG_NONE, &version, "Show application version and exit.", NULL },
        { NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL }
    };

    // parse command line arguments
    context = g_option_context_new ("[-a address] [-p port] [-f] [-v]");
    g_option_context_add_main_entries (context, entries, NULL);
    if (!g_option_context_parse (context, &argc, &argv, &error)) {
        g_fprintf (stderr, "Failed to parse command line options: %s\n", error->message);
        application_destroy (app);
        g_option_context_free (context);
        return -1;
    }
    g_option_context_free (context);

    // user provided alternative config path
    if (s_config && g_strv_length (s_config) > 0) {
        g_free (app->conf_path);
        app->conf_path = g_strdup (s_config[0]);
        g_strfreev (s_config);
    }

    app->conf = conf_create ();
    if (access (app->conf_path, R_OK) == 0) {
        LOG_debug (APP_LOG, "Using config file: %s", app->conf_path);
        if (!conf_parse_file (app->conf, app->conf_path)) {
            LOG_err (APP_LOG, "Failed to parse configuration file: %s", app->conf_path);
            application_destroy (app);
            return -1;
        }

    // default values
    } else {
        // XXX: update !
        conf_set_int (app->conf, "log.level", LOG_msg);
        conf_set_boolean (app->conf, "app.foreground", FALSE);
        conf_set_string (app->conf, "tracker.address", "0.0.0.0");
        conf_set_int (app->conf, "tracker.port", 6969);
        conf_set_int (app->conf, "tracker.default_numwant", 50);
    }

    if (verbose)
        conf_set_int (app->conf, "log.level", LOG_debug);

    log_level = conf_get_int (app->conf, "log.level");

    // check if --version is specified
    if (version) {
            g_fprintf (stdout, "%s v%s\n", PACKAGE_NAME, VERSION);
            g_fprintf (stdout, "Copyright (C) 2013 Paul Ionkin <paul.ionkin@gmail.com>\n");
            g_fprintf (stdout, "Libraries:\n");
            g_fprintf (stdout, " GLib: %d.%d.%d   libevent: %s  glibc: %s\n", 
                    GLIB_MAJOR_VERSION, GLIB_MINOR_VERSION, GLIB_MICRO_VERSION, 
                    LIBEVENT_VERSION,
                    gnu_get_libc_version ()
            );
        return 0;
    }

    // set address from command line
    if (s_address && g_strv_length (s_address) >= 1) {
        conf_set_string (app->conf, "tracker.address", s_address[0]);
        g_strfreev (s_address);
    }
    if (port != 6969) {
        conf_set_int (app->conf, "tracker.port", port);
    }

    // set foreground
    if (foreground)
        conf_set_boolean (app->conf, "app.foreground", foreground);

    // check that peer_id is set
    if (!conf_get_string (app->conf, "tracker.address")) {
        LOG_err (APP_LOG, "Address is not set, please run  %s -a address !", argv[0]);
        application_destroy (app);
        return -1;
    }

    app->evbase = event_base_new ();
    if (!app->evbase) {
        LOG_err (APP_LOG, "Failed to create event base !");
        application_destroy (app);
        return -1;
    }

    app->dns_base = evdns_base_new (app->evbase, 1);
    if (!app->dns_base) {
        LOG_err (APP_LOG, "Failed to create DNS base !");
        application_destroy (app);
        return -1;
    }

    app->h_torrents = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, (GDestroyNotify) torrent_destroy);

    app->httpd = evhttp_new (app->evbase);
    if (evhttp_bind_socket (app->httpd, 
        conf_get_string (app->conf, "tracker.address"), 
        conf_get_int (app->conf, "tracker.port")) == -1) 
    {
        LOG_err (APP_LOG, "Failed to bind Tracker server to %s:%d",
            conf_get_string (app->conf, "tracker.address"), 
            conf_get_int (app->conf, "tracker.port")
        );
        return -1;
    }

    LOG_debug (APP_LOG, "Tracker server is running on %s:%d",
        conf_get_string (app->conf, "tracker.address"), 
        conf_get_int (app->conf, "tracker.port")
    );

    evhttp_set_cb (app->httpd, "/announce", tracker_app_on_announce_cb, app);
    evhttp_set_gencb (app->httpd, tracker_app_on_http_gen_cb, app);

    if (!conf_get_boolean (app->conf, "app.foreground"))
        wutils_daemonize ();

    // start the loop
    event_base_dispatch (app->evbase);

    application_destroy (app);

    return 0;
}
/*}}}*/
