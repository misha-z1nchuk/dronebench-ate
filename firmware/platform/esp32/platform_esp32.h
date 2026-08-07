/*
 * ESP32 implementation of firmware/core/include/dronebench/platform.h.
 *
 * This is the other side of the boundary the host implementation sits on. The
 * core calls the same six functions either way and cannot tell which one it
 * got — that property is what the STM32 port will rely on.
 */
#ifndef PLATFORM_ESP32_H
#define PLATFORM_ESP32_H

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

#endif /* PLATFORM_ESP32_H */
