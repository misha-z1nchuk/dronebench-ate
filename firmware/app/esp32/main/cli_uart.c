#include "cli_uart.h"

#include <string.h>

#include "driver/uart.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define CLI_UART_PORT      UART_NUM_0
#define CLI_UART_BAUD      115200

/* Sized for a burst from a host that pastes a whole command at once. The
   parser consumes bytes one at a time, so this only has to outlast the
   scheduling gap between two runs of the reader task. */
#define CLI_UART_RX_BUFFER 512
#define CLI_UART_CHUNK     64

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
    uart_write_bytes(CLI_UART_PORT, text, strlen(text));
}

static void cli_uart_task(void *arg)
{
    uint8_t chunk[CLI_UART_CHUNK];

    (void)arg;

    for (;;) {
        /* Blocks until bytes arrive or the timeout expires, so an idle
           console costs no CPU. A line split across two reads parses exactly
           as one that arrived whole — that is cli_feed()'s contract. */
        int n = uart_read_bytes(CLI_UART_PORT, chunk, sizeof chunk,
                                pdMS_TO_TICKS(50));
        if (n > 0) {
            cli_feed(&s_cli, (const char *)chunk, (size_t)n);
        }
    }
}

void cli_uart_start(const cli_command_t *commands, size_t command_count)
{
    const uart_config_t config = {
        .baud_rate  = CLI_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_param_config(CLI_UART_PORT, &config));
    ESP_ERROR_CHECK(uart_driver_install(CLI_UART_PORT, CLI_UART_RX_BUFFER, 0, 0,
                                        NULL, 0));

    cli_init(&s_cli, commands, command_count, cli_uart_output, NULL);

    xTaskCreate(cli_uart_task, "cli", CLI_TASK_STACK, NULL, CLI_TASK_PRIORITY,
                NULL);
}
