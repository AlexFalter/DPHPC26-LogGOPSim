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
  LogGOPSim/binary_reading/TraceFileView.cpp
  LogGOPSim/binary_reading/TraceReader.cpp
  LogGOPSim/binary_reading/NodeDecoder.cpp
  LogGOPSim/simulators/loggopsim/LogGOPSimAdapter.cpp
  LogGOPSim/simulators/atlahs/AtlahsAdapter.cpp)
target_include_directories(trace_schema PUBLIC ${PROJECT_SOURCE_DIR}/LogGOPSim)
target_link_libraries(trace_schema PRIVATE tomlplusplus::tomlplusplus)

# Optional integration with ATLAHS's existing packet simulator and GOAL scheduler.
option(BUILD_ATLAHS "Build and test the optional ATLAHS integration" OFF)
set(ATLAHS_SOURCE_DIR "${PROJECT_SOURCE_DIR}/LogGOPSim/simulators/atlahs/upstream"
  CACHE PATH "ATLAHS fork containing the shared-reader and explicit-GPU source edits")
if(BUILD_ATLAHS)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  set(ATLAHS_GENERATOR_DIR "${ATLAHS_SOURCE_DIR}/goal_gen/ai/nccl_goal_generator")
  if(NOT EXISTS "${ATLAHS_GENERATOR_DIR}/get_traced_events.py")
    message(FATAL_ERROR "ATLAHS source is missing: initialize simulators/atlahs/upstream or set ATLAHS_SOURCE_DIR")
  endif()
  file(STRINGS "${PROJECT_SOURCE_DIR}/LogGOPSim/simulators/atlahs/UPSTREAM_REVISION"
    ATLAHS_UPSTREAM_REVISION LIMIT_COUNT 1)
  add_custom_target(atlahs_generator
    COMMAND ${CMAKE_COMMAND} -E echo "ATLAHS generator source: ${ATLAHS_GENERATOR_DIR}/get_traced_events.py (--explicit-fields)"
    USES_TERMINAL VERBATIM)
  add_custom_target(atlahs_htsim
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

include(CTest)
if(BUILD_TESTING)
  # Legacy fixtures have no custom attributes; use an empty declaration file.
  # Tests pass their custom-field declarations explicitly, independent of config/.
  set(TEST_PLAIN_FIELDS "${PROJECT_SOURCE_DIR}/../tests/empty_custom_fields.toml")
  set(TEST_CUSTOM_FIELDS "${PROJECT_SOURCE_DIR}/../tests/converter_custom_fields.toml")
  set(TEST_ATLAHS_FIELDS "${PROJECT_SOURCE_DIR}/../tests/atlahs_custom_fields.toml")
  add_executable(test_custom_fields ../tests/custom_fields.cpp)
  target_link_libraries(test_custom_fields PRIVATE trace_schema)
  add_test(NAME custom_fields COMMAND test_custom_fields)
  add_executable(test_record_layout ../tests/record_layout.cpp)
  target_link_libraries(test_record_layout PRIVATE trace_schema)
  add_test(NAME record_layout COMMAND test_record_layout)
  add_executable(test_rank_packing ../tests/rank_packing.cpp)
  target_link_libraries(test_rank_packing PRIVATE trace_schema)
  add_test(NAME rank_packing COMMAND test_rank_packing)
  add_executable(test_rank_table ../tests/rank_table.cpp)
  target_link_libraries(test_rank_table PRIVATE trace_schema)
  add_test(NAME rank_table COMMAND test_rank_table)
  add_executable(test_trace_writer ../tests/trace_writer.cpp)
  target_link_libraries(test_trace_writer PRIVATE trace_schema)
  add_test(NAME trace_writer COMMAND test_trace_writer ${CMAKE_CURRENT_BINARY_DIR}/trace_writer_test.bin)
  add_executable(test_trace_reader ../tests/trace_reader.cpp)
  target_link_libraries(test_trace_reader PRIVATE trace_schema)
  add_test(NAME trace_reader COMMAND test_trace_reader)
  add_executable(test_node_field_values ../tests/node_field_values.cpp)
  target_link_libraries(test_node_field_values PRIVATE trace_schema)
  add_test(NAME node_field_values COMMAND test_node_field_values)
  add_executable(test_record_packing ../tests/record_packing.cpp)
  target_link_libraries(test_record_packing PRIVATE trace_schema)
  add_test(NAME record_packing COMMAND test_record_packing)
  add_executable(test_binary_header ../tests/binary_header.cpp)
  target_link_libraries(test_binary_header PRIVATE trace_schema)
  add_test(NAME binary_header COMMAND test_binary_header)
  add_executable(test_binary_io ../tests/binary_io.cpp)
  target_link_libraries(test_binary_io PRIVATE trace_schema)
  add_test(NAME binary_io COMMAND test_binary_io)
  add_executable(test_goal_reader ../tests/goal_reader.cpp)
  target_link_libraries(test_goal_reader PRIVATE goal_reader)
  add_test(NAME goal_reader COMMAND test_goal_reader ${CMAKE_CURRENT_BINARY_DIR}/goal_reader_test.goal)
  set_tests_properties(custom_fields record_layout rank_packing rank_table trace_writer trace_reader
    node_field_values record_packing binary_header binary_io goal_reader PROPERTIES LABELS unit)
  if(BUILD_ATLAHS)
    add_executable(test_atlahs_matching ../tests/atlahs_matching.cpp)
    target_include_directories(test_atlahs_matching PRIVATE ${ATLAHS_SOURCE_DIR}/sim/htsim-backend/sim)
    add_test(NAME atlahs_matching COMMAND test_atlahs_matching)
    set_tests_properties(atlahs_matching PROPERTIES LABELS unit)
  endif()
endif()
find_program(REFERENCE_PYTHON_EXECUTABLE NAMES python3)
if(BUILD_TESTING AND REFERENCE_PYTHON_EXECUTABLE)
  if(BUILD_ATLAHS)
    set(ATLAHS_GENERATOR_PYTHON "${REFERENCE_PYTHON_EXECUTABLE}" CACHE FILEPATH
      "Python interpreter with ATLAHS generator dependencies (NumPy, SciPy, PyYAML, tqdm)")
    add_test(NAME atlahs_generator
      COMMAND ${ATLAHS_GENERATOR_PYTHON} ${PROJECT_SOURCE_DIR}/../tests/check_atlahs_generator.py
        --generator ${ATLAHS_GENERATOR_DIR}
        --upstream ${ATLAHS_SOURCE_DIR}
        --upstream-revision ${ATLAHS_UPSTREAM_REVISION}
        --converter $<TARGET_FILE:txt2bin>
        --plain-fields ${TEST_PLAIN_FIELDS} --atlahs-fields ${TEST_ATLAHS_FIELDS}
        --simulator ${CMAKE_CURRENT_BINARY_DIR}/atlahs-htsim/datacenter/htsim_ndp
        --topology ${CMAKE_CURRENT_BINARY_DIR}/atlahs-htsim/datacenter/topologies/leaf_spine_tiny.topo)
    add_test(NAME atlahs_simulation
      COMMAND ${REFERENCE_PYTHON_EXECUTABLE} ${PROJECT_SOURCE_DIR}/../tests/check_atlahs_simulation.py
        --converter $<TARGET_FILE:txt2bin>
        --plain-fields ${TEST_PLAIN_FIELDS} --atlahs-fields ${TEST_ATLAHS_FIELDS}
        --simulator ${CMAKE_CURRENT_BINARY_DIR}/atlahs-htsim/datacenter/htsim_ndp
        --topology ${CMAKE_CURRENT_BINARY_DIR}/atlahs-htsim/datacenter/topologies/leaf_spine_tiny.topo)
    add_test(NAME atlahs_upstream_ndp
      COMMAND ${REFERENCE_PYTHON_EXECUTABLE} ${PROJECT_SOURCE_DIR}/../tests/check_atlahs_upstream.py
        --tests-dir ${CMAKE_CURRENT_BINARY_DIR}/atlahs-htsim/tests)
    set_tests_properties(atlahs_generator atlahs_simulation atlahs_upstream_ndp PROPERTIES LABELS integration TIMEOUT 60)
    set(ATLAHS_RECORDED_TRACE_DIR "" CACHE PATH "Nsight SQLite exports for the optional recorded-trace comparison")
    if(ATLAHS_RECORDED_TRACE_DIR)
      add_test(NAME atlahs_recorded
        COMMAND ${ATLAHS_GENERATOR_PYTHON} ${PROJECT_SOURCE_DIR}/../tests/check_atlahs_recorded.py
          --traces ${ATLAHS_RECORDED_TRACE_DIR}
          --generator ${ATLAHS_GENERATOR_DIR}
          --reference ${CMAKE_CURRENT_BINARY_DIR}/atlahs-reference
          --converter $<TARGET_FILE:txt2bin> --atlahs-fields ${TEST_ATLAHS_FIELDS}
          --simulator ${CMAKE_CURRENT_BINARY_DIR}/atlahs-htsim/datacenter/htsim_ndp
          --topology ${CMAKE_CURRENT_BINARY_DIR}/atlahs-htsim/datacenter/topologies/leaf_spine_16_1os.topo
          --nodes 16
          --output ${CMAKE_CURRENT_BINARY_DIR}/atlahs-recorded-run)
      set_tests_properties(atlahs_recorded PROPERTIES LABELS "integration;recorded" TIMEOUT 7200)
    endif()
  endif()
  add_test(NAME simulation_equivalence
    COMMAND ${REFERENCE_PYTHON_EXECUTABLE} ${PROJECT_SOURCE_DIR}/../tests/check_simulation_equivalence.py
      --converter $<TARGET_FILE:txt2bin>
      --plain-fields ${TEST_PLAIN_FIELDS} --custom-fields ${TEST_CUSTOM_FIELDS}
      --simulator $<TARGET_FILE:LogGOPSim>)
  add_test(NAME converter_self_describing
    COMMAND ${REFERENCE_PYTHON_EXECUTABLE} ${PROJECT_SOURCE_DIR}/../tests/check_converter_self_describing.py
      --converter $<TARGET_FILE:txt2bin>
      --plain-fields ${TEST_PLAIN_FIELDS} --custom-fields ${TEST_CUSTOM_FIELDS}
      --reference ${PROJECT_SOURCE_DIR}/../tests/reference/binomialtreebcast_8/schedule.goal)
  add_test(NAME converter_schema_report
    COMMAND ${REFERENCE_PYTHON_EXECUTABLE} ${PROJECT_SOURCE_DIR}/../tests/check_converter_schema_report.py
      --converter $<TARGET_FILE:txt2bin> --plain-fields ${TEST_PLAIN_FIELDS})
  add_test(NAME reference_binomialtreebcast
    COMMAND ${REFERENCE_PYTHON_EXECUTABLE} ${PROJECT_SOURCE_DIR}/../tests/check_reference_times.py
      --simulator $<TARGET_FILE:LogGOPSim> --converter $<TARGET_FILE:txt2bin>
      --plain-fields ${TEST_PLAIN_FIELDS})
  set_tests_properties(simulation_equivalence converter_self_describing converter_schema_report
    reference_binomialtreebcast PROPERTIES LABELS integration)
endif()
# test the c++ toolchain for some pattern
add_test(NAME schedgen_binomialtreebcast COMMAND schedgen -o schedule.goal --commsize 8 --ptrn binomialtreebcast)
add_test(NAME txt2bin_binomialtreebcast COMMAND txt2bin --custom-fields ${TEST_PLAIN_FIELDS} -i schedule.goal -o schedule.bin DEPENDS schedgen_binomialtreebcast)
add_test(NAME loggopsim_binomialtreebcast COMMAND LogGOPSim -f schedule.bin DEPENDS txt2bin_binomialtreebcast)

# test the python toolchain for some pattern
add_test(NAME schedgen2_bcast COMMAND ${PYTHON_EXECUTABLE} schedgen.py bcast --output schedule.goal WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/Schedgen2)
add_test(NAME txt2bin_schedgen2_bcast COMMAND txt2bin --custom-fields ${TEST_PLAIN_FIELDS} -i ${PROJECT_SOURCE_DIR}/Schedgen2/schedule.goal -o schedule.bin)
add_test(NAME loggopsim_schedgen2_bcast COMMAND LogGOPSim -f schedule.bin)

# test liballprof

# test liballprof2
#c wrapper test
add_executable(test_lap2_c ${PROJECT_SOURCE_DIR}/../tests/mpi_helloworld.c)
target_link_libraries(test_lap2_c mpipclog)
add_test(NAME trace_lap2_c COMMAND ${MPIEXEC_EXECUTABLE} --host localhost:4 ${MPIEXEC_NUMPROC_FLAG} 4 $<TARGET_FILE:test_lap2_c>)
add_test(NAME lap2_c_trace_exists  COMMAND ${CMAKE_COMMAND} -E cat lap2-trace-rank-1-of-4.txt)

# fortran wrapper test
add_executable(test_lap2_f ${PROJECT_SOURCE_DIR}/../tests/mpi_helloworld.f90)
target_link_libraries(test_lap2_f mpipflog)
add_test(NAME trace_lap2_f COMMAND ${MPIEXEC_EXECUTABLE} --host localhost:4 ${MPIEXEC_NUMPROC_FLAG} 4 $<TARGET_FILE:test_lap2_f>)
add_test(NAME lap2_f_trace_exists  COMMAND ${CMAKE_COMMAND} -E cat lap2-trace-rank-1-of-4.txt )

