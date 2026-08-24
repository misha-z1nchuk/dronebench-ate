#!/usr/bin/env python3
"""
Reads a DroneBench telemetry stream and puts it on disk without losing any of
it.

  python3 tools/serial_logger/logger.py                  # log whatever comes
  python3 tools/serial_logger/logger.py --simulate normal --start --for 8
  python3 tools/serial_logger/logger.py --replay run.txt  # no board needed

Only one program can hold the port, so the logger drives the session itself
rather than expecting a monitor to be open alongside it. That is what
--simulate and --start are for; without them it records whatever happens to
arrive, which for an idle bench is nothing at all.

Layout, one directory per invocation:

    data/sessions/session_20260824_143012/
        stream_001_samples.csv
        stream_001_summaries.csv
        stream_002_samples.csv        <- a second `start`, or a reconnect
        events.log
        errors.log

A new stream begins at every header line. That is not a detail of the link:
the firmware emits a header on each `start`, and a USB unplug cuts the board's
power, so it reboots and its microsecond clock restarts at zero. Splitting on
the header keeps timestamps monotonic inside every file, which is what every
piece of analysis downstream assumes without checking.

Nothing is discarded quietly. A line that cannot be parsed goes to errors.log
with its number and its raw text, and the counts are reported at exit.
"""

from __future__ import annotations

import argparse
import csv
import os
import sys
import time
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from protocol import (  # noqa: E402
    LineAssembler,
    LineKind,
    Parser,
    ProtocolError,
    Sample,
    Summary,
)

# How often the OS is told to put the file on the actual disk. Every line
# would be far too slow at 500 Hz; never would leave the last second of a
# session in a kernel buffer when the machine loses power.
SYNC_PERIOD_S = 1.0

# How long to wait before trying the port again after it goes away. Long
# enough not to spin, short enough that plugging the cable back in feels
# immediate.
RECONNECT_DELAY_S = 1.0

# Bytes with no newline anywhere is a sign the port is not carrying this
# protocol; reported rather than hidden.
READ_CHUNK = 4096

# Whether opening the port resets the board depends on the module: DTR and RTS
# drive EN and IO0 through the auto-reset transistors on boards that have them,
# and pyserial asserts both on open. On the board in use it does not — a run on
# 2026-08-24 saw 791 s of uptime survive the open — but a reset would cost the
# whole session, so the settle and the drain below stay. They cost 1.5 s per
# run and cover both kinds of board.
BOOT_SETTLE_S = 1.5

# The console echoes nothing, so a command is only known to have arrived when
# its OK,/ERR, reply comes back. This is how long to allow for that.
REPLY_TIMEOUT_S = 2.0


class SessionWriter:
    """Owns the session directory and the files inside it."""

    def __init__(self, root: Path) -> None:
        stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        self.dir = root / f"session_{stamp}"
        self.dir.mkdir(parents=True, exist_ok=True)

        self._events = (self.dir / "events.log").open("w", encoding="utf-8")
        self._errors = (self.dir / "errors.log").open("w", encoding="utf-8")

        self._stream_index = 0
        self._samples_file = None
        self._samples_csv = None
        self._summaries_file = None
        self._summaries_csv = None

        self._last_sync = time.monotonic()
        self.samples_written = 0
        self.summaries_written = 0
        self.errors_written = 0

    # --- logs -------------------------------------------------------------

    def event(self, message: str) -> None:
        stamp = datetime.now().strftime("%H:%M:%S")
        self._events.write(f"{stamp}  {message}\n")
        self._events.flush()
        print(f"{stamp}  {message}", file=sys.stderr)

    def error(self, line_no: int, raw: str, reason: str) -> None:
        """
        Both the reason and the line itself. The reason alone is not enough to
        tell a firmware bug from a corrupted link — the raw text is what says
        which, and by then the bytes are long gone.
        """
        self.errors_written += 1
        self._errors.write(f"line {line_no}: {reason}\n    {raw!r}\n")
        self._errors.flush()

    # --- streams ----------------------------------------------------------

    def begin_stream(self, rate_hz: int) -> None:
        self.end_stream()
        self._stream_index += 1
        base = f"stream_{self._stream_index:03d}"

        self._samples_file = (self.dir / f"{base}_samples.csv").open(
            "w", newline="", encoding="utf-8"
        )
        self._samples_csv = csv.writer(self._samples_file)
        self._samples_csv.writerow(Sample.CSV_COLUMNS)

        self._summaries_file = (self.dir / f"{base}_summaries.csv").open(
            "w", newline="", encoding="utf-8"
        )
        self._summaries_csv = csv.writer(self._summaries_file)
        self._summaries_csv.writerow(Summary.CSV_COLUMNS)

        self.event(f"stream {self._stream_index} open, rate {rate_hz} Hz")

    def end_stream(self) -> None:
        for handle in (self._samples_file, self._summaries_file):
            if handle is not None:
                handle.flush()
                os.fsync(handle.fileno())
                handle.close()
        self._samples_file = self._samples_csv = None
        self._summaries_file = self._summaries_csv = None

    # --- rows -------------------------------------------------------------

    def write_sample(self, sample: Sample) -> None:
        if self._samples_csv is None:
            return
        self._samples_csv.writerow(sample.csv_row())
        self.samples_written += 1

    def write_summary(self, summary: Summary) -> None:
        if self._summaries_csv is None:
            return
        self._summaries_csv.writerow(summary.csv_row())
        self.summaries_written += 1
        if not summary.is_trustworthy():
            self.event(
                f"summary is not fully trustworthy: n={summary.sample_count} "
                f"refused={summary.refused} dropped={summary.dropped}"
            )

    def maybe_sync(self) -> None:
        """
        flush() moves bytes out of Python into the OS; fsync() moves them out
        of the OS onto the disk. Only the first is needed to survive this
        process dying — which is what pulling the USB cable actually causes
        here — so flushing happens on every write and fsync on a timer.
        """
        now = time.monotonic()
        if self._samples_file is not None:
            self._samples_file.flush()
        if self._summaries_file is not None:
            self._summaries_file.flush()

        if now - self._last_sync < SYNC_PERIOD_S:
            return
        self._last_sync = now
        for handle in (self._samples_file, self._summaries_file):
            if handle is not None:
                os.fsync(handle.fileno())

    def close(self) -> None:
        self.end_stream()
        for handle in (self._events, self._errors):
            handle.flush()
            os.fsync(handle.fileno())
            handle.close()


class StreamConsumer:
    """Parses lines and hands the results to the writer. Board-agnostic."""

    def __init__(self, writer: SessionWriter) -> None:
        self.writer = writer
        self.parser = Parser()
        self.assembler = LineAssembler()
        self.line_no = 0
        self.last_console: str | None = None
        self._last_report = time.monotonic()
        self._samples_at_report = 0

    def reset(self) -> None:
        """A dropped link means the next stream negotiates its version again."""
        self.parser.reset()
        self.assembler = LineAssembler()

    def feed(self, chunk: bytes) -> None:
        for line in self.assembler.feed(chunk):
            self.line_no += 1
            try:
                kind, value = self.parser.feed_line(line)
            except ProtocolError as exc:
                self.writer.error(self.line_no, line, str(exc))
                continue

            if kind is LineKind.HEADER:
                self.writer.begin_stream(value.rate_hz)
            elif kind is LineKind.SAMPLE:
                self.writer.write_sample(value)
            elif kind is LineKind.SUMMARY:
                self.writer.write_summary(value)
            elif kind is LineKind.CONSOLE:
                self.writer.event(f"console: {value}")
                self.last_console = value

        if self.assembler.overruns:
            self.writer.event(
                f"{self.assembler.overruns} buffer overrun(s): bytes arriving "
                f"with no line ending — wrong baud rate, or not this protocol"
            )
            self.assembler.overruns = 0

        self.writer.maybe_sync()
        self._report()

    def _report(self) -> None:
        now = time.monotonic()
        elapsed = now - self._last_report
        if elapsed < 2.0:
            return
        delta = self.writer.samples_written - self._samples_at_report
        self._last_report = now
        self._samples_at_report = self.writer.samples_written
        print(
            f"\r{self.writer.samples_written} samples "
            f"({delta / elapsed:.0f}/s), "
            f"{self.writer.summaries_written} summaries, "
            f"{self.writer.errors_written} errors",
            end="",
            file=sys.stderr,
            flush=True,
        )


# --- sources --------------------------------------------------------------


def find_port() -> str:
    """
    Picks the one USB serial device. Refuses to guess between several, because
    a logger that quietly attached to the wrong board would produce a file
    that looks entirely normal.
    """
    from serial.tools import list_ports

    candidates = [
        p.device
        for p in list_ports.comports()
        # macOS lists a Bluetooth port and a debug console that are never it.
        if "Bluetooth" not in p.device and "debug-console" not in p.device
    ]
    if not candidates:
        raise SystemExit(
            "no serial port found. Plug the board in, then check with "
            "`ls /dev/cu.*` (use cu.*, not tty.* — tty blocks waiting for a "
            "carrier signal a USB bridge never sends)."
        )
    if len(candidates) > 1:
        raise SystemExit(
            "several serial ports present:\n  "
            + "\n  ".join(candidates)
            + "\nname one with -p."
        )
    return candidates[0]


def _pump(handle, consumer: StreamConsumer, writer: SessionWriter) -> None:
    """One read. Kept separate so command replies go through the same path."""
    waiting = getattr(handle, "in_waiting", 0)
    chunk = handle.read(max(1, min(waiting, READ_CHUNK)))
    if chunk:
        consumer.feed(chunk)
    else:
        writer.maybe_sync()


def send_command(handle, text: str, consumer: StreamConsumer,
                 writer: SessionWriter) -> str | None:
    """
    Sends one console command and waits for its reply.

    Waiting matters: `start` after a `simulate` that never arrived would run
    against the real ADC and produce a session of nothing but sensor failures,
    which looks like a hardware fault rather than a missed keystroke.
    """
    writer.event(f"-> {text}")
    consumer.last_console = None
    handle.write((text + "\n").encode("ascii"))
    handle.flush()

    deadline = time.monotonic() + REPLY_TIMEOUT_S
    while time.monotonic() < deadline:
        _pump(handle, consumer, writer)
        if consumer.last_console is not None:
            return consumer.last_console
    writer.event(f"no reply to {text!r} within {REPLY_TIMEOUT_S}s")
    return None


def run_serial(port: str, baud: int, consumer: StreamConsumer,
               writer: SessionWriter, profile: str | None, start: bool,
               duration: float | None) -> None:
    import serial

    deadline = None

    while True:
        handle = None
        try:
            # A read timeout rather than blocking forever, so Ctrl+C is
            # noticed promptly and a silent port can still be reported.
            handle = serial.Serial(port, baud, timeout=0.2)
            writer.event(f"connected to {port} at {baud} baud")
            consumer.reset()

            if profile or start:
                # Long enough for a board that did reset to have finished
                # booting, and harmless for one that did not. Either way the
                # input buffer goes: it holds a banner, or the tail of an
                # earlier session, and neither belongs in this recording.
                time.sleep(BOOT_SETTLE_S)
                handle.reset_input_buffer()
                writer.event("input buffer dropped before driving the session")

            if profile:
                send_command(handle, f"simulate {profile}", consumer, writer)
            if start:
                reply = send_command(handle, "start", consumer, writer)
                if reply is None or reply.startswith("ERR"):
                    writer.event("the bench refused to start; logging anyway")
                if duration is not None:
                    deadline = time.monotonic() + duration

            while True:
                _pump(handle, consumer, writer)
                if deadline is not None and time.monotonic() >= deadline:
                    send_command(handle, "stop", consumer, writer)
                    writer.event(f"logged for {duration}s as asked")
                    return
        except KeyboardInterrupt:
            if handle is not None and start:
                try:
                    send_command(handle, "stop", consumer, writer)
                except Exception:
                    pass
            raise
        except Exception as exc:  # pyserial raises several unrelated types
            writer.event(f"link lost ({exc.__class__.__name__}: {exc})")
            writer.end_stream()
            time.sleep(RECONNECT_DELAY_S)
            writer.event("retrying")
        finally:
            if handle is not None:
                handle.close()


def run_replay(path: Path, consumer: StreamConsumer) -> None:
    """
    Feeds a captured log through the same path as a live board, in small
    chunks that deliberately do not align with line boundaries — which is how
    a serial port actually delivers, and the only way this exercises the
    assembler rather than pretending lines arrive whole.
    """
    data = path.read_bytes()
    for start in range(0, len(data), 7):
        consumer.feed(data[start : start + 7])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    parser.add_argument("-p", "--port", help="serial device; found if omitted")
    parser.add_argument("-b", "--baud", type=int, default=460800)
    parser.add_argument(
        "-o", "--out", type=Path, default=Path("data/sessions"),
        help="where session directories go (default: data/sessions/, which "
             "is gitignored — data/baselines/ is the versioned one)",
    )
    parser.add_argument(
        "--replay", type=Path,
        help="read a saved log instead of a port; needs no board",
    )
    parser.add_argument(
        "--simulate", metavar="PROFILE",
        help="send `simulate PROFILE` once connected "
             "(normal, motor4_fail, motor2_high, sag, short)",
    )
    parser.add_argument(
        "--start", action="store_true",
        help="send `start` once connected, and `stop` on the way out",
    )
    parser.add_argument(
        "--for", dest="duration", type=float, metavar="SECONDS",
        help="stop and exit after this long; implies --start",
    )
    args = parser.parse_args()

    writer = SessionWriter(args.out)
    consumer = StreamConsumer(writer)
    writer.event(f"logging to {writer.dir}")

    try:
        if args.replay:
            run_replay(args.replay, consumer)
        else:
            run_serial(args.port or find_port(), args.baud, consumer, writer,
                       args.simulate, args.start or args.duration is not None,
                       args.duration)
    except KeyboardInterrupt:
        print(file=sys.stderr)
        writer.event("stopped by user")
    finally:
        writer.event(
            f"{writer.samples_written} samples, "
            f"{writer.summaries_written} summaries, "
            f"{consumer.parser.lines_console} console replies, "
            f"{writer.errors_written} errors, "
            f"{consumer.assembler.pending()} bytes unterminated at exit"
        )
        writer.close()

    print(writer.dir)
    return 1 if writer.errors_written else 0


if __name__ == "__main__":
    raise SystemExit(main())
