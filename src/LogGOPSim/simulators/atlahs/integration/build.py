"""Build the edited ATLAHS NDP sources outside their checkout.

ATLAHS's Makefiles write objects beside their inputs, so build a working copy in
the build directory. The readable, maintained sources stay in ../upstream.
"""

import argparse
import os
from pathlib import Path
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--include", type=Path, required=True)
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--sysroot", default="")
    args = parser.parse_args()

    source = args.source.resolve() / "sim/htsim-backend/sim"
    output = args.output.resolve()
    if not (source / "lgs/Parser.hpp").is_file():
        parser.error("--source must point to an ATLAHS checkout containing its htsim backend")
    if output == source or source in output.parents:
        parser.error("--output must be outside the supplied htsim source tree")

    shutil.copytree(source, output, dirs_exist_ok=True,
                    ignore=shutil.ignore_patterns("*.o", "*.a", "htsim_*", "parse_output"))
    # Prevent stale objects after changing source headers or compiler flags.
    for pattern in ("*.o", "*.a"):
        for artifact in output.rglob(pattern):
            artifact.unlink()

    environment = os.environ.copy()
    if args.sysroot:
        environment["SDKROOT"] = args.sysroot
    flags = "-O2 -g -std=c++23 -Wno-deprecated-declarations -Wno-register"
    common = ["make", "-j4", f"CC={args.compiler}", f"CFLAGS={flags}"]
    subprocess.run([*common, "libhtsim.a"], cwd=output, env=environment, check=True)
    include = f"-I.. -I. -I{args.include.resolve()}"
    library = f"-L.. {args.library.resolve()}"
    subprocess.run([*common, f"INCLUDE={include}", f"LIB={library}", "htsim_ndp"],
                   cwd=output / "datacenter", env=environment, check=True)
    subprocess.run([*common, "htsim_dumbell_ndp"],
                   cwd=output / "tests", env=environment, check=True)
    print(f"ATLAHS backend: {output / 'datacenter/htsim_ndp'}")


if __name__ == "__main__":
    main()
