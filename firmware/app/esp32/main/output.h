/*
 * The only thing in this firmware that writes to the console port.
 *
 * Three callers reach it — the processing task with telemetry, the CLI task
 * with command replies, and session start-up with the stream header — and
 * they run at different priorities on two cores.
 *
 * What this buys is that none of them ever waits. uart_write_bytes blocks
 * once the driver's transmit buffer is full, and the caller blocks with it —
 * which at 500 Hz means a measurement period lost to a busy port. Here a
 * sender copies its line and returns in microseconds; the only thing that
 * ever waits on the UART is the one task whose entire job is waiting.
 *
 * The second gain is that a line lost this way is counted rather than
 * absorbed. Blocking hides congestion as jitter; a drop counter names it.
 *
 * What this does NOT buy is atomicity across several calls, and an earlier
 * version of this comment claimed otherwise. One output_send() arrives whole,
 * exactly as one uart_write_bytes() did — but four of them in a row are four
 * queue entries, and telemetry can still land between them. So the rule from
 * day 7 still stands and is not optional: assemble a reply into one buffer
 * and send it once. cli_print_help() writes four fragments per command and is
 * the remaining place where that rule is broken.
 *
 * ---------------------------------------------------------------------------
 * CONTRACT
 * ---------------------------------------------------------------------------
 *
 * output_send() never blocks and never partially transmits. A line either
 * enters the queue whole or is dropped whole and counted. Blocking here would
 * put the caller's priority — possibly the 500 Hz measurement path — behind a
 * UART that is busy, which is the situation this module exists to prevent.
 *
 * A dropped line is a real loss and is reported as one. Anything reading the
 * stream can tell a gap from a quiet bench only because of that counter, and
 * a telemetry file with silent holes in it is worse than one that admits
 * them: the holes still look like measurements that were never taken.
 *
 * Ordering between callers is the queue's arrival order, and nothing more is
 * promised. A reply and a sample racing to enqueue may come out either way
 * round, and a reply built from several sends may have a sample between its
 * parts.
 */
#ifndef OUTPUT_H
#define OUTPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dronebench/telemetry.h"

/*
 * Deep enough to absorb a burst — a summary, a command reply and several
 * samples arriving together — without being so deep that a stalled port hides
 * behind it for seconds. At 500 Hz this is about 16 ms of telemetry.
 *
 * Each slot holds a whole line, so the queue costs
 * OUTPUT_QUEUE_DEPTH * sizeof(output_line_t) of RAM up front. Measure it
 * before growing it: this is the largest single allocation in the firmware.
 */
#define OUTPUT_QUEUE_DEPTH 8

typedef struct {
    /* Not NUL-terminated on purpose: telemetry is text today and the framed
       protocol of a later phase will not be. length is authoritative. */
    uint16_t length;
    char     text[TELEMETRY_LINE_MAX];
} output_line_t;

/*
 * Creates the queue and the writer task. Call once, after the platform has
 * configured the port and before any task that might send.
 */
void output_start(void);

/*
 * Queues one line. Returns false when the queue was full, in which case
 * nothing was written and the drop counter has been incremented.
 *
 * size above TELEMETRY_LINE_MAX is refused rather than truncated. A truncated
 * telemetry line is worse than a missing one — the host parses whichever
 * fields survived and calls the result a measurement.
 *
 * Safe from any task. Not safe from an ISR: use a FromISR variant if that ever
 * becomes necessary, because the blocking-capable API cannot be called with
 * interrupts disabled.
 */
bool output_send(const char *text, size_t size);

/* Lines lost because the queue was full, since output_start(). */
uint32_t output_dropped(void);

/*
 * Deepest backlog seen so far, out of OUTPUT_QUEUE_DEPTH.
 *
 * Reported because a queue that never exceeded two slots and a queue that
 * touched eight and dropped nothing look identical from the outside, and only
 * one of them has any margin left. This is the number that says whether
 * OUTPUT_QUEUE_DEPTH was chosen well — the same question uxTaskGetStackHigh-
 * WaterMark answers about stacks.
 *
 * Sampled inside the writer, not in output_send(). The writer outranks every
 * sender, so it empties the queue before a sender resumes, and a reading taken
 * there reports zero however busy the port was.
 */
uint32_t output_high_water(void);

#endif /* OUTPUT_H */
