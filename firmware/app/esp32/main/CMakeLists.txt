# The portable core is compiled straight into this component rather than
# wrapped as an IDF component. That keeps firmware/core/ free of any
# ESP-IDF build glue — the same sources are built by the host Makefile and,
# later, by the STM32 project.
idf_component_register(
    SRCS "main.c"
         "cli_uart.c"
         "telemetry_task.c"
         "output.c"
         "../../../core/protocol/cli.c"
         "../../../core/protocol/telemetry.c"
         "../../../core/measurements/sampler.c"
         "../../../core/measurements/metrics.c"
         "../../../core/measurements/simulator.c"
         "../../../core/state_machine/session.c"
         "../../../platform/esp32/platform_esp32.c"
    INCLUDE_DIRS "."
                 "../../../core/include"
                 "../../../platform/esp32"
    # An explicit REQUIRES turns off the special case where `main` implicitly
    # depends on every component in the build. That is worth having — the
    # dependencies become visible and the build stays smaller — but it means
    # anything outside IDF's common requirements has to be listed here.
    REQUIRES esp_driver_uart
             esp_driver_gpio
             esp_timer
)
