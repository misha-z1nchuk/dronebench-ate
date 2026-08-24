/*
 * The measurement loop.
 *
 * Everything the bench does while a test runs happens here: take a sample,
 * fold it into the metrics, put it on the wire. Once a second, send what the
 * firmware has accumulated.
 *
 * The session state lives inside this file and is not exported. Two tasks
 * touch it — this one and whichever task runs the CLI — so every path to it
 * goes through the functions below, which take the lock. Exposing the struct
 * would make it possible to read it without the lock, and a race that is easy
 * to write is a race that will be written.
 */
#ifndef TELEMETRY_TASK_H
#define TELEMETRY_TASK_H

#include <stdbool.h>

/* Creates the task. Call once, after platform_esp32_init(). */
void telemetry_task_start(void);

/*
 * Begin and end a measurement session. False when the session state machine
 * refuses — already running, nothing to stop, or an unacknowledged UNSAFE.
 * The reason is in the reply the CLI prints, not in the return value.
 */
bool telemetry_session_start(void);
bool telemetry_session_stop(void);

/* For the `status` command. Never NULL. */
const char *telemetry_state_name(void);

#endif /* TELEMETRY_TASK_H */
