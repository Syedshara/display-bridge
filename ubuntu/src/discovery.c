/*
 * discovery.c
 * mDNS service advertisement via Avahi (Linux Bonjour equivalent).
 *
 * Publishes a "_display-bridge._tcp" service so the Mac receiver
 * can auto-discover the sender's IP address via Bonjour/NWBrowser.
 *
 * Uses AvahiThreadedPoll to run the Avahi event loop in a dedicated
 * background thread — doesn't interfere with PipeWire or SRT threads.
 *
 * TXT record contains:
 *   version=1
 *   codec=hevc
 *   resolution=3072x1920
 *   fps=60
 */

#define _GNU_SOURCE
#include "discovery.h"
#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <avahi-client/client.h>
#include <avahi-client/publish.h>
#include <avahi-common/thread-watch.h>
#include <avahi-common/error.h>
#include <avahi-common/alternative.h>
#include <avahi-common/malloc.h>

#define DISC_ERR(fmt, ...) fprintf(stderr, "[discovery] ERROR: " fmt "\n", ##__VA_ARGS__)
#define DISC_INF(fmt, ...) fprintf(stdout, "[discovery] " fmt "\n", ##__VA_ARGS__)
#define DISC_WRN(fmt, ...) fprintf(stderr, "[discovery] WARN: " fmt "\n", ##__VA_ARGS__)

#define SERVICE_TYPE "_display-bridge._tcp"
#define SERVICE_NAME "display-bridge-sender"

struct db_discovery {
    AvahiThreadedPoll   *threaded_poll;
    AvahiClient         *client;
    AvahiEntryGroup     *group;
    int                  port;
    char                *name;  /* service name (may change if collision) */
};

/* Forward declarations */
static void entry_group_callback(AvahiEntryGroup *g, AvahiEntryGroupState state,
                                  void *userdata);
static void client_callback(AvahiClient *client, AvahiClientState state,
                             void *userdata);
static void create_service(db_discovery_t *d);

/* ===== Service registration ===== */

static void create_service(db_discovery_t *d)
{
    int ret;

    /* Create entry group if needed */
    if (!d->group) {
        d->group = avahi_entry_group_new(d->client, entry_group_callback, d);
        if (!d->group) {
            DISC_ERR("avahi_entry_group_new failed: %s",
                     avahi_strerror(avahi_client_errno(d->client)));
            return;
        }
    }

    /* If the group is empty, add our service */
    if (avahi_entry_group_is_empty(d->group)) {
        char res_txt[64];
        char fps_txt[16];
        snprintf(res_txt, sizeof(res_txt), "resolution=%dx%d",
                 DB_TARGET_WIDTH, DB_TARGET_HEIGHT);
        snprintf(fps_txt, sizeof(fps_txt), "fps=%d", DB_TARGET_FPS);

        ret = avahi_entry_group_add_service(
            d->group,
            AVAHI_IF_UNSPEC,     /* all interfaces */
            AVAHI_PROTO_INET,    /* IPv4 only (simpler for our use case) */
            0,                   /* no flags */
            d->name,
            SERVICE_TYPE,
            NULL,                /* domain = .local */
            NULL,                /* host = auto */
            (uint16_t)d->port,
            "version=1",
            "codec=hevc",
            res_txt,
            fps_txt,
            NULL                 /* sentinel */
        );

        if (ret == AVAHI_ERR_COLLISION) {
            /* Name collision — pick alternative */
            char *alt = avahi_alternative_service_name(d->name);
            DISC_WRN("Service name collision, renaming '%s' → '%s'", d->name, alt);
            avahi_free(d->name);
            d->name = alt;
            avahi_entry_group_reset(d->group);
            create_service(d);
            return;
        }

        if (ret < 0) {
            DISC_ERR("avahi_entry_group_add_service failed: %s", avahi_strerror(ret));
            return;
        }

        /* Commit the entry group */
        ret = avahi_entry_group_commit(d->group);
        if (ret < 0) {
            DISC_ERR("avahi_entry_group_commit failed: %s", avahi_strerror(ret));
            return;
        }
    }
}

/* ===== Avahi callbacks ===== */

static void entry_group_callback(AvahiEntryGroup *g, AvahiEntryGroupState state,
                                  void *userdata)
{
    db_discovery_t *d = (db_discovery_t *)userdata;
    (void)g;

    switch (state) {
    case AVAHI_ENTRY_GROUP_ESTABLISHED:
        DISC_INF("Service '%s' (%s) established on port %d",
                 d->name, SERVICE_TYPE, d->port);
        break;

    case AVAHI_ENTRY_GROUP_COLLISION: {
        char *alt = avahi_alternative_service_name(d->name);
        DISC_WRN("Service name collision, renaming '%s' → '%s'", d->name, alt);
        avahi_free(d->name);
        d->name = alt;
        create_service(d);
        break;
    }

    case AVAHI_ENTRY_GROUP_FAILURE:
        DISC_ERR("Entry group failure: %s",
                 avahi_strerror(avahi_client_errno(d->client)));
        break;

    case AVAHI_ENTRY_GROUP_UNCOMMITED:
    case AVAHI_ENTRY_GROUP_REGISTERING:
        break;
    }
}

static void client_callback(AvahiClient *client, AvahiClientState state,
                             void *userdata)
{
    db_discovery_t *d = (db_discovery_t *)userdata;
    d->client = client;

    switch (state) {
    case AVAHI_CLIENT_S_RUNNING:
        /* Server is running — register our service */
        DISC_INF("Avahi client running, registering service...");
        create_service(d);
        break;

    case AVAHI_CLIENT_S_COLLISION:
    case AVAHI_CLIENT_S_REGISTERING:
        /* Server is re-registering — reset our group */
        if (d->group)
            avahi_entry_group_reset(d->group);
        break;

    case AVAHI_CLIENT_FAILURE:
        DISC_ERR("Avahi client failure: %s",
                 avahi_strerror(avahi_client_errno(client)));
        avahi_threaded_poll_quit(d->threaded_poll);
        break;

    case AVAHI_CLIENT_CONNECTING:
        DISC_INF("Connecting to Avahi daemon...");
        break;
    }
}

/* ===== Public API ===== */

db_discovery_t *db_discovery_start(int service_port)
{
    db_discovery_t *d = calloc(1, sizeof(db_discovery_t));
    if (!d) return NULL;

    d->port = service_port;
    d->name = avahi_strdup(SERVICE_NAME);
    if (!d->name) {
        free(d);
        return NULL;
    }

    /* Create threaded poll object */
    d->threaded_poll = avahi_threaded_poll_new();
    if (!d->threaded_poll) {
        DISC_ERR("avahi_threaded_poll_new failed");
        avahi_free(d->name);
        free(d);
        return NULL;
    }

    /* Create Avahi client */
    int error;
    d->client = avahi_client_new(
        avahi_threaded_poll_get(d->threaded_poll),
        AVAHI_CLIENT_NO_FAIL,  /* don't fail if daemon isn't running yet */
        client_callback, d,
        &error
    );

    if (!d->client) {
        DISC_ERR("avahi_client_new failed: %s", avahi_strerror(error));
        avahi_threaded_poll_free(d->threaded_poll);
        avahi_free(d->name);
        free(d);
        return NULL;
    }

    /* Start the background thread */
    if (avahi_threaded_poll_start(d->threaded_poll) < 0) {
        DISC_ERR("avahi_threaded_poll_start failed");
        avahi_client_free(d->client);
        avahi_threaded_poll_free(d->threaded_poll);
        avahi_free(d->name);
        free(d);
        return NULL;
    }

    DISC_INF("mDNS discovery started (service=%s, port=%d)", SERVICE_TYPE, service_port);
    return d;
}

void db_discovery_stop(db_discovery_t *d)
{
    if (!d) return;

    /* Stop the event loop thread */
    avahi_threaded_poll_stop(d->threaded_poll);

    /* Clean up */
    if (d->client)
        avahi_client_free(d->client);
    /* Note: freeing the client also frees the entry group */

    avahi_threaded_poll_free(d->threaded_poll);
    avahi_free(d->name);
    free(d);

    DISC_INF("mDNS discovery stopped");
}
