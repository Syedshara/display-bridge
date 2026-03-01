/*
 * capture.c
 * PipeWire-based Wayland screen capture via org.gnome.Mutter.ScreenCast.
 *
 * Flow:
 *   1. D-Bus call to org.gnome.Mutter.ScreenCast:
 *      CreateSession -> RecordMonitor("Virtual-1") -> Start
 *      No user picker — connector name is specified directly.
 *   2. PipeWireStreamAdded signal delivers a PipeWire node ID.
 *   3. PipeWire stream connects to that node.
 *   4. Frames arrive as spa_buffer objects — either DMA-BUF or MemPtr.
 *   5. The frame callback delivers data to the caller.
 */

#define _GNU_SOURCE
#include "capture.h"
#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>

#include <gio/gio.h>
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/buffer/buffer.h>
#include <spa/debug/types.h>
#include <spa/param/video/type-info.h>

#define LOG_ERR(fmt, ...) fprintf(stderr, "[capture] ERROR: " fmt "\n", ##__VA_ARGS__)
#define LOG_INF(fmt, ...) fprintf(stdout, "[capture] " fmt "\n", ##__VA_ARGS__)

#define MUTTER_BUS_NAME       "org.gnome.Mutter.ScreenCast"
#define MUTTER_OBJ_PATH       "/org/gnome/Mutter/ScreenCast"
#define MUTTER_SCREENCAST_IF  "org.gnome.Mutter.ScreenCast"
#define MUTTER_SESSION_IF     "org.gnome.Mutter.ScreenCast.Session"
#define MUTTER_STREAM_IF      "org.gnome.Mutter.ScreenCast.Stream"

#define VIRTUAL_CONNECTOR     "Virtual-1"

/* ===== Internal capture state ===== */
struct db_capture {
    /* Config */
    int                  target_width;
    int                  target_height;
    db_capture_callback_t callback;
    void                *userdata;

    /* Mutter ScreenCast state */
    GDBusConnection     *dbus;
    char                *session_handle;
    uint32_t             pw_node_id;

    /* PipeWire state */
    struct pw_main_loop *pw_loop;
    struct pw_context   *pw_ctx;
    struct pw_core      *pw_core;
    struct pw_stream    *pw_stream;
    struct spa_hook      stream_listener;
    struct spa_video_info_raw video_info;
    int                  video_info_valid;

    /* Control */
    volatile int         running;
};

/* ---------- Mutter ScreenCast D-Bus flow ---------- */

typedef struct {
    GMainLoop *loop;
    uint32_t   node_id;
    int        timed_out;
} pw_stream_added_ctx_t;

static void on_pw_stream_added(GDBusConnection *conn,
                               const gchar *sender_name,
                               const gchar *object_path,
                               const gchar *interface_name,
                               const gchar *signal_name,
                               GVariant *parameters,
                               gpointer user_data)
{
    (void)conn; (void)sender_name; (void)object_path;
    (void)interface_name; (void)signal_name;
    pw_stream_added_ctx_t *ctx = user_data;
    g_variant_get(parameters, "(u)", &ctx->node_id);
    LOG_INF("PipeWireStreamAdded: node_id=%u", ctx->node_id);
    g_main_loop_quit(ctx->loop);
}

static gboolean mutter_timeout_cb(gpointer data)
{
    pw_stream_added_ctx_t *ctx = data;
    ctx->timed_out = 1;
    g_main_loop_quit(ctx->loop);
    return G_SOURCE_REMOVE;
}

/*
 * mutter_screencast_init()
 *
 * Uses org.gnome.Mutter.ScreenCast to capture Virtual-1 directly,
 * with no user-facing picker dialog.
 *
 * On success: sets cap->dbus, cap->session_handle, cap->pw_node_id.
 * Returns 0 on success, -1 on failure.
 */
static int mutter_screencast_init(db_capture_t *cap)
{
    GError *err = NULL;

    /* Step 1: Connect to session D-Bus */
    cap->dbus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
    if (!cap->dbus) {
        LOG_ERR("Cannot connect to session D-Bus: %s",
                err ? err->message : "unknown");
        if (err) g_error_free(err);
        return -1;
    }

    /* Step 2: CreateSession */
    LOG_INF("Mutter ScreenCast: CreateSession...");
    {
        GVariantBuilder opts;
        g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&opts, "{sv}", "disable-animations",
                              g_variant_new_boolean(FALSE));

        GVariant *reply = g_dbus_connection_call_sync(
            cap->dbus,
            MUTTER_BUS_NAME,
            MUTTER_OBJ_PATH,
            MUTTER_SCREENCAST_IF,
            "CreateSession",
            g_variant_new("(a{sv})", &opts),
            G_VARIANT_TYPE("(o)"),
            G_DBUS_CALL_FLAGS_NONE,
            10000, NULL, &err);

        if (!reply) {
            LOG_ERR("CreateSession failed: %s",
                    err ? err->message : "unknown");
            if (err) g_error_free(err);
            return -1;
        }

        const gchar *session_path = NULL;
        g_variant_get(reply, "(&o)", &session_path);
        cap->session_handle = g_strdup(session_path);
        g_variant_unref(reply);
        LOG_INF("Session path: %s", cap->session_handle);
    }

    /* Step 3: RecordMonitor("Virtual-1") */
    LOG_INF("Mutter ScreenCast: RecordMonitor(%s)...", VIRTUAL_CONNECTOR);
    char *stream_path = NULL;
    {
        GVariantBuilder opts;
        g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);
        /* cursor-mode: 1 = cursor embedded in stream */
        g_variant_builder_add(&opts, "{sv}", "cursor-mode",
                              g_variant_new_uint32(1));

        GVariant *reply = g_dbus_connection_call_sync(
            cap->dbus,
            MUTTER_BUS_NAME,
            cap->session_handle,
            MUTTER_SESSION_IF,
            "RecordMonitor",
            g_variant_new("(sa{sv})", VIRTUAL_CONNECTOR, &opts),
            G_VARIANT_TYPE("(o)"),
            G_DBUS_CALL_FLAGS_NONE,
            10000, NULL, &err);

        if (!reply) {
            LOG_ERR("RecordMonitor failed: %s",
                    err ? err->message : "unknown");
            if (err) g_error_free(err);
            return -1;
        }

        const gchar *sp = NULL;
        g_variant_get(reply, "(&o)", &sp);
        stream_path = g_strdup(sp);
        g_variant_unref(reply);
        LOG_INF("Stream path: %s", stream_path);
    }

    /* Step 4: Subscribe to PipeWireStreamAdded BEFORE calling Start */
    pw_stream_added_ctx_t ctx = { .loop = NULL, .node_id = 0, .timed_out = 0 };
    ctx.loop = g_main_loop_new(NULL, FALSE);

    guint sub_id = g_dbus_connection_signal_subscribe(
        cap->dbus,
        MUTTER_BUS_NAME,
        MUTTER_STREAM_IF,
        "PipeWireStreamAdded",
        stream_path,
        NULL,
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_pw_stream_added, &ctx, NULL);

    /* Step 5: Start session */
    LOG_INF("Mutter ScreenCast: Start...");
    {
        GVariant *reply = g_dbus_connection_call_sync(
            cap->dbus,
            MUTTER_BUS_NAME,
            cap->session_handle,
            MUTTER_SESSION_IF,
            "Start",
            g_variant_new("()"),
            NULL,
            G_DBUS_CALL_FLAGS_NONE,
            10000, NULL, &err);

        if (!reply) {
            LOG_ERR("Start failed: %s",
                    err ? err->message : "unknown");
            if (err) g_error_free(err);
            g_dbus_connection_signal_unsubscribe(cap->dbus, sub_id);
            g_main_loop_unref(ctx.loop);
            g_free(stream_path);
            return -1;
        }
        g_variant_unref(reply);
    }

    /* Step 6: Wait for PipeWireStreamAdded (10-second timeout) */
    GSource *timeout_src = g_timeout_source_new_seconds(10);
    g_source_set_callback(timeout_src, mutter_timeout_cb, &ctx, NULL);
    g_source_attach(timeout_src, g_main_loop_get_context(ctx.loop));

    g_main_loop_run(ctx.loop);

    g_source_destroy(timeout_src);
    g_source_unref(timeout_src);
    g_dbus_connection_signal_unsubscribe(cap->dbus, sub_id);
    g_main_loop_unref(ctx.loop);
    g_free(stream_path);

    if (ctx.timed_out || ctx.node_id == 0) {
        LOG_ERR("Timed out waiting for PipeWireStreamAdded");
        return -1;
    }

    cap->pw_node_id = ctx.node_id;
    LOG_INF("Mutter ScreenCast ready, PipeWire node_id=%u", cap->pw_node_id);
    return 0;
}

/* ---------- PipeWire stream callbacks ---------- */

static void on_stream_param_changed(void *data, uint32_t id,
                                    const struct spa_pod *param)
{
    db_capture_t *cap = data;
    if (!param || id != SPA_PARAM_Format)
        return;

    struct spa_video_info info;
    if (spa_format_video_parse(param, &info) < 0) {
        LOG_ERR("Failed to parse video format");
        return;
    }

    if (info.media_subtype != SPA_MEDIA_SUBTYPE_raw) {
        LOG_ERR("Unexpected media subtype: %d", info.media_subtype);
        return;
    }

    cap->video_info = info.info.raw;
    cap->video_info_valid = 1;

    LOG_INF("Stream format: %dx%d, format=%d, framerate=%d/%d",
            cap->video_info.size.width, cap->video_info.size.height,
            cap->video_info.format,
            cap->video_info.framerate.num, cap->video_info.framerate.denom);

    /* Request DMA-BUF buffers for zero-copy. We specify buffer params
     * so PipeWire knows to give us DMA-BUF when possible. */
    uint8_t buf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));

    const struct spa_pod *params[2];
    params[0] = spa_pod_builder_add_object(&b,
        SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
        SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, 8),
        SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(
            (1 << SPA_DATA_DmaBuf) | (1 << SPA_DATA_MemPtr)));

    pw_stream_update_params(cap->pw_stream, params, 1);
}

static void on_stream_process(void *data)
{
    db_capture_t *cap = data;
    struct pw_buffer *pw_buf;

    pw_buf = pw_stream_dequeue_buffer(cap->pw_stream);
    if (!pw_buf) return;

    struct spa_buffer *spa_buf = pw_buf->buffer;
    if (!spa_buf || spa_buf->n_datas == 0)
        goto done;

    struct spa_data *d = &spa_buf->datas[0];
    if (!cap->video_info_valid)
        goto done;

    int width  = cap->video_info.size.width;
    int height = cap->video_info.size.height;

    if (d->type == SPA_DATA_DmaBuf) {
        /* Zero-copy DMA-BUF path */
        int dmabuf_fd = (int)d->fd;

        if (cap->callback) {
            /* Pass the DMA-BUF fd encoded in the data pointer:
             * We use a special convention — if data pointer is NULL
             * but the stride encodes the fd, the caller knows it's DMA-BUF.
             * Actually, let's use a cleaner approach: pass the fd as a
             * negative stride, and the actual stride in the userdata.
             *
             * Better: The callback receives raw pointer data. For DMA-BUF,
             * main.c will call the encoder's dmabuf path directly.
             * We encode the info into the callback: data=NULL means DMA-BUF,
             * stride holds the fd, width/height are valid, userdata carries
             * the actual stride and offset. We'll use a dedicated struct. */

            /* For DMA-BUF frames, we store metadata and pass a sentinel.
             * The main.c integration will detect data==NULL and use the
             * dmabuf encoder path instead. We pack the dmabuf info into
             * the stride field. This is ugly but avoids changing the
             * callback signature. The proper way is to extend the callback.
             *
             * Let's just pack dmabuf_fd into the first 4 bytes of a static
             * struct that the callback can interpret. Actually, the simplest
             * clean approach: we'll use the fact that capture.h's callback
             * gets both stride and userdata. We'll set data=NULL as the
             * DMA-BUF sentinel. The caller must check for data==NULL. */

            /*
             * DMA-BUF callback convention:
             *   data = NULL          → this is a DMA-BUF frame
             *   width, height        → frame dimensions
             *   stride               → DMA-BUF fd (as int, cast to int)
             *
             * The actual stride and offset can be queried via the fd.
             * The caller (main.c) will call db_encoder_encode_dmabuf().
             */
            cap->callback(NULL, width, height, dmabuf_fd, cap->userdata);
        }
    } else if (d->type == SPA_DATA_MemPtr || d->type == SPA_DATA_MemFd) {
        /* Memory-mapped path — CPU copy fallback */
        uint8_t *frame_data = SPA_PTROFF(d->data, d->chunk->offset, uint8_t);
        int stride = d->chunk->stride;

        if (d->chunk->size > 0 && frame_data && cap->callback) {
            cap->callback(frame_data, width, height, stride, cap->userdata);
        }
    }

done:
    pw_stream_queue_buffer(cap->pw_stream, pw_buf);
}

static void on_stream_state_changed(void *data,
                                    enum pw_stream_state old,
                                    enum pw_stream_state state,
                                    const char *error)
{
    db_capture_t *cap = data;
    LOG_INF("Stream state: %s -> %s%s%s",
            pw_stream_state_as_string(old),
            pw_stream_state_as_string(state),
            error ? " error: " : "",
            error ? error : "");

    if (state == PW_STREAM_STATE_ERROR) {
        cap->running = 0;
        pw_main_loop_quit(cap->pw_loop);
    }
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_stream_state_changed,
    .param_changed = on_stream_param_changed,
    .process = on_stream_process,
};

/* ---------- PipeWire stream setup ---------- */

static int setup_pw_stream(db_capture_t *cap)
{
    pw_init(NULL, NULL);

    cap->pw_loop = pw_main_loop_new(NULL);
    if (!cap->pw_loop) {
        LOG_ERR("Cannot create PipeWire main loop");
        return -1;
    }

    cap->pw_ctx = pw_context_new(pw_main_loop_get_loop(cap->pw_loop),
                                  NULL, 0);
    if (!cap->pw_ctx) {
        LOG_ERR("Cannot create PipeWire context");
        return -1;
    }

    cap->pw_core = pw_context_connect(cap->pw_ctx, NULL, 0);
    if (!cap->pw_core) {
        LOG_ERR("Cannot connect to PipeWire daemon");
        return -1;
    }

    /* Create stream */
    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Video",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Screen",
        NULL);

    cap->pw_stream = pw_stream_new(cap->pw_core,
                                    "display-bridge-capture",
                                    props);
    if (!cap->pw_stream) {
        LOG_ERR("Cannot create PipeWire stream");
        return -1;
    }

    pw_stream_add_listener(cap->pw_stream, &cap->stream_listener,
                           &stream_events, cap);

    /* Build format params.
     *
     * GNOME's ScreenCast portal node offers BGRx/BGRA/RGBx/RGBA at the
     * display's fixed native resolution.  It does NOT offer NV12 on the
     * CPU/memory path (NV12 only comes via DMA-BUF with explicit modifiers).
     *
     * We deliberately omit SPA_FORMAT_VIDEO_size and
     * SPA_FORMAT_VIDEO_framerate so the portal node can dictate both — it
     * always captures at the display's native resolution and its own
     * framerate.  Constraining size/fps here causes "no more input formats"
     * because the node sees our preferred rectangle (which may differ from
     * the actual display) and rejects the negotiation.
     *
     * The actual negotiated size/fps is reported in on_stream_param_changed.
     */
    uint8_t buf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));

    const struct spa_pod *params[1];
    params[0] = spa_pod_builder_add_object(&b,
        SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType,    SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(6,
            SPA_VIDEO_FORMAT_BGRx,   /* default — most common on GNOME */
            SPA_VIDEO_FORMAT_BGRx,
            SPA_VIDEO_FORMAT_BGRA,
            SPA_VIDEO_FORMAT_RGBx,
            SPA_VIDEO_FORMAT_RGBA,
            SPA_VIDEO_FORMAT_NV12)); /* NV12 last — DMA-BUF path only */

    /* Connect stream to the portal's PipeWire node */
    int ret = pw_stream_connect(cap->pw_stream,
                                PW_DIRECTION_INPUT,
                                cap->pw_node_id,
                                PW_STREAM_FLAG_AUTOCONNECT |
                                PW_STREAM_FLAG_MAP_BUFFERS,
                                params, 1);
    if (ret < 0) {
        LOG_ERR("pw_stream_connect failed: %s", strerror(-ret));
        return -1;
    }

    LOG_INF("PipeWire stream connected to node %u", cap->pw_node_id);
    return 0;
}

/* ========== Public API ========== */

db_capture_t *db_capture_init(int target_width, int target_height,
                               db_capture_callback_t callback, void *userdata)
{
    db_capture_t *cap = calloc(1, sizeof(db_capture_t));
    if (!cap) return NULL;

    cap->target_width  = target_width;
    cap->target_height = target_height;
    cap->callback      = callback;
    cap->userdata      = userdata;
    cap->running       = 0;

    /* Step 1: Mutter ScreenCast negotiation (D-Bus) */
    if (mutter_screencast_init(cap) != 0) {
        LOG_ERR("Mutter ScreenCast negotiation failed");
        g_free(cap->session_handle);
        free(cap);
        return NULL;
    }

    /* Step 2: Set up PipeWire stream */
    if (setup_pw_stream(cap) != 0) {
        LOG_ERR("PipeWire stream setup failed");
        db_capture_destroy(cap);
        return NULL;
    }

    return cap;
}

int db_capture_start(db_capture_t *cap)
{
    if (!cap || !cap->pw_loop) return -1;

    cap->running = 1;
    LOG_INF("Starting capture loop...");

    /* This blocks until pw_main_loop_quit() is called */
    int ret = pw_main_loop_run(cap->pw_loop);

    cap->running = 0;
    LOG_INF("Capture loop exited (ret=%d)", ret);
    return ret;
}

void db_capture_stop(db_capture_t *cap)
{
    if (!cap) return;
    cap->running = 0;
    if (cap->pw_loop)
        pw_main_loop_quit(cap->pw_loop);
}

void db_capture_destroy(db_capture_t *cap)
{
    if (!cap) return;

    if (cap->pw_stream) {
        pw_stream_disconnect(cap->pw_stream);
        pw_stream_destroy(cap->pw_stream);
    }
    if (cap->pw_core)
        pw_core_disconnect(cap->pw_core);
    if (cap->pw_ctx)
        pw_context_destroy(cap->pw_ctx);
    if (cap->pw_loop)
        pw_main_loop_destroy(cap->pw_loop);

    pw_deinit();

    /* Clean up D-Bus / Mutter ScreenCast state */
    if (cap->dbus)
        g_object_unref(cap->dbus);
    g_free(cap->session_handle);

    free(cap);
    LOG_INF("Capture destroyed");
}
