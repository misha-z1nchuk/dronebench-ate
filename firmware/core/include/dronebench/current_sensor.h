/*
 * Turns the ACS724's two divider nodes into amperes.
 *
 * The sensor is ratiometric: both its zero and its sensitivity are fractions
 * of its own supply. At VCC = 5 V the bidirectional 50 A part sits at 2.5 V
 * with no current and moves 40 mV per ampere; at 4.8 V it sits at 2.4 V and
 * moves 38.4 mV. Plan section 4.2 has the numbers — USB wanders 4.7-5.2 V, and
 * a zero calibrated once in volts drifts by up to 3 A of current that is not
 * there.
 *
 * WHY EVERYTHING HERE IS A RATIO
 *
 * Write the output as a fraction of the supply, r = OUT / VCC. Then
 *
 *     r = r0 + S * I          with r0 = 0.5 and S = 0.008 per ampere
 *
 * and VCC is gone from the equation entirely:
 *
 *     I = (r - r0) / S
 *
 * Two things cancel that would otherwise each need a calibration:
 *
 *   - VCC itself. That is the point of a ratiometric sensor, and the reason
 *     the third ADC channel exists.
 *   - VDDA, the ADC's own reference. OUT and VCC are both read against it, so
 *     it appears in the numerator and the denominator of r. The current path
 *     does not depend on VREFINT at all.
 *
 * What does not cancel is the two dividers, because they differ (÷2 for OUT,
 * ÷3 for VCC). Their measured ratios go in as out_divider and vcc_divider. An
 * error in either scales r by a constant — and a constant factor on r scales
 * r0 by the same factor when the zero is measured through the same dividers.
 * So divider error survives only as a gain error on the current, the size of
 * the resistor mismatch, and never as an offset. The offset is the one that
 * matters at the small currents this bench measures.
 *
 * WHAT IS STILL MEASURED IN VOLTS
 *
 * Only the plausibility of VCC. The sensor is specified for 4.5-5.5 V, and a
 * VCC node reading far outside that means the supply wire is off — at which
 * point r is a division by a floating pin and the result is a number, not a
 * measurement. That check tolerates a percent of VDDA error easily, so it can
 * use whatever VDDA the platform last established.
 */
#ifndef DRONEBENCH_CURRENT_SENSOR_H
#define DRONEBENCH_CURRENT_SENSOR_H

#include <stdbool.h>

/* ACS724LLCTR-50AB: 40 mV/A at 5 V, i.e. 0.008 of VCC per ampere. The
   reciprocal is what the arithmetic uses. */
#define CURRENT_SENSOR_ACS724_50AB_AMPS_PER_RATIO 125.0f

/* Datasheet supply range. Outside it the ratiometric model is not promised. */
#define CURRENT_SENSOR_VCC_MIN_V 4.5f
#define CURRENT_SENSOR_VCC_MAX_V 5.5f

/*
 * The output swings 0.5 V to VCC - 0.5 V, i.e. 0.1 to 0.9 of the supply, and
 * clips there. A reading at the clip is "at least this much", not a value, so
 * it is refused rather than reported as the range limit.
 */
#define CURRENT_SENSOR_RATIO_MIN 0.1f
#define CURRENT_SENSOR_RATIO_MAX 0.9f

/*
 * How far a measured zero may sit from the ideal 0.5 and still be accepted.
 * The datasheet offset plus 3 % divider mismatch is well inside ±0.02; ±0.05
 * is ~6 A. A zero outside it was taken with current flowing, or with OUT and
 * VCC swapped — and storing it would shift every later reading by that much.
 */
#define CURRENT_SENSOR_ZERO_TOLERANCE 0.05f

typedef struct {
    float out_divider;    /* node / pin for OUT, measured in circuit on day 11 */
    float vcc_divider;    /* node / pin for VCC, same */
    float amps_per_ratio; /* 1 / S */

    float zero_ratio;     /* r at no current; meaningful only when zeroed */
    bool  zeroed;
} current_sensor_t;

/* Not zeroed afterwards: amps are refused until current_sensor_set_zero(). */
void current_sensor_init(current_sensor_t *sensor, float out_divider,
                         float vcc_divider, float amps_per_ratio);

/*
 * r = OUT / VCC at the sensor pins, from the two node readings.
 *
 * Both readings are millivolts, so both carry the same VDDA; it divides out
 * of r and is left only in the VCC window check, which does not mind it.
 *
 * Returns false, leaving *ratio untouched, on a non-finite input, a VCC
 * outside the supply range, or an output at the clip.
 */
bool current_sensor_ratio(const current_sensor_t *sensor, float out_node_mv,
                          float vcc_node_mv, float *ratio);

/* Refuses a zero further than CURRENT_SENSOR_ZERO_TOLERANCE from 0.5. */
bool current_sensor_set_zero(current_sensor_t *sensor, float zero_ratio);

/* Amperes. Same refusals as current_sensor_ratio(), plus "not zeroed". */
bool current_sensor_amps(const current_sensor_t *sensor, float out_node_mv,
                         float vcc_node_mv, float *amps);

/* VCC at the sensor pin in volts, for display. False on a non-finite input. */
bool current_sensor_vcc_v(const current_sensor_t *sensor, float vcc_node_mv,
                          float *vcc_v);

#endif /* DRONEBENCH_CURRENT_SENSOR_H */
