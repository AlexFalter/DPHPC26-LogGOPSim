#include "binary_reading/decoding/runners/templated_schema/TemplatedSchemaRunner.hpp"
#include "binary_reading/decoding/runners/templated_schema/KnownSchemas.hpp"

namespace traceschema {

std::unique_ptr<const PlanRunner> create_templated_schema_runner(const DecodePlan& plan) {
    return create_first_matching<KnownTemplatedSchemaRunners>(plan);
}

} // namespace traceschema
