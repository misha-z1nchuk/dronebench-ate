/*
 * Telemetry: the wire format between the bench and the host.
 *
 * Two line types at different rates, because they answer different questions.
 *
 *   TLM  once, at session start   protocol version and field list
 *   S    500 Hz                   raw readings, positional, 20 bytes
 *   U    1 Hz                     the firmware's own accumulations
 *
 * ---------------------------------------------------------------------------
 * WHY DERIVED VALUES ARE STILL SENT
 * ---------------------------------------------------------------------------
 *
 * Power and consumed charge can be computed by the host from the raw samples,
 * so sending them 500 times a second would be redundant. But the firmware sees
 * every sample and the host sees only the ones that survived the link — if
 * anything is dropped, the host's integral is quietly low while the firmware's
 * is right.
 *
 * So they are sent, once per second, from the side that has the complete
 * picture. The difference between the two integrals is the cost of the lost
 * samples, expressed in mAh — which is a measurement of how much the data can
 * be trusted, not just what it says.
 *
 * ---------------------------------------------------------------------------
 * PARSING
 * ---------------------------------------------------------------------------
 *
 * A parser that has not seen a TLM header, or has seen one with a version it
 * does not know, must refuse every following line. Guessing at an unknown
 * format produces numbers rather than errors, and numbers get believed.
 *
 * Malformed lines are rejected, never partially applied. A line missing its
 * last field must not leave the caller holding two good values and one stale
 * one.
 */
#ifndef DRONEBENCH_TELEMETRY_H
#define DRONEBENCH_TELEMETRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dronebench/types.h"

#define TELEMETRY_VERSION 2

/* Longest line this format can produce, including newline and terminator.
   The U line is the wide one: 65 characters of fixed text, five uint32 counters
   at 10 digits each, and four floats whose width is bounded by physics rather
   than by the type — roughly 155. Rounded up, because an encoder that starts
   refusing to emit summaries would do so silently. */
#define TELEMETRY_LINE_MAX 192

typedef enum {
    TELEMETRY_LINE_NONE = 0,   /* blank or comment — not an error */
    TELEMETRY_LINE_HEADER,
    TELEMETRY_LINE_SAMPLE,
    TELEMETRY_LINE_SUMMARY,
    TELEMETRY_LINE_BAD,
} telemetry_line_kind_t;

/*
 * What the session amounts to so far.
 *
 * Every field is cumulative from the start of the session, not per-interval;
 * the line is merely emitted once a second. A host wanting a per-second rate
 * differences two consecutive summaries, which it can only do because these
 * are totals.
 *
 * sample_count is here because without it the other numbers are ambiguous: a
 * session whose sensors never answered reports min_voltage_v as a perfectly
 * good 0.000 V, indistinguishable from a flat pack. The count is what says
 * which of the two happened.
 *
 * The three refusal counters are kept apart rather than summed, because they
 * point at three different repairs — dead wiring, a clock that went backwards,
 * and a reading that was not a finite number. At one line per second the
 * bytes are free, and a diagnostic bench that knows why it failed and does not
 * say so is throwing away the answer.
 */
typedef struct {
    float    consumed_mah;
    float    consumed_wh;
    float    min_voltage_v;
    float    max_current_a;
    uint32_t sample_count;     /* accepted, and folded into the figures above */
    uint32_t sensor_failures;  /* a required channel did not answer */
    uint32_t rejected_time;    /* the clock did not advance */
    uint32_t rejected_value;   /* a reading was not a finite number */
    uint32_t gaps;             /* accepted, but after an over-long interval */
    uint32_t dropped;          /* taken, but never made it into the port */
} telemetry_summary_t;

typedef struct {
    telemetry_line_kind_t kind;
    union {
        struct {
            uint32_t version;
            uint32_t rate_hz;
        } header;
        power_sample_t      sample;
        telemetry_summary_t summary;
    } as;
} telemetry_line_t;

/*
 * Encoders. Each writes a NUL-terminated line ending in '\n' and returns the
 * number of characters written, or 0 if the buffer was too small — in which
 * case nothing usable was written and the caller must not transmit it.
 *
 * A truncated telemetry line is worse than a missing one: the host would
 * parse whatever fields survived and treat the result as a measurement.
 */
size_t telemetry_encode_header(char *buf, size_t size, uint32_t rate_hz);
size_t telemetry_encode_sample(char *buf, size_t size,
                               const power_sample_t *sample);
size_t telemetry_encode_summary(char *buf, size_t size,
                                const telemetry_summary_t *summary);

/*
 * Parser. Holds the negotiated version, so it can refuse lines it cannot
 * interpret rather than guessing.
 */
typedef struct {
    bool     have_header;
    uint32_t version;
    uint32_t rate_hz;

    uint32_t lines_parsed;
    uint32_t lines_rejected;
} telemetry_parser_t;

void telemetry_parser_init(telemetry_parser_t *parser);

/*
 * Parses one line, with or without its trailing newline. *out is written only
 * when the return value is true.
 *
 * Returns false for a malformed line, for any line before a valid header, and
 * for a header whose version is not TELEMETRY_VERSION.
 */
bool telemetry_parse_line(telemetry_parser_t *parser, const char *line,
                          telemetry_line_t *out);

#endif /* DRONEBENCH_TELEMETRY_H */
