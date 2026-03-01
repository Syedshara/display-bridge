/*
 * encoder.h
 * VAAPI H.265 hardware encoder for Intel Arrow Lake.
 * Zero-copy path: accepts DMA-BUF fds from PipeWire capture.
 */

#ifndef DB_ENCODER_H
#define DB_ENCODER_H

#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <stdint.h>

/* Maximum surfaces in the encode pool (double-buffered pipeline) */
#define DB_ENCODER_SURFACE_POOL 4

typedef struct {
    int           drm_fd;
    VADisplay     va_display;
    VAConfigID    config_id;
    VAContextID   context_id;

    /* Reconstructed reference surfaces (used internally by encoder) */
    VASurfaceID   recon_surfaces[DB_ENCODER_SURFACE_POOL];
    int           num_recon_surfaces;
    int           current_recon_idx;

    /* Coded output buffer */
    VABufferID    coded_buf;
    int           coded_buf_size;

    /* Encode parameters */
    int           width;
    int           height;
    int           bitrate_kbps;
    int           fps;
    uint8_t       codec;       /* DB_CODEC_HEVC or DB_CODEC_AV1 */
    uint64_t      frame_count;
    int           idr_interval;

    /* Reusable scratch buffer for BGRx→NV12 conversion (CPU fallback path) */
    uint8_t      *conv_buf;
    size_t        conv_buf_size;
} db_encoder_t;

/* Initialize VAAPI encoder. Returns 0 on success.
 * Opens the DRM render node, creates VA display, config, context,
 * and allocates reconstructed reference surfaces + coded buffer. */
int db_encoder_init(db_encoder_t *enc, int width, int height,
                    int bitrate_kbps, int fps, uint8_t codec);

/* Encode a frame from a DMA-BUF fd (zero-copy from PipeWire).
 * dmabuf_fd: DMA-BUF file descriptor for the NV12 frame
 * dmabuf_stride: stride (pitch) in bytes of the Y plane
 * dmabuf_offset: offset to start of pixel data (usually 0)
 * drm_modifier: DRM format modifier (DRM_FORMAT_MOD_LINEAR or tiled)
 * out_buf: receives pointer to encoded bitstream (VA-mapped, do NOT free)
 * out_size: receives size of encoded bitstream
 * is_keyframe: receives 1 if IDR frame, 0 otherwise
 *
 * Call db_encoder_release_frame() when done with the output data. */
int db_encoder_encode_dmabuf(db_encoder_t *enc, int dmabuf_fd,
                             uint32_t dmabuf_stride, uint32_t dmabuf_offset,
                             uint64_t drm_modifier,
                             uint8_t **out_buf, uint32_t *out_size,
                             int *is_keyframe);

/* Fallback: encode from CPU-side NV12 data (memcpy into VA surface).
 * Used when PipeWire doesn't provide a DMA-BUF. */
int db_encoder_encode_nv12(db_encoder_t *enc, const uint8_t *nv12_data,
                           uint32_t stride,
                           uint8_t **out_buf, uint32_t *out_size,
                           int *is_keyframe);

/* Fallback: encode from CPU-side BGRx data.
 * Converts BGRx → NV12 (BT.601) into a reusable internal buffer, then
 * calls db_encoder_encode_nv12(). Used when PipeWire negotiates BGRx format. */
int db_encoder_encode_bgrx(db_encoder_t *enc, const uint8_t *bgrx_data,
                            uint32_t stride,
                            uint8_t **out_buf, uint32_t *out_size,
                            int *is_keyframe);

/* Release the last encoded frame's buffer mapping. */
void db_encoder_release_frame(db_encoder_t *enc);

/* Force an IDR frame on the next encode call. */
void db_encoder_force_idr(db_encoder_t *enc);

/* Dynamically adjust the target bitrate (kbps).
 * Takes effect on the next encoded frame via rate control params. */
void db_encoder_set_bitrate(db_encoder_t *enc, int bitrate_kbps);

/* Destroy encoder and free resources. */
void db_encoder_destroy(db_encoder_t *enc);

#endif /* DB_ENCODER_H */
