#include "cli_uart.h"

#include <string.h>

#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "dronebench/platform.h"
#include "esp_task_wdt.h"

#include "output.h"
#include "platform_esp32.h"

#define CLI_UART_CHUNK 64

/* Measured 2026-08-24: 1768 bytes used of 3072, with every handler exercised.
   The largest consumer is snprintf into a CLI_LINE_MAX buffer. */
#define CLI_TASK_STACK     3072
#define CLI_TASK_PRIORITY  5

/* One console, one parser. Static rather than heap: the bench must not have a
   failure mode that only appears when memory is tight. */
static cli_t s_cli;

static void cli_uart_output(void *ctx, const char *text)
{
    (void)ctx;

    /* Through the queue rather than to the port, so a reply cannot be split
       by a telemetry line arriving between two of its fragments. Day 7 fixed
       that by assembling each reply into one buffer before writing; the rule
       held only as long as everyone remembered it. Now a caller cannot break
       the ordering even by trying. */
    output_send(text, strlen(text));
}

static void cli_uart_task(void *arg)
{
    uint8_t chunk[CLI_UART_CHUNK];

    (void)arg;

    /* NULL is "this task", so it must run here rather than in
       cli_uart_start(), which executes on whoever called it. */
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    for (;;) {
        /* Blocks until bytes arrive or the timeout expires, so an idle
           console costs no CPU. A line split across two reads parses exactly
           as one that arrived whole — that is cli_feed()'s contract. */
        int n = uart_read_bytes(PLATFORM_CONSOLE_UART_PORT, chunk,
                                sizeof chunk, pdMS_TO_TICKS(50));
        if (n > 0) {
            cli_feed(&s_cli, (const char *)chunk, (size_t)n);
        }

        /* The 50 ms read timeout above is what makes this reachable at all: a
           console blocked forever on an idle port would never report itself
           alive, and the watchdog would reboot a perfectly healthy bench for
           having nobody typing at it. */
        platform_watchdog_feed();
    }
}

void cli_uart_start(const cli_command_t *commands, size_t command_count)
{
    /* The port is already configured — platform_esp32_init() owns it. */
    cli_init(&s_cli, commands, command_count, cli_uart_output, NULL);

    xTaskCreate(cli_uart_task, "cli", CLI_TASK_STACK, NULL, CLI_TASK_PRIORITY,
                NULL);
}
