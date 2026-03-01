/*
 * streamer.h
 * SRT streaming — sends encoded video frames to receiver.
 */

#ifndef DB_STREAMER_H
#define DB_STREAMER_H

#include <stdint.h>

typedef struct db_streamer db_streamer_t;

/* Initialize SRT streamer.
 * listen_port: port to listen on for incoming connections.
 * latency_ms: SRT latency (lower = less buffer, more risk).
 * Returns opaque streamer handle, or NULL on failure. */
db_streamer_t *db_streamer_init(int listen_port, int latency_ms);

/* Wait for a receiver to connect. Blocks until connection or timeout. */
int db_streamer_accept(db_streamer_t *s, int timeout_ms);

/* Send an encoded frame.
 * data/size: encoded bitstream.
 * pts: presentation timestamp (microseconds).
 * is_keyframe: 1 if IDR frame.
 * Returns 0 on success. */
int db_streamer_send(db_streamer_t *s, const uint8_t *data, uint32_t size,
                     uint64_t pts, int is_keyframe);

/* Send a heartbeat packet (no payload, just packet header).
 * Should be called when no video has been sent for >1 second.
 * Returns 0 on success. */
int db_streamer_send_heartbeat(db_streamer_t *s);

/* Check if receiver is still connected. */
int db_streamer_is_connected(db_streamer_t *s);

/* SRT connection stats for adaptive bitrate. */
typedef struct {
    double rtt_ms;           /* Round-trip time in ms */
    int    pkt_loss_total;   /* Total packets lost */
    double send_rate_mbps;   /* Send rate in Mbps */
    int    avail_send_buf;   /* Available sender buffer (bytes) */
} db_streamer_stats_t;

/* Query current SRT connection stats.
 * Returns 0 on success, -1 if not connected. */
int db_streamer_get_stats(db_streamer_t *s, db_streamer_stats_t *out);

/* Close connection and free resources. */
void db_streamer_destroy(db_streamer_t *s);

#endif /* DB_STREAMER_H */
