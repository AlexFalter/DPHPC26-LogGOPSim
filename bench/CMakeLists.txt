# Benchmarks (Google Benchmark) and the traces they read. See bench/README.md.

find_package(benchmark 1.9 QUIET)
if(NOT benchmark_FOUND)
  include(FetchContent)
  FetchContent_Declare(googlebenchmark
    URL https://github.com/google/benchmark/archive/refs/tags/v1.9.1.tar.gz
    URL_HASH SHA256=32131c08ee31eeff2c8968d7e874f3cb648034377dfc32a4c377fa8796d84981
    SYSTEM)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googlebenchmark)
endif()

# Benchmark traces, generated with schedgen, optionally rewritten by traces/vary_fields.py and
# converted to both formats: <build>/bench/traces/<name>.{legacy,sd}.bin. See bench/README.md for
# what each one shows.
find_program(BENCH_PYTHON NAMES python3 REQUIRED)
set(BENCH_TRACE_DIR "${CMAKE_CURRENT_BINARY_DIR}/traces")
set(BENCH_TRACE_FILES)
# add_bench_trace(name pattern ranks [VARY wide|atlahs])
function(add_bench_trace name pattern ranks)
  cmake_parse_arguments(TRACE "" "VARY" "" ${ARGN})
  set(fields "${PROJECT_SOURCE_DIR}/../tests/data/empty_custom_fields.toml")
  set(goal "${BENCH_TRACE_DIR}/${name}.goal")
  set(generated "${goal}")
  set(vary_command)
  if(TRACE_VARY)
    set(generated "${BENCH_TRACE_DIR}/${name}.schedgen.goal")
    set(vary_command COMMAND ${BENCH_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/traces/vary_fields.py ${TRACE_VARY}
                             ${generated} ${goal}
                     COMMAND ${CMAKE_COMMAND} -E remove ${generated})
    if(TRACE_VARY STREQUAL "atlahs")
      set(fields "${PROJECT_SOURCE_DIR}/../tests/data/atlahs_custom_fields.toml")
    endif()
  endif()
  set(legacy "${BENCH_TRACE_DIR}/${name}.legacy.bin")
  set(packed "${BENCH_TRACE_DIR}/${name}.sd.bin")
  set(outputs ${packed})
  set(legacy_command)
  if(NOT TRACE_VARY STREQUAL "atlahs") # the legacy format has no custom fields
    list(APPEND outputs ${legacy})
    set(legacy_command COMMAND $<TARGET_FILE:txt2bin> -i ${goal} -o ${legacy} --format legacy --custom-fields ${fields})
  endif()
  add_custom_command(OUTPUT ${outputs}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BENCH_TRACE_DIR}
    COMMAND $<TARGET_FILE:schedgen> --ptrn ${pattern} --commsize ${ranks} -o ${generated}
    ${vary_command}
    ${legacy_command}
    COMMAND $<TARGET_FILE:txt2bin> -i ${goal} -o ${packed} --format self-describing --custom-fields ${fields}
    COMMAND ${CMAKE_COMMAND} -E remove ${goal}
    DEPENDS schedgen txt2bin ${CMAKE_CURRENT_SOURCE_DIR}/traces/vary_fields.py
    COMMENT "Generating benchmark trace ${name}"
    VERBATIM)
  set(BENCH_TRACE_FILES ${BENCH_TRACE_FILES} ${outputs} PARENT_SCOPE)
endfunction()
add_bench_trace(bcast_8 binomialtreebcast 8)
add_bench_trace(bcast_1k binomialtreebcast 1024)
add_bench_trace(bcast_64k binomialtreebcast 65536)
add_bench_trace(dissem_16k dissemination 16384)
add_bench_trace(ring_256 allreduce_ring 256)
add_bench_trace(alltoall_256 linear_alltoall 256)
add_bench_trace(wide_64k binomialtreebcast 65536 VARY wide)
add_bench_trace(atlahs_64k binomialtreebcast 65536 VARY atlahs)
add_custom_target(bench_traces DEPENDS ${BENCH_TRACE_FILES})

add_executable(decode_bench decoding/decode_bench.cpp)
target_link_libraries(decode_bench PRIVATE trace_schema benchmark::benchmark)
target_compile_definitions(decode_bench PRIVATE BENCH_TRACE_DIR="${BENCH_TRACE_DIR}")

# The original LogGOPSim, the last upstream commit before this project, as the end-to-end
# baseline. It is exported from the git history and built with its own CMake files and build
# type (RelWithDebInfo, as ours) into <build>/bench/original/build/{LogGOPSim,txt2bin}. Not part
# of all; build it with the loggopsim_original target. See bench/README.md.
set(LOGGOPSIM_ORIGINAL_COMMIT 76451de16546c7ea39651c183cbc532f4a3457c0 CACHE STRING
    "Upstream LogGOPSim commit used as the end-to-end baseline")
find_package(Git QUIET)
if(GIT_FOUND)
  set(ORIGINAL_DIR "${CMAKE_CURRENT_BINARY_DIR}/original")
  add_custom_command(OUTPUT ${ORIGINAL_DIR}/build/LogGOPSim ${ORIGINAL_DIR}/build/txt2bin
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${ORIGINAL_DIR}/source
    COMMAND ${CMAKE_COMMAND} -E make_directory ${ORIGINAL_DIR}/source
    COMMAND ${GIT_EXECUTABLE} -C ${PROJECT_SOURCE_DIR}/.. archive --output=${ORIGINAL_DIR}/source.tar
            ${LOGGOPSIM_ORIGINAL_COMMIT}
    COMMAND ${CMAKE_COMMAND} -E chdir ${ORIGINAL_DIR}/source ${CMAKE_COMMAND} -E tar xf ${ORIGINAL_DIR}/source.tar
    COMMAND ${CMAKE_COMMAND} -S ${ORIGINAL_DIR}/source/src -B ${ORIGINAL_DIR}/build
            -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER} -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
    COMMAND ${CMAKE_COMMAND} --build ${ORIGINAL_DIR}/build --target LogGOPSim txt2bin
    COMMENT "Building the original LogGOPSim (${LOGGOPSIM_ORIGINAL_COMMIT})"
    VERBATIM)
  add_custom_target(loggopsim_original DEPENDS ${ORIGINAL_DIR}/build/LogGOPSim ${ORIGINAL_DIR}/build/txt2bin)
endif()
