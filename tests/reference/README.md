# Legacy simulation reference

`binomialtreebcast_8` captures the existing eight-rank C++ broadcast test,
using an explicit one-byte message size and the original simulator defaults.

- `schedule.goal`: fixed generator output.
- `legacy.bin`: original binary for backward-compatibility checks.
- `expected_times.json`: stable per-rank finish times used by the check.
- `original_output.txt`: complete original output for inspection; timing/speed
  lines are not compared.
- `metadata.json`: source commit, commands/settings, platform, byte order,
  and binary checksum.

Run from the build directory:

```sh
python3 ../tests/check_reference_times.py --simulator ./LogGOPSim --converter ./txt2bin
```

The check runs both the saved legacy binary and a fresh conversion of the
saved text, comparing host times against the same reference. It uses temporary
files and does not rewrite references. The legacy binary is native-endian;
this fixture was captured on little-endian ARM64 macOS.

These results preserve original behavior; they are not an independent proof
that the simulator is correct. Add focused schedules separately as needed.
