#!/usr/bin/env bash
# Creates a self-contained build toolchain WITHOUT root, for machines that lack gengetopt, re2c,
# gfortran or the Graphviz headers (the ETH student cluster, or WSL without sudo).
#
# usage: bash bench/env/setup_env.sh [--prefix DIR] [--with-llvm]
#   then: source bench/env/activate.sh
#
# Downloads the static micromamba binary from https://micro.mamba.pm and these conda-forge packages
# into DIR/<arch> (default DIR: $BENCH_ENV or ~/.lgs-bench-env; about 1.5 GB, 3 GB with LLVM):
#   gcc/gxx/gfortran 13, cmake, ninja, gengetopt, re2c, graphviz, git, python 3.12 with numpy,
#   matplotlib and pyyaml; with --with-llvm also llvmdev 18 (enables the orc runner).
# The architecture is that of the machine running this script: for the GB10 nodes (aarch64) run it
# inside a job on such a node, e.g. srun -A <tag> --gpus gb10:1 -t 60 bash bench/env/setup_env.sh
set -euo pipefail
PREFIX="${BENCH_ENV:-$HOME/.lgs-bench-env}"
LLVM=0
while [ $# -gt 0 ]; do
  case "$1" in
    --prefix) PREFIX="$2"; shift 2 ;;
    --with-llvm) LLVM=1; shift ;;
    *) echo "unknown argument $1" >&2; exit 2 ;;
  esac
done
ARCH="$(uname -m)"
case "$ARCH" in x86_64) PLATFORM=linux-64 ;; aarch64) PLATFORM=linux-aarch64 ;; *) echo "unsupported $ARCH" >&2; exit 1 ;; esac
mkdir -p "$PREFIX/bin"
MAMBA="$PREFIX/bin/micromamba-$ARCH"
if [ ! -x "$MAMBA" ]; then
  echo "downloading micromamba for $PLATFORM"
  curl -fsSL "https://micro.mamba.pm/api/micromamba/$PLATFORM/latest" | tar -xj -C "$PREFIX" bin/micromamba
  mv "$PREFIX/bin/micromamba" "$MAMBA"
fi
export MAMBA_ROOT_PREFIX="$PREFIX/mamba-root"
PKGS=(gcc=13 gxx=13 gfortran=13 "cmake>=3.20" ninja gengetopt re2c graphviz git python=3.12 numpy matplotlib pyyaml)
[ "$LLVM" = 1 ] && PKGS+=(llvmdev=18 zstd zlib)
"$MAMBA" create -y -p "$PREFIX/$ARCH" -c conda-forge --override-channels "${PKGS[@]}"
echo "toolchain ready in $PREFIX/$ARCH; activate with: source bench/env/activate.sh"
