/*
 * Telemetry encoding and parsing. The contract is in telemetry.h.
 *
 * The encoders refuse to emit a truncated line, and the parser refuses to
 * interpret one. Both follow from the same rule: a wrong number that looks
 * like a measurement is worse than no number, because it gets believed.
 */
#include "dronebench/telemetry.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SAMPLE_FIELDS  3
#define MAX_FIELDS     12

/* --- encoding ------------------------------------------------------------ */

/*
 * snprintf reports the length it wanted, not the length it wrote. Anything at
 * or above the buffer size means the trailing NUL did not fit, so the contents
 * are a prefix of the intended line — which is exactly the shape of a valid
 * line with different values.
 */
static size_t finish(int n, size_t size)
{
    if (n < 0 || (size_t)n >= size) {
        return 0;
    }
    return (size_t)n;
}

size_t telemetry_encode_header(char *buf, size_t size, uint32_t rate_hz)
{
    int n = snprintf(buf, size, "TLM,v=%d,rate_hz=%" PRIu32 ",fields=t_us|v|i\n",
                     TELEMETRY_VERSION, rate_hz);

    return finish(n, size);
}

size_t telemetry_encode_sample(char *buf, size_t size,
                               const power_sample_t *sample)
{
    int n = snprintf(buf, size, "S,%" PRIu64 ",%.3f,%.3f\n",
                     sample->timestamp_us, (double)sample->voltage_v,
                     (double)sample->current_a);

    return finish(n, size);
}

size_t telemetry_encode_summary(char *buf, size_t size,
                                const telemetry_summary_t *summary)
{
    int n = snprintf(buf, size,
                     "U,mah=%.3f,wh=%.4f,vmin=%.3f,imax=%.3f,"
                     "rej=%" PRIu32 ",gaps=%" PRIu32 ",drop=%" PRIu32 "\n",
                     (double)summary->consumed_mah,
                     (double)summary->consumed_wh,
                     (double)summary->min_voltage_v,
                     (double)summary->max_current_a, summary->rejected,
                     summary->gaps, summary->dropped);

    return finish(n, size);
}

/* --- field helpers ------------------------------------------------------- */

/*
 * Splits in place on commas. Returns false if there are more fields than the
 * caller can hold — an over-long line is corrupt whatever it contains.
 */
static bool split(char *s, char **fields, int max, int *count)
{
    int n = 0;

    fields[n++] = s;
    for (char *p = s; *p != '\0'; p++) {
        if (*p == ',') {
            if (n >= max) {
                return false;
            }
            *p = '\0';
            fields[n++] = p + 1;
        }
    }
    *count = n;
    return true;
}

/*
 * strtoull and strtof both report where they stopped. An unmoved end pointer
 * means nothing was parsed, which is the only way to tell "0" from "banana" —
 * both return zero. A non-NUL end pointer means trailing rubbish, which is
 * what a line clipped by a UART overrun looks like once it is split.
 */
static bool parse_u64(const char *s, uint64_t *out)
{
    char              *end;
    unsigned long long value;

    if (*s == '\0') {
        return false;
    }

    errno = 0;
    value = strtoull(s, &end, 10);

    if (end == s || *end != '\0' || errno == ERANGE) {
        return false;
    }
    *out = (uint64_t)value;
    return true;
}

static bool parse_u32(const char *s, uint32_t *out)
{
    uint64_t wide;

    if (!parse_u64(s, &wide) || wide > UINT32_MAX) {
        return false;
    }
    *out = (uint32_t)wide;
    return true;
}

static bool parse_f32(const char *s, float *out)
{
    char *end;
    float value;

    if (*s == '\0') {
        return false;
    }

    errno = 0;
    value = strtof(s, &end);

    if (end == s || *end != '\0' || errno == ERANGE) {
        return false;
    }
    *out = value;
    return true;
}

/* Splits "key=value" in place. Returns NULL when there is no '='. */
static char *split_key(char *field)
{
    char *eq = strchr(field, '=');

    if (eq == NULL) {
        return NULL;
    }
    *eq = '\0';
    return eq + 1;
}

/* --- line parsers -------------------------------------------------------- */

static bool parse_header(telemetry_parser_t *parser, char *body,
                         telemetry_line_t *out)
{
    char    *fields[MAX_FIELDS];
    int      count;
    uint32_t version = 0;
    uint32_t rate_hz = 0;
    bool     have_version = false;
    bool     have_rate = false;

    if (!split(body, fields, MAX_FIELDS, &count)) {
        return false;
    }

    for (int i = 0; i < count; i++) {
        char *value = split_key(fields[i]);

        if (value == NULL) {
            return false;
        }
        if (strcmp(fields[i], "v") == 0) {
            have_version = parse_u32(value, &version);
            if (!have_version) {
                return false;
            }
        } else if (strcmp(fields[i], "rate_hz") == 0) {
            have_rate = parse_u32(value, &rate_hz);
            if (!have_rate) {
                return false;
            }
        }
        /* Unknown keys are ignored on purpose: a later version adding a field
           must not break a reader that predates it. */
    }

    if (!have_version || !have_rate || version != TELEMETRY_VERSION) {
        return false;
    }

    parser->have_header = true;
    parser->version = version;
    parser->rate_hz = rate_hz;

    out->kind = TELEMETRY_LINE_HEADER;
    out->as.header.version = version;
    out->as.header.rate_hz = rate_hz;
    return true;
}

static bool parse_sample(char *body, telemetry_line_t *out)
{
    char          *fields[MAX_FIELDS];
    int            count;
    power_sample_t sample = { 0 };

    if (!split(body, fields, MAX_FIELDS, &count) || count != SAMPLE_FIELDS) {
        return false;
    }

    if (!parse_u64(fields[0], &sample.timestamp_us) ||
        !parse_f32(fields[1], &sample.voltage_v) ||
        !parse_f32(fields[2], &sample.current_a)) {
        return false;
    }

    /* The reference channel is not on the wire at this rate; the summary
       carries what the firmware made of it. */
    sample.current_ref_a = 0.0f;

    out->kind = TELEMETRY_LINE_SAMPLE;
    out->as.sample = sample;
    return true;
}

static bool parse_summary(char *body, telemetry_line_t *out)
{
    static const char *const KEYS[] = { "mah",  "wh",   "vmin", "imax",
                                        "rej",  "gaps", "drop" };
    const uint32_t ALL_SEEN = 0x7Fu; /* seven fields, all required */

    char               *fields[MAX_FIELDS];
    int                 count;
    telemetry_summary_t summary = { 0 };
    uint32_t            seen = 0;

    if (!split(body, fields, MAX_FIELDS, &count)) {
        return false;
    }

    for (int i = 0; i < count; i++) {
        char *value = split_key(fields[i]);
        bool  ok = true;

        if (value == NULL) {
            return false;
        }

        if (strcmp(fields[i], KEYS[0]) == 0) {
            ok = parse_f32(value, &summary.consumed_mah);
            seen |= 1u << 0;
        } else if (strcmp(fields[i], KEYS[1]) == 0) {
            ok = parse_f32(value, &summary.consumed_wh);
            seen |= 1u << 1;
        } else if (strcmp(fields[i], KEYS[2]) == 0) {
            ok = parse_f32(value, &summary.min_voltage_v);
            seen |= 1u << 2;
        } else if (strcmp(fields[i], KEYS[3]) == 0) {
            ok = parse_f32(value, &summary.max_current_a);
            seen |= 1u << 3;
        } else if (strcmp(fields[i], KEYS[4]) == 0) {
            ok = parse_u32(value, &summary.rejected);
            seen |= 1u << 4;
        } else if (strcmp(fields[i], KEYS[5]) == 0) {
            ok = parse_u32(value, &summary.gaps);
            seen |= 1u << 5;
        } else if (strcmp(fields[i], KEYS[6]) == 0) {
            ok = parse_u32(value, &summary.dropped);
            seen |= 1u << 6;
        }

        if (!ok) {
            return false;
        }
    }

    if (seen != ALL_SEEN) {
        return false;
    }

    out->kind = TELEMETRY_LINE_SUMMARY;
    out->as.summary = summary;
    return true;
}

/* --- entry point --------------------------------------------------------- */

void telemetry_parser_init(telemetry_parser_t *parser)
{
    *parser = (telemetry_parser_t){ 0 };
}

bool telemetry_parse_line(telemetry_parser_t *parser, const char *line,
                          telemetry_line_t *out)
{
    char   work[TELEMETRY_LINE_MAX];
    size_t len = strlen(line);
    bool   ok;

    /* The line arrives const because the caller may be reusing its buffer.
       Splitting needs to write NULs, so it happens on a copy. */
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        len--;
    }

    if (len >= sizeof work) {
        parser->lines_rejected++;
        return false;
    }

    memcpy(work, line, len);
    work[len] = '\0';

    /* Blank lines come from terminal echo and line noise. They carry nothing,
       but they are not corruption — counting them as rejections would make the
       rejection count useless for judging link quality. */
    if (len == 0) {
        out->kind = TELEMETRY_LINE_NONE;
        return true;
    }

    if (strncmp(work, "TLM,", 4) == 0) {
        ok = parse_header(parser, work + 4, out);
    } else if (!parser->have_header) {
        /* Without a negotiated version there is no way to know what the fields
           mean. Guessing would produce numbers instead of errors. */
        ok = false;
    } else if (strncmp(work, "S,", 2) == 0) {
        ok = parse_sample(work + 2, out);
    } else if (strncmp(work, "U,", 2) == 0) {
        ok = parse_summary(work + 2, out);
    } else {
        ok = false;
    }

    if (ok) {
        parser->lines_parsed++;
    } else {
        parser->lines_rejected++;
    }
    return ok;
}
