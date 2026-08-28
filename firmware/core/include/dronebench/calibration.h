/*
 * Turns what the ADC path reports into what the multimeter would have said.
 *
 * The chain from a battery to a number has four places to be wrong — the
 * divider's real ratio, the ADC's reference voltage, its integral
 * non-linearity, and the offset of whatever sits in front of it. Modelling
 * each one separately means four estimates, each with its own uncertainty,
 * multiplied together. Measuring the chain end to end against an instrument
 * that is better than it replaces all four with one fit and one residual.
 *
 * So this is deliberately not "the divider coefficient". It is the whole path
 * from an ADC reading to volts at the point the reference was clipped to, and
 * the resistor values never enter the arithmetic. They were measured on day 11
 * for a different purpose: to tell a wrong divider from a lying ADC when the
 * fit comes out somewhere unexpected.
 *
 * WHERE THE REFERENCE GOES
 *
 * At the divider's input, not its node — so `reference_v` is the voltage the
 * battery would have, and calibration_apply() returns battery volts with
 * nothing left to divide. Plan section 9 day 12 argues for the node instead,
 * on the grounds that a 3.5-digit meter resolves 1 mV at 2.18 V and only
 * 10 mV at 4.35 V. That argument is about the instrument, and it stops
 * applying the moment the instrument resolves 1 mV across the whole range.
 *
 * The plan's second argument does still apply and is worth stating: reference
 * and ADC must see the same voltage at the same instant. With a source built
 * from resistors off a regulated rail that is free. With a discharging battery
 * it is not, and this module would then be fitting the drift as if it were a
 * property of the ADC.
 *
 * WHAT THE RESIDUAL IS FOR
 *
 * A straight line through five points always exists. Whether it belongs there
 * is a separate question, and the residual is the only thing that answers it.
 * Risk 3 in the plan is that the ESP32's ADC is non-linear even inside
 * 0.15-2.4 V; if it is, the fit still succeeds and the residual is where that
 * shows up. A residual far above the ADC's own noise means a straight line is
 * the wrong model and the fallback — a correction table — is needed.
 *
 * The residual is also the honest input to the error budget in plan section
 * 10.1. It is measured, not assumed, and it already contains every effect the
 * calibration did not remove.
 */
#ifndef DRONEBENCH_CALIBRATION_H
#define DRONEBENCH_CALIBRATION_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Two points define a line and leave nothing over to check it with. The third
 * is what makes a residual mean anything, so it is the minimum rather than the
 * mathematical one.
 */
#define CALIBRATION_MIN_POINTS 3

/* Day 12 asks for five. The margin is for the correction-table fallback. */
#define CALIBRATION_MAX_POINTS 16

typedef struct {
    /* What the ADC path reported, in millivolts, after multisampling and
       whatever vendor conversion the platform applies. Not raw counts: the
       counts alone are meaningless without the attenuation and the reference,
       and those belong to the platform. */
    float measured_mv;

    /* What the reference instrument read at the same instant, in volts. */
    float reference_v;
} calibration_point_t;

typedef struct {
    float scale_v_per_mv;
    float offset_v;

    /* Worst single miss of the fitted line, in volts. This is the number to
       compare against the error budget's target. */
    float max_residual_v;

    /* Root mean square of the residuals — the standard uncertainty of the
       fit, and the figure that belongs in section 10.1. Smaller than the max
       by construction; reporting only the max overstates the typical error,
       reporting only the RMS hides the worst case. */
    float rms_residual_v;

    uint32_t points;

    /* False until a fit has succeeded. Everything downstream must refuse to
       produce a number while this is false — an uncalibrated reading is not a
       rough measurement, it is an unrelated one. */
    bool valid;
} calibration_t;

/* Clears to the uncalibrated state. Call before anything else. */
void calibration_init(calibration_t *cal);

/*
 * Least-squares fit of reference_v against measured_mv.
 *
 * Returns false and leaves *cal untouched when the fit would be meaningless:
 * too few points, any value not finite, every point at the same reading, or a
 * negative slope. The last one cannot happen to a working divider and is
 * there because the commonest way to get this wrong is to swap the two
 * columns while typing the table in — which produces a perfectly good fit of
 * the wrong thing.
 */
bool calibration_fit(calibration_t *cal, const calibration_point_t *points,
                     uint32_t count);

/*
 * Applies the fit. Returns false — leaving *volts_out untouched — when the
 * calibration is not valid or the reading is not finite.
 *
 * Same contract as sampler_take(): on anything but success the output keeps
 * whatever the caller had there, so a missed check cannot be mistaken for a
 * fresh measurement.
 */
bool calibration_apply(const calibration_t *cal, float measured_mv,
                       float *volts_out);

#endif /* DRONEBENCH_CALIBRATION_H */
