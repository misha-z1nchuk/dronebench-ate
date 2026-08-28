/*
 * End-to-end calibration of the voltage path. Contract in calibration.h.
 */
#include "dronebench/calibration.h"

#include <math.h>
#include <string.h>

/*
 * Accumulated in double rather than float on purpose.
 *
 * The sums of squares here run to ~1e7 for readings in millivolts, and a
 * float carries about seven significant digits — so the last additions would
 * land below the ulp of the running total and simply not happen. The
 * symptom is not an error, it is a slope quietly biased by however many
 * points were swallowed.
 *
 * The ESP32 has no double-precision unit, so this is software arithmetic. It
 * costs nothing that matters: a fit runs once, at calibration, never in the
 * 500 Hz sampling path. Everything the sampler touches stays float.
 */
static bool finite_point(const calibration_point_t *point)
{
    return isfinite(point->measured_mv) && isfinite(point->reference_v);
}

void calibration_init(calibration_t *cal)
{
    memset(cal, 0, sizeof *cal);
    cal->valid = false;
}

bool calibration_fit(calibration_t *cal, const calibration_point_t *points,
                     uint32_t count)
{
    double sum_x = 0.0, sum_y = 0.0, sum_xx = 0.0, sum_xy = 0.0;
    double denominator, scale, offset;
    double max_residual = 0.0, sum_squared = 0.0;
    uint32_t i;

    if (count < CALIBRATION_MIN_POINTS || count > CALIBRATION_MAX_POINTS) {
        return false;
    }

    for (i = 0; i < count; i++) {
        double x, y;

        if (!finite_point(&points[i])) {
            return false;
        }

        x = (double)points[i].measured_mv;
        y = (double)points[i].reference_v;

        sum_x += x;
        sum_y += y;
        sum_xx += x * x;
        sum_xy += x * y;
    }

    /*
     * Zero when every point sits at the same reading — five measurements of
     * one voltage, which describes no line at all. Without this check the
     * division produces an infinity, the fit "succeeds", and every later
     * reading converts to inf or nan.
     */
    denominator = (double)count * sum_xx - sum_x * sum_x;
    if (denominator == 0.0 || !isfinite(denominator)) {
        return false;
    }

    scale = ((double)count * sum_xy - sum_x * sum_y) / denominator;
    offset = (sum_y - scale * sum_x) / (double)count;

    if (!isfinite(scale) || !isfinite(offset)) {
        return false;
    }

    /*
     * A divider attenuates; it cannot invert. A negative slope means the two
     * columns were entered the wrong way round, and that mistake fits just as
     * well as the right one — the line through the transposed points is
     * perfectly straight, so the residual will not catch it either.
     */
    if (scale <= 0.0) {
        return false;
    }

    for (i = 0; i < count; i++) {
        double predicted = scale * (double)points[i].measured_mv + offset;
        double residual = (double)points[i].reference_v - predicted;

        if (residual < 0.0) {
            residual = -residual;
        }
        if (residual > max_residual) {
            max_residual = residual;
        }
        sum_squared += residual * residual;
    }

    cal->scale_v_per_mv = (float)scale;
    cal->offset_v = (float)offset;
    cal->max_residual_v = (float)max_residual;
    cal->rms_residual_v = (float)sqrt(sum_squared / (double)count);
    cal->points = count;
    cal->valid = true;
    return true;
}

bool calibration_apply(const calibration_t *cal, float measured_mv,
                       float *volts_out)
{
    float volts;

    if (!cal->valid || !isfinite(measured_mv)) {
        return false;
    }

    volts = cal->scale_v_per_mv * measured_mv + cal->offset_v;
    if (!isfinite(volts)) {
        return false;
    }

    *volts_out = volts;
    return true;
}
