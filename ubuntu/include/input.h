/*
 * input.h
 * Receives mouse/keyboard events from Mac and injects via uinput.
 */

#ifndef DB_INPUT_H
#define DB_INPUT_H

#include <stdint.h>

typedef struct db_input db_input_t;

/* Initialize input injection and receiver.
 * listen_port: UDP port to listen on for input events.
 * Returns opaque input handle, or NULL on failure. */
db_input_t *db_input_init(int listen_port);

/* Start listening for input events. Blocks until db_input_stop(). */
int db_input_start(db_input_t *inp);

/* Stop listening. */
void db_input_stop(db_input_t *inp);

/* Destroy input handler and free resources. */
void db_input_destroy(db_input_t *inp);

#endif /* DB_INPUT_H */
