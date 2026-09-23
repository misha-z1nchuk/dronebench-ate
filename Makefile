# Host build for the portable core.
#
# The core must compile and pass its tests on a plain desktop compiler, with no
# ESP-IDF and no board attached. If a change breaks that, the change leaked a
# platform dependency into the core.
#
#   make test     build and run the unit tests        (no board needed)
#   make venv     create .venv and install pyserial    (once)
#   make pytest   run the host-tool tests             (no board needed)
#   make log      record a telemetry session to CSV   (needs board)
#   make report   turn a recording into a report       (no board needed)
#   make msp      ask the flight controller what it is  (needs drone)
#   make fw       build the ESP32 firmware            (needs ESP-IDF)
#   make flash    build, flash and open the monitor   (needs board)
#   make monitor  open the serial monitor             (needs board)
#   make clean    remove host build artifacts
#
# -Werror is deliberate: warnings in embedded C are usually bugs. To silence it
# temporarily while experimenting: make WARN_ERROR=

CC ?= cc

WARN_ERROR ?= -Werror
WARN := -Wall -Wextra $(WARN_ERROR) \
        -Wshadow -Wpointer-arith -Wstrict-prototypes -Wmissing-prototypes \
        -Wold-style-definition

# -MMD -MP make the compiler emit a .d file listing every header each object
# depends on. Without them, editing a header changes nothing until the next
# `make clean` — the build reports success while running the old code, which
# is a very expensive kind of quiet.
CFLAGS := -std=c11 $(WARN) -O1 -g -MMD -MP \
          -Ifirmware/core/include -Ifirmware/platform/host -Ifirmware/tests

BUILD := build

CORE_SRC := $(wildcard firmware/core/*/*.c)
HOST_SRC := $(wildcard firmware/platform/host/*.c)
TEST_SRC := $(wildcard firmware/tests/*.c)
SRC      := $(CORE_SRC) $(HOST_SRC) $(TEST_SRC)
OBJ      := $(patsubst %.c,$(BUILD)/%.o,$(SRC))
BIN      := $(BUILD)/run_tests

.PHONY: all test clean check-core

all: test

test: check-core $(BIN)
	@echo
	@./$(BIN)

# The portable core earns its name only as long as nobody quietly includes a
# vendor header in it. That rule is easy to state, easy to break during a late
# debugging session, and invisible in review once it has been broken — so it
# is checked on every test run rather than trusted.
check-core:
	@if grep -rn --include='*.c' --include='*.h' \
	        -E '^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"](esp_|driver/|freertos/|soc/|hal/|stm32)' \
	        firmware/core; then \
	    echo; \
	    echo "ERROR: firmware/core must not depend on any vendor SDK."; \
	    echo "Add what you need to platform.h and implement it per target."; \
	    exit 1; \
	fi

$(BIN): $(OBJ)
	@mkdir -p $(dir $@)
	@$(CC) $(OBJ) -o $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

-include $(OBJ:.o=.d)

clean:
	rm -rf $(BUILD)

# --- ESP32 target build -----------------------------------------------------
#
# Thin wrappers around idf.py. They only work once the ESP-IDF environment is
# active in the current shell, which is per-terminal, not once per machine:
#
#     . ~/esp/esp-idf/export.sh

IDF_PROJECT := firmware/app/esp32

define require_idf
@command -v idf.py > /dev/null || { \
    echo "idf.py is not on PATH — activate the ESP-IDF environment first:"; \
    echo; \
    echo "    . ~/esp/esp-idf/export.sh"; \
    echo; \
    exit 1; }
endef

.PHONY: fw flash monitor

fw:
	$(require_idf)
	idf.py -C $(IDF_PROJECT) build

flash:
	$(require_idf)
	idf.py -C $(IDF_PROJECT) flash monitor

monitor:
	$(require_idf)
	idf.py -C $(IDF_PROJECT) monitor

# --- host tools -------------------------------------------------------------
#
# The logger parses the same wire format the firmware emits, and refuses the
# same lines. Its tests need neither a board nor pyserial, so they belong in
# the same habit as `make test`: run them before believing a change.

TOOLS    := tools/serial_logger
MSP      := tools/msp
REPORTS  := tools/report_generator
BLACKBOX := tools/blackbox
VENV     := .venv

# Prefer the project virtualenv when it exists. macOS ships an
# externally-managed Python that refuses `pip install` outright (PEP 668), so
# `make venv` is the supported way to get pyserial. The parser tests need no
# dependency at all and run under either interpreter.
PY := $(if $(wildcard $(VENV)/bin/python3),$(VENV)/bin/python3,python3)

.PHONY: venv pytest log report msp console blackbox

venv:
	python3 -m venv $(VENV)
	$(VENV)/bin/pip install --quiet --upgrade pip
	$(VENV)/bin/pip install --quiet -r $(TOOLS)/requirements.txt
	@echo "ready: $(VENV)"

pytest:
	$(PY) -m unittest discover -s $(TOOLS)
	$(PY) -m unittest discover -s $(REPORTS)
	$(PY) -m unittest discover -s $(MSP)
	$(PY) -m unittest discover -s $(BLACKBOX)

# make log                      -> find the board, write to data/sessions/
# make log ARGS="-p /dev/cu.x"  -> anything logger.py accepts
log:
	@test -x $(VENV)/bin/python3 || { \
	    echo "pyserial is not installed. Run: make venv"; exit 1; }
	$(PY) $(TOOLS)/logger.py $(ARGS)

# make report SESSION=data/sessions/session_20260824_183054
# Exit code carries the verdict: 0 pass, 1 warning, 2 fail, 3 inconclusive.
report:
	@test -n "$(SESSION)" || { \
	    echo "usage: make report SESSION=data/sessions/session_..."; exit 1; }
	$(PY) $(REPORTS)/generate.py $(SESSION) $(ARGS)

# Reads a log the flight controller wrote itself — no board, no port, nothing
# to plug in. One directory of plots per session in the file.
#
# make blackbox LOG=~/Desktop/BTFL_BLACKBOX_LOG_....BBL
# make blackbox LOG=... ARGS="--session 4"    -> only that session
# make blackbox LOG=... ARGS="--list"         -> what is in the file, no plots
blackbox:
	@test -n "$(LOG)" || { \
	    echo "usage: make blackbox LOG=path/to/BTFL_....BBL"; exit 1; }
	@test -x $(VENV)/bin/python3 || { \
	    echo "orangebox is not installed. Run: make venv"; exit 1; }
	$(PY) $(BLACKBOX)/plot_log.py $(LOG) $(ARGS)

# Phase 1b, step 1. Propellers off, no battery — the FC runs from USB.
# make msp                       -> auto-detect the flight controller
# make msp ARGS="override"         -> step 2, does the FC take SET_MOTOR
# make msp ARGS="timeout"          -> step 3, how long it survives silence
# make msp ARGS="hold --seconds 10"
msp:
	@test -x $(VENV)/bin/python3 || { \
	    echo "pyserial is not installed. Run: make venv"; exit 1; }
	$(PY) $(MSP)/spike.py $(if $(ARGS),$(ARGS),read)

# An interactive terminal on the board's console, for the CLI: help, status,
# adc, vdda, nodes, cal, volts, zero, amps, start, stop. `make monitor` is idf.py and only speaks to the
# ESP32; this one is just a serial port and works with whichever board is
# plugged in.
#
# make console                    -> first /dev/cu.usbmodem* found
# make console PORT=/dev/cu.xyz   -> say which one
CONSOLE_BAUD := 460800

console:
	@test -x $(VENV)/bin/python3 || { \
	    echo "pyserial is not installed. Run: make venv"; exit 1; }
	@port="$(PORT)"; \
	 if [ -z "$$port" ]; then port=$$(ls /dev/cu.usbmodem* 2>/dev/null | head -1); fi; \
	 test -n "$$port" || { \
	     echo "no /dev/cu.usbmodem* — is the board plugged in?"; exit 1; }; \
	 echo "$$port at $(CONSOLE_BAUD) baud — Ctrl-] to quit"; \
	 $(PY) -m serial.tools.miniterm --eol LF "$$port" $(CONSOLE_BAUD)
