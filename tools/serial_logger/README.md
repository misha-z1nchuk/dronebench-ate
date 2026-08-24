# serial_logger

Reads the telemetry stream from the bench and writes it to CSV without losing
any of it.

```
make venv                       # once: creates .venv, installs pyserial
make log                        # finds the board, writes to data/sessions/
make log ARGS="-p /dev/cu.usbserial-110"
make pytest                     # parser tests; no board, no pyserial
```

## Files

| | |
|---|---|
| `protocol.py` | The wire format, host side. No dependencies, so it can be tested and reused by the report generator. |
| `logger.py` | The port, the reconnect loop, the files on disk. |
| `test_protocol.py` | 31 tests, mostly about corrupt input. |

## Output

```
data/sessions/session_20260824_143012/
    stream_001_samples.csv      t_us,voltage_v,current_a,current_ref_a
    stream_001_summaries.csv    sample_count,consumed_mah,…,gaps,dropped
    stream_002_samples.csv      a second `start`, or a reconnect
    events.log                  connections, stream boundaries, warnings
    errors.log                  every refused line, with its number and text
```

`data/sessions/` is gitignored — captures are regenerated, not versioned.
Reference baselines belong in `data/baselines/`.

## Three decisions worth knowing

**A new file at every header.** The firmware emits a header on each `start`,
and unplugging USB cuts the board's power so it reboots with its microsecond
clock back at zero. Splitting on the header keeps timestamps monotonic inside
every file, which everything downstream assumes without checking.

**Bytes, not `readline()`.** `readline()` with a timeout returns whatever it
has when the timeout expires, and the caller cannot tell that from a line that
genuinely ended. A 28-byte sample line routinely arrives as two reads. So
`LineAssembler` holds the tail until its newline shows up.

**The reader is exactly as strict as the writer.** The firmware refuses to emit
a line it cannot complete; a reader that accepted salvageable fragments would
undo that and log the very corruption the firmware avoided producing. Same
version check, same required fields, same refusal of non-finite values — with
one exception: `current_ref_a` may be `nan`, because "no INA226 fitted" has to
stay distinguishable from "the reference measured 0 A".
