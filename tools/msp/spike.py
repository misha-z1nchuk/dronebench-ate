"""
Phase 1b, step 1 — ask the flight controller what it is.

Read-only. Nothing here can make a motor turn, which is the point: the link,
the framing and the command ids are proven before anything is commanded.

    python3 tools/msp/spike.py read
    python3 tools/msp/spike.py read --port /dev/cu.usbmodem0x80000001
    python3 tools/msp/spike.py read --json          # for the journal

Propellers off. No battery — the FC runs from USB for this step, and the
plan calls for the battery to stay out until the analog path has been
validated on a resistive load.
"""

from __future__ import annotations

import argparse
import json
import sys
import time

import client
import protocol as p

# Low enough to be visibly "commanded" rather than "stopped", far below
# anything that would move an airframe. With no battery nothing turns at all;
# the value exists so the read-back can prove the command was accepted.
TEST_THROTTLE = 1050

STOPPED = 1000

# How often the override state is sampled while timing the FC's timeout.
# Fast enough to resolve a timeout in the hundreds of milliseconds, slow
# enough that the polling itself is not a stream of MSP traffic that might
# keep the override alive — which would make the experiment measure itself.
POLL_INTERVAL_S = 0.02


def gather(fc: client.MspClient) -> dict:
    identity = fc.identity()
    status = fc.status()
    return {
        "port": fc.port_name,
        "variant": identity.variant,
        "fc_version": str(identity.version),
        "api_version": str(identity.api),
        "board": identity.board,
        "cycle_time_us": status.cycle_time_us,
        "i2c_errors": status.i2c_errors,
        "sensors": status.sensors(),
        "armed": status.armed,
        "flight_mode_flags": status.flight_mode_flags,
        "motors": list(fc.motors()),
    }


def render(info: dict) -> str:
    lines = [
        f"port        {info['port']}",
        f"firmware    {info['variant']} {info['fc_version']}",
        f"api         {info['api_version']}",
        f"board       {info['board']}",
        f"cycle time  {info['cycle_time_us']} us",
        f"i2c errors  {info['i2c_errors']}",
        f"armed       {'YES' if info['armed'] else 'no'}",
        "",
        "sensors",
    ]
    for name, present in info["sensors"].items():
        lines.append(f"  {name:<14}{'yes' if present else 'no'}")

    lines += ["", "motor outputs (1000 = stopped)"]
    for index, value in enumerate(info["motors"], start=1):
        lines.append(f"  motor {index}       {value}")
    return "\n".join(lines)


def cmd_read(args: argparse.Namespace) -> int:
    port = args.port or client.find_port()
    with client.MspClient(port) as fc:
        info = gather(fc)

    print(json.dumps(info, indent=2) if args.json else render(info))

    # A non-zero cycle time means the scheduler is running. Zero would mean
    # the reply parsed but described a controller that is not looping, which
    # is a different problem from no reply at all.
    if info["cycle_time_us"] == 0:
        print("\nwarning: cycle time is 0 — the FC answered but is not "
              "running its scheduler", file=sys.stderr)
        return 1
    return 0


def require_safe(fc: client.MspClient) -> None:
    """Refuse to command motors on an armed flight controller.

    Not a substitute for the propellers being off — nothing readable over a
    wire can establish that, and the plan puts it first in the safety
    checklist for that reason. This only removes the one unsafe state the
    bench can actually see.
    """
    if fc.status().armed:
        raise SystemExit("FC reports ARMED. Disarm before commanding motors.")


def set_motor(fc: client.MspClient, motor: int, value: int) -> None:
    values = [STOPPED] * 4
    values[motor - 1] = value
    fc.send(p.Cmd.SET_MOTOR.value, p.encode_set_motor(values))


def stop_all(fc: client.MspClient) -> None:
    fc.send(p.Cmd.SET_MOTOR.value, p.encode_set_motor([STOPPED] * 4))


def cmd_override(args: argparse.Namespace) -> int:
    """Step 2 — does the FC accept MSP_SET_MOTOR at all?

    Answered by writing a value and reading it back. Nothing has to turn: with
    no battery the ESC rail is unpowered, and what is being tested is whether
    the controller took the command, not whether a motor can act on it.
    """
    port = args.port or client.find_port()
    with client.MspClient(port) as fc:
        require_safe(fc)
        before = fc.motors()
        print(f"before      {list(before[:4])}")

        try:
            set_motor(fc, args.motor, args.value)
            time.sleep(0.05)
            after = fc.motors()
        finally:
            stop_all(fc)

        print(f"after       {list(after[:4])}")

        accepted = after[args.motor - 1] == args.value
        print(f"\nverdict     {'ACCEPTED' if accepted else 'IGNORED'}")
        if not accepted:
            print("The FC answered every frame but did not take the value. "
                  "That is a policy refusal, not a link fault — check arming "
                  "flags and motor protocol before blaming the wiring.")
        return 0 if accepted else 1


def survives_silence(fc: client.MspClient, motor: int, value: int,
                     silence_s: float) -> bool:
    """Set the override, say nothing for silence_s, then look once.

    The single read happens after the silence, so it cannot have influenced
    what it measures. That is the whole design: an earlier version watched the
    override by polling MSP_MOTOR every 20 ms, which is itself a stream of MSP
    traffic. If Betaflight's timeout counts any MSP activity rather than
    SET_MOTOR specifically, that version would have kept the override alive
    and reported "no timeout" — an experiment measuring its own instrument.
    """
    stop_all(fc)
    time.sleep(0.1)
    set_motor(fc, motor, value)
    time.sleep(silence_s)
    return fc.motors()[motor - 1] == value


def cmd_timeout(args: argparse.Namespace) -> int:
    """Step 3, first half — how long does an override survive?

    Two measurements, because they answer different questions:

      silent    override set, then no traffic at all
      polled    override set, then MSP_MOTOR read every 20 ms

    If they agree, the timeout counts SET_MOTOR. If the polled one survives
    much longer, the timeout counts any MSP traffic — and that changes what
    the runner has to do on days 18-19, from "repeat SET_MOTOR" to "keep the
    link busy with anything".
    """
    port = args.port or client.find_port()
    with client.MspClient(port) as fc:
        require_safe(fc)
        try:
            # Binary search on the silence that the override survives. Each
            # trial is independent and costs one read, so nothing observed
            # here was disturbed by observing it.
            if survives_silence(fc, args.motor, args.value, args.limit):
                silent = None
            else:
                low, high = 0.0, args.limit
                for _ in range(7):
                    middle = (low + high) / 2
                    if survives_silence(fc, args.motor, args.value, middle):
                        low = middle
                    else:
                        high = middle
                silent = (low + high) / 2

            # The polled variant, for comparison only.
            stop_all(fc)
            time.sleep(0.1)
            set_motor(fc, args.motor, args.value)
            started = time.monotonic()
            polled = None
            while time.monotonic() - started < args.limit:
                time.sleep(POLL_INTERVAL_S)
                if fc.motors()[args.motor - 1] != args.value:
                    polled = time.monotonic() - started
                    break
        finally:
            stop_all(fc)

    def show(name: str, value: float | None) -> str:
        held = f"held past {args.limit:.1f} s"
        return f"{name:<12}{held if value is None else f'{value*1000:.0f} ms'}"

    print(show("silent", silent))
    print(show("polled", polled))
    print()

    if silent is None and polled is None:
        print("No timeout inside the window. Rerun with a larger --limit "
              "before concluding there is none.")
        return 0

    if silent is not None and polled is None:
        print(f"The override dies after {silent*1000:.0f} ms of silence but "
              "survives while MSP_MOTOR is being polled.")
        print("So the timeout counts ANY MSP traffic, not SET_MOTOR. The "
              "runner must keep the link busy; it need not resend the same "
              "command.")
        return 0

    if silent is not None and polled is not None:
        print(f"Both die at roughly the same point, so the timeout counts "
              f"SET_MOTOR itself.")
        print(f"The runner must repeat SET_MOTOR faster than "
              f"{silent*1000:.0f} ms. Half of it, {silent*500:.0f} ms, is "
              "the usual margin.")
    return 0


def cmd_hold(args: argparse.Namespace) -> int:
    """Step 3, second half — can a steady throttle be held for N seconds?

    This is the actual requirement. A motor test compares current profiles,
    and a profile is only comparable if the commanded throttle was constant
    for the whole window. Anything that drops out mid-window turns a
    measurement into a story about MSP.
    """
    port = args.port or client.find_port()
    with client.MspClient(port) as fc:
        require_safe(fc)
        period = args.period / 1000.0
        dropouts = 0
        samples = 0
        started = time.monotonic()

        try:
            while time.monotonic() - started < args.seconds:
                set_motor(fc, args.motor, args.value)
                time.sleep(period)
                samples += 1
                if fc.motors()[args.motor - 1] != args.value:
                    dropouts += 1
        finally:
            stop_all(fc)

        elapsed = time.monotonic() - started

    print(f"held        {elapsed:.1f} s at {args.value}")
    print(f"repeats     {samples} every {args.period:.0f} ms")
    print(f"dropouts    {dropouts}")
    print(f"\nverdict     {'STEADY' if dropouts == 0 else 'NOT STEADY'}")
    return 0 if dropouts == 0 else 1


def add_motor_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--motor", type=int, default=1, choices=(1, 2, 3, 4),
                        help="which motor to command (default 1)")
    parser.add_argument("--value", type=int, default=TEST_THROTTLE,
                        help=f"output value, 1000 = stopped "
                             f"(default {TEST_THROTTLE})")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="serial port; auto-detected if omitted")
    sub = parser.add_subparsers(dest="command", required=True)

    read = sub.add_parser("read", help="identity, sensors, motor outputs")
    read.add_argument("--json", action="store_true",
                      help="machine-readable, for the measurement journal")
    read.set_defaults(func=cmd_read)

    override = sub.add_parser(
        "override", help="step 2 — does the FC accept SET_MOTOR?")
    add_motor_args(override)
    override.set_defaults(func=cmd_override)

    timeout = sub.add_parser(
        "timeout", help="step 3 — how long an override survives silence")
    add_motor_args(timeout)
    timeout.add_argument("--limit", type=float, default=5.0,
                         help="how long to watch before giving up (seconds)")
    timeout.set_defaults(func=cmd_timeout)

    hold = sub.add_parser(
        "hold", help="step 3 — can a steady throttle be held?")
    add_motor_args(hold)
    hold.add_argument("--seconds", type=float, default=5.0,
                      help="how long to hold (default 5)")
    hold.add_argument("--period", type=float, default=50.0,
                      help="milliseconds between repeats (default 50)")
    hold.set_defaults(func=cmd_hold)

    args = parser.parse_args(argv)
    try:
        return args.func(args)
    except p.MspRefused as refusal:
        # The link works. Reported separately because it means the command
        # table is wrong, not the wiring.
        print(f"refused: {refusal}", file=sys.stderr)
        return 2
    except client.MspTimeout as timeout:
        print(f"no answer: {timeout}", file=sys.stderr)
        return 3
    except p.MspError as error:
        print(f"bad frame: {error}", file=sys.stderr)
        return 4


if __name__ == "__main__":
    sys.exit(main())
