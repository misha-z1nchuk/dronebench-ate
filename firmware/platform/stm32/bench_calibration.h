/*
 * Calibration of this bench — this NUCLEO, these dividers, this ACS724.
 *
 * Constants in source rather than in flash, on purpose. A calibration is a
 * measurement, and a measurement belongs next to the record of how it was
 * made: the commit that changes a number here is the one that says which
 * meter, which points, and what the residual was. Flash would hold the same
 * number with none of that, and would lose it on the next full-chip erase.
 *
 * The firmware can also calibrate at runtime (`cal`, `zero`); those replace
 * these values until the next reset, and print the lines to paste here.
 *
 * A scale or zero of 0 means "not calibrated", and the matching path refuses
 * to report. That is the state until day 12 and day 13 have been done on the
 * bench.
 */
#ifndef BENCH_CALIBRATION_H
#define BENCH_CALIBRATION_H

/* ---- Voltage, day 12: `cal fit` ------------------------------------------ */

/*
 * 2026-09-23, three points, R_s from the 5 V rail into divider A's input,
 * reference at group 22 against the breadboard's minus rail:
 *
 *     R_s     reference    node (cal add)   residual
 *     20k     2.577 V      1289.56 mV       +0.34 mV
 *     10k     3.442 V      1719.33 mV       -1.07 mV
 *     6.8k    3.844 V      1917.84 mV       +0.73 mV
 *
 * Fitted off the board from the `cal add` replies: the board lost the points
 * when USB was pulled before `cal fit`. The fourth point read 4.86 mV at the
 * node — the breadboard was unpowered at that instant — and was dropped.
 *
 * The offset is the ground drop, not the ADC: 12-14 mV between the NUCLEO's
 * GND and the star point, doubled by the divider, is -24 to -28 mV at the
 * input. Moving the GND wire invalidates this line.
 *
 * Three points is the minimum; two more above 4.1 V are still owed.
 */
#define BENCH_VOLTAGE_SCALE_V_PER_MV 0.00201599f
#define BENCH_VOLTAGE_OFFSET_V       -0.023077f
/* Kept for `cal` to report, so a reset does not forget how good the fit was. */
#define BENCH_VOLTAGE_RMS_RESIDUAL_V 0.000775f
#define BENCH_VOLTAGE_MAX_RESIDUAL_V 0.001072f
#define BENCH_VOLTAGE_POINTS         3u

/* ---- Current, day 13: `zero` --------------------------------------------- */

/* OUT / VCC at no current. Ideal 0.5. */
#define BENCH_CURRENT_ZERO_RATIO 0.0f

/*
 * Divider ratios, node / pin, measured in circuit on day 11 (PROGRESS.md,
 * "Зібрані дільники"). Only their ratio to each other reaches the current,
 * and only as gain — see current_sensor.h.
 */
#define BENCH_OUT_DIVIDER 0.5008f
#define BENCH_VCC_DIVIDER 0.3323f

#endif /* BENCH_CALIBRATION_H */
