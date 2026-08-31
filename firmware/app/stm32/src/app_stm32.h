/*
 * DroneBench ATE — STM32 application layer.
 *
 * The ESP32 build runs three FreeRTOS tasks. This one is a superloop, and
 * that is a deliberate first step rather than a simplification to be
 * apologised for: the point of tonight's port is to find out whether the
 * portable core runs unchanged on a second vendor's silicon, and a scheduler
 * added at the same time would make any failure ambiguous.
 *
 * Wire it into the CubeMX-generated main.c, which owns clock and peripheral
 * setup and must keep owning it — regeneration overwrites everything outside
 * the USER CODE markers:
 *
 *     // USER CODE BEGIN 2
 *     app_stm32_init();
 *     // USER CODE END 2
 *
 *     while (1)
 *     {
 *       // USER CODE BEGIN 3
 *       app_stm32_poll();
 *     }
 */
#ifndef APP_STM32_H
#define APP_STM32_H

/* Call once, after HAL_Init(), the clock config and every MX_*_Init(). */
void app_stm32_init(void);

/* Call forever. Never blocks longer than one telemetry line takes to send. */
void app_stm32_poll(void);

#endif /* APP_STM32_H */
