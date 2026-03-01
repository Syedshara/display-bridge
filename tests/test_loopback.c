#define _GNU_SOURCE
/*
 * test_loopback.c
 * Self-contained loopback test: VAAPI encode → SRT stream → receiver thread.
 * No external tools (ffplay/ffprobe) needed.
 *
 * Validates:
 *  - VAAPI HEVC encoder init and encode at target resolution
 *  - SRT listener, accept, and frame packing
 *  - SRT file+message mode: large frames (>1316 bytes) survive round-trip
 *  - Protocol header packing/unpacking
 *  - End-to-end bitstream delivery
 *
 * Usage:  ./test_loopback [frames]   (default 30 frames)
 */

#include "encoder.h"
#include "streamer.h"
#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <arpa/inet.h>

#include <srt/srt.h>

static volatile int g_running = 1;
static void on_signal(int s) { (void)s; g_running = 0; }

/* ===== Receiver thread ===== */

typedef struct {
    int           port;
    int           latency_ms;
    int           frames_received;
    int           bytes_received;
    int           keyframes_received;
    int           errors;
    int           done;         /* set by main thread when sender finishes */
    pthread_t     thread;
} receiver_ctx_t;

static void *receiver_thread(void *arg)
{
    receiver_ctx_t *rx = (receiver_ctx_t *)arg;
    rx->frames_received = 0;
    rx->bytes_received  = 0;
    rx->keyframes_received = 0;
    rx->errors = 0;

    /* Small delay to let the listener start first */
    usleep(200000);

    /* Create SRT caller socket */
    SRTSOCKET sock = srt_create_socket();
    if (sock == SRT_INVALID_SOCK) {
        fprintf(stderr, "[rx] srt_create_socket failed: %s\n", srt_getlasterror_str());
        rx->errors++;
        return NULL;
    }

    /* Match sender: file mode + message API */
    int transtype = SRTT_FILE;
    srt_setsockflag(sock, SRTO_TRANSTYPE, &transtype, sizeof(transtype));
    int msgapi = 1;
    srt_setsockflag(sock, SRTO_MESSAGEAPI, &msgapi, sizeof(msgapi));
    srt_setsockflag(sock, SRTO_LATENCY, &rx->latency_ms, sizeof(rx->latency_ms));
    int yes = 1;
    srt_setsockflag(sock, SRTO_RCVSYN, &yes, sizeof(yes));

    /* Connect to sender */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)rx->port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (srt_connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "[rx] srt_connect failed: %s\n", srt_getlasterror_str());
        srt_close(sock);
        rx->errors++;
        return NULL;
    }
    printf("[rx] Connected to sender\n");

    /* Receive loop */
    uint8_t *buf = malloc(8 * 1024 * 1024);  /* 8MB buffer */
    if (!buf) {
        srt_close(sock);
        rx->errors++;
        return NULL;
    }

    while (!rx->done || 1) {
        /* Use epoll with a timeout so we can check the done flag */
        int epoll = srt_epoll_create();
        int events = SRT_EPOLL_IN;
        srt_epoll_add_usock(epoll, sock, &events);

        SRT_EPOLL_EVENT ready[1];
        int ret = srt_epoll_uwait(epoll, ready, 1, 1000);  /* 1s timeout */
        srt_epoll_release(epoll);

        if (ret <= 0) {
            if (rx->done) break;
            continue;
        }

        int rcvd = srt_recvmsg2(sock, (char *)buf, 8 * 1024 * 1024, NULL);
        if (rcvd <= 0) {
            SRT_SOCKSTATUS state = srt_getsockstate(sock);
            if (state == SRTS_BROKEN || state == SRTS_CLOSED ||
                state == SRTS_NONEXIST) {
                printf("[rx] Connection closed by sender\n");
                break;
            }
            if (rx->done) break;
            continue;
        }

        /* Validate packet structure */
        if (rcvd < (int)(sizeof(db_packet_header_t) + sizeof(db_video_header_t))) {
            fprintf(stderr, "[rx] Packet too small: %d bytes\n", rcvd);
            rx->errors++;
            continue;
        }

        db_packet_header_t *pkt = (db_packet_header_t *)buf;
        db_video_header_t *vid = (db_video_header_t *)(buf + sizeof(db_packet_header_t));

        if (pkt->type != DB_PKT_VIDEO_FRAME) {
            fprintf(stderr, "[rx] Unexpected packet type: 0x%02x\n", pkt->type);
            rx->errors++;
            continue;
        }

        int payload_data_size = rcvd - (int)(sizeof(db_packet_header_t) +
                                              sizeof(db_video_header_t));
        if (payload_data_size <= 0) {
            fprintf(stderr, "[rx] Empty HEVC payload\n");
            rx->errors++;
            continue;
        }

        rx->frames_received++;
        rx->bytes_received += rcvd;
        if (vid->keyframe)
            rx->keyframes_received++;

        if (rx->frames_received <= 5 || rx->frames_received % 30 == 0) {
            printf("[rx] Frame %d: %d bytes total, HEVC=%d bytes, codec=%d, kf=%d, seq=%d\n",
                   rx->frames_received, rcvd, payload_data_size,
                   vid->codec, vid->keyframe, pkt->seq);
        }

        if (rx->done && rx->frames_received >= rx->done)
            break;
    }

    free(buf);
    srt_close(sock);
    return NULL;
}

/* ===== Generate NV12 test pattern ===== */

static void generate_nv12(uint8_t *buf, int w, int h, int frame_num)
{
    int y_size = w * h;
    int bar_x = (frame_num * 4) % w;
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            int v = (col * 255 / w);
            if (col >= bar_x && col < bar_x + 16)
                v = 235;
            buf[row * w + col] = (uint8_t)v;
        }
    }
    memset(buf + y_size, 128, y_size / 2);
}

/* ===== Main: sender ===== */

int main(int argc, char *argv[])
{
    int total_frames = 30;
    if (argc > 1) total_frames = atoi(argv[1]);
    if (total_frames <= 0) total_frames = 30;

    int width  = 1920;
    int height = 1080;
    int fps    = 30;
    int bitrate_kbps = 8000;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    printf("[loopback] Self-contained test: encode %d frames → SRT → receiver thread\n",
           total_frames);

    /* Start encoder */
    printf("[loopback] Initializing VAAPI HEVC encoder (%dx%d @ %dfps)...\n",
           width, height, fps);
    db_encoder_t enc;
    if (db_encoder_init(&enc, width, height, bitrate_kbps, fps, DB_CODEC_HEVC) != 0) {
        fprintf(stderr, "[loopback] FAIL: encoder init\n");
        srt_cleanup();
        return 1;
    }
    printf("[loopback] Encoder OK\n");

    /* Start SRT streamer (listener) */
    printf("[loopback] Starting SRT listener on port %d...\n", DB_DEFAULT_VIDEO_PORT);
    db_streamer_t *strm = db_streamer_init(DB_DEFAULT_VIDEO_PORT, DB_SRT_LATENCY_MS);
    if (!strm) {
        fprintf(stderr, "[loopback] FAIL: streamer init\n");
        db_encoder_destroy(&enc);
        srt_cleanup();
        return 1;
    }

    /* Spawn receiver thread */
    receiver_ctx_t rx = {
        .port = DB_DEFAULT_VIDEO_PORT,
        .latency_ms = DB_SRT_LATENCY_MS,
        .done = 0
    };
    pthread_create(&rx.thread, NULL, receiver_thread, &rx);

    /* Wait for receiver to connect */
    if (db_streamer_accept(strm, 10000) != 0) {
        fprintf(stderr, "[loopback] FAIL: no receiver connected within 10s\n");
        rx.done = -1;
        pthread_join(rx.thread, NULL);
        db_streamer_destroy(strm);
        db_encoder_destroy(&enc);
        srt_cleanup();
        return 1;
    }
    printf("[loopback] Receiver connected\n");

    /* Allocate NV12 buffer */
    int nv12_size = width * height * 3 / 2;
    uint8_t *nv12_buf = malloc(nv12_size);
    if (!nv12_buf) {
        fprintf(stderr, "[loopback] FAIL: malloc\n");
        rx.done = -1;
        pthread_join(rx.thread, NULL);
        db_streamer_destroy(strm);
        db_encoder_destroy(&enc);
        srt_cleanup();
        return 1;
    }

    /* Encode + send loop */
    int sent_count = 0;
    int send_errors = 0;

    printf("[loopback] Encoding and streaming %d frames...\n", total_frames);

    for (int i = 0; i < total_frames && g_running; i++) {
        generate_nv12(nv12_buf, width, height, i);

        uint8_t *out_buf = NULL;
        uint32_t out_size = 0;
        int is_keyframe = 0;

        if (db_encoder_encode_nv12(&enc, nv12_buf, width,
                                   &out_buf, &out_size, &is_keyframe) != 0) {
            fprintf(stderr, "[loopback] WARNING: encode failed on frame %d\n", i);
            send_errors++;
            continue;
        }

        uint64_t pts = (uint64_t)i * 1000000ULL / (uint64_t)fps;
        if (db_streamer_send(strm, out_buf, out_size, pts, is_keyframe) != 0) {
            fprintf(stderr, "[loopback] WARNING: send failed on frame %d (%u bytes)\n",
                    i, out_size);
            db_encoder_release_frame(&enc);
            send_errors++;
            continue;
        }

        db_encoder_release_frame(&enc);
        sent_count++;

        if (i < 5 || (i + 1) % 30 == 0) {
            printf("[loopback] Sent frame %d: %u bytes, keyframe=%d\n",
                   i, out_size, is_keyframe);
        }
    }

    /* Signal receiver to finish and wait */
    printf("[loopback] Sender done. Waiting for receiver to drain...\n");
    rx.done = sent_count;
    usleep(500000);  /* Give receiver 500ms to drain */
    pthread_join(rx.thread, NULL);

    /* Results */
    printf("\n");
    printf("[loopback] ===== RESULTS =====\n");
    printf("[loopback] Frames sent:       %d / %d\n", sent_count, total_frames);
    printf("[loopback] Send errors:       %d\n", send_errors);
    printf("[loopback] Frames received:   %d\n", rx.frames_received);
    printf("[loopback] Bytes received:    %d\n", rx.bytes_received);
    printf("[loopback] Keyframes:         %d\n", rx.keyframes_received);
    printf("[loopback] Receive errors:    %d\n", rx.errors);

    int pass = (sent_count == total_frames &&
                send_errors == 0 &&
                rx.frames_received == sent_count &&
                rx.errors == 0 &&
                rx.keyframes_received >= 1);

    printf("[loopback] %s\n", pass ? "PASS" : "FAIL");

    free(nv12_buf);
    db_streamer_destroy(strm);
    db_encoder_destroy(&enc);
    /* Note: srt_cleanup() already called by db_streamer_destroy */

    return pass ? 0 : 1;
}
