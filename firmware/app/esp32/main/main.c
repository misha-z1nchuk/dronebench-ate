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

#include "esp_chip_info.h"
#include "esp_idf_version.h"
#include "esp_system.h"
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

void app_main(void) {
  uint32_t seconds = 0;

  print_banner();

  /* Heartbeat, so a silent board is distinguishable from a crashed one. */
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    seconds++;
    printf("uptime %lus\n", (unsigned long)seconds);
  }
}
