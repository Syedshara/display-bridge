/*
 * encoder.c
 * VAAPI H.265 (HEVC) hardware encoder for Intel Arrow Lake.
 *
 * Supports two input paths:
 *   1. DMA-BUF zero-copy (preferred): import PipeWire DMA-BUF fd directly as
 *      a VA surface — no CPU-side pixel copy.
 *   2. NV12 fallback: memcpy raw NV12 pixels into a VA surface when DMA-BUF
 *      is unavailable.
 *
 * Encode pipeline per frame:
 *   vaCreateSurfaces (import DMA-BUF) -> vaBeginPicture -> vaRenderPicture
 *   (packed headers + seq/pic/slice params + rate control) -> vaEndPicture
 *   -> vaSyncSurface -> vaMapBuffer(coded_buf) -> return bitstream.
 *
 * Packed headers:
 *   The Intel iHD driver on Arrow Lake requires packed VPS/SPS/PPS NALUs
 *   on IDR frames and packed slice headers on every frame. We serialize
 *   these using a simple bitstream writer per the H.265 spec.
 */

#define _GNU_SOURCE
#include "encoder.h"
#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <va/va_enc_hevc.h>
#include <drm_fourcc.h>

/* ---------- internal helpers ---------- */

#define LOG_ERR(fmt, ...) fprintf(stderr, "[encoder] ERROR: " fmt "\n", ##__VA_ARGS__)
#define LOG_INF(fmt, ...) fprintf(stdout, "[encoder] " fmt "\n", ##__VA_ARGS__)

#define VA_CHECK(call, label)                                   \
    do {                                                        \
        VAStatus _s = (call);                                   \
        if (_s != VA_STATUS_SUCCESS) {                          \
            LOG_ERR("%s failed: %s", #call, vaErrorStr(_s));    \
            goto label;                                         \
        }                                                       \
    } while (0)

/* Align value up to alignment boundary */
#define ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))

#define HEVC_CTU_SIZE 64

/* ============================================================
 *  Simple bitstream writer for HEVC NALUs
 * ============================================================ */

typedef struct {
    uint8_t  *buf;
    int       capacity;   /* bytes allocated */
    int       byte_pos;   /* current byte offset */
    int       bit_pos;    /* bits written in current byte (0..7) */
} bitstream_t;

static void bs_init(bitstream_t *bs, uint8_t *buf, int capacity)
{
    bs->buf = buf;
    bs->capacity = capacity;
    bs->byte_pos = 0;
    bs->bit_pos = 0;
    memset(buf, 0, capacity);
}

/* Total bits written (used by packed header submission) */
static inline int bs_bits_written(const bitstream_t *bs)
    __attribute__((unused));
static inline int bs_bits_written(const bitstream_t *bs)
{
    return bs->byte_pos * 8 + bs->bit_pos;
}

/* Write n bits (1..32) from value (MSB first) */
static void bs_write(bitstream_t *bs, uint32_t value, int n)
{
    for (int i = n - 1; i >= 0; i--) {
        int bit = (value >> i) & 1;
        bs->buf[bs->byte_pos] |= (bit << (7 - bs->bit_pos));
        bs->bit_pos++;
        if (bs->bit_pos == 8) {
            bs->bit_pos = 0;
            bs->byte_pos++;
        }
    }
}

/* Write a single bit */
static void bs_write1(bitstream_t *bs, int bit)
{
    bs_write(bs, bit & 1, 1);
}

/* Write unsigned exp-Golomb coded value */
static void bs_write_ue(bitstream_t *bs, uint32_t value)
{
    uint32_t v = value + 1;
    int bits = 0;
    uint32_t tmp = v;
    while (tmp > 0) { bits++; tmp >>= 1; }
    /* Write (bits-1) zeros */
    for (int i = 0; i < bits - 1; i++)
        bs_write1(bs, 0);
    /* Write the value in 'bits' bits */
    bs_write(bs, v, bits);
}

/* Write signed exp-Golomb coded value */
static void bs_write_se(bitstream_t *bs, int32_t value)
{
    uint32_t mapped;
    if (value > 0)
        mapped = (uint32_t)(2 * value - 1);
    else
        mapped = (uint32_t)(-2 * value);
    bs_write_ue(bs, mapped);
}

/* Byte-align: write trailing bits (1 + zeros) */
static void bs_trailing_bits(bitstream_t *bs)
{
    bs_write1(bs, 1);
    while (bs->bit_pos != 0)
        bs_write1(bs, 0);
}

/* Total bytes written (after byte-alignment) */
static int bs_bytes_written(const bitstream_t *bs)
{
    return bs->byte_pos + (bs->bit_pos > 0 ? 1 : 0);
}

/* ============================================================
 *  Annex B start code + NALU header writing
 * ============================================================ */

/* Write 4-byte start code + NALU header into output buffer.
 * Returns pointer past the written bytes. */
static uint8_t *write_start_code_and_nalu_header(uint8_t *dst,
                                                  int nal_unit_type,
                                                  int temporal_id_plus1)
{
    /* 4-byte start code */
    *dst++ = 0x00;
    *dst++ = 0x00;
    *dst++ = 0x00;
    *dst++ = 0x01;
    /* NALU header: forbidden_zero_bit(1) + nal_unit_type(6) + nuh_layer_id(6) + nuh_temporal_id_plus1(3) */
    *dst++ = (uint8_t)((nal_unit_type & 0x3F) << 1);
    *dst++ = (uint8_t)(temporal_id_plus1 & 0x07);
    return dst;
}

/* Add emulation prevention bytes (0x00 0x00 0x03 before 0x00/0x01/0x02/0x03).
 * dst must have room for worst-case 4/3 * src_len. Returns output length. */
static int add_emulation_prevention(uint8_t *dst, const uint8_t *src, int src_len)
{
    int dp = 0;
    int zeros = 0;
    for (int i = 0; i < src_len; i++) {
        if (zeros >= 2 && src[i] <= 0x03) {
            dst[dp++] = 0x03;  /* emulation prevention byte */
            zeros = 0;
        }
        dst[dp++] = src[i];
        if (src[i] == 0x00)
            zeros++;
        else
            zeros = 0;
    }
    return dp;
}

/* ============================================================
 *  HEVC VPS/SPS/PPS/Slice header serialization
 * ============================================================ */

/* profile_tier_level() — H.265 spec 7.3.3 */
static void write_profile_tier_level(bitstream_t *bs, int max_sub_layers_minus1)
{
    /* general_profile_space(2) + general_tier_flag(1) + general_profile_idc(5) */
    bs_write(bs, 0, 2);   /* profile_space = 0 */
    bs_write1(bs, 0);     /* tier_flag = 0 (Main tier) */
    bs_write(bs, 1, 5);   /* profile_idc = 1 (Main) */

    /* general_profile_compatibility_flag[32] */
    /* Set bit 1 (Main) and bit 2 (Main10 compat) */
    for (int i = 0; i < 32; i++) {
        bs_write1(bs, (i == 1 || i == 2) ? 1 : 0);
    }

    /* general_constraint_indicator_flags (48 bits) */
    bs_write1(bs, 1);  /* general_progressive_source_flag */
    bs_write1(bs, 0);  /* general_interlaced_source_flag */
    bs_write1(bs, 1);  /* general_non_packed_constraint_flag */
    bs_write1(bs, 1);  /* general_frame_only_constraint_flag */
    /* Remaining 44 bits of constraints = 0 */
    for (int i = 0; i < 44; i++)
        bs_write1(bs, 0);

    /* general_level_idc */
    bs_write(bs, 120, 8);  /* Level 4.0 = 30 * 4.0 */

    /* sub-layer stuff — we have 0 sub-layers so nothing extra */
    (void)max_sub_layers_minus1;
}

/* Write VPS NALU body (excluding start code + NALU header) */
static int write_vps(uint8_t *out_buf, int out_capacity, db_encoder_t *enc)
{
    (void)out_capacity;
    bitstream_t bs;
    uint8_t raw_buf[256];
    bs_init(&bs, raw_buf, sizeof(raw_buf));

    bs_write(&bs, 0, 4);       /* vps_video_parameter_set_id */
    bs_write1(&bs, 1);          /* vps_base_layer_internal_flag */
    bs_write1(&bs, 1);          /* vps_base_layer_available_flag */
    bs_write(&bs, 0, 6);       /* vps_max_layers_minus1 */
    bs_write(&bs, 0, 3);       /* vps_max_sub_layers_minus1 */
    bs_write1(&bs, 1);          /* vps_temporal_id_nesting_flag */
    bs_write(&bs, 0xFFFF, 16); /* vps_reserved_0xffff_16bits */

    write_profile_tier_level(&bs, 0);

    bs_write1(&bs, 0);  /* vps_sub_layer_ordering_info_present_flag */
    /* vps_max_dec_pic_buffering_minus1[0] */
    bs_write_ue(&bs, 1);   /* need 2 buffers for I+P */
    /* vps_max_num_reorder_pics[0] */
    bs_write_ue(&bs, 0);   /* no reorder */
    /* vps_max_latency_increase_plus1[0] */
    bs_write_ue(&bs, 0);

    bs_write(&bs, 0, 6);  /* vps_max_layer_id */
    bs_write_ue(&bs, 0);  /* vps_num_layer_sets_minus1 */

    /* VPS timing info */
    bs_write1(&bs, 1);     /* vps_timing_info_present_flag */
    bs_write(&bs, 1, 32);  /* vps_num_units_in_tick */
    bs_write(&bs, (uint32_t)enc->fps, 32);  /* vps_time_scale */
    bs_write1(&bs, 0);     /* vps_poc_proportional_to_timing_flag */
    bs_write_ue(&bs, 0);   /* vps_num_hrd_parameters */

    bs_write1(&bs, 0);     /* vps_extension_flag */
    bs_trailing_bits(&bs);

    int raw_len = bs_bytes_written(&bs);
    return add_emulation_prevention(out_buf, raw_buf, raw_len);
}

/* Write SPS NALU body */
static int write_sps(uint8_t *out_buf, int out_capacity, db_encoder_t *enc)
{
    (void)out_capacity;
    bitstream_t bs;
    uint8_t raw_buf[512];
    bs_init(&bs, raw_buf, sizeof(raw_buf));

    int aw = ALIGN(enc->width, HEVC_CTU_SIZE);
    int ah = ALIGN(enc->height, HEVC_CTU_SIZE);

    bs_write(&bs, 0, 4);   /* sps_video_parameter_set_id */
    bs_write(&bs, 0, 3);   /* sps_max_sub_layers_minus1 */
    bs_write1(&bs, 1);      /* sps_temporal_id_nesting_flag */

    write_profile_tier_level(&bs, 0);

    bs_write_ue(&bs, 0);   /* sps_seq_parameter_set_id */
    bs_write_ue(&bs, 1);   /* chroma_format_idc = 1 (4:2:0) */

    bs_write_ue(&bs, (uint32_t)aw);   /* pic_width_in_luma_samples */
    bs_write_ue(&bs, (uint32_t)ah);   /* pic_height_in_luma_samples */

    /* Conformance window — needed if coded size != original size */
    int need_conf = (aw != enc->width || ah != enc->height);
    bs_write1(&bs, need_conf);
    if (need_conf) {
        /* crop_unit_x = 2 for 4:2:0, crop_unit_y = 2 for 4:2:0 */
        int right_offset  = (aw - enc->width) / 2;
        int bottom_offset = (ah - enc->height) / 2;
        bs_write_ue(&bs, 0);               /* conf_win_left_offset */
        bs_write_ue(&bs, (uint32_t)right_offset);    /* conf_win_right_offset */
        bs_write_ue(&bs, 0);               /* conf_win_top_offset */
        bs_write_ue(&bs, (uint32_t)bottom_offset);   /* conf_win_bottom_offset */
    }

    bs_write_ue(&bs, 0);   /* bit_depth_luma_minus8 */
    bs_write_ue(&bs, 0);   /* bit_depth_chroma_minus8 */
    bs_write_ue(&bs, 8);   /* log2_max_pic_order_cnt_lsb_minus4 → POC bits = 12 */

    bs_write1(&bs, 0);     /* sps_sub_layer_ordering_info_present_flag */
    bs_write_ue(&bs, 1);   /* sps_max_dec_pic_buffering_minus1[0] */
    bs_write_ue(&bs, 0);   /* sps_max_num_reorder_pics[0] */
    bs_write_ue(&bs, 0);   /* sps_max_latency_increase_plus1[0] */

    /* Coding block / transform block sizes — match seq_param */
    bs_write_ue(&bs, 0);   /* log2_min_luma_coding_block_size_minus3 → min CU = 8 */
    bs_write_ue(&bs, 3);   /* log2_diff_max_min_luma_coding_block_size → max CU = 64 */
    bs_write_ue(&bs, 0);   /* log2_min_luma_transform_block_size_minus2 → min TU = 4 */
    bs_write_ue(&bs, 3);   /* log2_diff_max_min_luma_transform_block_size → max TU = 32 */
    bs_write_ue(&bs, 2);   /* max_transform_hierarchy_depth_inter */
    bs_write_ue(&bs, 2);   /* max_transform_hierarchy_depth_intra */

    bs_write1(&bs, 0);     /* scaling_list_enabled_flag */
    bs_write1(&bs, 1);     /* amp_enabled_flag */
    bs_write1(&bs, 0);     /* sample_adaptive_offset_enabled_flag */
    bs_write1(&bs, 0);     /* pcm_enabled_flag */
    /* pcm_enabled = 0, so no PCM params */

    /* Short-term reference picture sets — we use 0 in SPS, specify in slice */
    bs_write_ue(&bs, 0);   /* num_short_term_ref_pic_sets */

    bs_write1(&bs, 0);     /* long_term_ref_pics_present_flag */
    bs_write1(&bs, 0);     /* sps_temporal_mvp_enabled_flag */
    bs_write1(&bs, 1);     /* strong_intra_smoothing_enabled_flag */

    /* VUI */
    bs_write1(&bs, 0);     /* vui_parameters_present_flag — keep simple */

    bs_write1(&bs, 0);     /* sps_extension_present_flag */
    bs_trailing_bits(&bs);

    int raw_len = bs_bytes_written(&bs);
    return add_emulation_prevention(out_buf, raw_buf, raw_len);
}

/* Write PPS NALU body */
static int write_pps(uint8_t *out_buf, int out_capacity, db_encoder_t *enc)
{
    (void)out_capacity;
    bitstream_t bs;
    uint8_t raw_buf[256];
    bs_init(&bs, raw_buf, sizeof(raw_buf));

    (void)enc;

    bs_write_ue(&bs, 0);   /* pps_pic_parameter_set_id */
    bs_write_ue(&bs, 0);   /* pps_seq_parameter_set_id */
    bs_write1(&bs, 0);      /* dependent_slice_segments_enabled_flag */
    bs_write1(&bs, 0);      /* output_flag_present_flag */
    bs_write(&bs, 0, 3);   /* num_extra_slice_header_bits */
    bs_write1(&bs, 0);      /* sign_data_hiding_enabled_flag */
    bs_write1(&bs, 0);      /* cabac_init_present_flag */

    bs_write_ue(&bs, 0);   /* num_ref_idx_l0_default_active_minus1 */
    bs_write_ue(&bs, 0);   /* num_ref_idx_l1_default_active_minus1 */

    bs_write_se(&bs, 0);   /* init_qp_minus26 */
    bs_write1(&bs, 0);      /* constrained_intra_pred_flag */
    bs_write1(&bs, 0);      /* transform_skip_enabled_flag */
    bs_write1(&bs, 1);      /* cu_qp_delta_enabled_flag */
    /* cu_qp_delta_enabled → diff_cu_qp_delta_depth */
    bs_write_ue(&bs, 0);   /* diff_cu_qp_delta_depth */

    bs_write_se(&bs, 0);   /* pps_cb_qp_offset */
    bs_write_se(&bs, 0);   /* pps_cr_qp_offset */
    bs_write1(&bs, 0);      /* pps_slice_chroma_qp_offsets_present_flag */
    bs_write1(&bs, 0);      /* weighted_pred_flag */
    bs_write1(&bs, 0);      /* weighted_bipred_flag */
    bs_write1(&bs, 0);      /* transquant_bypass_enabled_flag */
    bs_write1(&bs, 0);      /* tiles_enabled_flag */
    bs_write1(&bs, 0);      /* entropy_coding_sync_enabled_flag */
    /* tiles_enabled=0, entropy_coding_sync=0 → no loop filter stuff needed here */

    bs_write1(&bs, 1);      /* loop_filter_across_slices_enabled_flag */
    bs_write1(&bs, 1);      /* deblocking_filter_control_present_flag */
    /* deblocking_filter_control_present */
    bs_write1(&bs, 0);      /* deblocking_filter_override_enabled_flag */
    bs_write1(&bs, 0);      /* pps_deblocking_filter_disabled_flag */
    /* disabled=0, so write offsets */
    bs_write_se(&bs, 0);   /* pps_beta_offset_div2 */
    bs_write_se(&bs, 0);   /* pps_tc_offset_div2 */

    bs_write1(&bs, 0);      /* pps_scaling_list_data_present_flag */
    bs_write1(&bs, 0);      /* lists_modification_present_flag */
    bs_write_ue(&bs, 0);   /* log2_parallel_merge_level_minus2 */
    bs_write1(&bs, 0);      /* slice_segment_header_extension_present_flag */
    bs_write1(&bs, 0);      /* pps_extension_present_flag */

    bs_trailing_bits(&bs);

    int raw_len = bs_bytes_written(&bs);
    return add_emulation_prevention(out_buf, raw_buf, raw_len);
}

/* Write slice segment header NALU body.
 * Only writes the header portion — the actual slice data comes from HW encoder.
 * Returns length of emulation-prevented data. */
static int write_slice_header(uint8_t *out_buf, int out_capacity,
                               db_encoder_t *enc, int is_idr)
{
    (void)out_capacity;
    bitstream_t bs;
    uint8_t raw_buf[256];
    bs_init(&bs, raw_buf, sizeof(raw_buf));

    int aw = ALIGN(enc->width, HEVC_CTU_SIZE);
    int ah = ALIGN(enc->height, HEVC_CTU_SIZE);
    (void)(aw * ah); /* num_ctus — used for documentation only */

    bs_write1(&bs, 1);  /* first_slice_segment_in_pic_flag */

    if (is_idr) {
        bs_write1(&bs, 0);  /* no_output_of_prior_pics_flag */
    }

    bs_write_ue(&bs, 0);    /* slice_pic_parameter_set_id */

    /* dependent_slice_segments_enabled_flag = 0 in PPS, so no dependent_slice_segment_flag */

    /* slice_segment_address: not present for first slice if only 1 slice per pic */
    /* Actually, since first_slice_segment_in_pic_flag=1, address is implicitly 0 */

    /* num_extra_slice_header_bits = 0 in PPS */

    bs_write_ue(&bs, is_idr ? 2 : 1);  /* slice_type: 2=I, 1=P */

    /* output_flag_present_flag = 0 in PPS */

    if (!is_idr) {
        /* pic_order_cnt_lsb: log2_max_poc_lsb = 12 bits */
        int poc = (int)(enc->frame_count % enc->idr_interval);
        bs_write(&bs, (uint32_t)poc, 12);

        /* short_term_ref_pic_set — inline, not from SPS */
        bs_write1(&bs, 0);  /* short_term_ref_pic_set_sps_flag = 0 */

        /* Write an inline STRPS for 1 previous reference */
        /* num_negative_pics */
        bs_write_ue(&bs, 1);
        /* num_positive_pics */
        bs_write_ue(&bs, 0);
        /* delta_poc_s0_minus1[0] = 0 → delta = -1 */
        bs_write_ue(&bs, 0);
        /* used_by_curr_pic_s0_flag[0] = 1 */
        bs_write1(&bs, 1);

        /* sps_temporal_mvp_enabled_flag = 0 in SPS, so no temporal MVP stuff */
    }

    /* sample_adaptive_offset_enabled_flag = 0 in SPS → no SAO flags */

    if (!is_idr) {
        /* num_ref_idx_active_override_flag */
        bs_write1(&bs, 0);  /* use PPS defaults (0+1=1 ref) */
    }

    /* cabac_init_present_flag=0 in PPS → no cabac_init_flag */

    /* slice_qp_delta */
    bs_write_se(&bs, 0);

    /* pps_slice_chroma_qp_offsets_present_flag = 0, no chroma QP offsets */

    /* deblocking_filter_override_enabled_flag=0 in PPS → no override */

    /* slice_loop_filter_across_slices_enabled_flag — only if
       pps_loop_filter_across_slices_enabled_flag != slice default.
       Not signaled when not overridden. Actually: it IS signaled if
       deblocking_filter_override_enabled_flag=0 and
       pps_loop_filter_across_slices_enabled_flag=1, then
       this flag is inferred as 1. So don't write it. */

    /* num_entry_point_offsets — tiles_enabled=0 && entropy_coding_sync=0 → not present */

    /* slice_segment_header_extension_present_flag = 0 → no extension */

    /* byte_alignment() */
    bs_trailing_bits(&bs);

    int raw_len = bs_bytes_written(&bs);
    return add_emulation_prevention(out_buf, raw_buf, raw_len);
}

/* ============================================================
 *  Submit packed header buffer pair to VAAPI
 * ============================================================ */

static VAStatus submit_packed_header(db_encoder_t *enc, int header_type,
                                      const uint8_t *data, int data_len_bits)
{
    VAStatus st;

    VAEncPackedHeaderParameterBuffer param;
    memset(&param, 0, sizeof(param));
    param.type = (uint32_t)header_type;
    param.bit_length = (uint32_t)data_len_bits;
    param.has_emulation_bytes = 1;  /* we already inserted 0x03 bytes */

    VABufferID param_buf;
    st = vaCreateBuffer(enc->va_display, enc->context_id,
                        VAEncPackedHeaderParameterBufferType,
                        sizeof(param), 1, &param, &param_buf);
    if (st != VA_STATUS_SUCCESS) return st;

    VABufferID data_buf;
    st = vaCreateBuffer(enc->va_display, enc->context_id,
                        VAEncPackedHeaderDataBufferType,
                        (data_len_bits + 7) / 8, 1, (void *)data, &data_buf);
    if (st != VA_STATUS_SUCCESS) {
        vaDestroyBuffer(enc->va_display, param_buf);
        return st;
    }

    VABufferID bufs[2] = { param_buf, data_buf };
    st = vaRenderPicture(enc->va_display, enc->context_id, bufs, 2);

    vaDestroyBuffer(enc->va_display, param_buf);
    vaDestroyBuffer(enc->va_display, data_buf);
    return st;
}

/* Build and submit packed VPS+SPS+PPS as a single sequence header */
static VAStatus submit_packed_sequence_headers(db_encoder_t *enc)
{
    uint8_t combined[2048];
    int pos = 0;
    uint8_t nalu_body[512];
    uint8_t *p;
    int len;

    /* VPS: NALU type 32 */
    p = write_start_code_and_nalu_header(combined + pos, 32, 1);
    pos = (int)(p - combined);
    len = write_vps(nalu_body, sizeof(nalu_body), enc);
    memcpy(combined + pos, nalu_body, len);
    pos += len;

    /* SPS: NALU type 33 */
    p = write_start_code_and_nalu_header(combined + pos, 33, 1);
    pos = (int)(p - combined);
    len = write_sps(nalu_body, sizeof(nalu_body), enc);
    memcpy(combined + pos, nalu_body, len);
    pos += len;

    /* PPS: NALU type 34 */
    p = write_start_code_and_nalu_header(combined + pos, 34, 1);
    pos = (int)(p - combined);
    len = write_pps(nalu_body, sizeof(nalu_body), enc);
    memcpy(combined + pos, nalu_body, len);
    pos += len;

    return submit_packed_header(enc, VAEncPackedHeaderSequence,
                                combined, pos * 8);
}

/* Build and submit packed slice header */
static VAStatus submit_packed_slice_header(db_encoder_t *enc, int is_idr)
{
    uint8_t combined[512];
    int pos = 0;
    uint8_t nalu_body[256];
    uint8_t *p;
    int len;

    int nal_type = is_idr ? 19 : 1;  /* IDR_W_RADL or TRAIL_R */
    p = write_start_code_and_nalu_header(combined + pos, nal_type, 1);
    pos = (int)(p - combined);
    len = write_slice_header(nalu_body, sizeof(nalu_body), enc, is_idr);
    memcpy(combined + pos, nalu_body, len);
    pos += len;

    return submit_packed_header(enc, VAEncPackedHeaderHEVC_Slice,
                                combined, pos * 8);
}

/* ============================================================
 *  Open DRM render node
 * ============================================================ */

static int open_drm_render_node(void)
{
    const char *devices[] = {
        "/dev/dri/renderD128",
        "/dev/dri/renderD129",
        NULL
    };
    for (int i = 0; devices[i]; i++) {
        int fd = open(devices[i], O_RDWR);
        if (fd >= 0) {
            LOG_INF("Opened DRM render node: %s", devices[i]);
            return fd;
        }
    }
    LOG_ERR("Cannot open any DRM render node");
    return -1;
}

static int calc_coded_buf_size(int width, int height)
{
    int size = width * height * 3 / 2;
    if (size < 4 * 1024 * 1024)
        size = 4 * 1024 * 1024;
    return size;
}

/* ============================================================
 *  Fill VA parameter buffers (seq/pic/slice)
 * ============================================================ */

static void fill_seq_param(VAEncSequenceParameterBufferHEVC *seq,
                           db_encoder_t *enc)
{
    memset(seq, 0, sizeof(*seq));

    seq->general_profile_idc = 1;
    seq->general_level_idc = 120;
    seq->general_tier_flag = 0;

    seq->intra_period = enc->idr_interval;
    seq->intra_idr_period = enc->idr_interval;
    seq->ip_period = 1;

    seq->bits_per_second = (uint32_t)enc->bitrate_kbps * 1000;
    seq->pic_width_in_luma_samples = ALIGN(enc->width, HEVC_CTU_SIZE);
    seq->pic_height_in_luma_samples = ALIGN(enc->height, HEVC_CTU_SIZE);

    seq->seq_fields.bits.chroma_format_idc = 1;
    seq->seq_fields.bits.separate_colour_plane_flag = 0;
    seq->seq_fields.bits.bit_depth_luma_minus8 = 0;
    seq->seq_fields.bits.bit_depth_chroma_minus8 = 0;
    seq->seq_fields.bits.scaling_list_enabled_flag = 0;
    seq->seq_fields.bits.strong_intra_smoothing_enabled_flag = 1;
    seq->seq_fields.bits.amp_enabled_flag = 1;
    seq->seq_fields.bits.sample_adaptive_offset_enabled_flag = 0;
    seq->seq_fields.bits.pcm_enabled_flag = 0;
    seq->seq_fields.bits.pcm_loop_filter_disabled_flag = 1;
    seq->seq_fields.bits.sps_temporal_mvp_enabled_flag = 0;
    seq->seq_fields.bits.low_delay_seq = 1;
    seq->seq_fields.bits.hierachical_flag = 0;

    seq->log2_min_luma_coding_block_size_minus3 = 0;
    seq->log2_diff_max_min_luma_coding_block_size = 3;
    seq->log2_min_transform_block_size_minus2 = 0;
    seq->log2_diff_max_min_transform_block_size = 3;
    seq->max_transform_hierarchy_depth_inter = 2;
    seq->max_transform_hierarchy_depth_intra = 2;

    /* VUI — match packed SPS (vui_parameters_present_flag=0) */
    seq->vui_parameters_present_flag = 0;
}

static void fill_pic_param(VAEncPictureParameterBufferHEVC *pic,
                           db_encoder_t *enc,
                           VASurfaceID input_surface,
                           VASurfaceID recon_surface,
                           VASurfaceID ref_surface,
                           int is_idr)
{
    memset(pic, 0, sizeof(*pic));
    (void)input_surface;

    pic->decoded_curr_pic.picture_id = recon_surface;
    pic->decoded_curr_pic.pic_order_cnt = (int32_t)(enc->frame_count % enc->idr_interval);
    pic->decoded_curr_pic.flags = 0;

    for (int i = 0; i < 15; i++) {
        pic->reference_frames[i].picture_id = VA_INVALID_ID;
        pic->reference_frames[i].flags = VA_PICTURE_HEVC_INVALID;
    }

    if (!is_idr && ref_surface != VA_INVALID_ID) {
        pic->reference_frames[0].picture_id = ref_surface;
        pic->reference_frames[0].pic_order_cnt =
            (int32_t)((enc->frame_count - 1) % enc->idr_interval);
        pic->reference_frames[0].flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE;
    }

    pic->coded_buf = enc->coded_buf;
    pic->collocated_ref_pic_index = 0xFF;  /* no collocated ref */
    pic->last_picture = 0;
    pic->pic_init_qp = 26;
    pic->diff_cu_qp_delta_depth = 0;
    pic->pps_cb_qp_offset = 0;
    pic->pps_cr_qp_offset = 0;
    pic->num_tile_columns_minus1 = 0;
    pic->num_tile_rows_minus1 = 0;
    pic->log2_parallel_merge_level_minus2 = 0;
    pic->ctu_max_bitsize_allowed = 0;
    pic->num_ref_idx_l0_default_active_minus1 = 0;
    pic->num_ref_idx_l1_default_active_minus1 = 0;
    pic->slice_pic_parameter_set_id = 0;
    pic->nal_unit_type = is_idr ? 19 : 1;

    pic->pic_fields.bits.idr_pic_flag = is_idr ? 1 : 0;
    pic->pic_fields.bits.coding_type = is_idr ? 1 : 2;
    pic->pic_fields.bits.reference_pic_flag = 1;
    pic->pic_fields.bits.dependent_slice_segments_enabled_flag = 0;
    pic->pic_fields.bits.sign_data_hiding_enabled_flag = 0;
    pic->pic_fields.bits.constrained_intra_pred_flag = 0;
    pic->pic_fields.bits.transform_skip_enabled_flag = 0;
    pic->pic_fields.bits.cu_qp_delta_enabled_flag = 1;
    pic->pic_fields.bits.weighted_pred_flag = 0;
    pic->pic_fields.bits.weighted_bipred_flag = 0;
    pic->pic_fields.bits.transquant_bypass_enabled_flag = 0;
    pic->pic_fields.bits.tiles_enabled_flag = 0;
    pic->pic_fields.bits.entropy_coding_sync_enabled_flag = 0;
    pic->pic_fields.bits.loop_filter_across_tiles_enabled_flag = 1;
    pic->pic_fields.bits.pps_loop_filter_across_slices_enabled_flag = 1;
    pic->pic_fields.bits.scaling_list_data_present_flag = 0;
    pic->pic_fields.bits.screen_content_flag = 0;
    pic->pic_fields.bits.enable_gpu_weighted_prediction = 0;
    pic->pic_fields.bits.no_output_of_prior_pics_flag = 0;
}

static void fill_slice_param(VAEncSliceParameterBufferHEVC *slice,
                             db_encoder_t *enc,
                             VASurfaceID ref_surface,
                             int is_idr)
{
    int width_in_ctus  = ALIGN(enc->width, HEVC_CTU_SIZE) / HEVC_CTU_SIZE;
    int height_in_ctus = ALIGN(enc->height, HEVC_CTU_SIZE) / HEVC_CTU_SIZE;

    memset(slice, 0, sizeof(*slice));

    slice->slice_segment_address = 0;
    slice->num_ctu_in_slice = width_in_ctus * height_in_ctus;
    slice->slice_type = is_idr ? 2 : 1;  /* 2=I, 1=P */
    slice->slice_pic_parameter_set_id = 0;

    slice->num_ref_idx_l0_active_minus1 = 0;
    slice->num_ref_idx_l1_active_minus1 = 0;

    for (int i = 0; i < 15; i++) {
        slice->ref_pic_list0[i].picture_id = VA_INVALID_ID;
        slice->ref_pic_list0[i].flags = VA_PICTURE_HEVC_INVALID;
        slice->ref_pic_list1[i].picture_id = VA_INVALID_ID;
        slice->ref_pic_list1[i].flags = VA_PICTURE_HEVC_INVALID;
    }

    if (!is_idr && ref_surface != VA_INVALID_ID) {
        slice->ref_pic_list0[0].picture_id = ref_surface;
        slice->ref_pic_list0[0].pic_order_cnt =
            (int32_t)((enc->frame_count - 1) % enc->idr_interval);
        slice->ref_pic_list0[0].flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE;
    }

    slice->max_num_merge_cand = 5;
    slice->slice_qp_delta = 0;
    slice->slice_cb_qp_offset = 0;
    slice->slice_cr_qp_offset = 0;
    slice->slice_beta_offset_div2 = 0;
    slice->slice_tc_offset_div2 = 0;

    slice->slice_fields.bits.last_slice_of_pic_flag = 1;
    slice->slice_fields.bits.dependent_slice_segment_flag = 0;
    slice->slice_fields.bits.slice_temporal_mvp_enabled_flag = 0;
    slice->slice_fields.bits.slice_sao_luma_flag = 0;
    slice->slice_fields.bits.slice_sao_chroma_flag = 0;
    slice->slice_fields.bits.num_ref_idx_active_override_flag = 0;
    slice->slice_fields.bits.mvd_l1_zero_flag = 0;
    slice->slice_fields.bits.cabac_init_flag = 0;
    slice->slice_fields.bits.slice_deblocking_filter_disabled_flag = 0;
    slice->slice_fields.bits.slice_loop_filter_across_slices_enabled_flag = 1;
    slice->slice_fields.bits.collocated_from_l0_flag = 1;
}

/* ============================================================
 *  Rate control / HRD / framerate misc parameters
 * ============================================================ */

static VAStatus submit_rate_control(db_encoder_t *enc)
{
    VAStatus st;
    VABufferID rc_buf;
    unsigned int rc_size = sizeof(VAEncMiscParameterBuffer) +
                           sizeof(VAEncMiscParameterRateControl);

    st = vaCreateBuffer(enc->va_display, enc->context_id,
                        VAEncMiscParameterBufferType,
                        rc_size, 1, NULL, &rc_buf);
    if (st != VA_STATUS_SUCCESS) return st;

    VAEncMiscParameterBuffer *misc;
    st = vaMapBuffer(enc->va_display, rc_buf, (void **)&misc);
    if (st != VA_STATUS_SUCCESS) {
        vaDestroyBuffer(enc->va_display, rc_buf);
        return st;
    }

    misc->type = VAEncMiscParameterTypeRateControl;
    VAEncMiscParameterRateControl *rc =
        (VAEncMiscParameterRateControl *)misc->data;
    memset(rc, 0, sizeof(*rc));
    rc->bits_per_second = (uint32_t)enc->bitrate_kbps * 1000;
    rc->target_percentage = 80;
    rc->window_size = 1000;
    rc->initial_qp = 26;
    rc->min_qp = 18;
    rc->basic_unit_size = 0;
    rc->rc_flags.bits.reset = (enc->frame_count == 0) ? 1 : 0;
    rc->rc_flags.bits.disable_frame_skip = 1;
    rc->rc_flags.bits.disable_bit_stuffing = 1;

    vaUnmapBuffer(enc->va_display, rc_buf);

    st = vaRenderPicture(enc->va_display, enc->context_id, &rc_buf, 1);
    vaDestroyBuffer(enc->va_display, rc_buf);
    return st;
}

static VAStatus submit_hrd_param(db_encoder_t *enc)
{
    VAStatus st;
    VABufferID hrd_buf;
    unsigned int hrd_size = sizeof(VAEncMiscParameterBuffer) +
                            sizeof(VAEncMiscParameterHRD);

    st = vaCreateBuffer(enc->va_display, enc->context_id,
                        VAEncMiscParameterBufferType,
                        hrd_size, 1, NULL, &hrd_buf);
    if (st != VA_STATUS_SUCCESS) return st;

    VAEncMiscParameterBuffer *misc;
    st = vaMapBuffer(enc->va_display, hrd_buf, (void **)&misc);
    if (st != VA_STATUS_SUCCESS) {
        vaDestroyBuffer(enc->va_display, hrd_buf);
        return st;
    }

    misc->type = VAEncMiscParameterTypeHRD;
    VAEncMiscParameterHRD *hrd = (VAEncMiscParameterHRD *)misc->data;
    memset(hrd, 0, sizeof(*hrd));
    hrd->initial_buffer_fullness = (uint32_t)enc->bitrate_kbps * 1000 / 2;
    hrd->buffer_size = (uint32_t)enc->bitrate_kbps * 1000;

    vaUnmapBuffer(enc->va_display, hrd_buf);

    st = vaRenderPicture(enc->va_display, enc->context_id, &hrd_buf, 1);
    vaDestroyBuffer(enc->va_display, hrd_buf);
    return st;
}

static VAStatus submit_framerate_param(db_encoder_t *enc)
{
    VAStatus st;
    VABufferID fr_buf;
    unsigned int fr_size = sizeof(VAEncMiscParameterBuffer) +
                           sizeof(VAEncMiscParameterFrameRate);

    st = vaCreateBuffer(enc->va_display, enc->context_id,
                        VAEncMiscParameterBufferType,
                        fr_size, 1, NULL, &fr_buf);
    if (st != VA_STATUS_SUCCESS) return st;

    VAEncMiscParameterBuffer *misc;
    st = vaMapBuffer(enc->va_display, fr_buf, (void **)&misc);
    if (st != VA_STATUS_SUCCESS) {
        vaDestroyBuffer(enc->va_display, fr_buf);
        return st;
    }

    misc->type = VAEncMiscParameterTypeFrameRate;
    VAEncMiscParameterFrameRate *fr =
        (VAEncMiscParameterFrameRate *)misc->data;
    memset(fr, 0, sizeof(*fr));
    fr->framerate = enc->fps;

    vaUnmapBuffer(enc->va_display, fr_buf);

    st = vaRenderPicture(enc->va_display, enc->context_id, &fr_buf, 1);
    vaDestroyBuffer(enc->va_display, fr_buf);
    return st;
}

/* ============================================================
 *  DMA-BUF import
 * ============================================================ */

static VASurfaceID import_dmabuf_surface(db_encoder_t *enc,
                                         int dmabuf_fd,
                                         uint32_t stride,
                                         uint32_t offset,
                                         uint64_t drm_modifier)
{
    VADRMPRIMESurfaceDescriptor desc;
    memset(&desc, 0, sizeof(desc));

    desc.fourcc = VA_FOURCC_NV12;
    desc.width  = (uint32_t)enc->width;
    desc.height = (uint32_t)enc->height;
    desc.num_objects = 1;
    desc.objects[0].fd = dmabuf_fd;
    desc.objects[0].size = stride * enc->height * 3 / 2;
    desc.objects[0].drm_format_modifier = drm_modifier;

    desc.num_layers = 2;
    desc.layers[0].drm_format = DRM_FORMAT_R8;
    desc.layers[0].num_planes = 1;
    desc.layers[0].object_index[0] = 0;
    desc.layers[0].offset[0] = offset;
    desc.layers[0].pitch[0] = stride;
    desc.layers[1].drm_format = DRM_FORMAT_GR88;
    desc.layers[1].num_planes = 1;
    desc.layers[1].object_index[0] = 0;
    desc.layers[1].offset[0] = offset + stride * (uint32_t)enc->height;
    desc.layers[1].pitch[0] = stride;

    VASurfaceAttrib attribs[2];
    attribs[0].type = VASurfaceAttribMemoryType;
    attribs[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attribs[0].value.type = VAGenericValueTypeInteger;
    attribs[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;

    attribs[1].type = VASurfaceAttribExternalBufferDescriptor;
    attribs[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attribs[1].value.type = VAGenericValueTypePointer;
    attribs[1].value.value.p = &desc;

    VASurfaceID surface = VA_INVALID_ID;
    VAStatus st = vaCreateSurfaces(enc->va_display, VA_RT_FORMAT_YUV420,
                                   enc->width, enc->height,
                                   &surface, 1, attribs, 2);
    if (st != VA_STATUS_SUCCESS) {
        LOG_ERR("import_dmabuf_surface: vaCreateSurfaces failed: %s",
                vaErrorStr(st));
        return VA_INVALID_ID;
    }
    return surface;
}

/* ============================================================
 *  Core encode: submit all buffers + packed headers
 * ============================================================ */

static int encode_surface(db_encoder_t *enc, VASurfaceID input_surface,
                          uint8_t **out_buf, uint32_t *out_size,
                          int *is_keyframe)
{
    int is_idr = (enc->frame_count % enc->idr_interval == 0);
    *is_keyframe = is_idr;

    int cur_recon  = enc->current_recon_idx;
    int prev_recon = (cur_recon + enc->num_recon_surfaces - 1) %
                     enc->num_recon_surfaces;
    VASurfaceID recon_surface = enc->recon_surfaces[cur_recon];
    VASurfaceID ref_surface   = is_idr ? VA_INVALID_ID
                                       : enc->recon_surfaces[prev_recon];

    /* Begin picture */
    VA_CHECK(vaBeginPicture(enc->va_display, enc->context_id, input_surface),
             fail);

    /* --- Sequence parameter buffer (every IDR) --- */
    if (is_idr) {
        VAEncSequenceParameterBufferHEVC seq;
        fill_seq_param(&seq, enc);

        VABufferID seq_buf;
        VA_CHECK(vaCreateBuffer(enc->va_display, enc->context_id,
                                VAEncSequenceParameterBufferType,
                                sizeof(seq), 1, &seq, &seq_buf), fail_end);
        VA_CHECK(vaRenderPicture(enc->va_display, enc->context_id,
                                 &seq_buf, 1), fail_end);
        vaDestroyBuffer(enc->va_display, seq_buf);

        /* Packed VPS+SPS+PPS headers */
        VAStatus ph_st = submit_packed_sequence_headers(enc);
        if (ph_st != VA_STATUS_SUCCESS) {
            LOG_ERR("submit_packed_sequence_headers failed: %s",
                    vaErrorStr(ph_st));
        }
    }

    /* --- Rate control + HRD + framerate misc params --- */
    submit_rate_control(enc);
    submit_hrd_param(enc);
    submit_framerate_param(enc);

    /* --- Picture parameter buffer --- */
    {
        VAEncPictureParameterBufferHEVC pic;
        fill_pic_param(&pic, enc, input_surface, recon_surface,
                       ref_surface, is_idr);

        VABufferID pic_buf;
        VA_CHECK(vaCreateBuffer(enc->va_display, enc->context_id,
                                VAEncPictureParameterBufferType,
                                sizeof(pic), 1, &pic, &pic_buf), fail_end);
        VA_CHECK(vaRenderPicture(enc->va_display, enc->context_id,
                                 &pic_buf, 1), fail_end);
        vaDestroyBuffer(enc->va_display, pic_buf);
    }

    /* --- Packed slice header --- */
    {
        VAStatus sh_st = submit_packed_slice_header(enc, is_idr);
        if (sh_st != VA_STATUS_SUCCESS) {
            LOG_ERR("submit_packed_slice_header failed: %s",
                    vaErrorStr(sh_st));
        }
    }

    /* --- Slice parameter buffer --- */
    {
        VAEncSliceParameterBufferHEVC slice;
        fill_slice_param(&slice, enc, ref_surface, is_idr);

        VABufferID slice_buf;
        VA_CHECK(vaCreateBuffer(enc->va_display, enc->context_id,
                                VAEncSliceParameterBufferType,
                                sizeof(slice), 1, &slice, &slice_buf),
                 fail_end);
        VA_CHECK(vaRenderPicture(enc->va_display, enc->context_id,
                                 &slice_buf, 1), fail_end);
        vaDestroyBuffer(enc->va_display, slice_buf);
    }

    /* End picture — kicks the hardware encoder */
    VA_CHECK(vaEndPicture(enc->va_display, enc->context_id), fail);

    /* Sync — wait for GPU to finish encoding */
    VA_CHECK(vaSyncSurface(enc->va_display, input_surface), fail);

    /* Map the coded buffer to get the bitstream */
    VACodedBufferSegment *seg = NULL;
    VA_CHECK(vaMapBuffer(enc->va_display, enc->coded_buf, (void **)&seg),
             fail);

    if (seg) {
        *out_buf  = (uint8_t *)seg->buf;
        *out_size = seg->size;
    } else {
        *out_buf  = NULL;
        *out_size = 0;
    }

    /* Advance state */
    enc->current_recon_idx = (cur_recon + 1) % enc->num_recon_surfaces;
    enc->frame_count++;
    return 0;

fail_end:
    vaEndPicture(enc->va_display, enc->context_id);
fail:
    return -1;
}

/* ============================================================
 *  Public API
 * ============================================================ */

int db_encoder_init(db_encoder_t *enc, int width, int height,
                    int bitrate_kbps, int fps, uint8_t codec)
{
    memset(enc, 0, sizeof(*enc));
    enc->width = width;
    enc->height = height;
    enc->bitrate_kbps = bitrate_kbps;
    enc->fps = fps;
    enc->codec = codec;
    enc->frame_count = 0;
    enc->idr_interval = fps * 2;
    enc->current_recon_idx = 0;
    enc->config_id = VA_INVALID_ID;
    enc->context_id = VA_INVALID_ID;
    enc->coded_buf = VA_INVALID_ID;
    enc->drm_fd = -1;

    if (codec != DB_CODEC_HEVC) {
        LOG_ERR("Only HEVC codec is currently supported (got %d)", codec);
        return -1;
    }

    enc->drm_fd = open_drm_render_node();
    if (enc->drm_fd < 0)
        return -1;

    enc->va_display = vaGetDisplayDRM(enc->drm_fd);
    if (!enc->va_display) {
        LOG_ERR("vaGetDisplayDRM failed");
        goto fail_drm;
    }

    int va_major, va_minor;
    VA_CHECK(vaInitialize(enc->va_display, &va_major, &va_minor), fail_drm);
    LOG_INF("VA-API %d.%d initialized", va_major, va_minor);

    /* Create encode config with VBR rate control AND packed headers */
    VAConfigAttrib attribs[3];
    attribs[0].type = VAConfigAttribRTFormat;
    attribs[0].value = VA_RT_FORMAT_YUV420;
    attribs[1].type = VAConfigAttribRateControl;
    attribs[1].value = VA_RC_VBR;
    attribs[2].type = VAConfigAttribEncPackedHeaders;
    attribs[2].value = VA_ENC_PACKED_HEADER_SEQUENCE |
                       VA_ENC_PACKED_HEADER_PICTURE  |
                       VA_ENC_PACKED_HEADER_SLICE;

    VA_CHECK(vaCreateConfig(enc->va_display, VAProfileHEVCMain,
                            VAEntrypointEncSlice,
                            attribs, 3, &enc->config_id), fail_va);
    LOG_INF("HEVC encode config created (packed headers enabled)");

    enc->num_recon_surfaces = DB_ENCODER_SURFACE_POOL;
    VA_CHECK(vaCreateSurfaces(enc->va_display, VA_RT_FORMAT_YUV420,
                              ALIGN(width, HEVC_CTU_SIZE),
                              ALIGN(height, HEVC_CTU_SIZE),
                              enc->recon_surfaces, enc->num_recon_surfaces,
                              NULL, 0), fail_config);
    LOG_INF("Created %d reconstructed surfaces (%dx%d)",
            enc->num_recon_surfaces,
            ALIGN(width, HEVC_CTU_SIZE), ALIGN(height, HEVC_CTU_SIZE));

    VA_CHECK(vaCreateContext(enc->va_display, enc->config_id,
                            ALIGN(width, HEVC_CTU_SIZE),
                            ALIGN(height, HEVC_CTU_SIZE),
                            VA_PROGRESSIVE,
                            enc->recon_surfaces, enc->num_recon_surfaces,
                            &enc->context_id), fail_surfaces);
    LOG_INF("HEVC encode context created");

    enc->coded_buf_size = calc_coded_buf_size(width, height);
    VA_CHECK(vaCreateBuffer(enc->va_display, enc->context_id,
                            VAEncCodedBufferType,
                            enc->coded_buf_size, 1, NULL,
                            &enc->coded_buf), fail_context);
    LOG_INF("Coded buffer: %d bytes", enc->coded_buf_size);

    /* Allocate reusable BGRx→NV12 conversion buffer */
    enc->conv_buf_size = (size_t)width * height * 3 / 2;
    enc->conv_buf = malloc(enc->conv_buf_size);
    if (!enc->conv_buf) {
        LOG_ERR("Failed to allocate conversion buffer (%zu bytes)",
                enc->conv_buf_size);
        goto fail_coded_buf;
    }

    LOG_INF("Encoder ready: %dx%d HEVC @ %d kbps, %d fps, IDR every %d frames",
            width, height, bitrate_kbps, fps, enc->idr_interval);
    return 0;

fail_coded_buf:
    vaDestroyBuffer(enc->va_display, enc->coded_buf);
    enc->coded_buf = VA_INVALID_ID;
fail_context:
    vaDestroyContext(enc->va_display, enc->context_id);
    enc->context_id = VA_INVALID_ID;
fail_surfaces:
    vaDestroySurfaces(enc->va_display, enc->recon_surfaces,
                      enc->num_recon_surfaces);
fail_config:
    vaDestroyConfig(enc->va_display, enc->config_id);
    enc->config_id = VA_INVALID_ID;
fail_va:
    vaTerminate(enc->va_display);
fail_drm:
    close(enc->drm_fd);
    enc->drm_fd = -1;
    return -1;
}

int db_encoder_encode_dmabuf(db_encoder_t *enc, int dmabuf_fd,
                             uint32_t dmabuf_stride, uint32_t dmabuf_offset,
                             uint64_t drm_modifier,
                             uint8_t **out_buf, uint32_t *out_size,
                             int *is_keyframe)
{
    VASurfaceID input = import_dmabuf_surface(enc, dmabuf_fd,
                                              dmabuf_stride, dmabuf_offset,
                                              drm_modifier);
    if (input == VA_INVALID_ID)
        return -1;

    int ret = encode_surface(enc, input, out_buf, out_size, is_keyframe);
    vaDestroySurfaces(enc->va_display, &input, 1);
    return ret;
}

int db_encoder_encode_nv12(db_encoder_t *enc, const uint8_t *nv12_data,
                           uint32_t stride,
                           uint8_t **out_buf, uint32_t *out_size,
                           int *is_keyframe)
{
    VASurfaceID input;
    VAStatus st = vaCreateSurfaces(enc->va_display, VA_RT_FORMAT_YUV420,
                                   ALIGN(enc->width, HEVC_CTU_SIZE),
                                   ALIGN(enc->height, HEVC_CTU_SIZE),
                                   &input, 1, NULL, 0);
    if (st != VA_STATUS_SUCCESS) {
        LOG_ERR("encode_nv12: vaCreateSurfaces failed: %s", vaErrorStr(st));
        return -1;
    }

    VAImage va_image;
    st = vaDeriveImage(enc->va_display, input, &va_image);
    if (st != VA_STATUS_SUCCESS) {
        LOG_ERR("encode_nv12: vaDeriveImage failed: %s", vaErrorStr(st));
        vaDestroySurfaces(enc->va_display, &input, 1);
        return -1;
    }

    uint8_t *surface_ptr;
    st = vaMapBuffer(enc->va_display, va_image.buf, (void **)&surface_ptr);
    if (st != VA_STATUS_SUCCESS) {
        LOG_ERR("encode_nv12: vaMapBuffer failed: %s", vaErrorStr(st));
        vaDestroyImage(enc->va_display, va_image.image_id);
        vaDestroySurfaces(enc->va_display, &input, 1);
        return -1;
    }

    /* Copy Y plane */
    const uint8_t *src = nv12_data;
    uint8_t *dst = surface_ptr + va_image.offsets[0];
    for (int y = 0; y < enc->height; y++) {
        memcpy(dst, src, enc->width);
        src += stride;
        dst += va_image.pitches[0];
    }

    /* Copy UV plane */
    src = nv12_data + stride * enc->height;
    dst = surface_ptr + va_image.offsets[1];
    for (int y = 0; y < enc->height / 2; y++) {
        memcpy(dst, src, enc->width);
        src += stride;
        dst += va_image.pitches[1];
    }

    vaUnmapBuffer(enc->va_display, va_image.buf);
    vaDestroyImage(enc->va_display, va_image.image_id);

    int ret = encode_surface(enc, input, out_buf, out_size, is_keyframe);
    vaDestroySurfaces(enc->va_display, &input, 1);
    return ret;
}

int db_encoder_encode_bgrx(db_encoder_t *enc, const uint8_t *bgrx_data,
                            uint32_t stride,
                            uint8_t **out_buf, uint32_t *out_size,
                            int *is_keyframe)
{
    /* Ensure conversion buffer is large enough */
    size_t needed = (size_t)enc->width * enc->height * 3 / 2;
    if (needed > enc->conv_buf_size || !enc->conv_buf) {
        free(enc->conv_buf);
        enc->conv_buf = malloc(needed);
        if (!enc->conv_buf) {
            LOG_ERR("encode_bgrx: realloc conv_buf failed (%zu bytes)", needed);
            enc->conv_buf_size = 0;
            return -1;
        }
        enc->conv_buf_size = needed;
    }

    /*
     * BGRx → NV12 conversion (BT.601 limited-range):
     *   Y  = ((  66*R + 129*G +  25*B + 128) >> 8) + 16
     *   Cb = (( -38*R -  74*G + 112*B + 128) >> 8) + 128
     *   Cr = (( 112*R -  94*G -  18*B + 128) >> 8) + 128
     *
     * BGRx byte order: byte[0]=B, byte[1]=G, byte[2]=R, byte[3]=X (ignored)
     * NV12 layout: packed Y plane (stride = enc->width), then interleaved
     * UV plane (stride = enc->width) at offset width*height.
     * UV subsampling: one Cb/Cr pair per 2×2 luma block (top-left sample).
     */
    uint8_t *nv12_y  = enc->conv_buf;
    uint8_t *nv12_uv = enc->conv_buf + (size_t)enc->width * enc->height;

    for (int y = 0; y < enc->height; y++) {
        const uint8_t *src_row = bgrx_data + (size_t)y * stride;
        uint8_t       *dst_y   = nv12_y   + (size_t)y * enc->width;

        for (int x = 0; x < enc->width; x++) {
            int b = src_row[x * 4 + 0];
            int g = src_row[x * 4 + 1];
            int r = src_row[x * 4 + 2];
            dst_y[x] = (uint8_t)(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
        }

        /* UV plane: one pair per 2×2 block — sample at even rows only */
        if ((y & 1) == 0 && y / 2 < enc->height / 2) {
            uint8_t *dst_uv = nv12_uv + (size_t)(y / 2) * enc->width;
            for (int x = 0; x < enc->width; x += 2) {
                int b = src_row[x * 4 + 0];
                int g = src_row[x * 4 + 1];
                int r = src_row[x * 4 + 2];
                dst_uv[x]     = (uint8_t)(((-38 * r -  74 * g + 112 * b + 128) >> 8) + 128);
                dst_uv[x + 1] = (uint8_t)(((112 * r -  94 * g -  18 * b + 128) >> 8) + 128);
            }
        }
    }

    return db_encoder_encode_nv12(enc, enc->conv_buf, (uint32_t)enc->width,
                                  out_buf, out_size, is_keyframe);
}

void db_encoder_release_frame(db_encoder_t *enc)
{
    if (enc->coded_buf != VA_INVALID_ID) {
        vaUnmapBuffer(enc->va_display, enc->coded_buf);
    }
}

void db_encoder_force_idr(db_encoder_t *enc)
{
    uint64_t remainder = enc->frame_count % enc->idr_interval;
    if (remainder != 0) {
        enc->frame_count += (enc->idr_interval - remainder);
    }
}

void db_encoder_set_bitrate(db_encoder_t *enc, int bitrate_kbps)
{
    if (!enc || bitrate_kbps <= 0) return;
    if (enc->bitrate_kbps != bitrate_kbps) {
        LOG_INF("Bitrate change: %d -> %d kbps", enc->bitrate_kbps, bitrate_kbps);
        enc->bitrate_kbps = bitrate_kbps;
    }
}

void db_encoder_destroy(db_encoder_t *enc)
{
    if (!enc) return;

    free(enc->conv_buf);
    enc->conv_buf = NULL;

    if (enc->coded_buf != VA_INVALID_ID)
        vaDestroyBuffer(enc->va_display, enc->coded_buf);
    if (enc->context_id != VA_INVALID_ID)
        vaDestroyContext(enc->va_display, enc->context_id);
    if (enc->num_recon_surfaces > 0)
        vaDestroySurfaces(enc->va_display, enc->recon_surfaces,
                          enc->num_recon_surfaces);
    if (enc->config_id != VA_INVALID_ID)
        vaDestroyConfig(enc->va_display, enc->config_id);
    if (enc->va_display)
        vaTerminate(enc->va_display);
    if (enc->drm_fd >= 0)
        close(enc->drm_fd);

    memset(enc, 0, sizeof(*enc));
    enc->drm_fd = -1;

    LOG_INF("Encoder destroyed");
}
