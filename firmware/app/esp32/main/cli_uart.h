/*
 * Binds the portable CLI core to the ESP32 console UART.
 *
 * Everything platform-specific about the command interface lives here: the
 * driver, the reader task, and the function that pushes bytes back out.
 * firmware/core/protocol/cli.c knows none of it, which is what lets the same
 * parser run under `make test` on a laptop and later on the STM32.
 */
#ifndef CLI_UART_H
#define CLI_UART_H

#include <stddef.h>

#include "dronebench/cli.h"

/*
 * Installs the UART driver and starts the task that feeds the parser.
 *
 * commands must outlive the call — pass a file-scope static const array.
 * Call after the boot banner: the driver takes over the port.
 */
void cli_uart_start(const cli_command_t *commands, size_t command_count);

#endif /* CLI_UART_H */
