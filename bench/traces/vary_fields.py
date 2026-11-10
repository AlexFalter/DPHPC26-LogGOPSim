#!/usr/bin/env python3
"""Rewrites a GOAL schedule's field values for benchmark scenarios; the graph stays the same.

usage: vary_fields.py MODE INPUT OUTPUT

  wide    random sizes up to 2^40, tags up to 2^31 and CPUs and NICs up to 255, so that no
          field is constant and several are wide enough to cross 64-bit word boundaries
  atlahs  ATLAHS message identities (the eight fields of tests/data/atlahs_custom_fields.toml)
          with tag 0, read by the ATLAHS adapter

The values are random but fixed (seeded), so every build generates the same trace.
"""

import random
import re
import sys


def wide(line, rng):
    line = re.sub(r"\b\d+b\b", lambda _: f"{rng.randint(1, 1 << 40)}b", line)
    line = re.sub(r"\btag \d+", lambda _: f"tag {rng.randint(0, 1 << 31)}", line)
    return line + f" cpu {rng.randint(0, 255)} nic {rng.randint(0, 255)}"


def atlahs(line, rng):
    line = re.sub(r"\btag \d+", "tag 0", line)
    return (line + f" gpu_id {rng.randint(0, 3)} peer_gpu_id {rng.randint(0, 3)} stream_id {rng.randint(0, 7)}"
            f" communicator_id {rng.randint(0, 1 << 48)} channel_id {rng.randint(0, 31)}"
            f" sequence_id {rng.randint(0, 1 << 40)} message_kind {rng.randint(0, 5)}"
            f" chunk_id {rng.randint(0, (1 << 64) - 1)}")


def main():
    if len(sys.argv) != 4 or sys.argv[1] not in ("wide", "atlahs"):
        sys.exit(__doc__)
    mode, source, target = sys.argv[1:]
    vary = wide if mode == "wide" else atlahs
    rng = random.Random(20261106)
    with open(source) as input, open(target, "w") as output:
        for line in input:
            line = line.rstrip("\n")
            if re.match(r"\s*l\d+:", line):
                line = vary(line, rng)
            output.write(line + "\n")


if __name__ == "__main__":
    main()
