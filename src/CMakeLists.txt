cmake_minimum_required(VERSION 3.20)
project(LogGOPSim LANGUAGES C CXX Fortran)

set(CMAKE_BUILD_TYPE RelWithDebInfo)

# If several versions of a package provide CMake packages, try to use the latest one.
set(CMAKE_FIND_PACKAGE_SORT_ORDER NATURAL)
set(CMAKE_FIND_PACKAGE_SORT_DIRECTION DEC)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/../cmake" ${CMAKE_MODULE_PATH})

find_package(tomlplusplus 3.4.0 QUIET)
if(NOT tomlplusplus_FOUND)
  include(FetchContent)
  FetchContent_Declare(tomlplusplus
    URL https://codeload.github.com/marzer/tomlplusplus/tar.gz/refs/tags/v3.4.0
    URL_HASH SHA256=8517f65938a4faae9ccf8ebb36631a38c1cadfb5efa85d9a72e15b9e97d25155)
  FetchContent_MakeAvailable(tomlplusplus)
endif()

add_library(trace_schema STATIC
  LogGOPSim/goal_parsing/CustomFields.cpp
  LogGOPSim/trace_format/FieldIdentity.cpp
  LogGOPSim/trace_format/FieldTypes.cpp
  LogGOPSim/trace_format/OffsetEncoding.cpp
  LogGOPSim/trace_format/RecordLayout.cpp
  LogGOPSim/trace_format/BinaryFormat.cpp
  LogGOPSim/goal_parsing/TraceAnalysis.cpp
  LogGOPSim/binary_writing/RecordPacking.cpp
  LogGOPSim/binary_writing/RankPacking.cpp
  LogGOPSim/binary_writing/TraceWriter.cpp
  LogGOPSim/binary_writing/LegacyTraceWriter.cpp
  LogGOPSim/binary_reading/MappedTraceFile.cpp
  LogGOPSim/binary_reading/TraceFileView.cpp
  LogGOPSim/binary_reading/SelfDescribingTraceReader.cpp
  LogGOPSim/binary_reading/TraceReader.cpp
  LogGOPSim/binary_reading/DependencyGraph.cpp
  LogGOPSim/binary_reading/decoding/Decoder.cpp
  LogGOPSim/binary_reading/decoding/DecodePlan.cpp
  LogGOPSim/simulators/loggopsim/LogGOPSimAdapter.cpp
  LogGOPSim/simulators/atlahs/AtlahsAdapter.cpp)
target_include_directories(trace_schema PUBLIC ${PROJECT_SOURCE_DIR}/LogGOPSim)
target_link_libraries(trace_schema PRIVATE tomlplusplus::tomlplusplus ${CMAKE_DL_LIBS})

# The plan runners add their own sources to trace_schema.
add_subdirectory(LogGOPSim/binary_reading/decoding/runners ${CMAKE_CURRENT_BINARY_DIR}/runners)

# Optional integration with ATLAHS's existing packet simulator and GOAL scheduler.
option(BUILD_ATLAHS "Build and test the optional ATLAHS integration" OFF)
# The ATLAHS fork is a separate checkout, by default next to this repository.
get_filename_component(ATLAHS_DEFAULT_SOURCE_DIR "${PROJECT_SOURCE_DIR}/../../atlahs" ABSOLUTE)
set(ATLAHS_SOURCE_DIR "${ATLAHS_DEFAULT_SOURCE_DIR}"
  CACHE PATH "Checkout of the ATLAHS fork (branch packed-trace-integration)")
if(BUILD_ATLAHS)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  set(ATLAHS_GENERATOR_DIR "${ATLAHS_SOURCE_DIR}/goal_gen/ai/nccl_goal_generator")
  if(NOT EXISTS "${ATLAHS_GENERATOR_DIR}/get_traced_events.py")
    message(FATAL_ERROR "ATLAHS not found at ${ATLAHS_SOURCE_DIR}. Clone it with\n"
      "  git clone --branch packed-trace-integration https://github.com/mgman03/atlahs.git ${ATLAHS_DEFAULT_SOURCE_DIR}\n"
      "or set ATLAHS_SOURCE_DIR to an existing checkout.")
  endif()
  add_custom_target(atlahs_generator
    COMMAND ${CMAKE_COMMAND} -E echo "ATLAHS generator source: ${ATLAHS_GENERATOR_DIR}/get_traced_events.py (--explicit-fields)"
    USES_TERMINAL VERBATIM)
  add_custom_target(atlahs_htsim ALL
    COMMAND ${Python3_EXECUTABLE}
      ${PROJECT_SOURCE_DIR}/LogGOPSim/simulators/atlahs/integration/build.py
      --source ${ATLAHS_SOURCE_DIR}
      --output ${CMAKE_CURRENT_BINARY_DIR}/atlahs-htsim
      --include ${PROJECT_SOURCE_DIR}/LogGOPSim
      --library $<TARGET_FILE:trace_schema>
      --compiler ${CMAKE_CXX_COMPILER}
      --sysroot "${CMAKE_OSX_SYSROOT}"
    DEPENDS trace_schema
    USES_TERMINAL VERBATIM)
  add_custom_target(atlahs_reference
    COMMAND ${Python3_EXECUTABLE}
      ${PROJECT_SOURCE_DIR}/LogGOPSim/simulators/atlahs/integration/prepare_reference.py
      --upstream ${ATLAHS_SOURCE_DIR}
      --output ${CMAKE_CURRENT_BINARY_DIR}/atlahs-reference
      --compiler ${CMAKE_CXX_COMPILER}
      --sysroot "${CMAKE_OSX_SYSROOT}"
    USES_TERMINAL VERBATIM)
endif()

include(${CMAKE_MODULE_PATH}/gengetopt.cmake)
include(${CMAKE_MODULE_PATH}/re2c.cmake)
find_gengetopt()
find_re2c()

# build loggopsim
find_package(Graphviz REQUIRED)
include_directories(${GRAPHVIZ_INCLUDE_DIRS})
add_gengetopt_files(LogGOPSim/simulators/loggopsim/loggopsim_cmdline)
add_executable(LogGOPSim
  LogGOPSim/simulators/loggopsim/loggopsim_cmdline.c
  LogGOPSim/simulators/loggopsim/LogGOPSim.cpp)
target_link_libraries(LogGOPSim ${GRAPHVIZ_CGRAPH_LIBRARY} trace_schema)

# build txt2bin
add_gengetopt_files(LogGOPSim/txt2bin_cmdline)
add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/GoalReader.cpp
  COMMAND ${RE2C_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/LogGOPSim/goal_parsing/GoalReader.re
    -o ${CMAKE_CURRENT_BINARY_DIR}/GoalReader.cpp
  DEPENDS LogGOPSim/goal_parsing/GoalReader.re
  COMMENT "Generating GOAL reader"
  VERBATIM)
add_library(goal_reader STATIC ${CMAKE_CURRENT_BINARY_DIR}/GoalReader.cpp)
target_include_directories(goal_reader PUBLIC ${PROJECT_SOURCE_DIR}/LogGOPSim)
target_link_libraries(goal_reader PUBLIC trace_schema)
add_executable(txt2bin LogGOPSim/txt2bin.cpp LogGOPSim/txt2bin_cmdline.c)
target_link_libraries(txt2bin PRIVATE goal_reader)
target_compile_definitions(txt2bin PRIVATE
  DEFAULT_CUSTOM_FIELDS_CONFIG_PATH="${PROJECT_SOURCE_DIR}/../config/custom_fields.toml")

# build Schedgen1 (Schedgen2 is pure python)
add_gengetopt_files(Schedgen/schedgen_cmdline)
add_executable(schedgen Schedgen/buffer_element.cpp Schedgen/schedgen.cpp Schedgen/process_trace.cpp Schedgen/schedgen_cmdline.c)

# build liballprof1
find_package(MPI)
include(FortranCInterface)
FortranCInterface_VERIFY()
FortranCInterface_HEADER(fc_mangle.h)
include_directories(SYSTEM ${MPI_INCLUDE_PATH})
add_custom_command(OUTPUT lap1_mpi_c_wrapper.c COMMAND ${PROJECT_SOURCE_DIR}/liballprof/gencode.py c   > lap1_mpi_c_wrapper.c  DEPENDS ${PROJECT_SOURCE_DIR}/liballprof/mpi_header.h)
add_custom_command(OUTPUT lap1_mpi_f_wrapper.c COMMAND ${PROJECT_SOURCE_DIR}/liballprof/gencode.py f77 > lap1_mpi_f_wrapper.c  DEPENDS ${PROJECT_SOURCE_DIR}/liballprof/mpi_header.h)
#include_directories(liballprof)
#add_library(mpipclog1 lap1_mpi_c_wrapper.c liballprof/sync.c)
#add_library(mpipflog1 lap1_mpi_f_wrapper.c liballprof/sync.c)


# build liballprof2
find_package(Clang)
find_package(Unwind)
add_custom_target(mpi_header_avail         DEPENDS ${PROJECT_SOURCE_DIR}/liballprof2/mpi.h)
add_custom_target(mpi_semantics_avail      DEPENDS mpi_sem.yml)
add_custom_target(mpi_wrapper_generated    DEPENDS mpi_c_wrapper.c mpi_f_wrapper.c )
add_custom_command(OUTPUT mpi_c_wrapper.c  mpi_f_wrapper.c COMMAND ${PROJECT_SOURCE_DIR}/liballprof2/gencode.py -s mpi_sem.yml                                                           DEPENDS mpi_semantics_avail)
add_custom_command(OUTPUT mpi_sem.yml                      COMMAND ${PROJECT_SOURCE_DIR}/liballprof2/gensem.py -l ${CLANG_INSTALL_PREFIX}/lib -m ${PROJECT_SOURCE_DIR}/liballprof2/mpi.h DEPENDS mpi_header_avail)
add_library(mpipclog SHARED mpi_c_wrapper.c)
add_library(mpipflog SHARED mpi_f_wrapper.c)
add_dependencies(mpipclog mpi_wrapper_generated)
add_dependencies(mpipflog mpi_wrapper_generated)
target_link_libraries(mpipclog ${MPI_C_LIBRARIES})
target_link_libraries(mpipflog ${MPI_Fortran_LIBRARIES})
if (Unwind_FOUND) 
  target_link_libraries(mpipclog unwind::unwind)
  target_link_libraries(mpipflog unwind::unwind)
endif()

option(LOGGOPSIM_BUILD_BENCHMARKS "Build the benchmarks in bench/ (fetches Google Benchmark)" ON)
if(LOGGOPSIM_BUILD_BENCHMARKS)
  add_subdirectory(${PROJECT_SOURCE_DIR}/../bench ${CMAKE_CURRENT_BINARY_DIR}/bench)
endif()

include(CTest)
if(BUILD_TESTING)
  # Legacy fixtures have no custom attributes; the toolchain tests below convert them with an
  # empty declaration file, independent of config/.
  set(TEST_PLAIN_FIELDS "${PROJECT_SOURCE_DIR}/../tests/data/empty_custom_fields.toml")
  find_package(GTest 1.15 QUIET)
  if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(googletest
      URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
      URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
      SYSTEM)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
  endif()
  include(GoogleTest)
  # Unit tests (tests/unit), built into <build>/tests/unit.
  add_subdirectory(${PROJECT_SOURCE_DIR}/../tests/unit ${CMAKE_CURRENT_BINARY_DIR}/tests/unit)
endif()
# Integration tests (tests/integration): pytest runs the built programs, one ctest test per file,
# labelled integration. TEST_PYTHON needs pytest (tests/integration/requirements.txt) and, for the
# ATLAHS tests, the ATLAHS generator's packages (NumPy, SciPy, PyYAML, tqdm).
find_program(TEST_PYTHON_DEFAULT NAMES python3)
set(TEST_PYTHON "${TEST_PYTHON_DEFAULT}" CACHE FILEPATH "Python interpreter with pytest for the integration tests")
if(BUILD_TESTING AND TEST_PYTHON)
  execute_process(COMMAND ${TEST_PYTHON} -m pytest --version RESULT_VARIABLE TEST_PYTHON_LACKS_PYTEST
                  OUTPUT_QUIET ERROR_QUIET)
  if(TEST_PYTHON_LACKS_PYTEST)
    message(WARNING "${TEST_PYTHON} has no pytest, so the integration tests are not registered. Install "
                    "tests/integration/requirements.txt or set TEST_PYTHON to an interpreter that has it.")
  else()
    set(INTEGRATION_TEST_DIR "${PROJECT_SOURCE_DIR}/../tests/integration")
    function(add_integration_test name file)
      add_test(NAME ${name}
        COMMAND ${TEST_PYTHON} -m pytest ${INTEGRATION_TEST_DIR}/${file} -q
          --build-dir ${CMAKE_CURRENT_BINARY_DIR} --atlahs-dir ${ATLAHS_SOURCE_DIR} ${ARGN}
        WORKING_DIRECTORY ${INTEGRATION_TEST_DIR})
      set_tests_properties(${name} PROPERTIES LABELS integration ENVIRONMENT PYTHONDONTWRITEBYTECODE=1)
    endfunction()
    add_integration_test(simulation_equivalence test_simulation_equivalence.py)
    add_integration_test(converter_self_describing test_converter_self_describing.py)
    add_integration_test(converter_schema_report test_converter_schema_report.py)
    add_integration_test(reference_binomialtreebcast test_reference_times.py)
    add_integration_test(toolchain test_toolchain.py)
    if(LLVM_FOUND)
      add_integration_test(runner_simulation test_runner_simulation.py --require-orc)
    else()
      add_integration_test(runner_simulation test_runner_simulation.py)
    endif()
    if(MPIEXEC_EXECUTABLE)
      set(MPI_TEST_FLAGS --mpi-exec ${MPIEXEC_EXECUTABLE} --mpi-numproc-flag=${MPIEXEC_NUMPROC_FLAG})
      foreach(flag IN LISTS MPIEXEC_PREFLAGS)
        list(APPEND MPI_TEST_FLAGS --mpi-preflag=${flag})
      endforeach()
      foreach(flag IN LISTS MPIEXEC_POSTFLAGS)
        list(APPEND MPI_TEST_FLAGS --mpi-postflag=${flag})
      endforeach()
      add_integration_test(mpi_trace test_mpi_trace.py ${MPI_TEST_FLAGS})
      set_tests_properties(mpi_trace PROPERTIES PROCESSORS 4 LABELS "integration;mpi")
    endif()
    if(BUILD_ATLAHS)
      add_integration_test(atlahs_generator test_atlahs_generator.py --require-atlahs)
      add_integration_test(atlahs_simulation test_atlahs_simulation.py --require-atlahs)
      add_integration_test(atlahs_upstream_ndp test_atlahs_upstream.py --require-atlahs)
      set_tests_properties(atlahs_generator atlahs_simulation atlahs_upstream_ndp PROPERTIES TIMEOUT 60)
      set(ATLAHS_RECORDED_TRACE_DIR "" CACHE PATH "Nsight SQLite exports for the optional recorded-trace comparison")
      if(ATLAHS_RECORDED_TRACE_DIR)
        add_integration_test(atlahs_recorded test_atlahs_recorded.py --require-atlahs --recorded-traces ${ATLAHS_RECORDED_TRACE_DIR})
        set_tests_properties(atlahs_recorded PROPERTIES LABELS "integration;recorded" TIMEOUT 7200)
      endif()
    endif()
  endif()
endif()
# Toolchain and MPI tracing checks run as isolated pytest integration tests above.

# test liballprof

# test liballprof2
#c wrapper test
add_executable(test_lap2_c ${PROJECT_SOURCE_DIR}/../tests/liballprof/mpi_helloworld.c)
target_link_libraries(test_lap2_c mpipclog)

# fortran wrapper test
add_executable(test_lap2_f ${PROJECT_SOURCE_DIR}/../tests/liballprof/mpi_helloworld.f90)
target_link_libraries(test_lap2_f mpipflog)

