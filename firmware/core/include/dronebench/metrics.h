/*
 * Accumulates validated samples into a session result.
 *
 * Fed only samples the sampler returned SAMPLE_OK for. Everything here assumes
 * finite values and a forward-moving clock; the checking already happened.
 *
 * ---------------------------------------------------------------------------
 * CONTRACT
 * ---------------------------------------------------------------------------
 *
 * metrics_add() takes one sample and folds it in. The first sample establishes
 * the session start and contributes no charge — charge needs an interval, and
 * one instant is not an interval.
 *
 * Every later sample contributes current x (t - t_previous). This is rectangle
 * integration using the newer of the two readings; see the note on accuracy
 * below.
 *
 * metrics_result() may be called at any time and does not end the session.
 *
 * On an empty session every field is zero and sample_count is zero. A caller
 * must check sample_count before believing min_voltage_v, which would
 * otherwise read as a perfectly good 0 V.
 *
 * ---------------------------------------------------------------------------
 * WHY THE ACCUMULATORS ARE WIDER THAN THE RESULTS
 * ---------------------------------------------------------------------------
 *
 * A one-hour session at 500 Hz is 1.8 million additions. Accumulating that in
 * float loses about 1.5% of the total, because float precision is relative:
 * once the running sum reaches 3600, its smallest step is 4.3e-4, and the
 * 0.002 being added is only a few of those steps. Each addition rounds, the
 * roundings are biased the same way, and the loss grows linearly with the
 * sample count rather than cancelling out.
 *
 * The results are float because they leave here once. The accumulators are
 * double because they are touched on every sample. Neither the ESP32 nor the
 * STM32F446 has a double-precision FPU, so this arithmetic is emulated in
 * software — at 500 operations per second, against 240 MHz, that is not a
 * cost worth optimising away.
 */
#ifndef DRONEBENCH_METRICS_H
#define DRONEBENCH_METRICS_H

#include <stdbool.h>
#include <stdint.h>

#include "dronebench/types.h"

typedef struct {
    bool     has_previous;
    uint64_t first_timestamp_us;
    uint64_t last_timestamp_us;

    /* See the note above before narrowing either of these to float. */
    double charge_a_us;  /* current_a x microseconds, converted on read */
    double energy_w_us;   /* power_w  x microseconds, converted on read */
    double current_sum_a;  /* for the mean */

    float    min_voltage_v;
    float    max_voltage_v;
    float    max_current_a;
    uint32_t sample_count;
} metrics_t;

void metrics_init(metrics_t *metrics);

/* sample must be one the sampler accepted. */
void metrics_add(metrics_t *metrics, const power_sample_t *sample);

void metrics_result(const metrics_t *metrics, session_metrics_t *out);

#endif /* DRONEBENCH_METRICS_H */
