/*
 * protocol.h
 * Shared protocol definitions between Ubuntu sender and Mac receiver.
 * Used by both C (Ubuntu) and Swift (Mac) sides.
 */

#ifndef DISPLAY_BRIDGE_PROTOCOL_H
#define DISPLAY_BRIDGE_PROTOCOL_H

#include <stdint.h>

/* ===== Network Config ===== */
#define DB_DEFAULT_VIDEO_PORT   5000
#define DB_DEFAULT_INPUT_PORT   5001
#define DB_DEFAULT_CONTROL_PORT 5002
#define DB_SRT_LATENCY_MS       500   /* SRT latency in ms (match Mac receiver) */

/* ===== Video Config ===== */
#define DB_TARGET_WIDTH         2880  /* MacBook Pro 15" 2019 native (receiver display) */
#define DB_TARGET_HEIGHT        1800
#define DB_TARGET_FPS           60
#define DB_BITRATE_KBPS         40000 /* 40 Mbps for 2880x1800 HEVC */

/* ===== Codec IDs ===== */
#define DB_CODEC_HEVC           1
#define DB_CODEC_AV1            2

/* ===== Packet Types ===== */
#define DB_PKT_VIDEO_FRAME      0x01
#define DB_PKT_INPUT_EVENT      0x02
#define DB_PKT_CONTROL          0x03
#define DB_PKT_HEARTBEAT        0x04

/* ===== Control Commands ===== */
#define DB_CTRL_START           0x10
#define DB_CTRL_STOP            0x11
#define DB_CTRL_RESOLUTION      0x12
#define DB_CTRL_BITRATE         0x13

/* ===== Input Event Types ===== */
#define DB_INPUT_MOUSE_MOVE     0x20
#define DB_INPUT_MOUSE_BUTTON   0x21
#define DB_INPUT_MOUSE_SCROLL   0x22
#define DB_INPUT_KEY_DOWN       0x23
#define DB_INPUT_KEY_UP         0x24

/* ===== Packet Header (16 bytes) ===== */
typedef struct __attribute__((packed)) {
    uint8_t  type;        /* DB_PKT_* */
    uint8_t  flags;       /* reserved */
    uint16_t seq;         /* sequence number */
    uint32_t timestamp;   /* microseconds since start */
    uint32_t payload_size;/* payload size in bytes */
    uint32_t reserved;
} db_packet_header_t;

/* ===== Video Frame Header (follows packet header) ===== */
typedef struct __attribute__((packed)) {
    uint16_t width;
    uint16_t height;
    uint8_t  codec;       /* DB_CODEC_* */
    uint8_t  keyframe;    /* 1 = IDR frame */
    uint16_t reserved;
    uint64_t pts;         /* presentation timestamp (us) */
} db_video_header_t;

/* ===== Input Event (follows packet header) ===== */
typedef struct __attribute__((packed)) {
    uint8_t  event_type;  /* DB_INPUT_* */
    uint8_t  reserved;
    int16_t  x;           /* mouse x or key code */
    int16_t  y;           /* mouse y or modifier */
    int16_t  value;       /* button id, scroll delta, etc */
} db_input_event_t;

/* ===== Control Message (follows packet header) ===== */
typedef struct __attribute__((packed)) {
    uint8_t  command;     /* DB_CTRL_* */
    uint8_t  reserved[3];
    uint32_t value1;
    uint32_t value2;
} db_control_msg_t;

#endif /* DISPLAY_BRIDGE_PROTOCOL_H */
