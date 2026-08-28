# msp

Talks to the flight controller over MultiWii Serial Protocol. Built for
phase 1b, whose only job was to answer one question before the rest of the
project was designed around a guess about it.

```
make msp                              # identity, sensors, motor outputs
make msp ARGS="override"              # does the FC accept SET_MOTOR?
make msp ARGS="timeout --limit 30"    # how long an override survives
make msp ARGS="hold --seconds 10"     # can a steady throttle be held?
make pytest                           # 27 tests, no drone
```

Propellers off. The battery stays out — the FC runs from USB, the ESC rail
does not, and nothing can turn.

## Files

| | |
|---|---|
| `protocol.py` | Frames, checksums, payload decoders. No serial port, so it is testable. |
| `client.py` | The port: framing by length, resynchronisation, three distinct failures. |
| `spike.py` | The four experiments. |
| `test_protocol.py` | 27 tests on frames built in the test. |

## What it found

Betaflight 4.5.3, board SG47, disarmed, USB power, no battery:

- `MSP_SET_MOTOR` is accepted, and `MSP_MOTOR` reads the mixer's real output
  rather than an echo of what was written
- the override survived **30 seconds of complete silence** — not one byte
  sent after the single command
- a steady throttle held for 10 s across 142 repeats with zero dropouts

So a controlled, reproducible motor test is possible, and days 19–21 can be
designed around direct override instead of the RC-channel fallback.

**With the caveat attached to it:** none of this was measured with a battery
connected. Betaflight may behave differently when VBAT is present. That is
day 17's experiment, not this one's conclusion.

## Three failures, kept apart

```
MspTimeout   nothing came back        link, port or FC is not listening
MspRefused   '$M!' came back          link works, command does not
MspError     something came back wrong link works and is corrupting data
```

Collapsing them into "it did not work" discards the only information that
says which end to look at.

## Two mistakes worth keeping

**The setter that answers.** `send()` originally wrote SET_MOTOR and
returned, assuming a setter produces no reply. Betaflight acknowledges it
with an empty frame, which then sat in the buffer waiting for the next
`request()` to collect it under the wrong command id. The only reason this
surfaced as an error rather than as data is that `request()` checks the
reply's command against the one it asked for — without that, an empty payload
read as `MSP_MOTOR` decodes to eight motors at zero.

**The experiment that measured itself.** The first `timeout` watched the
override by polling `MSP_MOTOR` every 20 ms. Polling is MSP traffic. If
Betaflight's timeout counted any activity rather than SET_MOTOR specifically,
that version would have kept the override alive and reported "no timeout".
It now uses independent trials — set, stay silent for T, read once — and
reports the polled variant alongside for comparison, because a disagreement
between the two would itself be the answer.
