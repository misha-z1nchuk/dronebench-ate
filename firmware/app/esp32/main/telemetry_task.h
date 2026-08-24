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
#include <stdint.h>

/* Creates the pipeline's two tasks and the queue between them. Call once,
   after platform_esp32_init() and after output_start(). */
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

/*
 * Two ways a sample can fail to exist, kept apart because they mean different
 * things. Missed periods say the bench could not keep up with its own sample
 * rate; queue drops say it sampled fine but processing fell behind. Summed
 * into one figure on the wire — the host cannot act on the difference — but
 * separate here, because the repair is not the same.
 */
uint32_t telemetry_missed_periods(void);
uint32_t telemetry_queue_drops(void);

#endif /* TELEMETRY_TASK_H */
