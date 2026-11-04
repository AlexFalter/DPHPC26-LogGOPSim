#ifndef LOGGOPSIM_CUSTOMFIELDS_HPP
#define LOGGOPSIM_CUSTOMFIELDS_HPP

#include "trace_format/RecordLayout.hpp"

#include <istream>
#include <span>

namespace traceschema {

void validate_custom_fields(std::span<const FieldValueInfo> fields);

// Load TOML declarations in array order. Ranges come from operation values.
[[nodiscard]] std::vector<FieldValueInfo> read_custom_fields(std::istream& input);

} // namespace traceschema
#endif
