/*
 * set_mode.c - Force custom 2880x1800@60Hz mode on vkms Virtual-1 via DRM ioctl
 *
 * vkms does not use EDID - it uses drm_add_modes_noedid() which has a hardcoded
 * list. We bypass this entirely by:
 *   1. Getting the current CRTC and connector IDs
 *   2. Creating a custom drmModeModeInfo for 2880x1800@60Hz
 *   3. Calling drmModeSetCrtc() with the custom mode
 *
 * This makes the kernel believe the CRTC is in 2880x1800 mode and fires
 * a hotplug event that Mutter picks up via MonitorsChanged.
 *
 * Usage: sudo ./set_mode
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#ifndef DRM_MODE_CONNECTOR_VIRTUAL
#define DRM_MODE_CONNECTOR_VIRTUAL 15
#endif

/*
 * 2880x1800@60Hz CVT-RB timing (computed in prior session):
 * H: active=2880, blank=160, total=3040; front=48, sync=32, back=80
 * V: active=1800, blank=30, total=1830; front=3, sync=6, back=21
 * Pixel clock: 333790 kHz → 59.997 Hz
 */
static drmModeModeInfo make_2880x1800_mode(void)
{
    drmModeModeInfo m;
    memset(&m, 0, sizeof(m));

    m.clock       = 333790;   /* kHz */
    m.hdisplay    = 2880;
    m.hsync_start = 2928;     /* hdisplay + front porch (48) */
    m.hsync_end   = 2960;     /* hsync_start + sync width (32) */
    m.htotal      = 3040;     /* hdisplay + blank (160) */
    m.vdisplay    = 1800;
    m.vsync_start = 1803;     /* vdisplay + front porch (3) */
    m.vsync_end   = 1809;     /* vsync_start + sync width (6) */
    m.vtotal      = 1830;     /* vdisplay + blank (30) */
    m.vrefresh    = 60;
    m.flags       = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC; /* CVT-RB: -H +V */
    m.type        = DRM_MODE_TYPE_USERDEF;
    snprintf(m.name, DRM_DISPLAY_MODE_LEN, "2880x1800");

    return m;
}

int main(void)
{
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("open /dev/dri/card0"); return 1; }

    /* Request master (needed for modesetting) */
    if (drmSetMaster(fd) < 0) {
        fprintf(stderr, "drmSetMaster failed: %s\n", strerror(errno));
        fprintf(stderr, "Note: if 'Device or resource busy', Mutter holds master.\n");
        fprintf(stderr, "Try: sudo ./set_mode  (while GNOME is running)\n");
        /* Continue anyway - atomic might work without master in some configs */
    }

    drmModeRes *res = drmModeGetResources(fd);
    if (!res) { perror("drmModeGetResources"); close(fd); return 1; }

    printf("CRTCs: %d, Connectors: %d, Encoders: %d\n",
           res->count_crtcs, res->count_connectors, res->count_encoders);

    /* Find Virtual connector */
    uint32_t conn_id = 0, enc_id = 0, crtc_id = 0;
    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
        if (!c) continue;
        if (c->connector_type == DRM_MODE_CONNECTOR_VIRTUAL) {
            conn_id = c->connector_id;
            enc_id  = c->encoder_id;
            printf("Virtual connector: id=%u encoder_id=%u connection=%d modes=%d\n",
                   conn_id, enc_id, c->connection, c->count_modes);
        }
        drmModeFreeConnector(c);
    }

    if (!conn_id) {
        fprintf(stderr, "Virtual connector not found\n");
        drmModeFreeResources(res);
        close(fd);
        return 1;
    }

    /* Find CRTC for this connector via encoder */
    if (enc_id) {
        drmModeEncoder *enc = drmModeGetEncoder(fd, enc_id);
        if (enc) {
            crtc_id = enc->crtc_id;
            printf("Encoder %u -> CRTC %u (possible_crtcs=0x%x)\n",
                   enc_id, crtc_id, enc->possible_crtcs);
            drmModeFreeEncoder(enc);
        }
    }

    /* If no CRTC via encoder, pick first available */
    if (!crtc_id && res->count_crtcs > 0) {
        crtc_id = res->crtcs[0];
        printf("Using first CRTC: %u\n", crtc_id);
    }

    if (!crtc_id) {
        fprintf(stderr, "No CRTC available\n");
        drmModeFreeResources(res);
        close(fd);
        return 1;
    }

    /* Show current CRTC state */
    drmModeCrtc *crtc = drmModeGetCrtc(fd, crtc_id);
    if (crtc) {
        printf("Current CRTC %u: fb=%u, mode=%dx%d@%d, valid=%d\n",
               crtc_id, crtc->buffer_id,
               crtc->mode.hdisplay, crtc->mode.vdisplay, crtc->mode.vrefresh,
               crtc->mode_valid);
        drmModeFreeCrtc(crtc);
    }

    /* Build our custom mode */
    drmModeModeInfo mode = make_2880x1800_mode();
    printf("\nAttempting to set mode: %dx%d@%d clock=%ukHz\n",
           mode.hdisplay, mode.vdisplay, mode.vrefresh, mode.clock);

    /*
     * We need a framebuffer to set a mode. Since we just want Mutter to see
     * the mode (not actually render), try DRM_IOCTL_MODE_SETCRTC with fb=0
     * which on vkms should still update the mode.
     */
    int ret = drmModeSetCrtc(fd, crtc_id, 0 /* fb_id */, 0, 0,
                              &conn_id, 1, &mode);
    if (ret < 0) {
        fprintf(stderr, "drmModeSetCrtc failed: %s (errno=%d)\n", strerror(errno), errno);
        if (errno == EACCES || errno == EBUSY) {
            fprintf(stderr, "\nMutter holds DRM master. We need to use a different approach.\n");
            fprintf(stderr, "Suggestion: Use wlr-randr or gnome-randr to add custom mode.\n");
        }
    } else {
        printf("drmModeSetCrtc succeeded!\n");
    }

    drmDropMaster(fd);
    drmModeFreeResources(res);
    close(fd);
    return (ret < 0) ? 1 : 0;
}
