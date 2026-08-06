/*
 * DroneBench ATE — ESP32 entry point.
 *
 * Day 1 scope: prove the board builds, flashes and talks. The banner exists
 * because every later debugging session starts by asking "which build is on
 * this board, and how did it get here?" — a bench that answers that in its
 * first three lines of output saves hours.
 */
#include <inttypes.h>
#include <stdio.h>

#include "cli_uart.h"
#include "esp_chip_info.h"
#include "esp_idf_version.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#define FW_VERSION "0.1.0"

static const char *reset_reason_to_string(esp_reset_reason_t reason) {
  switch (reason) {
  case ESP_RST_UNKNOWN:
    return "unknown";
  case ESP_RST_POWERON:
    return "power-on";
  case ESP_RST_EXT:
    return "external pin";
  case ESP_RST_SW:
    return "software reset";
  case ESP_RST_PANIC:
    return "exception/panic";
  case ESP_RST_INT_WDT:
    return "interrupt watchdog";
  case ESP_RST_TASK_WDT:
    return "task watchdog";
  case ESP_RST_WDT:
    return "other watchdogs";
  case ESP_RST_DEEPSLEEP:
    return "deep sleep exit";
  case ESP_RST_BROWNOUT:
    return "brownout";
  case ESP_RST_SDIO:
    return "SDIO reset";
  case ESP_RST_USB:
    return "USB reset";
  case ESP_RST_JTAG:
    return "JTAG reset";
  case ESP_RST_EFUSE:
    return "eFuse reset";
  case ESP_RST_PWR_GLITCH:
    return "power glitch";
  case ESP_RST_CPU_LOCKUP:
    return "CPU lockup";
  }
  return "unknown";
}

static void print_banner(void) {
  printf("\n");
  printf("=================================\n");
  printf(" DroneBench ATE\n");
  printf("=================================\n");
  printf(" firmware    %s\n", FW_VERSION);
  printf(" built       %s %s\n", __DATE__, __TIME__);
  printf(" idf         %s\n", esp_get_idf_version());

  esp_chip_info_t chip_info;
  esp_chip_info(&chip_info);

  printf(" chip model  %s\n",
         (chip_info.model == CHIP_ESP32) ? "ESP32" : "Unknown");
  printf(" cores       %d\n", chip_info.cores);
  printf(" silicon rev v%d.%02d\n", chip_info.revision / 100,
         chip_info.revision % 100);
  printf(" heap free    %" PRIu32 " bytes\n", esp_get_free_heap_size());

  printf(" features    %s%s%s\n",
         (chip_info.features & CHIP_FEATURE_WIFI_BGN) ? "WIFI " : "",
         (chip_info.features & CHIP_FEATURE_BT) ? "BT " : "",
         (chip_info.features & CHIP_FEATURE_BLE) ? "BLE" : "");

  esp_reset_reason_t reason = esp_reset_reason();
  const char *reason_str = reset_reason_to_string(reason);

  printf(" reset reason %s\n", reason_str);

  printf("=================================\n\n");
}

/* ---------------------------------------------------------------------------
 * Commands
 *
 * This table is the entire registry — cli.c knows no command names of its own,
 * which is why it can be tested with three invented ones.
 *
 * Handlers run on the CLI task and must not block. A handler that waits is a
 * console that cannot accept `stop` while a motor is spinning.
 *
 * Replies are machine-readable on purpose: in phase 3 the Python runner sends
 * these same commands and has to tell success from failure programmatically,
 * not by reading prose.
 * ------------------------------------------------------------------------- */

static void cmd_help(cli_t *cli, int argc, char **argv) {
  (void)argc;
  (void)argv;
  cli_print_help(cli);
}

static void cmd_version(cli_t *cli, int argc, char **argv) {
  (void)argc;
  (void)argv;

  /* TODO(day2): reply with firmware version and build date, in the same
   * key=value shape the parser already uses for errors:
   *
   *     OK,version,fw=0.1.0,built=Aug  6 2026
   *
   * Build the line with snprintf into a local buffer, then cli_write() it.
   * snprintf returns the length it WANTED to write — compare against the
   * buffer size to detect truncation instead of shipping a cut-off reply.
   */
  cli_write(cli, "OK,version,fw=" FW_VERSION ",built=" __DATE__ "\n");
}

static void cmd_status(cli_t *cli, int argc, char **argv) {
  (void)argc;
  (void)argv;

  /* TODO(day2): report what the bench is doing.
   *
   * There is no session state machine yet — that is day 6. Until then the two
   * numbers worth having are uptime and free heap: together they reveal a
   * silent reboot and a slow leak, the two failures that would otherwise be
   * discovered halfway through a motor test.
   *
   *     OK,status,uptime_s=41,heap_free=253120
   *
   * esp_timer_get_time() returns microseconds since boot as int64_t and needs
   * esp_timer.h. esp_get_free_heap_size() is already available here.
   */
  cli_write(cli, "OK,status,uptime_s=");
  char buf[64];
  snprintf(buf, sizeof(buf), "%" PRIi64, esp_timer_get_time() / 1000000);
  cli_write(cli, buf);
  cli_write(cli, ",heap_free=");
  snprintf(buf, sizeof(buf), "%" PRIu32, esp_get_free_heap_size());
  cli_write(cli, buf);
  cli_write(cli, "\n");
}

static const cli_command_t COMMANDS[] = {
    {"help", "list available commands", cmd_help},
    {"version", "firmware version and build date", cmd_version},
    {"status", "current bench state", cmd_status},
};

void app_main(void) {
  print_banner();

  cli_uart_start(COMMANDS, sizeof COMMANDS / sizeof COMMANDS[0]);

  printf("type 'help' for commands\n\n");

  /* app_main returns and its task is deleted; the CLI task keeps running.
   *
   * The uptime heartbeat is gone: an interactive console is a better liveness
   * signal, and a line printed every second would fight with whatever is being
   * typed. */
}
