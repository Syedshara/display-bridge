/*
 * input.c
 * Receives mouse/keyboard events from Mac receiver via UDP and injects
 * them into the Ubuntu system using the Linux uinput subsystem.
 *
 * Protocol:
 *   Each UDP datagram contains: db_packet_header_t + db_input_event_t
 *   Events are translated to Linux input_event structs and written to
 *   a virtual uinput device that acts as a combined mouse + keyboard.
 *
 * Permissions:
 *   /dev/uinput requires either root or the 'input' group.
 *   The user should add themselves to the input group:
 *     sudo usermod -aG input $USER
 */

#define _GNU_SOURCE
#include "input.h"
#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <linux/uinput.h>
#include <linux/input-event-codes.h>

#define LOG_ERR(fmt, ...) fprintf(stderr, "[input] ERROR: " fmt "\n", ##__VA_ARGS__)
#define LOG_INF(fmt, ...) fprintf(stdout, "[input] " fmt "\n", ##__VA_ARGS__)

/* Maximum UDP datagram size for input events */
#define INPUT_BUF_SIZE 512

struct db_input {
    int             uinput_fd;
    int             udp_sock;
    int             listen_port;
    volatile int    running;
};

/* ---------- uinput device setup ---------- */

static int setup_uinput_device(void)
{
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        LOG_ERR("Cannot open /dev/uinput: %s (are you in the 'input' group?)",
                strerror(errno));
        return -1;
    }

    /* Enable event types */
    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0 ||
        ioctl(fd, UI_SET_EVBIT, EV_REL) < 0 ||
        ioctl(fd, UI_SET_EVBIT, EV_ABS) < 0 ||
        ioctl(fd, UI_SET_EVBIT, EV_SYN) < 0) {
        LOG_ERR("ioctl UI_SET_EVBIT failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    /* Enable all keyboard keys */
    for (int i = 0; i < KEY_MAX; i++) {
        ioctl(fd, UI_SET_KEYBIT, i);
    }

    /* Enable mouse buttons */
    ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT);
    ioctl(fd, UI_SET_KEYBIT, BTN_MIDDLE);
    ioctl(fd, UI_SET_KEYBIT, BTN_SIDE);
    ioctl(fd, UI_SET_KEYBIT, BTN_EXTRA);

    /* Enable relative axes (mouse movement) */
    ioctl(fd, UI_SET_RELBIT, REL_X);
    ioctl(fd, UI_SET_RELBIT, REL_Y);
    ioctl(fd, UI_SET_RELBIT, REL_WHEEL);
    ioctl(fd, UI_SET_RELBIT, REL_HWHEEL);

    /* Enable absolute axes (for absolute mouse positioning) */
    ioctl(fd, UI_SET_ABSBIT, ABS_X);
    ioctl(fd, UI_SET_ABSBIT, ABS_Y);

    /* Configure the virtual device */
    struct uinput_setup setup;
    memset(&setup, 0, sizeof(setup));
    strncpy(setup.name, "display-bridge-input", UINPUT_MAX_NAME_SIZE - 1);
    setup.id.bustype = BUS_VIRTUAL;
    setup.id.vendor  = 0xDB01;  /* display-bridge vendor */
    setup.id.product = 0x0001;
    setup.id.version = 1;

    /* Set absolute axis parameters */
    struct uinput_abs_setup abs_setup;

    memset(&abs_setup, 0, sizeof(abs_setup));
    abs_setup.code = ABS_X;
    abs_setup.absinfo.minimum = 0;
    abs_setup.absinfo.maximum = DB_TARGET_WIDTH - 1;
    abs_setup.absinfo.resolution = 1;
    if (ioctl(fd, UI_ABS_SETUP, &abs_setup) < 0) {
        LOG_ERR("UI_ABS_SETUP ABS_X failed: %s", strerror(errno));
    }

    memset(&abs_setup, 0, sizeof(abs_setup));
    abs_setup.code = ABS_Y;
    abs_setup.absinfo.minimum = 0;
    abs_setup.absinfo.maximum = DB_TARGET_HEIGHT - 1;
    abs_setup.absinfo.resolution = 1;
    if (ioctl(fd, UI_ABS_SETUP, &abs_setup) < 0) {
        LOG_ERR("UI_ABS_SETUP ABS_Y failed: %s", strerror(errno));
    }

    if (ioctl(fd, UI_DEV_SETUP, &setup) < 0) {
        LOG_ERR("UI_DEV_SETUP failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        LOG_ERR("UI_DEV_CREATE failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    /* Give the system a moment to register the new device */
    usleep(100000);

    LOG_INF("uinput virtual device created: display-bridge-input");
    return fd;
}

/* Write a single input event to uinput */
static void emit_event(int fd, unsigned short type, unsigned short code,
                       int value)
{
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type  = type;
    ev.code  = code;
    ev.value = value;
    write(fd, &ev, sizeof(ev));
}

/* Write a SYN_REPORT to flush the event batch */
static void emit_syn(int fd)
{
    emit_event(fd, EV_SYN, SYN_REPORT, 0);
}

/* Translate a db_input_event_t into Linux input events */
static void handle_input_event(db_input_t *inp, const db_input_event_t *evt)
{
    switch (evt->event_type) {
    case DB_INPUT_MOUSE_MOVE:
        /* Absolute mouse move — x, y are screen coordinates */
        emit_event(inp->uinput_fd, EV_ABS, ABS_X, evt->x);
        emit_event(inp->uinput_fd, EV_ABS, ABS_Y, evt->y);
        emit_syn(inp->uinput_fd);
        break;

    case DB_INPUT_MOUSE_BUTTON:
        /* value field: button code (1=left, 2=right, 3=middle)
         * y field: 1=press, 0=release */
        {
            unsigned short btn;
            switch (evt->value) {
            case 1:  btn = BTN_LEFT;   break;
            case 2:  btn = BTN_RIGHT;  break;
            case 3:  btn = BTN_MIDDLE; break;
            default: btn = BTN_LEFT;   break;
            }
            emit_event(inp->uinput_fd, EV_KEY, btn, evt->y);
            emit_syn(inp->uinput_fd);
        }
        break;

    case DB_INPUT_MOUSE_SCROLL:
        /* value: scroll delta (positive=up, negative=down)
         * x: horizontal scroll delta */
        if (evt->value != 0)
            emit_event(inp->uinput_fd, EV_REL, REL_WHEEL, evt->value);
        if (evt->x != 0)
            emit_event(inp->uinput_fd, EV_REL, REL_HWHEEL, evt->x);
        emit_syn(inp->uinput_fd);
        break;

    case DB_INPUT_KEY_DOWN:
        /* x field: Linux key code */
        emit_event(inp->uinput_fd, EV_KEY, (unsigned short)evt->x, 1);
        emit_syn(inp->uinput_fd);
        break;

    case DB_INPUT_KEY_UP:
        /* x field: Linux key code */
        emit_event(inp->uinput_fd, EV_KEY, (unsigned short)evt->x, 0);
        emit_syn(inp->uinput_fd);
        break;

    default:
        LOG_ERR("Unknown input event type: 0x%02x", evt->event_type);
        break;
    }
}

/* ---------- UDP socket setup ---------- */

static int setup_udp_listener(int port)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        LOG_ERR("socket() failed: %s", strerror(errno));
        return -1;
    }

    /* Allow address reuse */
    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    /* Set receive timeout for clean shutdown */
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERR("bind() port %d failed: %s", port, strerror(errno));
        close(sock);
        return -1;
    }

    LOG_INF("UDP input listener on port %d", port);
    return sock;
}

/* ========== Public API ========== */

db_input_t *db_input_init(int listen_port)
{
    db_input_t *inp = calloc(1, sizeof(db_input_t));
    if (!inp) return NULL;

    inp->listen_port = listen_port;
    inp->running = 0;

    /* Set up uinput virtual device */
    inp->uinput_fd = setup_uinput_device();
    if (inp->uinput_fd < 0) {
        free(inp);
        return NULL;
    }

    /* Set up UDP listener */
    inp->udp_sock = setup_udp_listener(listen_port);
    if (inp->udp_sock < 0) {
        ioctl(inp->uinput_fd, UI_DEV_DESTROY);
        close(inp->uinput_fd);
        free(inp);
        return NULL;
    }

    LOG_INF("Input handler ready on port %d", listen_port);
    return inp;
}

int db_input_start(db_input_t *inp)
{
    if (!inp) return -1;

    inp->running = 1;
    LOG_INF("Input event loop started");

    uint8_t buf[INPUT_BUF_SIZE];

    while (inp->running) {
        ssize_t n = recv(inp->udp_sock, buf, sizeof(buf), 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* Timeout — check running flag and continue */
                continue;
            }
            if (errno == EINTR) continue;
            LOG_ERR("recv() failed: %s", strerror(errno));
            break;
        }

        /* Need at least a packet header + input event */
        if ((size_t)n < sizeof(db_packet_header_t) + sizeof(db_input_event_t))
            continue;

        const db_packet_header_t *pkt = (const db_packet_header_t *)buf;
        if (pkt->type != DB_PKT_INPUT_EVENT)
            continue;

        const db_input_event_t *evt = (const db_input_event_t *)
            (buf + sizeof(db_packet_header_t));
        handle_input_event(inp, evt);
    }

    LOG_INF("Input event loop exited");
    return 0;
}

void db_input_stop(db_input_t *inp)
{
    if (!inp) return;
    inp->running = 0;
}

void db_input_destroy(db_input_t *inp)
{
    if (!inp) return;

    if (inp->uinput_fd >= 0) {
        ioctl(inp->uinput_fd, UI_DEV_DESTROY);
        close(inp->uinput_fd);
    }
    if (inp->udp_sock >= 0)
        close(inp->udp_sock);

    free(inp);
    LOG_INF("Input handler destroyed");
}
