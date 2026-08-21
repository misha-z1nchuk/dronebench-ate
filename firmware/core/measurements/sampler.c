/*
 * TODO(day4): implement against the contract in sampler.h.
 *
 * `make test` shows what is expected — firmware/tests/test_sampler.c is the
 * specification.
 *
 * Suggested order:
 *
 *   1. sampler_init, sampler_had_trouble    — state and counters
 *   2. the happy path                       — read, fill, count, remember
 *   3. required channels failing
 *   4. non-finite values
 *   5. non-monotonic time
 *   6. gaps and max_interval_us
 *
 * Notes:
 *
 *   - isfinite() is in <math.h> and is the check you want. Testing for NAN
 *     alone misses infinity, which an ADC scaling bug produces just as easily
 *     (a divide by a zero calibration coefficient).
 *   - the optional reference channel failing is not an error. NAN is a value,
 *     not a failure code.
 *   - fill *out only once the sample is known to be good.
 */
#include "dronebench/sampler.h"

#include <math.h>

#include "dronebench/platform.h"

void sampler_init(sampler_t *sampler, uint64_t max_gap_us) {
  sampler->max_gap_us = max_gap_us;
  sampler->has_previous = false;

  sampler->last_timestamp_us = 0;
  sampler->accepted = 0;
  sampler->sensor_failures = 0;
  sampler->rejected_time = 0;
  sampler->rejected_value = 0;
  sampler->gaps = 0;

  sampler->max_interval_us = 0;
}

sample_result_t sampler_take(sampler_t *sampler, power_sample_t *out) {
  uint64_t now_us = platform_time_us();
  float voltage_v;
  float current_a;
  float ref_a;

  /* Required channels. Either one missing means there is no sample at all —
     voltage without current is not half a measurement, it is none. */
  if (!platform_adc_read_voltage(&voltage_v) ||
      !platform_adc_read_current(&current_a)) {
    sampler->sensor_failures++;
    return SAMPLE_SENSOR_FAILED;
  }

  /* Optional reference. Its absence is a value, not a failure: the bench runs
     degraded rather than not at all when the INA226 is not fitted. */
  if (!platform_ref_read_current(&ref_a)) {
    ref_a = NAN;
  }

  /* isfinite() rather than a NAN test: infinity is what a divide by a zero
     calibration coefficient produces, and it passes every range check. */
  if (!isfinite(voltage_v) || !isfinite(current_a)) {
    sampler->rejected_value++;
    return SAMPLE_REJECTED_VALUE;
  }

  /* Only meaningful once there is something to compare against. */
  if (sampler->has_previous && now_us <= sampler->last_timestamp_us) {
    sampler->rejected_time++;
    return SAMPLE_REJECTED_TIME;
  }

  if (sampler->has_previous) {
    uint64_t interval_us = now_us - sampler->last_timestamp_us;

    if (interval_us > sampler->max_gap_us) {
      sampler->gaps++;
    }
    if (interval_us > sampler->max_interval_us) {
      sampler->max_interval_us = interval_us;
    }
  }

  /* Nothing above this line touched *out. A caller that got anything other
     than SAMPLE_OK still holds whatever it had before. */
  out->timestamp_us = now_us;
  out->voltage_v = voltage_v;
  out->current_a = current_a;
  out->current_ref_a = ref_a;

  sampler->last_timestamp_us = now_us;
  sampler->has_previous = true;
  sampler->accepted++;

  return SAMPLE_OK;
}

bool sampler_had_trouble(const sampler_t *sampler) {
  return sampler->sensor_failures != 0 || sampler->rejected_time != 0 ||
         sampler->rejected_value != 0 || sampler->gaps != 0;
}
