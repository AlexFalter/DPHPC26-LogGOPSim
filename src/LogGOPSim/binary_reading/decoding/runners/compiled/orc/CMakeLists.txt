target_sources(trace_schema PRIVATE OrcLoader.cpp)

# Optional in-process compilation of the decode plan (LOGGOPSIM_DECODER=orc); without LLVM,
# orc falls back to the interpreted runner.
option(LOGGOPSIM_WITH_LLVM "Build the LLVM ORC plan compiler if LLVM is found" ON)
if(LOGGOPSIM_WITH_LLVM)
  # LLVMConfig appends to CMAKE_MODULE_PATH, which the includes below use as a single directory.
  set(LOGGOPSIM_MODULE_PATH "${CMAKE_MODULE_PATH}")
  find_package(LLVM CONFIG QUIET)
  set(CMAKE_MODULE_PATH "${LOGGOPSIM_MODULE_PATH}")
endif()
if(LLVM_FOUND)
  message(STATUS "LLVM ${LLVM_PACKAGE_VERSION}: ORC plan compiler enabled")
  if(LLVM_LINK_LLVM_DYLIB)
    set(LOGGOPSIM_LLVM_LIBRARIES LLVM)
  else()
    llvm_map_components_to_libnames(LOGGOPSIM_LLVM_LIBRARIES orcjit native passes)
  endif()
  # A plugin opened on first use, so that runs without orc do not pay LLVM's load time.
  add_library(loggopsim_orc MODULE OrcCompiler.cpp)
  target_include_directories(loggopsim_orc PRIVATE ${PROJECT_SOURCE_DIR}/LogGOPSim)
  target_include_directories(loggopsim_orc SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
  target_link_directories(loggopsim_orc PRIVATE ${LLVM_LIBRARY_DIRS})
  target_link_libraries(loggopsim_orc PRIVATE ${LOGGOPSIM_LLVM_LIBRARIES})
  add_dependencies(trace_schema loggopsim_orc)
  target_compile_definitions(trace_schema PUBLIC LOGGOPSIM_HAVE_LLVM
    PRIVATE LOGGOPSIM_ORC_LIBRARY="$<TARGET_FILE:loggopsim_orc>")
endif()
