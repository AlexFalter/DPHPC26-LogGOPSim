# CompiledRunner runs a compiled plan; each subfolder is a compiler and a runner of its own.
target_sources(trace_schema PRIVATE CompiledRunner.cpp)
add_subdirectory(c)
add_subdirectory(orc)
