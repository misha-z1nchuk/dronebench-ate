/*
 * ACS724 node readings to amperes. Contract and the argument for doing it as
 * a ratio in current_sensor.h.
 */
#include "dronebench/current_sensor.h"

#include <math.h>
#include <string.h>

void current_sensor_init(current_sensor_t *sensor, float out_divider,
                         float vcc_divider, float amps_per_ratio)
{
    memset(sensor, 0, sizeof *sensor);
    sensor->out_divider    = out_divider;
    sensor->vcc_divider    = vcc_divider;
    sensor->amps_per_ratio = amps_per_ratio;
    sensor->zeroed         = false;
}

bool current_sensor_vcc_v(const current_sensor_t *sensor, float vcc_node_mv,
                          float *vcc_v)
{
    float volts;

    if (!isfinite(vcc_node_mv) || !(sensor->vcc_divider > 0.0f)) {
        return false;
    }
    volts = vcc_node_mv / sensor->vcc_divider / 1000.0f;
    if (!isfinite(volts)) {
        return false;
    }
    *vcc_v = volts;
    return true;
}

bool current_sensor_ratio(const current_sensor_t *sensor, float out_node_mv,
                          float vcc_node_mv, float *ratio)
{
    float vcc_v;
    float out_v;
    float r;

    if (!isfinite(out_node_mv) || !(sensor->out_divider > 0.0f)) {
        return false;
    }
    if (!current_sensor_vcc_v(sensor, vcc_node_mv, &vcc_v)) {
        return false;
    }

    /*
     * Also what keeps the division below finite: a disconnected VCC wire
     * reads near zero and fails here, rather than turning a floating pin into
     * a very large current.
     */
    if (vcc_v < CURRENT_SENSOR_VCC_MIN_V || vcc_v > CURRENT_SENSOR_VCC_MAX_V) {
        return false;
    }

    out_v = out_node_mv / sensor->out_divider / 1000.0f;
    r     = out_v / vcc_v;

    if (!isfinite(r) || r < CURRENT_SENSOR_RATIO_MIN ||
        r > CURRENT_SENSOR_RATIO_MAX) {
        return false;
    }

    *ratio = r;
    return true;
}

bool current_sensor_set_zero(current_sensor_t *sensor, float zero_ratio)
{
    if (!isfinite(zero_ratio) ||
        fabsf(zero_ratio - 0.5f) > CURRENT_SENSOR_ZERO_TOLERANCE) {
        return false;
    }
    sensor->zero_ratio = zero_ratio;
    sensor->zeroed     = true;
    return true;
}

bool current_sensor_amps(const current_sensor_t *sensor, float out_node_mv,
                         float vcc_node_mv, float *amps)
{
    float r;
    float result;

    if (!sensor->zeroed) {
        return false;
    }
    if (!current_sensor_ratio(sensor, out_node_mv, vcc_node_mv, &r)) {
        return false;
    }

    result = (r - sensor->zero_ratio) * sensor->amps_per_ratio;
    if (!isfinite(result)) {
        return false;
    }
    *amps = result;
    return true;
}
