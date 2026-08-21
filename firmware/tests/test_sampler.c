/*
 * Specification for the sampler. Read alongside sampler.h.
 *
 * These tests are the reason the host platform exists: every sensor fault a
 * real bench can suffer is reproduced here in microseconds, deterministically,
 * with no hardware and no waiting.
 */
#include "dronebench/sampler.h"

#include <math.h>

#include "dronebench/platform.h"
#include "platform_host.h"
#include "test_framework.h"
#include "tests.h"

#define GAP_LIMIT_US 10000 /* 10 ms — five periods at 500 Hz */

static void fresh(sampler_t *sampler)
{
    platform_host_reset();
    platform_host_set_readings(4.0f, 1.0f, 1.0f);
    sampler_init(sampler, GAP_LIMIT_US);
}

void test_sampler(void)
{
    sampler_t  s;
    power_sample_t sample;

    TF_CASE("a fresh sampler has no history and no trouble");
    {
        fresh(&s);
        CHECK_INT(s.accepted, 0);
        CHECK_INT(s.sensor_failures, 0);
        CHECK_INT(s.rejected_time, 0);
        CHECK_INT(s.rejected_value, 0);
        CHECK_INT(s.gaps, 0);
        CHECK(!sampler_had_trouble(&s));
    }

    TF_CASE("a good reading becomes a sample");
    {
        fresh(&s);
        platform_host_set_time_us(1000);
        platform_host_set_readings(4.083f, 0.42f, 0.397f);

        CHECK_INT(sampler_take(&s, &sample), SAMPLE_OK);
        CHECK_INT(sample.timestamp_us, 1000);
        CHECK_NEAR(sample.voltage_v, 4.083f, 0.0005f);
        CHECK_NEAR(sample.current_a, 0.42f, 0.0005f);
        CHECK_NEAR(sample.current_ref_a, 0.397f, 0.0005f);
        CHECK_INT(s.accepted, 1);
        CHECK(!sampler_had_trouble(&s));
    }

    TF_CASE("the first sample has no previous one to compare against");
    {
        fresh(&s);
        platform_host_set_time_us(0);
        CHECK_INT(sampler_take(&s, &sample), SAMPLE_OK);
        CHECK_INT(s.accepted, 1);
        CHECK_INT(s.rejected_time, 0);
        CHECK_INT(s.gaps, 0);
    }

    TF_CASE("a missing reference channel degrades the sample, not the session");
    {
        fresh(&s);
        platform_host_set_time_us(1000);
        platform_host_set_readings(4.0f, 1.5f, 1.5f);
        platform_host_set_sensor_ok(true, true, false);

        CHECK_INT(sampler_take(&s, &sample), SAMPLE_OK);
        CHECK_NEAR(sample.current_a, 1.5f, 0.0005f);
        CHECK(isnan(sample.current_ref_a));
        CHECK_INT(s.accepted, 1);
    }

    TF_CASE("a failed voltage read produces no sample");
    {
        fresh(&s);
        platform_host_set_time_us(1000);
        platform_host_set_sensor_ok(false, true, true);

        sample.timestamp_us = 0xDEAD;
        CHECK_INT(sampler_take(&s, &sample), SAMPLE_SENSOR_FAILED);
        CHECK_INT(sample.timestamp_us, 0xDEAD); /* untouched */
        CHECK_INT(s.accepted, 0);
        CHECK_INT(s.sensor_failures, 1);
        CHECK(sampler_had_trouble(&s));
    }

    TF_CASE("a failed current read produces no sample");
    {
        fresh(&s);
        platform_host_set_time_us(1000);
        platform_host_set_sensor_ok(true, false, true);

        CHECK_INT(sampler_take(&s, &sample), SAMPLE_SENSOR_FAILED);
        CHECK_INT(s.sensor_failures, 1);
    }

    TF_CASE("a non-finite reading is rejected");
    {
        fresh(&s);
        platform_host_set_time_us(1000);

        /* Infinity is what a divide by a zero calibration coefficient gives,
           and it survives every comparison a naive range check makes. */
        platform_host_set_readings(INFINITY, 1.0f, 1.0f);
        CHECK_INT(sampler_take(&s, &sample), SAMPLE_REJECTED_VALUE);

        platform_host_set_readings(4.0f, NAN, 1.0f);
        CHECK_INT(sampler_take(&s, &sample), SAMPLE_REJECTED_VALUE);

        CHECK_INT(s.rejected_value, 2);
        CHECK_INT(s.accepted, 0);
        CHECK(sampler_had_trouble(&s));
    }

    TF_CASE("a clock that stands still or goes backwards is rejected");
    {
        fresh(&s);

        platform_host_set_time_us(5000);
        CHECK_INT(sampler_take(&s, &sample), SAMPLE_OK);

        /* Same instant twice: two samples one microsecond apart would be
           believable, two at the identical timestamp are not. */
        CHECK_INT(sampler_take(&s, &sample), SAMPLE_REJECTED_TIME);

        platform_host_set_time_us(4000);
        CHECK_INT(sampler_take(&s, &sample), SAMPLE_REJECTED_TIME);

        CHECK_INT(s.rejected_time, 2);
        CHECK_INT(s.accepted, 1);
    }

    TF_CASE("a rejected sample does not become the new reference point");
    {
        fresh(&s);

        platform_host_set_time_us(5000);
        CHECK_INT(sampler_take(&s, &sample), SAMPLE_OK);

        platform_host_set_time_us(3000);
        CHECK_INT(sampler_take(&s, &sample), SAMPLE_REJECTED_TIME);

        /* If the rejected 3000 had been remembered, this would now pass —
           and one glitch would have rewritten the session's timeline. */
        platform_host_set_time_us(4000);
        CHECK_INT(sampler_take(&s, &sample), SAMPLE_REJECTED_TIME);

        platform_host_set_time_us(6000);
        CHECK_INT(sampler_take(&s, &sample), SAMPLE_OK);
        CHECK_INT(s.accepted, 2);
    }

    TF_CASE("intervals inside the limit are not gaps");
    {
        fresh(&s);

        for (int i = 1; i <= 5; i++) {
            platform_host_set_time_us((uint64_t)i * 2000); /* 2 ms apart */
            CHECK_INT(sampler_take(&s, &sample), SAMPLE_OK);
        }
        CHECK_INT(s.accepted, 5);
        CHECK_INT(s.gaps, 0);
        CHECK_INT(s.max_interval_us, 2000);
        CHECK(!sampler_had_trouble(&s));
    }

    TF_CASE("an over-long interval is accepted but counted");
    {
        fresh(&s);

        platform_host_set_time_us(1000);
        CHECK_INT(sampler_take(&s, &sample), SAMPLE_OK);

        platform_host_advance_us(50000); /* 50 ms — five times the limit */
        CHECK_INT(sampler_take(&s, &sample), SAMPLE_OK);

        CHECK_INT(s.accepted, 2);
        CHECK_INT(s.gaps, 1);
        CHECK_INT(s.max_interval_us, 50000);
        CHECK(sampler_had_trouble(&s));
    }

    TF_CASE("an interval exactly at the limit is not a gap");
    {
        fresh(&s);

        platform_host_set_time_us(1000);
        CHECK_INT(sampler_take(&s, &sample), SAMPLE_OK);

        platform_host_advance_us(GAP_LIMIT_US);
        CHECK_INT(sampler_take(&s, &sample), SAMPLE_OK);

        CHECK_INT(s.gaps, 0);
        CHECK_INT(s.max_interval_us, GAP_LIMIT_US);
    }

    TF_CASE("max_interval_us keeps the worst, not the last");
    {
        fresh(&s);

        platform_host_set_time_us(1000);
        CHECK_INT(sampler_take(&s, &sample), SAMPLE_OK);

        platform_host_advance_us(40000);
        CHECK_INT(sampler_take(&s, &sample), SAMPLE_OK);

        platform_host_advance_us(1000);
        CHECK_INT(sampler_take(&s, &sample), SAMPLE_OK);

        CHECK_INT(s.max_interval_us, 40000);
    }
}
