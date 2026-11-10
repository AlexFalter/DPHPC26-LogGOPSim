#include "binary_reading/decoding/runners/RunnerRegistry.hpp"
#include "binary_reading/decoding/runners/compiled/CompiledRunner.hpp"
#include "binary_reading/decoding/runners/compiled/c/CCompiler.hpp"
#include "binary_reading/decoding/runners/compiled/orc/OrcLoader.hpp"
#include "binary_reading/decoding/runners/interpreted/InterpretedRunner.hpp"
#include "binary_reading/decoding/runners/templated_node/TemplatedNodeRunner.hpp"
#include "binary_reading/decoding/runners/templated_schema/TemplatedSchemaRunner.hpp"

#include <algorithm>
#include <cassert>

namespace traceschema {

namespace {

bool always() { return true; }

const RunnerInfo runners[] = {
    {
        .name = "plan",
        .description = "interprets the decode plan's steps in a loop",
        .handles_any_plan = true,
        .is_available = always,
        .create = [](const DecodePlan& plan) -> std::unique_ptr<const PlanRunner> {
            return std::make_unique<InterpretedRunner>(plan);
        },
    },
    {
        .name = "jit",
        .description = "compiles the plan with the system C compiler and caches the library",
        .handles_any_plan = true,
        .is_available = has_c_compiler,
        .create = [](const DecodePlan& plan) -> std::unique_ptr<const PlanRunner> {
            return CompiledRunner::create(plan, compile_with_c(plan), "jit");
        },
    },
    {
        .name = "orc",
        .description = "compiles the plan in-process with LLVM ORC",
        .handles_any_plan = true,
        .is_available = has_orc_compiler,
        .create = [](const DecodePlan& plan) -> std::unique_ptr<const PlanRunner> {
            return CompiledRunner::create(plan, compile_with_orc(plan), "orc");
        },
    },
    {
        .name = "templated_node",
        .description = "a template per simulator node type, with each member's size fixed",
        .handles_any_plan = false,
        .is_available = always,
        .create = create_templated_node_runner,
    },
    {
        .name = "templated_schema",
        .description = "a template per known trace schema, with every step constant",
        .handles_any_plan = false,
        .is_available = always,
        .create = create_templated_schema_runner,
    },
};

} // namespace

std::span<const RunnerInfo> get_runners() {
    return runners;
}

const RunnerInfo* find_runner(std::string_view name) {
    const auto found = std::ranges::find(runners, name, &RunnerInfo::name);
    return found == std::end(runners) ? nullptr : &*found;
}

const RunnerInfo& get_default_runner() {
    const auto* runner = find_runner("plan");
    assert(runner && "The default plan runner must be registered");
    return *runner;
}

} // namespace traceschema
