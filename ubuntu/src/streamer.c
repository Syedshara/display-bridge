/*
 * streamer.c
 * SRT streaming — sends encoded HEVC video frames to the Mac receiver.
 *
 * Architecture:
 *   - Sender runs in SRT listener (server) mode on a configured port.
 *   - Mac receiver connects as SRT caller (client).
 *   - Each encoded frame is sent as: db_packet_header_t + db_video_header_t
 *     + raw HEVC bitstream. SRT's message API handles framing.
 *   - Heartbeat packets are sent when no video data has been sent for >1s.
 *   - If the connection drops, the streamer goes back to accept() without
 *     tearing down the capture/encoder pipeline.
 */

#define _GNU_SOURCE
#include "streamer.h"
#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <arpa/inet.h>

#include <srt/srt.h>

#define STRM_ERR(fmt, ...) fprintf(stderr, "[streamer] ERROR: " fmt "\n", ##__VA_ARGS__)
#define STRM_INF(fmt, ...) fprintf(stdout, "[streamer] " fmt "\n", ##__VA_ARGS__)

/* Maximum SRT payload per message.
 * SRT can handle up to ~1.5GB per message in message mode.
 * We cap at 8MB which is more than enough for a single HEVC frame. */
#define MAX_FRAME_SIZE (8 * 1024 * 1024)

/* Internal send buffer: header + payload */
#define SEND_BUF_SIZE (sizeof(db_packet_header_t) + sizeof(db_video_header_t) + MAX_FRAME_SIZE)

struct db_streamer {
    SRTSOCKET   listen_sock;
    SRTSOCKET   client_sock;
    int         listen_port;
    int         latency_ms;
    int         connected;
    uint16_t    seq;
    uint64_t    start_time_us;
    uint8_t    *send_buf;     /* Pre-allocated send buffer */
};

/* Get monotonic time in microseconds */
static uint64_t get_time_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/* ========== Public API ========== */

db_streamer_t *db_streamer_init(int listen_port, int latency_ms)
{
    db_streamer_t *s = calloc(1, sizeof(db_streamer_t));
    if (!s) return NULL;

    s->listen_port = listen_port;
    s->latency_ms  = latency_ms;
    s->listen_sock = SRT_INVALID_SOCK;
    s->client_sock = SRT_INVALID_SOCK;
    s->connected   = 0;
    s->seq         = 0;
    s->start_time_us = get_time_us();

    /* Pre-allocate send buffer */
    s->send_buf = malloc(SEND_BUF_SIZE);
    if (!s->send_buf) {
        free(s);
        return NULL;
    }

    /* Initialize SRT library */
    if (srt_startup() != 0) {
        STRM_ERR("srt_startup failed");
        free(s->send_buf);
        free(s);
        return NULL;
    }

    /* Create listener socket */
    s->listen_sock = srt_create_socket();
    if (s->listen_sock == SRT_INVALID_SOCK) {
        STRM_ERR("srt_create_socket failed: %s", srt_getlasterror_str());
        free(s->send_buf);
        free(s);
        srt_cleanup();
        return NULL;
    }

    /* Configure socket options */
    int yes = 1;
    srt_setsockflag(s->listen_sock, SRTO_RCVSYN, &yes, sizeof(yes));  /* sync recv */
    srt_setsockflag(s->listen_sock, SRTO_SNDSYN, &yes, sizeof(yes));  /* sync send */
    srt_setsockflag(s->listen_sock, SRTO_LATENCY, &latency_ms, sizeof(latency_ms));

    /* Use file mode + message API: allows arbitrary-size messages (SRT
     * fragments internally across UDP packets).  SRTT_LIVE limits each
     * srt_sendmsg2 to ≤1316 bytes which is too small for HEVC IDR frames.
     * File+message mode gives us reliable, message-oriented, low-latency
     * delivery — the same semantics but without the payload cap. */
    int transtype = SRTT_FILE;
    srt_setsockflag(s->listen_sock, SRTO_TRANSTYPE, &transtype, sizeof(transtype));

    int msgapi = 1;
    srt_setsockflag(s->listen_sock, SRTO_MESSAGEAPI, &msgapi, sizeof(msgapi));

    /* Dual-stack: accept both IPv4 and IPv6 callers.
     * SRTO_IPV6ONLY=0 must be set before srt_bind. */
    int ipv6only = 0;
    srt_setsockflag(s->listen_sock, SRTO_IPV6ONLY, &ipv6only, sizeof(ipv6only));

    /* Send buffer: accommodate 200ms of data at peak bitrate + retransmissions. */
    int sndbuf = 16 * 1024 * 1024;
    srt_setsockflag(s->listen_sock, SRTO_SNDBUF, &sndbuf, sizeof(sndbuf));

    /* Send timeout: drop the frame rather than block the pipeline */
    int sndtimeo = 50;
    srt_setsockflag(s->listen_sock, SRTO_SNDTIMEO, &sndtimeo, sizeof(sndtimeo));

    /* Bind to all interfaces (IPv4 + IPv6) */
    struct sockaddr_in6 addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin6_family = AF_INET6;
    addr.sin6_port   = htons((uint16_t)listen_port);
    addr.sin6_addr   = in6addr_any;

    if (srt_bind(s->listen_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        STRM_ERR("srt_bind failed on port %d: %s", listen_port,
                srt_getlasterror_str());
        srt_close(s->listen_sock);
        free(s->send_buf);
        free(s);
        srt_cleanup();
        return NULL;
    }

    if (srt_listen(s->listen_sock, 1) != 0) {
        STRM_ERR("srt_listen failed: %s", srt_getlasterror_str());
        srt_close(s->listen_sock);
        free(s->send_buf);
        free(s);
        srt_cleanup();
        return NULL;
    }

    STRM_INF("SRT listener ready on port %d (latency=%dms)", listen_port,
            latency_ms);
    return s;
}

int db_streamer_accept(db_streamer_t *s, int timeout_ms)
{
    if (!s) return -1;

    /* If already connected, disconnect first */
    if (s->client_sock != SRT_INVALID_SOCK) {
        srt_close(s->client_sock);
        s->client_sock = SRT_INVALID_SOCK;
        s->connected = 0;
    }

    STRM_INF("Waiting for receiver connection (timeout=%dms)...", timeout_ms);

    /* Use epoll for timeout-based accept */
    int epoll_id = srt_epoll_create();
    if (epoll_id < 0) {
        STRM_ERR("srt_epoll_create failed");
        return -1;
    }

    int events = SRT_EPOLL_IN;
    srt_epoll_add_usock(epoll_id, s->listen_sock, &events);

    SRT_EPOLL_EVENT ready[1];
    int rlen = 1;
    int ret = srt_epoll_uwait(epoll_id, ready, rlen,
                               timeout_ms > 0 ? (int64_t)timeout_ms : -1);
    srt_epoll_release(epoll_id);

    if (ret <= 0) {
        if (ret == 0)
            STRM_INF("Accept timed out");
        else
            STRM_ERR("srt_epoll_uwait failed: %s", srt_getlasterror_str());
        return -1;
    }

    /* Accept the connection */
    struct sockaddr_storage client_addr;
    int addr_len = sizeof(client_addr);
    s->client_sock = srt_accept(s->listen_sock,
                                (struct sockaddr *)&client_addr, &addr_len);
    if (s->client_sock == SRT_INVALID_SOCK) {
        STRM_ERR("srt_accept failed: %s", srt_getlasterror_str());
        return -1;
    }

    /* Apply latency to the accepted socket too */
    srt_setsockflag(s->client_sock, SRTO_LATENCY,
                    &s->latency_ms, sizeof(s->latency_ms));

    char ip_str[INET6_ADDRSTRLEN];
    if (client_addr.ss_family == AF_INET6) {
        struct sockaddr_in6 *c6 = (struct sockaddr_in6 *)&client_addr;
        inet_ntop(AF_INET6, &c6->sin6_addr, ip_str, sizeof(ip_str));
        STRM_INF("Receiver connected from [%s]:%d", ip_str,
                ntohs(c6->sin6_port));
    } else {
        struct sockaddr_in *c4 = (struct sockaddr_in *)&client_addr;
        inet_ntop(AF_INET, &c4->sin_addr, ip_str, sizeof(ip_str));
        STRM_INF("Receiver connected from %s:%d", ip_str,
                ntohs(c4->sin_port));
    }

    s->connected = 1;
    s->seq = 0;
    s->start_time_us = get_time_us();
    return 0;
}

int db_streamer_send(db_streamer_t *s, const uint8_t *data, uint32_t size,
                     uint64_t pts, int is_keyframe)
{
    if (!s || !s->connected || s->client_sock == SRT_INVALID_SOCK)
        return -1;

    if (size > MAX_FRAME_SIZE) {
        STRM_ERR("Frame too large: %u > %d", size, MAX_FRAME_SIZE);
        return -1;
    }

    /* Build packet header */
    db_packet_header_t *pkt = (db_packet_header_t *)s->send_buf;
    memset(pkt, 0, sizeof(*pkt));
    pkt->type = DB_PKT_VIDEO_FRAME;
    pkt->seq = s->seq++;
    pkt->timestamp = (uint32_t)((get_time_us() - s->start_time_us) & 0xFFFFFFFF);
    pkt->payload_size = sizeof(db_video_header_t) + size;

    /* Build video header */
    db_video_header_t *vid = (db_video_header_t *)(s->send_buf +
                              sizeof(db_packet_header_t));
    memset(vid, 0, sizeof(*vid));
    vid->width = DB_TARGET_WIDTH;
    vid->height = DB_TARGET_HEIGHT;
    vid->codec = DB_CODEC_HEVC;
    vid->keyframe = is_keyframe ? 1 : 0;
    vid->pts = pts;

    /* Copy encoded bitstream */
    memcpy(s->send_buf + sizeof(db_packet_header_t) + sizeof(db_video_header_t),
           data, size);

    int total = (int)(sizeof(db_packet_header_t) + sizeof(db_video_header_t) + size);

    /* SRT send — message mode, entire frame in one call */
    int sent = srt_sendmsg2(s->client_sock, (const char *)s->send_buf,
                            total, NULL);
    if (sent < 0) {
        SRT_SOCKSTATUS state = srt_getsockstate(s->client_sock);
        if (state == SRTS_BROKEN || state == SRTS_CLOSED ||
            state == SRTS_NONEXIST) {
            STRM_ERR("Connection lost");
            s->connected = 0;
            srt_close(s->client_sock);
            s->client_sock = SRT_INVALID_SOCK;
        } else {
            STRM_ERR("srt_sendmsg2 failed: %s", srt_getlasterror_str());
        }
        return -1;
    }

    return 0;
}

int db_streamer_send_heartbeat(db_streamer_t *s)
{
    if (!s || !s->connected || s->client_sock == SRT_INVALID_SOCK)
        return -1;

    /* Build a heartbeat packet: just a packet header, no payload */
    db_packet_header_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = DB_PKT_HEARTBEAT;
    pkt.seq = s->seq++;
    pkt.timestamp = (uint32_t)((get_time_us() - s->start_time_us) & 0xFFFFFFFF);
    pkt.payload_size = 0;

    int sent = srt_sendmsg2(s->client_sock, (const char *)&pkt,
                            (int)sizeof(pkt), NULL);
    if (sent < 0) {
        SRT_SOCKSTATUS state = srt_getsockstate(s->client_sock);
        if (state == SRTS_BROKEN || state == SRTS_CLOSED ||
            state == SRTS_NONEXIST) {
            STRM_ERR("Connection lost during heartbeat");
            s->connected = 0;
            srt_close(s->client_sock);
            s->client_sock = SRT_INVALID_SOCK;
        }
        return -1;
    }

    return 0;
}

int db_streamer_is_connected(db_streamer_t *s)
{
    if (!s || s->client_sock == SRT_INVALID_SOCK)
        return 0;

    SRT_SOCKSTATUS state = srt_getsockstate(s->client_sock);
    if (state == SRTS_CONNECTED)
        return 1;

    if (state == SRTS_BROKEN || state == SRTS_CLOSED ||
        state == SRTS_NONEXIST) {
        s->connected = 0;
        srt_close(s->client_sock);
        s->client_sock = SRT_INVALID_SOCK;
    }
    return 0;
}

int db_streamer_get_stats(db_streamer_t *s, db_streamer_stats_t *out)
{
    if (!s || !out || s->client_sock == SRT_INVALID_SOCK || !s->connected)
        return -1;

    SRT_TRACEBSTATS perf;
    memset(&perf, 0, sizeof(perf));
    if (srt_bstats(s->client_sock, &perf, 0 /* clear=false */) != 0)
        return -1;

    out->rtt_ms         = perf.msRTT;
    out->pkt_loss_total = (int)perf.pktSndLossTotal;
    out->send_rate_mbps = perf.mbpsSendRate;
    out->avail_send_buf = (int)perf.byteAvailSndBuf;

    return 0;
}

void db_streamer_destroy(db_streamer_t *s)
{
    if (!s) return;

    if (s->client_sock != SRT_INVALID_SOCK)
        srt_close(s->client_sock);
    if (s->listen_sock != SRT_INVALID_SOCK)
        srt_close(s->listen_sock);

    free(s->send_buf);
    srt_cleanup();
    free(s);
    STRM_INF("Streamer destroyed");
}
