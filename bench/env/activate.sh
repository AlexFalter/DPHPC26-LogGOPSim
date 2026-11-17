# source bench/env/activate.sh — puts the toolchain created by bench/env/setup_env.sh first on PATH
# and points the build at it (compilers, Graphviz, LLVM). Harmless if the toolchain does not exist.
_lgs_env="${BENCH_ENV:-$HOME/.lgs-bench-env}/$(uname -m)"
if [ -d "$_lgs_env/bin" ]; then
  export PATH="$_lgs_env/bin:$PATH"
  export CC="$_lgs_env/bin/gcc" CXX="$_lgs_env/bin/g++" FC="$_lgs_env/bin/gfortran"
  export CMAKE_PREFIX_PATH="$_lgs_env${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
  export GRAPHVIZ_ROOT="$_lgs_env"
  export CPATH="$_lgs_env/include${CPATH:+:$CPATH}"
  export LIBRARY_PATH="$_lgs_env/lib${LIBRARY_PATH:+:$LIBRARY_PATH}"
  export LD_LIBRARY_PATH="$_lgs_env/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  if [ -d "$_lgs_env/lib/cmake/llvm" ]; then export LLVM_DIR="$_lgs_env/lib/cmake/llvm"; fi
  echo "lgs-bench toolchain: $_lgs_env ($("$CXX" --version | head -1))"
else
  echo "lgs-bench toolchain not found at $_lgs_env (using the system toolchain)" >&2
fi
unset _lgs_env
