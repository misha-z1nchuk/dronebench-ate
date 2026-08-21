/*
 * Specification for the metrics accumulator. Read alongside metrics.h.
 *
 * The interesting tests here are the ones with known analytic answers: a
 * constant current for a known time has exactly one correct mAh figure, and
 * anything that disagrees is a bug in the integration rather than a matter of
 * opinion.
 */
#include "dronebench/metrics.h"

#include <string.h>

#include "test_framework.h"
#include "tests.h"

/* One sample, built inline so each test reads as a small table of readings. */
static power_sample_t s(uint64_t t_us, float v, float a)
{
    power_sample_t sample;

    memset(&sample, 0, sizeof sample);
    sample.timestamp_us = t_us;
    sample.voltage_v = v;
    sample.current_a = a;
    sample.current_ref_a = 0.0f;
    return sample;
}

static void add(metrics_t *m, uint64_t t_us, float v, float a)
{
    power_sample_t sample = s(t_us, v, a);

    metrics_add(m, &sample);
}

void test_metrics(void)
{
    metrics_t         m;
    session_metrics_t r;

    TF_CASE("an empty session reports zeroes and no samples");
    {
        metrics_init(&m);
        metrics_result(&m, &r);

        CHECK_INT(r.sample_count, 0);
        CHECK_INT(r.duration_us, 0);
        CHECK_NEAR(r.consumed_mah, 0.0f, 1e-6f);
        CHECK_NEAR(r.consumed_wh, 0.0f, 1e-6f);
    }

    TF_CASE("the first sample starts the session and contributes no charge");
    {
        metrics_init(&m);
        add(&m, 1000, 4.0f, 2.0f);
        metrics_result(&m, &r);

        CHECK_INT(r.sample_count, 1);
        CHECK_INT(r.duration_us, 0);

        /* One instant is not an interval — there is nothing to integrate over
           yet, however large the current reads. */
        CHECK_NEAR(r.consumed_mah, 0.0f, 1e-6f);
        CHECK_NEAR(r.min_voltage_v, 4.0f, 0.001f);
        CHECK_NEAR(r.max_current_a, 2.0f, 0.001f);
    }

    TF_CASE("duration spans first to last sample");
    {
        metrics_init(&m);
        add(&m, 1000, 4.0f, 1.0f);
        add(&m, 3000, 4.0f, 1.0f);
        add(&m, 6000, 4.0f, 1.0f);
        metrics_result(&m, &r);

        CHECK_INT(r.duration_us, 5000);
        CHECK_INT(r.sample_count, 3);
    }

    TF_CASE("min and max track the extremes, not the last value");
    {
        metrics_init(&m);
        add(&m, 1000, 4.20f, 0.5f);
        add(&m, 2000, 3.10f, 8.0f); /* the sag and the peak */
        add(&m, 3000, 4.05f, 0.6f);
        metrics_result(&m, &r);

        CHECK_NEAR(r.min_voltage_v, 3.10f, 0.001f);
        CHECK_NEAR(r.max_voltage_v, 4.20f, 0.001f);
        CHECK_NEAR(r.max_current_a, 8.00f, 0.001f);
    }

    TF_CASE("one amp for one second is 1000/3600 mAh");
    {
        metrics_init(&m);
        add(&m, 0, 4.0f, 1.0f);
        add(&m, 1000000, 4.0f, 1.0f); /* one second later */
        metrics_result(&m, &r);

        /* 1 A for 1 s = 1 A*s = 1/3.6 mAh */
        CHECK_NEAR(r.consumed_mah, 1.0f / 3.6f, 0.0001f);

        /* 4 V x 1 A for 1 s = 4 J = 4/3600 Wh */
        CHECK_NEAR(r.consumed_wh, 4.0f / 3600.0f, 0.000001f);
    }

    TF_CASE("average current is the mean of the readings");
    {
        metrics_init(&m);
        add(&m, 0, 4.0f, 1.0f);
        add(&m, 1000, 4.0f, 2.0f);
        add(&m, 2000, 4.0f, 3.0f);
        metrics_result(&m, &r);

        CHECK_NEAR(r.avg_current_a, 2.0f, 0.001f);
    }

    TF_CASE("charge accumulates across a long session without drifting");
    {
        /* One hour at 500 Hz, one amp throughout: exactly 1000 mAh.
           This is the test that fails by about 1.5% if the accumulator is a
           float — not because anything went wrong, but because 1.8 million
           roundings all lean the same way. */
        const uint64_t period_us = 2000;
        const uint32_t samples = 1800001; /* fencepost: N intervals need N+1 */

        metrics_init(&m);
        for (uint32_t i = 0; i < samples; i++) {
            add(&m, (uint64_t)i * period_us, 4.0f, 1.0f);
        }
        metrics_result(&m, &r);

        CHECK_INT(r.sample_count, samples);
        CHECK_NEAR(r.consumed_mah, 1000.0f, 0.5f);   /* 0.05% */
        CHECK_NEAR(r.consumed_wh, 4.0f, 0.002f);
        CHECK_NEAR(r.avg_current_a, 1.0f, 0.001f);
    }

    TF_CASE("a varying current integrates to the area under it");
    {
        /* 2 A for one second, then 4 A for one second.
           Rectangle integration uses the newer reading for each interval, so
           the answer is 2 + 4 = 6 A*s rather than the trapezoid's 3 + ... —
           the tests encode which rule the implementation uses. */
        metrics_init(&m);
        add(&m, 0, 4.0f, 0.0f);
        add(&m, 1000000, 4.0f, 2.0f);
        add(&m, 2000000, 4.0f, 4.0f);
        metrics_result(&m, &r);

        CHECK_NEAR(r.consumed_mah, 6.0f / 3.6f, 0.001f);
    }

    TF_CASE("samples at the same voltage still update the count");
    {
        metrics_init(&m);
        for (int i = 0; i < 10; i++) {
            add(&m, (uint64_t)i * 1000, 4.0f, 1.0f);
        }
        metrics_result(&m, &r);

        CHECK_INT(r.sample_count, 10);
        CHECK_NEAR(r.min_voltage_v, 4.0f, 0.001f);
        CHECK_NEAR(r.max_voltage_v, 4.0f, 0.001f);
    }
}
