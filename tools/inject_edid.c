/*
 * inject_edid.c - Inject custom EDID into vkms Virtual-1 connector via DRM ioctl
 *
 * Opens /dev/dri/card0 (vkms), finds the Virtual-1 connector,
 * creates a blob property with our EDID bytes, then sets it on the connector.
 * This triggers drm_connector_update_edid_property() which fires a hotplug
 * uevent with HOTPLUG=1, causing Mutter to re-probe and rebuild mode list.
 *
 * Usage: sudo ./inject_edid <edid.bin>
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

static int read_edid(const char *path, uint8_t *buf, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror("fopen edid"); return -1; }
    *len = fread(buf, 1, 256, f);
    fclose(f);
    if (*len < 128) { fprintf(stderr, "EDID too short: %zu bytes\n", *len); return -1; }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <edid.bin>\n", argv[0]);
        return 1;
    }

    uint8_t edid_buf[256];
    size_t edid_len = 0;
    if (read_edid(argv[1], edid_buf, &edid_len) < 0)
        return 1;
    printf("Read %zu bytes of EDID\n", edid_len);

    /* Open vkms card */
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("open /dev/dri/card0"); return 1; }

    drmModeRes *res = drmModeGetResources(fd);
    if (!res) { perror("drmModeGetResources"); close(fd); return 1; }

    printf("Connectors: %d\n", res->count_connectors);

    int target_conn = -1;
    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *conn = drmModeGetConnector(fd, res->connectors[i]);
        if (!conn) continue;
        printf("  connector %d: type=%d id=%d connection=%d\n",
               i, conn->connector_type, conn->connector_id, conn->connection);
        /* vkms Virtual connector type is DRM_MODE_CONNECTOR_VIRTUAL = 15 */
        if (conn->connector_type == DRM_MODE_CONNECTOR_VIRTUAL) {
            target_conn = conn->connector_id;
            printf("  -> Found Virtual connector id=%d, modes=%d\n",
                   conn->connector_id, conn->count_modes);
            for (int m = 0; m < conn->count_modes && m < 5; m++) {
                printf("     mode[%d]: %dx%d@%d\n", m,
                       conn->modes[m].hdisplay, conn->modes[m].vdisplay,
                       conn->modes[m].vrefresh);
            }
        }
        drmModeFreeConnector(conn);
        if (target_conn >= 0) break;
    }

    if (target_conn < 0) {
        fprintf(stderr, "No Virtual connector found\n");
        drmModeFreeResources(res);
        close(fd);
        return 1;
    }

    /* List connector properties to find EDID property id */
    drmModeConnector *conn = drmModeGetConnectorCurrent(fd, target_conn);
    if (!conn) { perror("drmModeGetConnectorCurrent"); goto done; }

    printf("Connector %d properties: %d\n", target_conn, conn->count_props);
    uint32_t edid_prop_id = 0;
    for (int i = 0; i < conn->count_props; i++) {
        drmModePropertyRes *prop = drmModeGetProperty(fd, conn->props[i]);
        if (!prop) continue;
        printf("  prop[%d]: id=%d name='%s' flags=0x%x\n",
               i, prop->prop_id, prop->name, prop->flags);
        if (strcmp(prop->name, "EDID") == 0) {
            edid_prop_id = prop->prop_id;
            printf("  -> EDID property id = %d, current blob = %llu\n",
                   edid_prop_id, (unsigned long long)conn->prop_values[i]);
        }
        drmModeFreeProperty(prop);
    }
    drmModeFreeConnector(conn);

    if (edid_prop_id == 0) {
        fprintf(stderr, "EDID property not found on connector\n");
        /* This is OK - EDID property is read-only; try the override approach */
        /* Print current modes from edid_override and check if modes changed */
        conn = drmModeGetConnector(fd, target_conn);
        if (conn) {
            printf("Current modes after edid_override (%d total):\n", conn->count_modes);
            for (int m = 0; m < conn->count_modes; m++) {
                printf("  %dx%d@%d flags=0x%x\n",
                       conn->modes[m].hdisplay, conn->modes[m].vdisplay,
                       conn->modes[m].vrefresh, conn->modes[m].flags);
                if (conn->modes[m].hdisplay == 2880 && conn->modes[m].vdisplay == 1800)
                    printf("  *** FOUND 2880x1800! ***\n");
            }
            drmModeFreeConnector(conn);
        }
        goto done;
    }

    /*
     * EDID is a read-only blob property — we can't set it via ioctl.
     * The edid_override debugfs path is the only way. Let's verify
     * whether the override is being used by reading back modes.
     */
    printf("\nNote: EDID property is read-only (set by kernel from edid_override)\n");
    printf("Checking if edid_override took effect by reading connector modes:\n");
    conn = drmModeGetConnector(fd, target_conn);
    if (conn) {
        printf("Modes from DRM (%d total):\n", conn->count_modes);
        int found = 0;
        for (int m = 0; m < conn->count_modes; m++) {
            if (conn->modes[m].hdisplay == 2880 && conn->modes[m].vdisplay == 1800) {
                printf("  *** 2880x1800@%d FOUND in DRM mode list ***\n", conn->modes[m].vrefresh);
                found = 1;
            }
        }
        if (!found)
            printf("  2880x1800 NOT found. Total modes: %d\n", conn->count_modes);
        drmModeFreeConnector(conn);
    }

done:
    drmModeFreeResources(res);
    close(fd);
    return 0;
}
