# DroneBench ATE

Automated test equipment for 1S FPV drones. The bench answers one question —
*is this airframe safe to fly, and if not, what should I check next?* — by
measuring the power path while driving the flight controller through a
standardised test sequence.

**Status: in development.** The software pipeline is complete and runs end
to end on simulated measurements. The portable core runs on two vendors'
silicon, and on the STM32 the voltage path now measures a real battery:
**3.8129 V on the bench against 3.79 V on a multimeter**, inside the ±30 mV
target, through an end-to-end calibration whose worst point misses its line by
1.07 mV. The current path reads a live ACS724 with its zero in place, but no
real current has flowed through it yet. Results and how they were taken:
[On the bench](#on-the-bench).

- [Plan and schematics](dronebench_ate_final_plan.md) (Ukrainian)
- [Progress log](PROGRESS.md) (Ukrainian)

## Reproduce it

No board, no drone, no instruments. Four commands.

```sh
make venv                                        # once: .venv + pyserial
make test                                        # 555 checks / 125 cases — the C core
make pytest                                      # 106 tests — the Python tools
make report SESSION=data/baselines/motor4_fail   # a verdict from a known-bad recording
```

The last one prints the report in section 11 of the plan and exits `2`,
because that recording contains a motor that never drew current and the
bench is supposed to say so. Exit codes carry the verdict: `0` pass,
`1` warning, `2` fail, `3` inconclusive.

`data/baselines/motor4_fail/` is a synthetic recording with a defect whose
right answer is known in advance, committed on purpose — a fixture that needs
neither a board nor a broken drone. Its report is versioned alongside it, so
regenerating reproduces the committed text byte for byte except the timestamp,
and a change in the diagnostic rules shows up as a diff in review.

To replay a recorded stream through the logger as if a board were attached:

```sh
.venv/bin/python3 tools/serial_logger/logger.py --replay <saved-log>
```

## What it does

Measures battery voltage and current draw through a custom analog front-end,
cross-checked against an independent I2C reference so the bench can state its
own measurement uncertainty rather than assume it. Drives the flight
controller over MSP to run each motor separately with props removed, compares
current profiles, and classifies the result:

```
PASS · REPAIR_MINOR · REPAIR_ELECTRONICS · UNSAFE · UNKNOWN
```

Reports name what was observed, what could cause it, and which test isolates
it — never that a specific part has failed, unless an isolation test proved it.

## Layout

```
firmware/core/          portable logic — no vendor SDK headers, ever
firmware/platform/      host / esp32 / stm32 implementations of platform.h
firmware/app/           wiring for the target build
firmware/tests/         host unit tests for the core
tools/serial_logger/    reads telemetry off the wire, writes CSV
tools/report_generator/ segmentation, rules, verdicts, graphs
tools/msp/              talks MSP to the flight controller
tools/blackbox/         plots the flight controller's own .BBL log
test_profiles/          per-airframe limits
docs/                   measurement methodology, error budget
```

The core compiles and passes its tests on a desktop compiler with no board
attached. That constraint is what keeps it portable between ESP32 and STM32.

## Build

Host tests, no hardware needed:

```sh
make test
```

Firmware (ESP-IDF v5.5, target ESP32):

```sh
. ~/esp/esp-idf/export.sh
idf.py -C firmware/app/esp32 build flash monitor
```

Firmware (STM32 NUCLEO-F446RE):

Open `firmware/app/stm32/dronebench-stm32/` in STM32CubeIDE and build. The
project reaches the portable core through three linked folders instead of
copying it, which is why `.project` and `.cproject` are committed — CubeMX
cannot regenerate that wiring, and without it a fresh clone builds firmware
with no core in it.

Flashing needs no debug-probe software. The board presents a mass-storage
volume that programs whatever binary is copied onto it:

```sh
arm-none-eabi-objcopy -O binary Debug/dronebench-stm32.elf firmware.bin
cp firmware.bin /Volumes/NOD_F446RE/
```

Then talk to it over the ST-LINK's virtual serial port:

```sh
make console        # first /dev/cu.usbmodem* at 460800, Ctrl-] to quit
```

`help` lists the thirteen commands. The ones that matter on the bench:

| Command | What it is for |
|---|---|
| `nodes` | millivolts at all three divider nodes — the wiring check |
| `vdda` | the ADC reference, from VREFINT — compare with a meter on 3V3 |
| `cal add <V>` · `fit` | voltage calibration against a multimeter, with per-point residuals |
| `volts` · `amps` | calibrated one-shot readings |
| `zero` | the ACS724's no-current ratio, taken before each session |
| `simulate off` · `start` | real sensors, then the 500 Hz stream |

A fit is applied at once and printed as `#define` lines for
`firmware/platform/stm32/bench_calibration.h`, so it survives a reset only
once it is committed — next to the record of how it was measured.

On macOS and Linux, `install.sh` deliberately does not install CMake and Ninja
— it expects them from the system package manager. If `idf.py` reports
`"cmake" must be available on the PATH`, pull them into the IDF toolchain
instead of installing them system-wide:

```sh
python3 $IDF_PATH/tools/idf_tools.py install cmake ninja
```

## Editor

VS Code with the **clangd** extension. The Microsoft C/C++ extension is
disabled for this workspace in `.vscode/settings.json` — it tries to index the
whole ESP-IDF tree up front and stalls; clangd reads `compile_commands.json`
and indexes lazily.

Two config files make it work: `.clangd` strips four GCC-only flags that clangd
rejects outright, and `compile_flags.txt` covers the portable core, which no
`compile_commands.json` describes. Run `make fw` once before opening the ESP32
sources — the compilation database is generated by the build.

## On the bench

NUCLEO-F446RE, the analog front-end built on day 11, a 1S LiHV pack, one
multimeter with 1 mV resolution. 22–23 September 2026.

**Reference.** The chip derives VDDA from ST's factory VREFINT calibration:
3303–3305 mV, repeatable to 0.4 mV across flashes. The multimeter reads the
3V3 pin at 3.322 V. The 0.55 % gap is inside both instruments' tolerances
(factory calibration ±10 mV, meter ±19 mV at 3.3 V), so neither is declared
wrong — and neither needs to be: the calibration below is against the meter,
and the current path does not use VDDA at all.

**Voltage calibration.** Measured end to end instead of modelled: a resistor
from the 5 V rail into the divider's input makes the test voltage, the meter
reads it, `cal add` pairs it with the ADC. One least-squares line through the
whole chain — divider, reference, converter — and no resistor value enters it.

| Source | Meter | ADC node | Residual |
|---|---|---|---|
| 20 kΩ | 2.577 V | 1289.56 mV | +0.34 mV |
| 10 kΩ | 3.442 V | 1719.33 mV | −1.07 mV |
| 6.8 kΩ | 3.844 V | 1917.84 mV | +0.73 mV |

`V = 0.00201599 · mV − 0.02308`, RMS residual 0.78 mV. The −23 mV offset is
not the converter: 12–14 mV drops across the ground wire between the NUCLEO and
the breadboard's star point (≈1.2 Ω carrying the sensor's 10 mA), and the
divider doubles it. Measured directly, found stable to ±0.4 mV, and left for
the fit to absorb. Three points is the minimum the firmware accepts; two more
above 4.1 V are still owed.

**First real battery.** `volts` 3.8129 V (16 readings, 3.1 mV spread) against
the meter's 3.79 V: +23 mV, inside the ±30 mV target from plan §10.1. That the
difference matches the ground offset so closely is the next thing to check.

**Current path.** `I = (OUT/VCC − r0) · 125 A`. The ACS724 is ratiometric, so
the bench measures the ratio rather than volts: its supply cancels (USB 5 V
wanders), the ADC reference cancels (both channels read against it), and a
divider error can only scale the current, never offset it, because the zero is
taken through the same dividers. Host tests pin all three properties. On the
bench the sensor sits at ratio 0.500–0.503 with no current; one ADC count is
≈40 mA and single-read noise ±25 mA. The INA226 decision waits on a real
noise measurement.

What is not proven yet: current under load, and the 500 Hz stream from real
sensors (the timing budget is estimated at ~1.4 ms of the 2 ms period;
`status` reports the measured worst case as `worst_us`).

## Hardware

| | |
|---|---|
| MCU | ESP32-WROOM-32 → STM32 NUCLEO-F446RE |
| Current, analog | ACS724 + ADC1, ratiometric VCC correction |
| Current, reference | INA226 + 10 mΩ shunt over I2C |
| Voltage | 10k/10k divider, RC filtered |
| Device under test | BetaFPV Meteor75 Pro, 1S |

Pin map and schematics: [plan §4](dronebench_ate_final_plan.md).

## Safety

Motor current never passes through a breadboard or Dupont wiring. Props are
removed for every test. The full checklist is in the plan — read it before
connecting a battery.
