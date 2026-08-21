/*
 * Turns platform readings into validated samples.
 *
 * Everything downstream — mAh integration, sag detection, motor comparison,
 * the verdict — assumes its input is trustworthy. This is the only place that
 * decides whether it is. A bad sample admitted here becomes a wrong number in
 * a report that looks exactly like a right one.
 *
 * ---------------------------------------------------------------------------
 * CONTRACT
 * ---------------------------------------------------------------------------
 *
 * sampler_take() reads the clock and the sensors once and reports what it got:
 *
 *   SAMPLE_OK               *out is filled and may be used
 *   SAMPLE_SENSOR_FAILED    a required channel did not answer
 *   SAMPLE_REJECTED_TIME    the clock did not advance
 *   SAMPLE_REJECTED_VALUE   a reading was not a finite number
 *
 * On anything other than SAMPLE_OK, *out is untouched and the sample never
 * existed. Every outcome increments a counter, because the count of samples
 * a session failed to take is part of its result — a report built from half
 * the data it should have had must say so.
 *
 * Required vs optional channels:
 *
 *   voltage and current are required. If either fails, there is no sample.
 *   the reference current (INA226) is optional hardware. When it is absent
 *   the sample is still valid and current_ref_a is set to NAN — the bench
 *   runs degraded rather than not at all. Compare it with isnan(), never ==.
 *
 * Monotonic time:
 *
 *   a timestamp equal to or below the previous one is rejected. Duration,
 *   integration and timeout arithmetic all subtract two timestamps into an
 *   unsigned result, where a backwards clock does not produce a negative
 *   interval — it produces an enormous positive one.
 *
 * Gaps:
 *
 *   a sample arriving later than max_gap_us after the previous one is still
 *   accepted, but counted. Charge integrated across a gap is an estimate, and
 *   a report that hides which of its numbers were estimated is lying about its
 *   own confidence.
 */
#ifndef DRONEBENCH_SAMPLER_H
#define DRONEBENCH_SAMPLER_H

#include <stdbool.h>
#include <stdint.h>

#include "dronebench/types.h"

typedef enum {
    SAMPLE_OK = 0,
    SAMPLE_SENSOR_FAILED,
    SAMPLE_REJECTED_TIME,
    SAMPLE_REJECTED_VALUE,
} sample_result_t;

typedef struct {
    uint64_t max_gap_us;

    uint64_t last_timestamp_us;
    bool     has_previous;

    uint32_t accepted;
    uint32_t sensor_failures;
    uint32_t rejected_time;
    uint32_t rejected_value;
    uint32_t gaps;

    /* Largest interval seen between two accepted samples. Reported so the
       worst hiccup of a session is visible, not just how many there were. */
    uint64_t max_interval_us;
} sampler_t;

/*
 * max_gap_us is the interval above which an accepted sample is also counted as
 * following a gap. Pick it from the configured sample rate with headroom —
 * a few missed periods, not one.
 */
void sampler_init(sampler_t *sampler, uint64_t max_gap_us);

sample_result_t sampler_take(sampler_t *sampler, power_sample_t *out);

/* True when the session produced samples the caller should not fully trust. */
bool sampler_had_trouble(const sampler_t *sampler);

#endif /* DRONEBENCH_SAMPLER_H */
