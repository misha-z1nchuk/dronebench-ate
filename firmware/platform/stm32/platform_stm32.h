/*
 * STM32 implementation of firmware/core/include/dronebench/platform.h.
 *
 * The third implementation of the same six functions. The core cannot tell
 * which one it got — that was the claim the architecture made on day 1, and
 * this file is where the claim gets tested against a different vendor's HAL
 * rather than a different build of the same one.
 *
 * Target: NUCLEO-F446RE. The analog front-end is unchanged from the ESP32
 * build — same dividers, same ACS724, same star ground, per plan section 4.9.
 * Only the Dupont wires move.
 */
#ifndef PLATFORM_STM32_H
#define PLATFORM_STM32_H

#include <stdbool.h>

#include "dronebench/calibration.h"
#include "dronebench/current_sensor.h"
#include "dronebench/simulator.h"

/*
 * Expects CubeMX to have generated, with these exact default names:
 *
 *   huart2   USART2 on PA2/PA3, 115200 8N1 — wired to the ST-LINK virtual
 *            COM port on the NUCLEO, so `make log` connects to it unchanged
 *   hadc1    ADC1, 12-bit, scan off, continuous off, 1 conversion
 *
 * Channels are selected here rather than in CubeMX. Ranking them in the
 * generated code would put the pin map in two places, and the generated file
 * is the one that gets overwritten.
 */
void platform_stm32_init(void);

/*
 * The supply the ADC actually measured against, in millivolts.
 *
 * Not 3300. VDDA on a NUCLEO comes from an LDO fed by USB and sits wherever
 * the board's own load leaves it — a percent or so off nominal, which would
 * otherwise be inherited by every voltage the bench reports. Derived from the
 * factory VREFINT calibration; see the long comment in the .c file.
 *
 * Returns 0 if it could not be established, in which case every ADC read
 * refuses too.
 */
float platform_stm32_vdda_mv(void);

/*
 * The calibrations the real paths use. Loaded from bench_calibration.h at
 * init; `cal fit` and `zero` replace them until the next reset.
 *
 * These read the hardware regardless of `simulate`, because calibrating
 * against the simulator would be calibrating nothing.
 */
const calibration_t *platform_stm32_voltage_calibration(void);
void platform_stm32_set_voltage_calibration(const calibration_t *cal);
current_sensor_t *platform_stm32_current_sensor(void);

/*
 * Millivolts at the ACS724's two divider nodes, read back to back. The raw
 * half of `zero` and of the wiring check in `nodes`; the sampler goes through
 * platform_adc_read_current() instead.
 */
bool platform_stm32_read_current_nodes(float *out_node_mv, float *vcc_node_mv);

/*
 * Same contract as the ESP32 build: the core must not be able to tell that a
 * reading was synthetic, or the simulator would stop testing the code that
 * runs on real data. What the operator sees is the `simulate` command.
 */
void          platform_stm32_set_simulation(bool enabled, sim_profile_t profile);
bool          platform_stm32_simulation_enabled(void);
sim_profile_t platform_stm32_simulation_profile(void);

#endif /* PLATFORM_STM32_H */
