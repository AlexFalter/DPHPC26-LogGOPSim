# Passed by bench/build.sh as -DCMAKE_PROJECT_LogGOPSim_INCLUDE=<this file>: CMake includes it at
# the end of the project(LogGOPSim ...) call and the deferred call below adds bench/micro's decode
# microbenchmark once all targets exist. It links trace_schema exactly like the simulator, with the
# same compiler, flags and definitions, without modifying the worktree's CMake files.
set(_lgs_bench_micro_dir "${CMAKE_CURRENT_LIST_DIR}")
function(lgs_bench_add_micro)
  if(TARGET trace_schema AND NOT TARGET bench_decode_micro)
    add_executable(bench_decode_micro "${_lgs_bench_micro_dir}/decode_bench.cpp")
    target_link_libraries(bench_decode_micro PRIVATE trace_schema)
  endif()
endfunction()
cmake_language(DEFER CALL lgs_bench_add_micro)
