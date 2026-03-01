/*
 * main.c
 * display-bridge sender — pipeline orchestrator.
 *
 * Threading model:
 *   Thread 1 (main):    SRT accept + send loop, heartbeat, signal handling
 *   Thread 2 (capture): PipeWire event loop (db_capture_start blocks)
 *   Thread 3 (input):   UDP input event loop (db_input_start blocks)
 *
 * Data flow:
 *   PipeWire frame -> capture callback (Thread 2) -> encode (VAAPI, ~10ms)
 *   -> push to ring buffer -> Thread 1 drains ring buffer -> SRT send
 *
 * The ring buffer decouples PipeWire's real-time thread from SRT I/O.
 */

#define _GNU_SOURCE
#include "encoder.h"
#include "capture.h"
#include "streamer.h"
#include "input.h"
#include "discovery.h"
#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>

#define LOG_ERR(fmt, ...) fprintf(stderr, "[main] ERROR: " fmt "\n", ##__VA_ARGS__)
#define LOG_INF(fmt, ...) fprintf(stdout, "[main] " fmt "\n", ##__VA_ARGS__)
#define LOG_WRN(fmt, ...) fprintf(stderr, "[main] WARN: " fmt "\n", ##__VA_ARGS__)

/* ===== Ring buffer for encoded frames ===== */

#define RING_SIZE 8  /* power of 2 for efficiency */

typedef struct {
    uint8_t  *data;
    uint32_t  size;
    uint64_t  pts;
    int       is_keyframe;
} ring_entry_t;

typedef struct {
    ring_entry_t entries[RING_SIZE];
    volatile int head;  /* write index (capture thread) */
    volatile int tail;  /* read index (main thread) */
    pthread_mutex_t mutex;
    pthread_cond_t  cond;  /* signal main thread when frame available */
} ring_buffer_t;

static ring_buffer_t g_ring;

static void ring_init(ring_buffer_t *rb)
{
    memset(rb, 0, sizeof(*rb));
    pthread_mutex_init(&rb->mutex, NULL);
    pthread_cond_init(&rb->cond, NULL);
    /* Pre-allocate buffers for each slot (4MB each, enough for HEVC frames) */
    for (int i = 0; i < RING_SIZE; i++) {
        rb->entries[i].data = malloc(4 * 1024 * 1024);
        rb->entries[i].size = 0;
    }
}

static void ring_destroy(ring_buffer_t *rb)
{
    for (int i = 0; i < RING_SIZE; i++) {
        free(rb->entries[i].data);
    }
    pthread_mutex_destroy(&rb->mutex);
    pthread_cond_destroy(&rb->cond);
}

/* Push an encoded frame into the ring buffer. Called from capture thread. */
static int ring_push(ring_buffer_t *rb, const uint8_t *data, uint32_t size,
                     uint64_t pts, int is_keyframe)
{
    pthread_mutex_lock(&rb->mutex);
    int next_head = (rb->head + 1) % RING_SIZE;
    if (next_head == rb->tail) {
        /* Ring full — drop oldest frame */
        LOG_WRN("Ring buffer full, dropping oldest frame");
        rb->tail = (rb->tail + 1) % RING_SIZE;
    }

    ring_entry_t *e = &rb->entries[rb->head];
    if (size > 4 * 1024 * 1024) {
        /* Realloc if frame exceeds pre-allocated size */
        free(e->data);
        e->data = malloc(size);
        if (!e->data) {
            pthread_mutex_unlock(&rb->mutex);
            return -1;
        }
    }
    memcpy(e->data, data, size);
    e->size = size;
    e->pts = pts;
    e->is_keyframe = is_keyframe;
    rb->head = next_head;

    pthread_cond_signal(&rb->cond);
    pthread_mutex_unlock(&rb->mutex);
    return 0;
}

/* Pop an encoded frame from the ring buffer. Called from main thread.
 * Returns pointer to internal buffer (valid until next pop). */
static ring_entry_t *ring_pop(ring_buffer_t *rb, int timeout_ms)
{
    pthread_mutex_lock(&rb->mutex);
    while (rb->head == rb->tail) {
        if (timeout_ms <= 0) {
            pthread_mutex_unlock(&rb->mutex);
            return NULL;
        }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += (long)timeout_ms * 1000000L;
        ts.tv_sec += ts.tv_nsec / 1000000000L;
        ts.tv_nsec %= 1000000000L;
        int ret = pthread_cond_timedwait(&rb->cond, &rb->mutex, &ts);
        if (ret == ETIMEDOUT) {
            pthread_mutex_unlock(&rb->mutex);
            return NULL;
        }
    }

    ring_entry_t *e = &rb->entries[rb->tail];
    rb->tail = (rb->tail + 1) % RING_SIZE;
    pthread_mutex_unlock(&rb->mutex);
    return e;
}

/* ===== Global state ===== */

static volatile int g_running = 1;
static db_encoder_t  g_encoder;
static db_capture_t *g_capture = NULL;
static db_streamer_t *g_streamer = NULL;
static db_input_t   *g_input = NULL;
static db_discovery_t *g_discovery = NULL;

/* Monotonic time in microseconds */
static uint64_t get_time_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/* ===== Capture callback ===== */

/*
 * Called from PipeWire's thread for each captured frame.
 *
 * Convention from capture.c:
 *   - If data == NULL: this is a DMA-BUF frame. stride = dmabuf_fd.
 *     Call db_encoder_encode_dmabuf().
 *   - If data != NULL: this is a CPU-accessible BGRx frame.
 *     Call db_encoder_encode_bgrx().
 */
static void on_frame_captured(const uint8_t *data, int width, int height,
                              int stride, void *userdata)
{
    (void)userdata;

    /* Guard: reject frames from the wrong display (e.g. eDP-1 selected in
     * portal instead of Virtual-1).  Without this check a size mismatch
     * causes the BGRx→NV12 loop to read past the PipeWire buffer and
     * segfault. */
    if (width != DB_TARGET_WIDTH || height != DB_TARGET_HEIGHT) {
        static int warned = 0;
        if (!warned) {
            fprintf(stderr,
                "[main] ERROR: Capture size mismatch: got %dx%d, expected %dx%d\n"
                "[main] ERROR: Wrong display selected in portal!\n"
                "[main] ERROR: Delete ~/.config/display-bridge/portal_token and restart,\n"
                "[main] ERROR: then select the Virtual-1 (2880x1800) display — NOT Built-in.\n",
                width, height, DB_TARGET_WIDTH, DB_TARGET_HEIGHT);
            warned = 1;
        }
        return;
    }

    uint8_t *out_buf = NULL;
    uint32_t out_size = 0;
    int is_keyframe = 0;
    int ret;

    if (data == NULL) {
        /* DMA-BUF zero-copy path.
         * stride is actually the DMA-BUF fd (from capture.c convention).
         * We pass DRM_FORMAT_MOD_LINEAR — if PipeWire uses tiled formats,
         * this may need adjustment. */
        int dmabuf_fd = stride;
        uint32_t actual_stride = (uint32_t)(width);  /* NV12 Y stride = width */
        /* For NV12, the stride is typically aligned to some boundary.
         * PipeWire should provide this via the buffer metadata.
         * For now, assume stride = width (linear layout). */
        ret = db_encoder_encode_dmabuf(&g_encoder, dmabuf_fd,
                                       actual_stride, 0,
                                       0 /* DRM_FORMAT_MOD_LINEAR */,
                                       &out_buf, &out_size, &is_keyframe);
    } else {
        /* CPU fallback path — PipeWire delivers BGRx frames */
        ret = db_encoder_encode_bgrx(&g_encoder, data, (uint32_t)stride,
                                     &out_buf, &out_size, &is_keyframe);
    }

    if (ret != 0 || !out_buf || out_size == 0) {
        if (ret != 0)
            LOG_ERR("Encode failed");
        return;
    }

    /* Push encoded frame into ring buffer for the main thread to send */
    uint64_t pts = get_time_us();
    ring_push(&g_ring, out_buf, out_size, pts, is_keyframe);

    /* Release the VAAPI mapped buffer */
    db_encoder_release_frame(&g_encoder);
}

/* ===== Thread entry points ===== */

static void *capture_thread(void *arg)
{
    (void)arg;
    LOG_INF("Capture thread started");
    db_capture_start(g_capture);
    LOG_INF("Capture thread exited");
    return NULL;
}

static void *input_thread(void *arg)
{
    (void)arg;
    LOG_INF("Input thread started");
    db_input_start(g_input);
    LOG_INF("Input thread exited");
    return NULL;
}

/* ===== Signal handling ===== */

static void signal_handler(int sig)
{
    (void)sig;
    LOG_INF("Signal %d received, shutting down...", sig);
    g_running = 0;

    /* Break the PipeWire loop and input loop */
    if (g_capture) db_capture_stop(g_capture);
    if (g_input)   db_input_stop(g_input);
}

/* ===== Main ===== */

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    LOG_INF("display-bridge sender starting");
    LOG_INF("Target: %dx%d @ %dfps, HEVC %d kbps, SRT latency %dms",
            DB_TARGET_WIDTH, DB_TARGET_HEIGHT, DB_TARGET_FPS,
            DB_BITRATE_KBPS, DB_SRT_LATENCY_MS);

    /* Install signal handlers */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    /* Initialize ring buffer */
    ring_init(&g_ring);

    /* --- Step 1: Initialize encoder --- */
    LOG_INF("Initializing VAAPI encoder...");
    if (db_encoder_init(&g_encoder, DB_TARGET_WIDTH, DB_TARGET_HEIGHT,
                        DB_BITRATE_KBPS, DB_TARGET_FPS,
                        DB_CODEC_HEVC) != 0) {
        LOG_ERR("Failed to initialize encoder");
        return 1;
    }

    /* --- Step 2: Initialize capture --- */
    LOG_INF("Initializing PipeWire capture...");
    g_capture = db_capture_init(DB_TARGET_WIDTH, DB_TARGET_HEIGHT,
                                on_frame_captured, NULL);
    if (!g_capture) {
        LOG_ERR("Failed to initialize capture");
        db_encoder_destroy(&g_encoder);
        return 1;
    }

    /* --- Step 3: Initialize SRT streamer --- */
    LOG_INF("Initializing SRT streamer...");
    g_streamer = db_streamer_init(DB_DEFAULT_VIDEO_PORT, DB_SRT_LATENCY_MS);
    if (!g_streamer) {
        LOG_ERR("Failed to initialize streamer");
        db_capture_destroy(g_capture);
        db_encoder_destroy(&g_encoder);
        return 1;
    }

    /* --- Step 4: Initialize input handler --- */
    LOG_INF("Initializing input handler...");
    g_input = db_input_init(DB_DEFAULT_INPUT_PORT);
    if (!g_input) {
        LOG_WRN("Failed to initialize input handler (non-fatal, continuing without input)");
        /* Not fatal — we can still stream video without input forwarding */
    }

    /* --- Step 5: Start mDNS discovery advertisement --- */
    LOG_INF("Starting mDNS service advertisement...");
    g_discovery = db_discovery_start(DB_DEFAULT_VIDEO_PORT);
    if (!g_discovery) {
        LOG_WRN("Failed to start mDNS discovery (non-fatal, receiver must use IP)");
        /* Non-fatal — receiver can still connect by IP address */
    }

    /* --- Step 6: Start worker threads --- */
    pthread_t cap_tid, inp_tid;
    int has_input_thread = 0;

    if (pthread_create(&cap_tid, NULL, capture_thread, NULL) != 0) {
        LOG_ERR("Failed to create capture thread");
        goto cleanup;
    }

    if (g_input) {
        if (pthread_create(&inp_tid, NULL, input_thread, NULL) != 0) {
            LOG_WRN("Failed to create input thread");
        } else {
            has_input_thread = 1;
        }
    }

    /* --- Step 7: Main loop — accept connections and send frames --- */
    LOG_INF("Entering main loop...");
    while (g_running) {
        /* Wait for a receiver to connect */
        LOG_INF("Waiting for Mac receiver on SRT port %d...",
                DB_DEFAULT_VIDEO_PORT);
        if (db_streamer_accept(g_streamer, 5000) != 0) {
            /* Timeout or error — retry */
            if (g_running) continue;
            break;
        }

        /* Force IDR on new connection so receiver can start decoding */
        db_encoder_force_idr(&g_encoder);
        LOG_INF("Receiver connected — streaming");

        uint64_t last_send_time = get_time_us();
        uint64_t last_stats_time = get_time_us();
        int current_bitrate = 15000;
        db_encoder_set_bitrate(&g_encoder, current_bitrate);
        int prev_loss = 0;

        /* Stream loop — drain ring buffer and send to receiver */
        while (g_running && db_streamer_is_connected(g_streamer)) {
            ring_entry_t *entry = ring_pop(&g_ring, 100);

            if (entry && entry->size > 0) {
                int ret = db_streamer_send(g_streamer, entry->data,
                                           entry->size, entry->pts,
                                           entry->is_keyframe);
                if (ret != 0) {
                    LOG_ERR("Send failed, receiver may have disconnected");
                    break;
                }
                last_send_time = get_time_us();
            } else {
                /* No frame available — check if we need a heartbeat */
                uint64_t now = get_time_us();
                if (now - last_send_time > 1000000) {
                    /* Keep connection alive when no video is being sent */
                    if (db_streamer_send_heartbeat(g_streamer) != 0) {
                        LOG_ERR("Heartbeat send failed, receiver may have disconnected");
                        break;
                    }
                    last_send_time = now;
                }
            }

            /* --- Adaptive bitrate: poll SRT stats every 2 seconds --- */
            uint64_t now_abs = get_time_us();
            if (now_abs - last_stats_time > 2000000) {
                last_stats_time = now_abs;
                db_streamer_stats_t stats;
                if (db_streamer_get_stats(g_streamer, &stats) == 0) {
                    int new_loss = stats.pkt_loss_total;
                    int loss_delta = new_loss - prev_loss;
                    prev_loss = new_loss;

                    int target = current_bitrate;

                    if (loss_delta > 5 || stats.rtt_ms > 50.0) {
                        /* Network congestion — reduce bitrate by 40%, min 5 Mbps */
                        target = current_bitrate * 60 / 100;
                        if (target < 5000) target = 5000;
                        LOG_WRN("Congestion detected (loss=%d, rtt=%.1fms) -> %d kbps",
                                loss_delta, stats.rtt_ms, target);
                    } else if (loss_delta == 0 && stats.rtt_ms < 20.0 &&
                               current_bitrate < DB_BITRATE_KBPS) {
                        /* Network healthy — ramp up by 10%, max DB_BITRATE_KBPS */
                        target = current_bitrate * 115 / 100;
                        if (target > DB_BITRATE_KBPS) target = DB_BITRATE_KBPS;
                    }

                    if (target != current_bitrate) {
                        current_bitrate = target;
                        db_encoder_set_bitrate(&g_encoder, current_bitrate);
                    }
                }
            }
        }

        LOG_INF("Receiver disconnected, waiting for reconnect...");
    }

cleanup:
    LOG_INF("Shutting down...");

    /* Stop threads */
    if (g_capture)
        db_capture_stop(g_capture);
    if (g_input)
        db_input_stop(g_input);

    pthread_join(cap_tid, NULL);
    if (has_input_thread)
        pthread_join(inp_tid, NULL);

    /* Destroy everything */
    if (g_discovery) db_discovery_stop(g_discovery);
    if (g_input)    db_input_destroy(g_input);
    if (g_streamer) db_streamer_destroy(g_streamer);
    if (g_capture)  db_capture_destroy(g_capture);
    db_encoder_destroy(&g_encoder);
    ring_destroy(&g_ring);

    LOG_INF("display-bridge sender stopped");
    return 0;
}
