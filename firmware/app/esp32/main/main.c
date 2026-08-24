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
#include <string.h>

#include "cli_uart.h"
#include "dronebench/platform.h"
#include "dronebench/simulator.h"
#include "output.h"
#include "platform_esp32.h"
#include "telemetry_task.h"

#include "esp_chip_info.h"
#include "esp_idf_version.h"
#include "esp_rom_uart.h"
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

  /* Every part is a compile-time constant, so adjacent string literals are
     concatenated by the compiler. No buffer, no formatting, nothing that can
     be truncated at runtime. */
  cli_write(cli, "OK,version,fw=" FW_VERSION ",built=" __DATE__ "\n");
}

static void cmd_status(cli_t *cli, int argc, char **argv) {
  (void)argc;
  (void)argv;

  /* Assembled once and written once. While a session runs, the telemetry task
     is putting a line into this same port every 2 ms, and uart_write_bytes is
     atomic only within a single call — a reply built from five writes comes
     out with a measurement wedged into the middle of it.

     96 bytes covers the worst case with room to spare: the longest state name
     is 8 characters, uptime is at most 20 digits and heap at most 10. */
  char buf[160];

  /* state first: it answers "what is the bench doing", which is the question
     being asked. uptime and heap are diagnostics — together they reveal a
     silent reboot and a slow leak, the two failures that would otherwise be
     found halfway through a motor test. */
  /* The four loss counters are here rather than only in the telemetry summary
     because they answer a question asked between tests, not during one: did
     this bench keep up? missed and qdrop are the pipeline falling behind
     itself; odrop is the link. ohw against OUTPUT_QUEUE_DEPTH says whether
     the queue was sized well — the same question the stack high-water marks
     answer for stacks, and equally unanswerable by guessing. */
  snprintf(buf, sizeof buf,
           "OK,status,state=%s,uptime_s=%" PRIi64 ",heap_free=%" PRIu32
           ",missed=%" PRIu32 ",qdrop=%" PRIu32 ",odrop=%" PRIu32
           ",ohw=%" PRIu32 "\n",
           telemetry_state_name(), platform_time_us() / 1000000,
           esp_get_free_heap_size(), telemetry_missed_periods(),
           telemetry_queue_drops(), output_dropped(), output_high_water());
  cli_write(cli, buf);
}

/*
 * Stack headroom, per task, in bytes.
 *
 * uxTaskGetStackHighWaterMark reports the least free space a task has ever
 * had, not what it has now — a number that can only be collected by running
 * the thing, which is why every stack size in this firmware started as a
 * guess with a comment admitting it.
 *
 * In bytes because StackType_t is uint8_t on Xtensa. On a Cortex-M with
 * vanilla FreeRTOS the same call returns words, and the same number means
 * four times as much.
 */
static void cmd_tasks(cli_t *cli, int argc, char **argv) {
  static const char *const NAMES[] = {"measure", "process", "output", "cli"};
  char                     buf[96];

  (void)argc;
  (void)argv;

  for (size_t i = 0; i < sizeof NAMES / sizeof NAMES[0]; i++) {
    TaskHandle_t handle = xTaskGetHandle(NAMES[i]);

    if (handle == NULL) {
      snprintf(buf, sizeof buf, "OK,task,name=%s,state=absent\n", NAMES[i]);
    } else {
      snprintf(buf, sizeof buf, "OK,task,name=%s,stack_free_b=%" PRIu32 "\n",
               NAMES[i], (uint32_t)uxTaskGetStackHighWaterMark(handle));
    }
    cli_write(cli, buf);
  }
}

/*
 * start and stop take no arguments, and both report a refusal by naming the
 * state they were refused from. The session functions return only a bool; the
 * reason is already in the state machine, so there is nothing to invent —
 * RUNNING means one is already under way, UNSAFE means the previous result has
 * not been acknowledged, IDLE means there is nothing to stop.
 */
static void cmd_start(cli_t *cli, int argc, char **argv) {
  (void)argv;

  char buf[64];

  if (argc != 1) {
    cli_write(cli, "ERR,usage,start\n");
    return;
  }

  if (telemetry_session_start()) {
    cli_write(cli, "OK,start\n");
    return;
  }

  snprintf(buf, sizeof buf, "ERR,start,state=%s\n", telemetry_state_name());
  cli_write(cli, buf);
}

static void cmd_stop(cli_t *cli, int argc, char **argv) {
  (void)argv;

  char buf[64];

  if (argc != 1) {
    cli_write(cli, "ERR,usage,stop\n");
    return;
  }

  if (telemetry_session_stop()) {
    cli_write(cli, "OK,stop\n");
    return;
  }

  snprintf(buf, sizeof buf, "ERR,stop,state=%s\n", telemetry_state_name());
  cli_write(cli, buf);
}

static void cmd_simulate(cli_t *cli, int argc, char **argv) {
  sim_profile_t profile;
  char          buf[64];

  if (argc == 1) {
    snprintf(buf, sizeof buf, "OK,simulate,enabled=%d,profile=%s\n",
             platform_esp32_simulation_enabled() ? 1 : 0,
             simulator_profile_name(platform_esp32_simulation_profile()));
    cli_write(cli, buf);
    return;
  }

  if (argc != 2) {
    cli_write(cli, "ERR,usage,simulate [off|<profile>]\n");
    return;
  }

  if (strcmp(argv[1], "off") == 0) {
    /* Selects the real ADC, which has nothing behind it until day 12. The
       bench will then report sensor failures — honestly, and by design. */
    platform_esp32_set_simulation(false, SIM_PROFILE_NORMAL);
    cli_write(cli, "OK,simulate,enabled=0\n");
    return;
  }

  if (!simulator_profile_from_name(argv[1], &profile)) {
    cli_write(cli, "ERR,unknown_profile,");
    cli_write(cli, argv[1]);
    cli_write(cli, "\n");
    return;
  }

  platform_esp32_set_simulation(true, profile);
  snprintf(buf, sizeof buf, "OK,simulate,enabled=1,profile=%s\n", argv[1]);
  cli_write(cli, buf);
}

static const cli_command_t COMMANDS[] = {
    {"help", "list available commands", cmd_help},
    {"version", "firmware version and build date", cmd_version},
    {"status", "current bench state", cmd_status},
    {"start", "begin a measurement session", cmd_start},
    {"stop", "end the current session", cmd_stop},
    {"tasks", "stack headroom of every task, in bytes", cmd_tasks},
    {"simulate", "select a simulated profile, or off", cmd_simulate},
};

void app_main(void) {
  print_banner();

  /* printf buffers, and the tail of the banner is still in the UART FIFO at
     this point. Installing the driver reconfigures the peripheral and drops
     whatever had not gone out yet — which is why the banner used to stop
     mid-word.
     Two calls because there are two buffers: fflush moves bytes from stdio
     into the UART, and the wait lets the hardware finish sending them.
     The banner deliberately stays first: it answers "which build is on this
     board", and that question matters most when whatever follows crashes. */
  fflush(stdout);
  esp_rom_output_tx_wait_idle(PLATFORM_CONSOLE_UART_PORT);

  platform_esp32_init();

  /* First of the three: it creates the queue every other task sends into, and
     a send against a null handle is a crash rather than a lost line. */
  output_start();

  /* Before cli_uart_start, not after: this creates the mutex that start, stop
     and status all take. Accepting commands first leaves a window of a few
     milliseconds in which a typed command reaches xSemaphoreTake(NULL), and
     the resulting assert fires inside the kernel — where the backtrace points
     at FreeRTOS rather than at the order of these two lines. */
  telemetry_task_start();

  cli_uart_start(COMMANDS, sizeof COMMANDS / sizeof COMMANDS[0]);

  printf("type 'help' for commands\n\n");

  /* app_main returns and its task is deleted; the CLI task keeps running.
   *
   * The uptime heartbeat is gone: an interactive console is a better liveness
   * signal, and a line printed every second would fight with whatever is being
   * typed. */
}
