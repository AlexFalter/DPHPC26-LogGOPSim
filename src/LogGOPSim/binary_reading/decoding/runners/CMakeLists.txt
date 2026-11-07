# The plan runners: one folder each, which adds its sources to trace_schema. RunnerRegistry.cpp
# lists them; see README.md for adding or removing one.
target_sources(trace_schema PRIVATE PlanRunner.cpp RunnerRegistry.cpp)
add_subdirectory(interpreted)
add_subdirectory(compiled)
add_subdirectory(templated_node)
add_subdirectory(templated_schema)
