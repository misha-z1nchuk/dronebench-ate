/*
 * ESP32 implementation of firmware/core/include/dronebench/platform.h.
 *
 * This is the other side of the boundary the host implementation sits on. The
 * core calls the same six functions either way and cannot tell which one it
 * got — that property is what the STM32 port will rely on.
 */
#ifndef PLATFORM_ESP32_H
#define PLATFORM_ESP32_H

#include <stdbool.h>

#include "dronebench/simulator.h"

/*
 * The console port. Shared so that the module reading from it and the platform
 * writing to it cannot drift apart — two copies of a port number is one copy
 * too many.
 */
#define PLATFORM_CONSOLE_UART_PORT 0

/*
 * Configures everything the platform owns: the status LED and the console
 * UART driver. Call once, before anything in the core runs and before any
 * module tries to read the console.
 */
void platform_esp32_init(void);

/*
 * Where platform_adc_read_*() gets its numbers.
 *
 * The analog front-end does not exist yet, so the real ADC path fails on every
 * call and the simulator is the default. Once day 12 implements the ADC this
 * stops being a stand-in and becomes what plan section 8 describes: the way a
 * fault is reproduced without breaking hardware.
 *
 * Simulated readings are not marked in any way at this level. That is on
 * purpose — the core must not be able to tell, or the simulator would stop
 * testing the code that runs on real data. What the operator sees instead is
 * the `simulate` command and the state it reports.
 */
void          platform_esp32_set_simulation(bool enabled, sim_profile_t profile);
bool          platform_esp32_simulation_enabled(void);
sim_profile_t platform_esp32_simulation_profile(void);

#endif /* PLATFORM_ESP32_H */
