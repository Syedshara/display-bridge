/*
 * discovery.h
 * mDNS service advertisement via Avahi.
 *
 * Publishes "_display-bridge._tcp" service so the Mac receiver
 * can auto-discover the sender's IP address via Bonjour.
 */

#ifndef DB_DISCOVERY_H
#define DB_DISCOVERY_H

typedef struct db_discovery db_discovery_t;

/* Initialize and start mDNS service advertisement.
 * service_port: the SRT listen port to advertise (typically 5000).
 * Returns opaque discovery handle, or NULL on failure.
 * The Avahi event loop runs in its own background thread. */
db_discovery_t *db_discovery_start(int service_port);

/* Stop advertising and free resources. */
void db_discovery_stop(db_discovery_t *d);

#endif /* DB_DISCOVERY_H */
