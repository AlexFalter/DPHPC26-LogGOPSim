"""Build a pinned ATLAHS reference, adding only opt-in match observation."""
import argparse
import io
import os
from pathlib import Path
import shutil
import subprocess
import tarfile


def observe_matches(source):
    path = source / "sim/htsim-backend/sim/datacenter/logsim-interface.cpp"
    text = path.read_text()
    anchor = "comm_deps.push_back(dep);"
    if text.count(anchor) != 2:
        raise RuntimeError("Expected exactly two upstream match observation sites")
    text = text.replace(anchor, anchor + r'''
                        if (std::getenv("ATLAHS_TRACE_MATCHES"))
                            std::cout << "ATLAHS_MATCH " << std::get<0>(dep) << " " << std::get<1>(dep)
                                      << " " << std::get<2>(dep) << " " << std::get<3>(dep) << "\n";''')
    # Enable observation at the two match sites without enabling the upstream
    # dependency-file writer. Neither value feeds scheduling or matching.
    text = text.replace("if (comm_dep_file_given) {",
                        'if (comm_dep_file_given || std::getenv("ATLAHS_TRACE_MATCHES")) {')
    path.write_text(text)


def prepare(upstream, output, compiler, sysroot=""):
    if sysroot:
        os.environ["SDKROOT"] = sysroot
    revision = (Path(__file__).resolve().parent.parent / "UPSTREAM_REVISION").read_text().strip()
    source = output / "source"
    if source.exists():
        shutil.rmtree(source)
    source.mkdir(parents=True)
    archive = subprocess.check_output(["git", "-C", str(upstream), "archive", revision,
                                      "sim/htsim-backend/sim", "sim/LogGOPSim",
                                      "goal_gen/ai/nccl_goal_generator"])
    with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
        tar.extractall(source, filter="data")
    observe_matches(source)
    backend = source / "sim/htsim-backend/sim"
    for pattern in ("*.o", "*.a"):
        for artifact in backend.rglob(pattern):
            artifact.unlink()
    # Rebuild even if the pinned commit contains a prebuilt executable.
    (backend / "datacenter/htsim_ndp").unlink(missing_ok=True)
    # The upstream compiler already accepts this diagnostic; Clang needs the
    # compatibility flag.
    flags = "-O2 -g -std=c++17 -Wno-register -Wno-non-pod-varargs"
    common = ["make", "-j4", f"CC={compiler}", f"CFLAGS={flags}"]
    subprocess.run([*common, "libhtsim.a"], cwd=backend, check=True)
    subprocess.run([*common, "htsim_ndp"], cwd=backend / "datacenter", check=True)
    converter = source / "sim/LogGOPSim"
    lexer = converter / "txt2bin.re"
    # Upstream omits main's return type, which modern C++ compilers reject.
    # Preserve its original conversion behavior, including 32-bit tag narrowing.
    lexer.write_text(lexer.read_text().replace("\nmain(int argc, char **argv)",
                                               "\nint main(int argc, char **argv)"))
    subprocess.run(["make", "-B", "txt2bin", f"CXX={compiler}"], cwd=converter, check=True)
    (output / "revision.txt").write_text(revision + "\n")
    print(f"Reference source and binary: {source}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--upstream", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--compiler", default="c++")
    parser.add_argument("--sysroot", default="")
    args = parser.parse_args()
    prepare(args.upstream.resolve(), args.output.resolve(), args.compiler, args.sysroot)
