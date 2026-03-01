/*
 * test_encode_only.c
 * Quick test: init encoder + encode 5 NV12 frames, no SRT needed.
 * Validates the vaEndPicture fix.
 */
#include "encoder.h"
#include "protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void generate_nv12(uint8_t *buf, int w, int h, int n)
{
    int y_size = w * h;
    int bar = (n * 8) % w;
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++)
            buf[r * w + c] = (c >= bar && c < bar + 16) ? 235 : (uint8_t)(c * 255 / w);
    memset(buf + y_size, 128, y_size / 2);
}

int main(void)
{
    int w = 1920, h = 1080, fps = 30, kbps = 8000;

    printf("[encode_only] Init encoder %dx%d...\n", w, h);
    db_encoder_t enc;
    if (db_encoder_init(&enc, w, h, kbps, fps, DB_CODEC_HEVC) != 0) {
        fprintf(stderr, "[encode_only] FAIL: encoder init\n");
        return 1;
    }
    printf("[encode_only] Encoder OK\n");

    int nv12_size = w * h * 3 / 2;
    uint8_t *nv12 = malloc(nv12_size);
    if (!nv12) { fprintf(stderr, "malloc\n"); return 1; }

    for (int i = 0; i < 5; i++) {
        generate_nv12(nv12, w, h, i);

        uint8_t *out = NULL;
        uint32_t out_sz = 0;
        int kf = 0;

        int rc = db_encoder_encode_nv12(&enc, nv12, w, &out, &out_sz, &kf);
        if (rc != 0) {
            fprintf(stderr, "[encode_only] FAIL: encode frame %d returned %d\n", i, rc);
            free(nv12);
            db_encoder_destroy(&enc);
            return 1;
        }
        printf("[encode_only] Frame %d: %u bytes, keyframe=%d\n", i, out_sz, kf);
        db_encoder_release_frame(&enc);
    }

    free(nv12);
    db_encoder_destroy(&enc);
    printf("[encode_only] PASS — 5 frames encoded successfully\n");
    return 0;
}
