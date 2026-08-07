#include "cli_uart.h"

#include <string.h>

#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "dronebench/platform.h"
#include "platform_esp32.h"

#define CLI_UART_CHUNK 64

/* Measured with uxTaskGetStackHighWaterMark once handlers exist; 3 KB is a
   starting point, not a measurement. */
#define CLI_TASK_STACK     3072
#define CLI_TASK_PRIORITY  5

/* One console, one parser. Static rather than heap: the bench must not have a
   failure mode that only appears when memory is tight. */
static cli_t s_cli;

static void cli_uart_output(void *ctx, const char *text)
{
    (void)ctx;

    /* One place in the firmware turns bytes into UART traffic. When the
       console moves to another transport, only that place changes. */
    platform_uart_write(text, strlen(text));
}

static void cli_uart_task(void *arg)
{
    uint8_t chunk[CLI_UART_CHUNK];

    (void)arg;

    for (;;) {
        /* Blocks until bytes arrive or the timeout expires, so an idle
           console costs no CPU. A line split across two reads parses exactly
           as one that arrived whole — that is cli_feed()'s contract. */
        int n = uart_read_bytes(PLATFORM_CONSOLE_UART_PORT, chunk,
                                sizeof chunk, pdMS_TO_TICKS(50));
        if (n > 0) {
            cli_feed(&s_cli, (const char *)chunk, (size_t)n);
        }
    }
}

void cli_uart_start(const cli_command_t *commands, size_t command_count)
{
    /* The port is already configured — platform_esp32_init() owns it. */
    cli_init(&s_cli, commands, command_count, cli_uart_output, NULL);

    xTaskCreate(cli_uart_task, "cli", CLI_TASK_STACK, NULL, CLI_TASK_PRIORITY,
                NULL);
}
