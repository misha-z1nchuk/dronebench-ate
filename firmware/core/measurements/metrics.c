/*
 * TODO(day5): implement against the contract in metrics.h.
 *
 * firmware/tests/test_metrics.c is the specification; `make test` shows what
 * is missing.
 *
 * Suggested order:
 *
 *   1. metrics_init, metrics_result on an empty session
 *   2. the first sample     — start the session, no charge yet
 *   3. min / max / count    — plain comparisons
 *   4. charge and energy    — the integration, and the reason for double
 *   5. average current
 *
 * Unit note. Timestamps are microseconds and currents are amperes, so the
 * natural accumulator unit is A*us. Converting once on read keeps every
 * addition in the same units and avoids scaling 1.8 million times:
 *
 *     1 A * 1 us = 1e-6 A*s
 *     1 mAh      = 3.6 A*s = 3.6e6 A*us
 *
 * so   mah = charge_a_us / 3.6e6
 * and  wh  = energy_w_us  / 3.6e9
 */
#include "dronebench/metrics.h"

#include <string.h>

void metrics_init(metrics_t *metrics) { *metrics = (metrics_t){0}; }

void metrics_add(metrics_t *metrics, const power_sample_t *sample) {
  if (!metrics->has_previous) {
    /* The first sample establishes the session. min and max are set by
       assignment rather than comparison — a zero-initialised minimum would
       stay zero forever, since no real voltage is below it. */
    metrics->first_timestamp_us = sample->timestamp_us;
    metrics->min_voltage_v = sample->voltage_v;
    metrics->max_voltage_v = sample->voltage_v;
    metrics->max_current_a = sample->current_a;
    metrics->has_previous = true;
  } else {
    uint64_t dt_us = sample->timestamp_us - metrics->last_timestamp_us;

    /* The cast is on the first operand deliberately. Without it the product
       is computed in float and only the finished result widens on assignment,
       so the value reaching the double accumulator has already been rounded
       to seven digits — the accumulator would be wide and its input would
       not. */
    metrics->charge_a_us += (double)sample->current_a * (double)dt_us;
    metrics->energy_w_us +=
        (double)sample->voltage_v * (double)sample->current_a * (double)dt_us;

    if (sample->voltage_v < metrics->min_voltage_v) {
      metrics->min_voltage_v = sample->voltage_v;
    }
    if (sample->voltage_v > metrics->max_voltage_v) {
      metrics->max_voltage_v = sample->voltage_v;
    }
    if (sample->current_a > metrics->max_current_a) {
      metrics->max_current_a = sample->current_a;
    }
  }

  metrics->current_sum_a += sample->current_a;
  metrics->sample_count++;
  metrics->last_timestamp_us = sample->timestamp_us;
}

void metrics_result(const metrics_t *metrics, session_metrics_t *out) {
  *out = (session_metrics_t){0};

  out->sample_count = metrics->sample_count;
  out->min_voltage_v = metrics->min_voltage_v;
  out->max_voltage_v = metrics->max_voltage_v;
  out->max_current_a = metrics->max_current_a;

  if (metrics->sample_count > 0) {
    out->duration_us = metrics->last_timestamp_us - metrics->first_timestamp_us;
    out->avg_current_a =
        (float)(metrics->current_sum_a / metrics->sample_count);
  }

  out->consumed_mah = (float)(metrics->charge_a_us / 3.6e6);
  out->consumed_wh = (float)(metrics->energy_w_us / 3.6e9);
}
