/*
 * Specification for the telemetry format. Read alongside telemetry.h.
 *
 * Roughly half of these are malformed input. That ratio is deliberate: the
 * happy path is exercised every second the bench runs, while the broken paths
 * are exercised exactly when something is already going wrong and the operator
 * most needs the tool to behave.
 */
#include "dronebench/telemetry.h"

#include <math.h>
#include <string.h>

#include "test_framework.h"
#include "tests.h"

/* A parser that has already accepted a valid header — the starting point for
   everything except the header tests themselves. */
static void ready(telemetry_parser_t *p)
{
    telemetry_line_t line;

    telemetry_parser_init(p);
    telemetry_parse_line(p, "TLM,v=2,rate_hz=500,fields=t_us|v|i|i_ref",
                         &line);
}

void test_telemetry(void)
{
    telemetry_parser_t p;
    telemetry_line_t   line;
    char               buf[TELEMETRY_LINE_MAX];

    /* --- encoding ---------------------------------------------------- */

    TF_CASE("the header carries the version and the rate");
    {
        size_t n = telemetry_encode_header(buf, sizeof buf, 500);

        CHECK(n > 0);
        CHECK_STR(buf, "TLM,v=2,rate_hz=500,fields=t_us|v|i|i_ref\n");
        CHECK_INT(n, strlen(buf));
    }

    TF_CASE("a sample line is positional and short");
    {
        power_sample_t s = { .timestamp_us = 1000000,
                             .voltage_v = 4.083f,
                             .current_a = 0.420f,
                             .current_ref_a = 0.0f };
        size_t n = telemetry_encode_sample(buf, sizeof buf, &s);

        CHECK(n > 0);
        CHECK_STR(buf, "S,1000000,4.083,0.420,0.000\n");

        /* The whole reason for the positional format: at 500 Hz this line is
           sent 1.8 million times an hour, and field names would be 60 bytes
           of the 80 each time. At this width 500 Hz costs 14 kB/s against the
           46 kB/s the link carries. */
        CHECK(n <= 30);
    }

    TF_CASE("a summary line names its fields");
    {
        telemetry_summary_t s = { .consumed_mah = 12.4f,
                                  .consumed_wh = 0.051f,
                                  .min_voltage_v = 3.71f,
                                  .max_current_a = 8.2f,
                                  .sample_count = 500,
                                  .sensor_failures = 3,
                                  .rejected_time = 2,
                                  .rejected_value = 1,
                                  .gaps = 1,
                                  .dropped = 0 };
        size_t n = telemetry_encode_summary(buf, sizeof buf, &s);

        CHECK(n > 0);
        CHECK_STR(buf,
                  "U,n=500,mah=12.400,wh=0.0510,vmin=3.710,imax=8.200,"
                  "sensor=3,rej_time=2,rej_val=1,gaps=1,drop=0\n");
        CHECK(n < TELEMETRY_LINE_MAX);
    }

    TF_CASE("an encoder refuses a buffer it cannot fill completely");
    {
        power_sample_t s = { .timestamp_us = 1000000,
                             .voltage_v = 4.083f,
                             .current_a = 0.420f,
                             .current_ref_a = 0.0f };
        char small[8];

        /* A truncated telemetry line is worse than a missing one: the host
           would parse the fields that survived and call the result a
           measurement. */
        CHECK_INT(telemetry_encode_sample(small, sizeof small, &s), 0);
        CHECK_INT(telemetry_encode_header(small, sizeof small, 500), 0);
    }

    /* --- header ------------------------------------------------------ */

    TF_CASE("a valid header is accepted and remembered");
    {
        telemetry_parser_init(&p);
        CHECK(telemetry_parse_line(
            &p, "TLM,v=2,rate_hz=500,fields=t_us|v|i|i_ref", &line));
        CHECK_INT(line.kind, TELEMETRY_LINE_HEADER);
        CHECK_INT(line.as.header.version, 2);
        CHECK_INT(line.as.header.rate_hz, 500);
        CHECK(p.have_header);
    }

    TF_CASE("a header with an unknown version is refused");
    {
        telemetry_parser_init(&p);

        /* Version 3 might mean anything. Parsing it as version 2 would
           produce numbers rather than errors, and numbers get believed. The
           same applies backwards: v1 put three fields on an S line, so a v1
           log read as v2 would silently lose the reference channel. */
        CHECK(!telemetry_parse_line(&p, "TLM,v=3,rate_hz=500,fields=x", &line));
        CHECK(!telemetry_parse_line(&p, "TLM,v=1,rate_hz=500,fields=x", &line));
        CHECK(!p.have_header);
        CHECK_INT(p.lines_rejected, 2);
    }

    TF_CASE("samples before a header are refused");
    {
        telemetry_parser_init(&p);
        CHECK(!telemetry_parse_line(&p, "S,1000000,4.083,0.420,0.000", &line));
        CHECK_INT(p.lines_rejected, 1);
    }

    /* --- samples ----------------------------------------------------- */

    TF_CASE("a sample round-trips through encode and parse");
    {
        power_sample_t in = { .timestamp_us = 1234567,
                              .voltage_v = 3.977f,
                              .current_a = 2.501f,
                              .current_ref_a = 0.0f };

        telemetry_encode_sample(buf, sizeof buf, &in);
        ready(&p);
        CHECK(telemetry_parse_line(&p, buf, &line));

        CHECK_INT(line.kind, TELEMETRY_LINE_SAMPLE);
        CHECK_INT(line.as.sample.timestamp_us, 1234567);
        CHECK_NEAR(line.as.sample.voltage_v, 3.977f, 0.0005f);
        CHECK_NEAR(line.as.sample.current_a, 2.501f, 0.0005f);
    }

    TF_CASE("an absent reference channel round-trips as NAN, not as zero");
    {
        power_sample_t in = { .timestamp_us = 1000,
                              .voltage_v = 4.000f,
                              .current_a = 1.000f,
                              .current_ref_a = NAN };

        /* Until the INA226 exists this is the normal case, and it has to stay
           distinguishable from a genuine reading of 0 A — a shorted shunt and
           a missing chip are not the same fault. */
        telemetry_encode_sample(buf, sizeof buf, &in);
        ready(&p);
        CHECK(telemetry_parse_line(&p, buf, &line));
        CHECK(isnan(line.as.sample.current_ref_a));

        /* And a real zero must not come back as NAN. */
        in.current_ref_a = 0.0f;
        telemetry_encode_sample(buf, sizeof buf, &in);
        CHECK(telemetry_parse_line(&p, buf, &line));
        CHECK(!isnan(line.as.sample.current_ref_a));
    }

    TF_CASE("a trailing newline is optional");
    {
        ready(&p);
        CHECK(telemetry_parse_line(&p, "S,1000,4.000,1.000,0.000\n", &line));
        CHECK_INT(line.as.sample.timestamp_us, 1000);

        CHECK(telemetry_parse_line(&p, "S,2000,4.000,1.000,0.000", &line));
        CHECK_INT(line.as.sample.timestamp_us, 2000);
    }

    TF_CASE("negative current parses — the sensor is bidirectional");
    {
        ready(&p);
        CHECK(telemetry_parse_line(&p, "S,1000,4.000,-0.250,-0.249", &line));
        CHECK_NEAR(line.as.sample.current_a, -0.250f, 0.0005f);
    }

    TF_CASE("a sample missing a field is rejected outright");
    {
        ready(&p);

        /* Not "two good values and one stale one" — the caller must not be
           left holding a partly-updated sample it thinks is complete. */
        CHECK(!telemetry_parse_line(&p, "S,1000,4.000,1.000", &line));
        CHECK(!telemetry_parse_line(&p, "S,1000,4.000", &line));
        CHECK(!telemetry_parse_line(&p, "S", &line));
        CHECK_INT(p.lines_rejected, 3);
    }

    TF_CASE("a sample with a non-numeric field is rejected");
    {
        ready(&p);
        CHECK(!telemetry_parse_line(&p, "S,1000,banana,1.000,0.000", &line));
        CHECK(!telemetry_parse_line(&p, "S,abc,4.000,1.000,0.000", &line));
        CHECK(!telemetry_parse_line(&p, "S,1000,,1.000,0.000", &line));
        CHECK_INT(p.lines_rejected, 3);
    }

    TF_CASE("a sample with extra fields is rejected");
    {
        ready(&p);
        CHECK(!telemetry_parse_line(&p, "S,1000,4.000,1.000,0.000,9.999", &line));
    }

    TF_CASE("a line cut off mid-number is rejected");
    {
        ready(&p);

        /* What a UART overrun actually looks like: the line simply stops. */
        CHECK(!telemetry_parse_line(&p, "S,1000,4.000,1.0", &line));
        CHECK(!telemetry_parse_line(&p, "S,10", &line));
    }

    TF_CASE("an unknown line type is rejected, not guessed at");
    {
        ready(&p);
        CHECK(!telemetry_parse_line(&p, "X,1000,4.000,1.000,0.000", &line));
        CHECK(!telemetry_parse_line(&p, "SAMPLE,1000,4.000,1.000,0.000", &line));
    }

    TF_CASE("blank lines are ignored without being errors");
    {
        ready(&p);

        /* Line noise and terminal echo produce these. They carry no data, but
           they are not corruption either — counting them as rejections would
           make the error count meaningless. */
        CHECK(telemetry_parse_line(&p, "", &line));
        CHECK_INT(line.kind, TELEMETRY_LINE_NONE);
        CHECK(telemetry_parse_line(&p, "\n", &line));
        CHECK_INT(line.kind, TELEMETRY_LINE_NONE);
        CHECK_INT(p.lines_rejected, 0);
    }

    /* --- summaries --------------------------------------------------- */

    TF_CASE("a summary round-trips through encode and parse");
    {
        telemetry_summary_t in = { .consumed_mah = 12.4f,
                                   .consumed_wh = 0.051f,
                                   .min_voltage_v = 3.71f,
                                   .max_current_a = 8.2f,
                                   .sample_count = 498,
                                   .sensor_failures = 3,
                                   .rejected_time = 2,
                                   .rejected_value = 1,
                                   .gaps = 1,
                                   .dropped = 7 };

        telemetry_encode_summary(buf, sizeof buf, &in);
        ready(&p);
        CHECK(telemetry_parse_line(&p, buf, &line));

        CHECK_INT(line.kind, TELEMETRY_LINE_SUMMARY);
        CHECK_NEAR(line.as.summary.consumed_mah, 12.4f, 0.001f);
        CHECK_NEAR(line.as.summary.consumed_wh, 0.051f, 0.0001f);
        CHECK_NEAR(line.as.summary.min_voltage_v, 3.71f, 0.001f);
        CHECK_NEAR(line.as.summary.max_current_a, 8.2f, 0.001f);
        CHECK_INT(line.as.summary.sample_count, 498);
        CHECK_INT(line.as.summary.sensor_failures, 3);
        CHECK_INT(line.as.summary.rejected_time, 2);
        CHECK_INT(line.as.summary.rejected_value, 1);
        CHECK_INT(line.as.summary.gaps, 1);
        CHECK_INT(line.as.summary.dropped, 7);
    }

    TF_CASE("summary fields may arrive in any order");
    {
        ready(&p);

        /* Named fields exist precisely so the order is not part of the
           contract — a version that adds a field must not break a reader
           that does not know about it. */
        CHECK(telemetry_parse_line(
            &p, "U,drop=0,gaps=1,rej_val=1,rej_time=2,sensor=3,imax=8.200,"
                "vmin=3.710,wh=0.0510,mah=12.400,n=500", &line));
        CHECK_NEAR(line.as.summary.consumed_mah, 12.4f, 0.001f);
        CHECK_INT(line.as.summary.dropped, 0);
    }

    TF_CASE("a summary missing a field is rejected");
    {
        ready(&p);
        CHECK(!telemetry_parse_line(&p, "U,mah=12.400,wh=0.0510", &line));
    }

    TF_CASE("a summary with a garbled value is rejected");
    {
        ready(&p);
        /* Every other field is present and well formed, so this fails on the
           garbled value rather than on a missing one. */
        CHECK(!telemetry_parse_line(
            &p, "U,n=500,mah=x,wh=0.0510,vmin=3.710,imax=8.200,sensor=3,"
                "rej_time=2,rej_val=1,gaps=1,drop=0",
            &line));
    }

    /* --- counters ---------------------------------------------------- */

    TF_CASE("the parser counts what it accepted and what it threw away");
    {
        ready(&p);
        telemetry_parse_line(&p, "S,1000,4.000,1.000,0.000", &line);
        telemetry_parse_line(&p, "S,2000,4.000,1.000,0.000", &line);
        telemetry_parse_line(&p, "S,bad", &line);
        telemetry_parse_line(&p, "", &line);

        /* The header counts as parsed; the blank line counts as neither. */
        CHECK_INT(p.lines_parsed, 3);
        CHECK_INT(p.lines_rejected, 1);
    }
}
