/*
 * Shared data types for the DroneBench portable core.
 *
 * These structs cross every boundary in the system: producer tasks, metric
 * accumulators, the telemetry encoder and the host-side parser all agree on
 * them. Changing a field here is a protocol change — bump the telemetry
 * version in telemetry.h when that happens.
 */
#ifndef DRONEBENCH_TYPES_H
#define DRONEBENCH_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * One measurement of the power path.
 *
 * timestamp_us is monotonic since boot, not wall-clock. 64 bits because a
 * 32-bit microsecond counter wraps after ~71 minutes, which is shorter than
 * a long bench session.
 *
 * current_ref_a comes from the independent I2C reference channel (INA226) and
 * is NAN when that channel is absent or has not been configured. Never compare
 * it with == ; use isnan().
 */
typedef struct {
    uint64_t timestamp_us;
    float    voltage_v;
    float    current_a;
    float    current_ref_a;
} power_sample_t;

/*
 * Aggregate result of one measurement session.
 *
 * sample_count counts samples that were accepted. rejected_count counts
 * samples refused as implausible (non-monotonic timestamp, non-finite value).
 * gap_count counts accepted samples that arrived after an unexpectedly long
 * silence — charge integrated across such a gap is an estimate, and a report
 * that hides this would be lying about its own confidence.
 */
typedef struct {
    float    min_voltage_v;
    float    max_voltage_v;
    float    max_current_a;
    float    avg_current_a;
    float    consumed_mah;
    float    consumed_wh;
    uint64_t duration_us;
    uint32_t sample_count;
    uint32_t rejected_count;
    uint32_t gap_count;
} session_metrics_t;

#endif /* DRONEBENCH_TYPES_H */
