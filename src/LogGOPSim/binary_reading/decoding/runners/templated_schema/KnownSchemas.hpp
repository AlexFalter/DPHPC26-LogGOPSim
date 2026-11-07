#ifndef LOGGOPSIM_KNOWNSCHEMAS_HPP
#define LOGGOPSIM_KNOWNSCHEMAS_HPP

#include "binary_reading/decoding/runners/templated_schema/TemplatedSchemaRunner.hpp"

#include <tuple>

namespace traceschema {

// The exact plans compiled into the program, one TemplatedSchemaRunner each: the plans of the
// benchmark traces (bench/CMakeLists.txt). A trace without one falls back to the interpreted
// runner.

using KnownTemplatedSchemaRunners = std::tuple<
    // bcast_64k: binomialtreebcast, 65536 ranks.
    TemplatedSchemaRunner<0, 1,
        DecodePlan::Step{FieldSource{0, 1, false, false, 3u, 1u, 2u}, FieldDestination{OutputStruct::Node, 56, 1, 0u}},
        DecodePlan::Step{FieldSource{0, 3, false, false, 65535u, 0u, 65535u}, FieldDestination{OutputStruct::Node, 60, 4, 4294967295u}},
        DecodePlan::Step{FieldSource{0, 0, false, false, 0u, 1u, 0u}, FieldDestination{OutputStruct::Node, 64, 8, 0u}},
        DecodePlan::Step{FieldSource{0, 0, false, false, 0u, 0u, 0u}, FieldDestination{OutputStruct::Node, 72, 4, 4294967295u}},
        DecodePlan::Step{FieldSource{0, 0, false, false, 0u, 0u, 0u}, FieldDestination{OutputStruct::Node, 76, 1, 0u}},
        DecodePlan::Step{FieldSource{0, 0, false, false, 0u, 0u, 0u}, FieldDestination{OutputStruct::Node, 77, 1, 0u}},
        DecodePlan::Step{FieldSource{0, 0, false, false, 1u, 0u, 1u}, FieldDestination{OutputStruct::Node, 4, 4, 0u}},
        DecodePlan::Step{FieldSource{2, 7, false, false, 15u, 0u, 15u}, FieldDestination{OutputStruct::DependencyLists, 0, 4, 0u}},
        DecodePlan::Step{FieldSource{2, 3, false, false, 15u, 0u, 15u}, FieldDestination{OutputStruct::DependencyLists, 4, 4, 0u}},
        DecodePlan::Step{FieldSource{3, 3, false, false, 15u, 0u, 15u}, FieldDestination{OutputStruct::DependencyLists, 8, 4, 0u}},
        DecodePlan::Step{FieldSource{0, 0, false, false, 0u, 0u, 0u}, FieldDestination{OutputStruct::DependencyLists, 12, 4, 0u}}>,
    // dissem_16k: dissemination, 16384 ranks.
    TemplatedSchemaRunner<0, 1,
        DecodePlan::Step{FieldSource{0, 1, false, false, 3u, 1u, 2u}, FieldDestination{OutputStruct::Node, 56, 1, 0u}},
        DecodePlan::Step{FieldSource{0, 3, false, false, 16383u, 0u, 16383u}, FieldDestination{OutputStruct::Node, 60, 4, 4294967295u}},
        DecodePlan::Step{FieldSource{0, 0, false, false, 0u, 1u, 0u}, FieldDestination{OutputStruct::Node, 64, 8, 0u}},
        DecodePlan::Step{FieldSource{0, 0, false, false, 0u, 0u, 0u}, FieldDestination{OutputStruct::Node, 72, 4, 4294967295u}},
        DecodePlan::Step{FieldSource{0, 0, false, false, 0u, 0u, 0u}, FieldDestination{OutputStruct::Node, 76, 1, 0u}},
        DecodePlan::Step{FieldSource{0, 0, false, false, 0u, 0u, 0u}, FieldDestination{OutputStruct::Node, 77, 1, 0u}},
        DecodePlan::Step{FieldSource{0, 0, false, false, 1u, 0u, 1u}, FieldDestination{OutputStruct::Node, 4, 4, 0u}},
        DecodePlan::Step{FieldSource{2, 2, false, false, 15u, 0u, 13u}, FieldDestination{OutputStruct::DependencyLists, 0, 4, 0u}},
        DecodePlan::Step{FieldSource{2, 1, false, false, 1u, 0u, 1u}, FieldDestination{OutputStruct::DependencyLists, 4, 4, 0u}},
        DecodePlan::Step{FieldSource{2, 6, false, false, 15u, 0u, 13u}, FieldDestination{OutputStruct::DependencyLists, 8, 4, 0u}},
        DecodePlan::Step{FieldSource{0, 0, false, false, 0u, 0u, 0u}, FieldDestination{OutputStruct::DependencyLists, 12, 4, 0u}}>,
    // alltoall_256: linear_alltoall, 256 ranks.
    TemplatedSchemaRunner<0, 1,
        DecodePlan::Step{FieldSource{0, 0, false, false, 3u, 1u, 2u}, FieldDestination{OutputStruct::Node, 56, 1, 0u}},
        DecodePlan::Step{FieldSource{0, 2, false, false, 255u, 0u, 255u}, FieldDestination{OutputStruct::Node, 60, 4, 4294967295u}},
        DecodePlan::Step{FieldSource{0, 0, false, false, 0u, 1u, 0u}, FieldDestination{OutputStruct::Node, 64, 8, 0u}},
        DecodePlan::Step{FieldSource{0, 0, false, false, 0u, 0u, 0u}, FieldDestination{OutputStruct::Node, 72, 4, 4294967295u}},
        DecodePlan::Step{FieldSource{0, 0, false, false, 0u, 0u, 0u}, FieldDestination{OutputStruct::Node, 76, 1, 0u}},
        DecodePlan::Step{FieldSource{0, 0, false, false, 0u, 0u, 0u}, FieldDestination{OutputStruct::Node, 77, 1, 0u}},
        DecodePlan::Step{FieldSource{0, 0, false, false, 0u, 0u, 0u}, FieldDestination{OutputStruct::Node, 4, 4, 0u}},
        DecodePlan::Step{FieldSource{0, 0, false, false, 0u, 0u, 0u}, FieldDestination{OutputStruct::DependencyLists, 0, 4, 0u}},
        DecodePlan::Step{FieldSource{0, 0, false, false, 0u, 0u, 0u}, FieldDestination{OutputStruct::DependencyLists, 4, 4, 0u}},
        DecodePlan::Step{FieldSource{0, 0, false, false, 0u, 0u, 0u}, FieldDestination{OutputStruct::DependencyLists, 8, 4, 0u}},
        DecodePlan::Step{FieldSource{0, 0, false, false, 0u, 0u, 0u}, FieldDestination{OutputStruct::DependencyLists, 12, 4, 0u}}>,
    // atlahs_64k: binomialtreebcast, 65536 ranks, ATLAHS identities (AtlahsAdapter).
    TemplatedSchemaRunner<0, 1,
        DecodePlan::Step{FieldSource{0, 1, false, false, 3u, 1u, 2u}, FieldDestination{OutputStruct::Node, 56, 1, 0u}},
        DecodePlan::Step{FieldSource{0, 3, false, false, 65535u, 0u, 65535u}, FieldDestination{OutputStruct::Node, 60, 4, 4294967295u}},
        DecodePlan::Step{FieldSource{0, 0, false, false, 0u, 1u, 0u}, FieldDestination{OutputStruct::Node, 64, 8, 0u}},
        DecodePlan::Step{FieldSource{0, 0, false, false, 0u, 0u, 0u}, FieldDestination{OutputStruct::Node, 72, 4, 4294967295u}},
        DecodePlan::Step{FieldSource{0, 0, false, false, 0u, 0u, 0u}, FieldDestination{OutputStruct::Node, 76, 1, 0u}},
        DecodePlan::Step{FieldSource{0, 0, false, false, 0u, 0u, 0u}, FieldDestination{OutputStruct::Node, 77, 1, 0u}},
        DecodePlan::Step{FieldSource{3, 7, false, false, 3u, 0u, 3u}, FieldDestination{OutputStruct::Node, 80, 4, 0u}},
        DecodePlan::Step{FieldSource{24, 4, false, false, 3u, 0u, 3u}, FieldDestination{OutputStruct::Node, 84, 4, 0u}},
        DecodePlan::Step{FieldSource{24, 1, false, false, 7u, 0u, 7u}, FieldDestination{OutputStruct::Node, 88, 4, 0u}},
        DecodePlan::Step{FieldSource{4, 1, false, false, 281474976710655u, 3703831873u, 281471104613097u}, FieldDestination{OutputStruct::Node, 96, 8, 0u}},
        DecodePlan::Step{FieldSource{10, 1, false, false, 31u, 0u, 31u}, FieldDestination{OutputStruct::Node, 104, 8, 0u}},
        DecodePlan::Step{FieldSource{10, 6, false, false, 1099511627775u, 16680040u, 1099476066713u}, FieldDestination{OutputStruct::Node, 112, 8, 0u}},
        DecodePlan::Step{FieldSource{15, 6, false, false, 7u, 0u, 5u}, FieldDestination{OutputStruct::Node, 120, 8, 0u}},
        DecodePlan::Step{FieldSource{16, 1, true, false, 18446744073709551615u, 78939142137974u, 18446431917924032078u}, FieldDestination{OutputStruct::Node, 128, 8, 0u}},
        DecodePlan::Step{FieldSource{0, 0, false, false, 1u, 0u, 1u}, FieldDestination{OutputStruct::Node, 4, 4, 0u}},
        DecodePlan::Step{FieldSource{2, 7, false, false, 15u, 0u, 15u}, FieldDestination{OutputStruct::DependencyLists, 0, 4, 0u}},
        DecodePlan::Step{FieldSource{2, 3, false, false, 15u, 0u, 15u}, FieldDestination{OutputStruct::DependencyLists, 4, 4, 0u}},
        DecodePlan::Step{FieldSource{3, 3, false, false, 15u, 0u, 15u}, FieldDestination{OutputStruct::DependencyLists, 8, 4, 0u}},
        DecodePlan::Step{FieldSource{0, 0, false, false, 0u, 0u, 0u}, FieldDestination{OutputStruct::DependencyLists, 12, 4, 0u}}>,
    // bcast_1k: binomialtreebcast, 1024 ranks.
    TemplatedSchemaRunner<0, 1,
        DecodePlan::Step{FieldSource{0, 1, false, false, 3u, 1u, 2u}, FieldDestination{OutputStruct::Node, 56, 1, 0u}},
        DecodePlan::Step{FieldSource{0, 3, false, false, 1023u, 0u, 1023u}, FieldDestination{OutputStruct::Node, 60, 4, 4294967295u}},
        DecodePlan::Step{FieldSource{0, 0, false, false, 0u, 1u, 0u}, FieldDestination{OutputStruct::Node, 64, 8, 0u}},
        DecodePlan::Step{FieldSource{0, 0, false, false, 0u, 0u, 0u}, FieldDestination{OutputStruct::Node, 72, 4, 4294967295u}},
        DecodePlan::Step{FieldSource{0, 0, false, false, 0u, 0u, 0u}, FieldDestination{OutputStruct::Node, 76, 1, 0u}},
        DecodePlan::Step{FieldSource{0, 0, false, false, 0u, 0u, 0u}, FieldDestination{OutputStruct::Node, 77, 1, 0u}},
        DecodePlan::Step{FieldSource{0, 0, false, false, 1u, 0u, 1u}, FieldDestination{OutputStruct::Node, 4, 4, 0u}},
        DecodePlan::Step{FieldSource{2, 1, false, false, 15u, 0u, 9u}, FieldDestination{OutputStruct::DependencyLists, 0, 4, 0u}},
        DecodePlan::Step{FieldSource{1, 5, false, false, 15u, 0u, 9u}, FieldDestination{OutputStruct::DependencyLists, 4, 4, 0u}},
        DecodePlan::Step{FieldSource{2, 5, false, false, 15u, 0u, 9u}, FieldDestination{OutputStruct::DependencyLists, 8, 4, 0u}},
        DecodePlan::Step{FieldSource{0, 0, false, false, 0u, 0u, 0u}, FieldDestination{OutputStruct::DependencyLists, 12, 4, 0u}}>,
    // bcast_8: binomialtreebcast, 8 ranks.
    TemplatedSchemaRunner<0, 1,
        DecodePlan::Step{FieldSource{0, 1, false, false, 3u, 1u, 2u}, FieldDestination{OutputStruct::Node, 56, 1, 0u}},
        DecodePlan::Step{FieldSource{0, 3, false, false, 7u, 0u, 7u}, FieldDestination{OutputStruct::Node, 60, 4, 4294967295u}},
        DecodePlan::Step{FieldSource{0, 0, false, false, 0u, 1u, 0u}, FieldDestination{OutputStruct::Node, 64, 8, 0u}},
        DecodePlan::Step{FieldSource{0, 0, false, false, 0u, 0u, 0u}, FieldDestination{OutputStruct::Node, 72, 4, 4294967295u}},
        DecodePlan::Step{FieldSource{0, 0, false, false, 0u, 0u, 0u}, FieldDestination{OutputStruct::Node, 76, 1, 0u}},
        DecodePlan::Step{FieldSource{0, 0, false, false, 0u, 0u, 0u}, FieldDestination{OutputStruct::Node, 77, 1, 0u}},
        DecodePlan::Step{FieldSource{0, 0, false, false, 1u, 0u, 1u}, FieldDestination{OutputStruct::Node, 4, 4, 0u}},
        DecodePlan::Step{FieldSource{1, 0, false, false, 3u, 0u, 2u}, FieldDestination{OutputStruct::DependencyLists, 0, 4, 0u}},
        DecodePlan::Step{FieldSource{0, 6, false, false, 3u, 0u, 2u}, FieldDestination{OutputStruct::DependencyLists, 4, 4, 0u}},
        DecodePlan::Step{FieldSource{1, 2, false, false, 3u, 0u, 2u}, FieldDestination{OutputStruct::DependencyLists, 8, 4, 0u}},
        DecodePlan::Step{FieldSource{0, 0, false, false, 0u, 0u, 0u}, FieldDestination{OutputStruct::DependencyLists, 12, 4, 0u}}>,
    // ring_256: allreduce_ring, 256 ranks.
    TemplatedSchemaRunner<0, 1,
        DecodePlan::Step{FieldSource{0, 1, false, false, 3u, 1u, 2u}, FieldDestination{OutputStruct::Node, 56, 1, 0u}},
        DecodePlan::Step{FieldSource{0, 3, false, false, 255u, 0u, 255u}, FieldDestination{OutputStruct::Node, 60, 4, 4294967295u}},
        DecodePlan::Step{FieldSource{0, 0, false, false, 0u, 0u, 0u}, FieldDestination{OutputStruct::Node, 64, 8, 0u}},
        DecodePlan::Step{FieldSource{0, 0, false, false, 0u, 0u, 0u}, FieldDestination{OutputStruct::Node, 72, 4, 4294967295u}},
        DecodePlan::Step{FieldSource{0, 0, false, false, 0u, 0u, 0u}, FieldDestination{OutputStruct::Node, 76, 1, 0u}},
        DecodePlan::Step{FieldSource{0, 0, false, false, 0u, 0u, 0u}, FieldDestination{OutputStruct::Node, 77, 1, 0u}},
        DecodePlan::Step{FieldSource{0, 0, false, false, 1u, 0u, 1u}, FieldDestination{OutputStruct::Node, 4, 4, 0u}},
        DecodePlan::Step{FieldSource{1, 4, false, false, 511u, 0u, 509u}, FieldDestination{OutputStruct::DependencyLists, 0, 4, 0u}},
        DecodePlan::Step{FieldSource{1, 3, false, false, 1u, 0u, 1u}, FieldDestination{OutputStruct::DependencyLists, 4, 4, 0u}},
        DecodePlan::Step{FieldSource{2, 5, false, false, 511u, 0u, 509u}, FieldDestination{OutputStruct::DependencyLists, 8, 4, 0u}},
        DecodePlan::Step{FieldSource{0, 0, false, false, 0u, 0u, 0u}, FieldDestination{OutputStruct::DependencyLists, 12, 4, 0u}}>,
    // wide_64k: binomialtreebcast, 65536 ranks, random wide values.
    TemplatedSchemaRunner<0, 1,
        DecodePlan::Step{FieldSource{0, 1, false, false, 3u, 1u, 2u}, FieldDestination{OutputStruct::Node, 56, 1, 0u}},
        DecodePlan::Step{FieldSource{0, 3, false, false, 65535u, 0u, 65535u}, FieldDestination{OutputStruct::Node, 60, 4, 4294967295u}},
        DecodePlan::Step{FieldSource{2, 3, false, false, 1099511627775u, 26250620u, 1099483593096u}, FieldDestination{OutputStruct::Node, 64, 8, 0u}},
        DecodePlan::Step{FieldSource{7, 3, false, false, 2147483647u, 9732u, 2147453082u}, FieldDestination{OutputStruct::Node, 72, 4, 4294967295u}},
        DecodePlan::Step{FieldSource{11, 2, false, false, 255u, 0u, 255u}, FieldDestination{OutputStruct::Node, 76, 1, 0u}},
        DecodePlan::Step{FieldSource{12, 2, false, false, 255u, 0u, 255u}, FieldDestination{OutputStruct::Node, 77, 1, 0u}},
        DecodePlan::Step{FieldSource{0, 0, false, false, 1u, 0u, 1u}, FieldDestination{OutputStruct::Node, 4, 4, 0u}},
        DecodePlan::Step{FieldSource{13, 6, false, false, 15u, 0u, 15u}, FieldDestination{OutputStruct::DependencyLists, 0, 4, 0u}},
        DecodePlan::Step{FieldSource{13, 2, false, false, 15u, 0u, 15u}, FieldDestination{OutputStruct::DependencyLists, 4, 4, 0u}},
        DecodePlan::Step{FieldSource{14, 2, false, false, 15u, 0u, 15u}, FieldDestination{OutputStruct::DependencyLists, 8, 4, 0u}},
        DecodePlan::Step{FieldSource{0, 0, false, false, 0u, 0u, 0u}, FieldDestination{OutputStruct::DependencyLists, 12, 4, 0u}}>>;

} // namespace traceschema
#endif
