#!/usr/bin/env bash
# Builds every benchmark configuration from its own git worktree with one compiler and one set of
# flags (bench/README.md, "Builds").
#
# usage: bench/build.sh [--builds base,master,...] [--jobs N] [--llvm auto|on|off] [--gbench]
#                       [--fetch] [--allow-flag-mismatch] [--list]
#
#   BENCH_WORK          worktrees, builds, traces (default: bench/.work; keep it on a Linux file system)
#   CC / CXX / FC       compilers (default gcc / g++ / gfortran; all builds use the same ones)
#   BENCH_CFLAGS        extra C flags for every build (default: none)
#   BENCH_CXXFLAGS      extra C++ flags for every build (default: none)
#   LLVM_DIR            LLVM CMake package directory for the orc runner (default: from llvm-config)
#
# BASE (kind "upstream") is built with upstream's own CMake files and is never modified. NEW builds
# get the phase-timing hooks (bench/instrumentation) when instrument=true. Afterwards the compile
# command of LogGOPSim.cpp of every build is compared: all optimization/code-generation flags must
# be identical (only -std differs: upstream sets C++11, NEW needs C++23); the result goes to
# $BENCH_WORK/build/flags-check.json and a mismatch fails the script.
set -euo pipefail

BENCH="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$BENCH/.." && pwd)"
WORK="${BENCH_WORK:-$BENCH/.work}"
mkdir -p "$WORK"
WORK="$(cd "$WORK" && pwd)"
BUILDS_JSON="$BENCH/configs/builds.json"

BUILDS=""
JOBS="$(nproc 2>/dev/null || echo 4)"
LLVM_MODE="auto"
GBENCH="OFF"
FETCH=0
ALLOW_MISMATCH=0
while [ $# -gt 0 ]; do
  case "$1" in
    --builds) BUILDS="$2"; shift 2 ;;
    --jobs|-j) JOBS="$2"; shift 2 ;;
    --llvm) LLVM_MODE="$2"; shift 2 ;;
    --gbench) GBENCH="ON"; shift ;;
    --fetch) FETCH=1; shift ;;
    --allow-flag-mismatch) ALLOW_MISMATCH=1; shift ;;
    --list) python3 -c "import json;[print(k, '-', v['ref'], '-', v['description']) for k, v in json.load(open('$BUILDS_JSON'))['builds'].items()]"; exit 0 ;;
    -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done
[ -n "$BUILDS" ] || BUILDS="$(python3 -c "import json;print(','.join(json.load(open('$BUILDS_JSON'))['builds']))")"

case "$WORK" in
  /mnt/[a-zA-Z]/*) echo "WARNING: BENCH_WORK=$WORK is on a Windows drive (WSL DrvFs): builds and traces there are" \
                        "slow and mmap/page-cache behaviour differs. Use a Linux path, e.g. BENCH_WORK=~/lgs-bench." >&2 ;;
esac

# ------------------------------------------------------------------ toolchain checks
CC="${CC:-gcc}"; CXX="${CXX:-g++}"; FC="${FC:-gfortran}"
missing=()
for tool in git cmake python3 gengetopt re2c "$CC" "$CXX" "$FC"; do
  command -v "$tool" >/dev/null 2>&1 || missing+=("$tool")
done
if ! echo '#include <graphviz/cgraph.h>' | "$CC" -E -x c ${GRAPHVIZ_ROOT:+-I"$GRAPHVIZ_ROOT/include"} - >/dev/null 2>&1; then
  missing+=("graphviz headers (libgraphviz-dev)"); fi
if [ ${#missing[@]} -gt 0 ]; then
  echo "ERROR: missing build dependencies: ${missing[*]}" >&2
  echo "  Debian/Ubuntu: sudo apt-get install cmake gengetopt re2c gfortran libgraphviz-dev llvm-dev" >&2
  echo "  without root (cluster): bash bench/env/setup_env.sh, then: source bench/env/activate.sh" >&2
  exit 1
fi
CC="$(command -v "$CC")"; CXX="$(command -v "$CXX")"; FC="$(command -v "$FC")"
CMAKE_VERSION="$(cmake --version | head -1 | awk '{print $3}')"
python3 - "$CMAKE_VERSION" <<'EOF' || { echo "ERROR: CMake >= 3.20 required" >&2; exit 1; }
import sys
v = tuple(int(x) for x in sys.argv[1].split(".")[:2])
sys.exit(0 if v >= (3, 20) else 1)
EOF
GENERATOR=()
if command -v ninja >/dev/null 2>&1; then GENERATOR=(-G Ninja); fi

LLVM_ARGS=(-DLOGGOPSIM_WITH_LLVM=OFF)
if [ "$LLVM_MODE" != "off" ]; then
  if [ -z "${LLVM_DIR:-}" ]; then
    for cfg in llvm-config llvm-config-20 llvm-config-19 llvm-config-18 llvm-config-17 llvm-config-16 llvm-config-15; do
      if command -v "$cfg" >/dev/null 2>&1; then LLVM_DIR="$("$cfg" --cmakedir)"; break; fi
    done
  fi
  if [ -n "${LLVM_DIR:-}" ] && [ -d "$LLVM_DIR" ]; then
    LLVM_ARGS=(-DLOGGOPSIM_WITH_LLVM=ON "-DLLVM_DIR=$LLVM_DIR")
  elif [ "$LLVM_MODE" = "on" ]; then
    echo "ERROR: --llvm on but no LLVM CMake package found (set LLVM_DIR)" >&2; exit 1
  else
    echo "NOTE: LLVM not found: the orc runner is unavailable (NEW-orc is reported as unavailable)." >&2
  fi
fi

[ "$FETCH" = 1 ] && git -C "$REPO" fetch --all --prune

build_field() { python3 -c "import json,sys;b=json.load(open('$BUILDS_JSON'))['builds']['$1'];v=b.get('$2','');print(' '.join(v) if isinstance(v,list) else v)"; }

has_target() { # build_dir target  (list first: grep -q would SIGPIPE the producer under pipefail)
  local list
  if [ -f "$1/build.ninja" ]; then list="$(ninja -C "$1" -t targets all 2>/dev/null)"; grep -q "^$2:" <<< "$list"
  else list="$(cmake --build "$1" --target help 2>/dev/null)"; grep -q "\.\.\. $2\$" <<< "$list"; fi
}

IFS=',' read -r -a NAMES <<< "$BUILDS"
for name in "${NAMES[@]}"; do
  ref="$(build_field "$name" ref)"
  [ -n "$ref" ] || { echo "ERROR: unknown build '$name' (see $BUILDS_JSON)" >&2; exit 2; }
  kind="$(build_field "$name" kind)"
  instrument="$(build_field "$name" instrument)"
  targets="$(build_field "$name" targets)"
  commit="$(git -C "$REPO" rev-parse --verify "$ref^{commit}")" || {
    echo "ERROR: cannot resolve '$ref' for build '$name' (try --fetch)" >&2; exit 1; }
  wt="$WORK/src/$name"
  bdir="$WORK/build/$name"
  echo "=== $name: $ref = $commit ($kind${instrument:+, instrument=$instrument})"

  # A clean worktree at exactly this commit (instrumentation from an earlier build is discarded).
  if [ -e "$wt/.git" ]; then
    git -C "$wt" checkout --quiet --detach --force "$commit"
    git -C "$wt" reset --quiet --hard "$commit"
    git -C "$wt" clean -fdxq
  else
    git -C "$REPO" worktree prune
    git -C "$REPO" worktree add --detach --force "$wt" "$commit" >/dev/null
  fi
  if [ "$kind" = "new" ] && [ "$instrument" = "True" ]; then
    python3 "$BENCH/instrumentation/apply_phase_timing.py" "$wt"
  fi

  args=(-S "$wt/src" -B "$bdir" ${GENERATOR[@]+"${GENERATOR[@]}"}
        "-DCMAKE_C_COMPILER=$CC" "-DCMAKE_CXX_COMPILER=$CXX" "-DCMAKE_Fortran_COMPILER=$FC"
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON "-DCMAKE_C_FLAGS=${BENCH_CFLAGS:-}" "-DCMAKE_CXX_FLAGS=${BENCH_CXXFLAGS:-}"
        # Clang is only used by liballprof2 (MPI tracing), which is never built here; Ubuntu's
        # partially installed libclang-*-dev ships a CMake package that aborts configuration.
        -DCMAKE_DISABLE_FIND_PACKAGE_Clang=ON)
  if [ -n "${GRAPHVIZ_ROOT:-}" ]; then args+=("-DGRAPHVIZ_ROOT=$GRAPHVIZ_ROOT"); fi
  if [ "$kind" = "new" ]; then
    args+=(-DBUILD_TESTING=OFF "-DLOGGOPSIM_BUILD_BENCHMARKS=$GBENCH" -DBUILD_ATLAHS=OFF "${LLVM_ARGS[@]}")
    case " $targets " in *" bench_decode_micro "*) args+=("-DCMAKE_PROJECT_LogGOPSim_INCLUDE=$BENCH/micro/inject.cmake") ;; esac
  fi
  rm -f "$bdir/CMakeCache.txt"
  cmake "${args[@]}" > "$WORK/build-$name.configure.log" 2>&1 || {
    tail -30 "$WORK/build-$name.configure.log" >&2; echo "ERROR: configure of $name failed (log: $WORK/build-$name.configure.log)" >&2; exit 1; }
  build_targets=()
  for t in $targets; do
    if has_target "$bdir" "$t"; then build_targets+=("$t")
    elif [ "$t" = "LogGOPSim" ] || [ "$t" = "txt2bin" ] || [ "$t" = "bench_decode_micro" ]; then
      echo "ERROR: required target $t missing in $name" >&2; exit 1
    else echo "NOTE: optional target $t does not exist in $name" >&2; fi
  done
  if has_target "$bdir" loggopsim_orc; then build_targets+=(loggopsim_orc); fi
  cmake --build "$bdir" -j "$JOBS" --target "${build_targets[@]}" > "$WORK/build-$name.build.log" 2>&1 || {
    tail -40 "$WORK/build-$name.build.log" >&2; echo "ERROR: build of $name failed (log: $WORK/build-$name.build.log)" >&2; exit 1; }

  python3 - "$name" "$ref" "$commit" "$kind" "$instrument" "$wt" "$bdir" "$CC" "$CXX" "$FC" "$CMAKE_VERSION" \
           "${GENERATOR[*]:-Unix Makefiles}" "${build_targets[*]}" "${args[*]}" <<'EOF'
import json, os, shlex, subprocess, sys, time
from pathlib import Path
name, ref, commit, kind, instrument, wt, bdir, cc, cxx, fc, cmake_version, generator, targets, args = sys.argv[1:]
bdir, wt = Path(bdir), Path(wt)

def version(tool):
    try:
        return subprocess.run([tool, "--version"], capture_output=True, text=True).stdout.splitlines()[0]
    except Exception as e:
        return f"unknown ({e})"

def find(binary):
    for p in [bdir / binary] + sorted(bdir.rglob(binary)):
        if p.is_file() and os.access(p, os.X_OK):
            return str(p)
    return None

commands = json.loads((bdir / "compile_commands.json").read_text())
entry = next(c for c in commands if c["file"].endswith("LogGOPSim.cpp") and "simulators" in c["file"] or
             c["file"].endswith("src/LogGOPSim/LogGOPSim.cpp"))
argv = entry.get("arguments") or shlex.split(entry["command"])
keep, std = [], None
skip_next = False
for tok in argv[1:]:
    if skip_next:
        skip_next = False
        continue
    if tok in ("-o", "-c", "-I", "-isystem", "-D", "-MF", "-MT", "-MQ", "-include"):
        skip_next = True
        continue
    if tok.startswith("-std="):
        std = tok
    elif tok.startswith(("-O", "-g", "-f", "-m", "-pthread")) or tok == "-DNDEBUG":
        keep.append(tok)
stamp = wt / ".bench-phase-timing.json"
cache = {}
for line in (bdir / "CMakeCache.txt").read_text().splitlines():
    if "=" in line and not line.startswith(("#", "//")):
        k, _, v = line.partition("=")
        cache[k.split(":")[0]] = v
manifest = {
    "name": name, "ref": ref, "commit": commit, "kind": kind, "instrumented": instrument == "True",
    "instrumentation": json.loads(stamp.read_text()) if stamp.exists() else None,
    "worktree": str(wt), "build_dir": str(bdir),
    "worktree_status": subprocess.run(["git", "-C", str(wt), "status", "--porcelain"], capture_output=True,
                                      text=True).stdout.splitlines(),
    "binaries": {b: find(b) for b in ("LogGOPSim", "txt2bin", "schedgen", "first_pass_bench", "bench_decode_micro")},
    "orc_plugin": next((str(p) for p in bdir.rglob("*loggopsim_orc*") if p.suffix in (".so", ".dylib")), None),
    "llvm_enabled": cache.get("LLVM_DIR") not in (None, "", "LLVM_DIR-NOTFOUND") and cache.get("LOGGOPSIM_WITH_LLVM") == "ON",
    "llvm_dir": cache.get("LLVM_DIR"),
    "compiler": {"cc": cc, "cxx": cxx, "fc": fc, "cc_version": version(cc), "cxx_version": version(cxx),
                 "fc_version": version(fc)},
    "jit_cc": cc,
    "cmake_version": cmake_version, "generator": generator, "cmake_args": args, "targets": targets.split(),
    "build_type": cache.get("CMAKE_BUILD_TYPE"),
    "cxx_flags_release_with_debinfo": cache.get("CMAKE_CXX_FLAGS_RELWITHDEBINFO"),
    "cxx_flags": cache.get("CMAKE_CXX_FLAGS"), "c_flags": cache.get("CMAKE_C_FLAGS"),
    "loggopsim_compile_command": " ".join(argv),
    "flags_normalized": sorted(set(keep)), "cxx_std": std,
    "built_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
}
(bdir / "bench-build.json").write_text(json.dumps(manifest, indent=2) + "\n")
missing = [b for b in ("LogGOPSim",) if not manifest["binaries"][b]]
if missing:
    sys.exit(f"ERROR: {name}: binaries not found: {missing}")
print(f"    {name}: {manifest['compiler']['cxx_version']}; flags {' '.join(manifest['flags_normalized'])} {std}")
EOF
done

# ------------------------------------------------------------------ identical flags
python3 - "$WORK" "$ALLOW_MISMATCH" "${NAMES[@]}" <<'EOF'
import json, sys
from pathlib import Path
work, allow, names = Path(sys.argv[1]), sys.argv[2] == "1", sys.argv[3:]
builds = {}
for d in sorted((work / "build").glob("*/bench-build.json")):
    m = json.loads(d.read_text())
    builds[m["name"]] = m
flags = {n: m["flags_normalized"] for n, m in builds.items()}
compilers = {n: m["compiler"]["cxx_version"] for n, m in builds.items()}
same_flags = len({tuple(v) for v in flags.values()}) == 1
same_compiler = len(set(compilers.values())) == 1
result = {"identical_flags": same_flags, "identical_compiler": same_compiler, "flags": flags,
          "compilers": compilers, "std": {n: m["cxx_std"] for n, m in builds.items()},
          "note": "Only -std may differ: upstream's CMakeLists sets CMAKE_CXX_STANDARD 11, NEW requires 23."}
(work / "build" / "flags-check.json").write_text(json.dumps(result, indent=2) + "\n")
print(f"flags identical across {sorted(builds)}: {same_flags}; compiler identical: {same_compiler}")
if not (same_flags and same_compiler):
    for n in builds:
        print(f"  {n}: {compilers[n]} | {' '.join(flags[n])} {builds[n]['cxx_std']}")
    if not allow:
        sys.exit("ERROR: builds differ in compiler or flags (see flags-check.json; --allow-flag-mismatch to continue)")
EOF
echo "builds ready in $WORK/build"
