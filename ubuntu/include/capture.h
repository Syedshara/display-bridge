/*
 * capture.h
 * PipeWire-based Wayland screen capture.
 */

#ifndef DB_CAPTURE_H
#define DB_CAPTURE_H

#include <stdint.h>

typedef void (*db_capture_callback_t)(const uint8_t *data, int width,
                                       int height, int stride, void *userdata);

typedef struct db_capture db_capture_t;

/* Initialize PipeWire capture via ScreenCast portal.
 * target_width/height: desired capture resolution.
 * callback: called for each frame with NV12 data.
 * Returns opaque capture handle, or NULL on failure. */
db_capture_t *db_capture_init(int target_width, int target_height,
                               db_capture_callback_t callback, void *userdata);

/* Start capturing frames. Blocks until db_capture_stop() is called. */
int db_capture_start(db_capture_t *cap);

/* Stop capturing (call from another thread or signal handler). */
void db_capture_stop(db_capture_t *cap);

/* Destroy capture and free resources. */
void db_capture_destroy(db_capture_t *cap);

#endif /* DB_CAPTURE_H */
