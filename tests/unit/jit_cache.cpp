#include "binary_reading/decoding/Decoder.hpp"
#include "TestHelpers.hpp"
#include "binary_reading/decoding/runners/compiled/CompiledRunner.hpp"
#include "binary_reading/decoding/runners/compiled/c/CCompiler.hpp"
#include "binary_reading/decoding/runners/compiled/orc/OrcLoader.hpp"

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <string>

using namespace traceschema;

namespace {

class JitCache : public testing::Test {
protected:
    JitCache() {
        std::ofstream script(compiler_);
        script << R"SH(#!/bin/sh
printf 'compile\n' >> "$LOGGOPSIM_TEST_COMPILER_LOG"
if [ "$LOGGOPSIM_TEST_COMPILER_MODE" = fail ]; then exit 1; fi
output=
source=
previous=
for argument in "$@"; do
    if [ "$previous" = -o ]; then output="$argument"; fi
    previous="$argument"
    source="$argument"
done
if [ "$LOGGOPSIM_TEST_COMPILER_MODE" = missing-symbol ]; then
    printf 'int another_symbol(void) { return 1; }\n' > "$source"
fi
cc "$@" || exit $?
if [ "$LOGGOPSIM_TEST_COMPILER_MODE" = corrupt ]; then
    printf 'not a shared library\n' > "$output"
fi
)SH";
        script.close();
        std::filesystem::permissions(compiler_, std::filesystem::perms::owner_all);
    }

    RecordLayout layout(std::uint64_t base = 42) const {
        return create_packed_schema({{std::string{"value"}, FieldType::UnsignedInteger, 7, 0,
                                      OffsetEncoding{FieldValue{base}, 85}}});
    }
    std::unique_ptr<CompiledRunner> compile(std::uint64_t base = 42) const {
        const std::array destinations{FieldDestination{OutputStruct::Node, 0, 8, 0}};
        const auto plan = DecodePlan::create(layout(base).fields, destinations);
        return CompiledRunner::create(plan, compile_with_c(plan), "jit");
    }
    void expect_value(const CompiledRunner& runner, std::uint64_t value) const {
        std::array<std::byte, 16> record{};
        record[0] = std::byte{3};
        std::uint64_t result = 0;
        std::byte* targets[] = {reinterpret_cast<std::byte*>(&result)};
        ASSERT_TRUE(runner.run(record.data(), targets));
        EXPECT_EQ(result, value);
    }
    int invocations() const {
        std::ifstream input(log_);
        int count = 0;
        for (std::string line; std::getline(input, line);) ++count;
        return count;
    }
    std::vector<std::filesystem::path> libraries() const {
        std::vector<std::filesystem::path> result;
        if (std::filesystem::is_directory(cache_))
            for (const auto& entry : std::filesystem::directory_iterator(cache_))
                if (entry.path().extension() == ".so") result.push_back(entry.path());
        return result;
    }

    test_helpers::TemporaryDirectory directory_;
    const std::filesystem::path cache_ = directory_.path() / "cache";
    const std::filesystem::path compiler_ = directory_.path() / "cc";
    const std::filesystem::path log_ = directory_.path() / "compiler.log";
    test_helpers::ScopedEnvironment cache_env_{"LOGGOPSIM_JIT_CACHE", cache_.string()};
    test_helpers::ScopedEnvironment compiler_env_{"LOGGOPSIM_JIT_CC", compiler_.string()};
    test_helpers::ScopedEnvironment log_env_{"LOGGOPSIM_TEST_COMPILER_LOG", log_.string()};
    test_helpers::ScopedEnvironment mode_env_{"LOGGOPSIM_TEST_COMPILER_MODE", "normal"};
};

TEST_F(JitCache, CompilesColdAndReusesWarmEntry) {
    EXPECT_TRUE(libraries().empty());
    const auto cold = compile();
    ASSERT_NE(cold, nullptr);
    expect_value(*cold, 45);
    ASSERT_EQ(libraries().size(), 1u);
    const auto library = libraries().front();
    const auto timestamp = std::filesystem::last_write_time(library);
    EXPECT_EQ(invocations(), 1);
    const auto warm = compile();
    ASSERT_NE(warm, nullptr);
    expect_value(*warm, 45);
    EXPECT_EQ(invocations(), 1) << "warm cache launched the compiler";
    EXPECT_EQ(std::filesystem::last_write_time(library), timestamp);
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(cache_), std::filesystem::directory_iterator{}), 1);
}

TEST_F(JitCache, ReadersShareTheLoadedLibraryAndReleaseItWhenUnused) {
    const std::array destinations{FieldDestination{OutputStruct::Node, 0, 8, 0}};
    const auto plan = DecodePlan::create(layout().fields, destinations);
    auto first = compile_with_c(plan);
    auto second = compile_with_c(plan);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(first.owner, second.owner);
    EXPECT_EQ(invocations(), 1);
    std::weak_ptr<void> lifetime = first.owner;
    first = {};
    EXPECT_FALSE(lifetime.expired());
    auto runner = CompiledRunner::create(plan, std::move(second), "jit");
    ASSERT_NE(runner, nullptr);
    expect_value(*runner, 45);
    runner.reset();
    EXPECT_TRUE(lifetime.expired());
    auto reloaded = CompiledRunner::create(plan, compile_with_c(plan), "jit");
    ASSERT_NE(reloaded, nullptr);
    expect_value(*reloaded, 45);
    EXPECT_EQ(invocations(), 1) << "Unloading must preserve the on-disk cache";
}

TEST_F(JitCache, UncachedOrcCodeHasIndependentOwners) {
    if (!has_orc_compiler()) GTEST_SKIP() << "LLVM ORC is unavailable";
    test_helpers::ScopedEnvironment no_reuse("LOGGOPSIM_ORC_REUSE", "0");
    const std::array destinations{FieldDestination{OutputStruct::Node, 0, 8, 0}};
    const auto plan = DecodePlan::create(layout().fields, destinations);
    auto first = compile_with_orc(plan);
    auto second = compile_with_orc(plan);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    std::weak_ptr<void> lifetime = first.owner;
    first = {};
    EXPECT_TRUE(lifetime.expired());
    auto runner = CompiledRunner::create(plan, std::move(second), "orc");
    ASSERT_NE(runner, nullptr);
    expect_value(*runner, 45);
}

TEST_F(JitCache, DifferentPlansUseDifferentEntries) {
    const auto first = compile(42), second = compile(43);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    expect_value(*first, 45);
    expect_value(*second, 46);
    EXPECT_EQ(libraries().size(), 2u);
    EXPECT_EQ(invocations(), 2);
}

class UnusableJitCache : public JitCache, public testing::WithParamInterface<const char*> {};

TEST_P(UnusableJitCache, RebuildsAnUnusableEntry) {
    test_helpers::ScopedEnvironment bad_output("LOGGOPSIM_TEST_COMPILER_MODE", GetParam());
    // Populate the cache through the real compiler path, but do not load a valid library
    // into this process: dlopen's own cache must not hide the damaged artifact.
    EXPECT_EQ(compile(), nullptr);
    ASSERT_EQ(libraries().size(), 1u);
    EXPECT_EQ(invocations(), 1);
    test_helpers::ScopedEnvironment repaired_output("LOGGOPSIM_TEST_COMPILER_MODE", "normal");
    const auto repaired = compile();
    ASSERT_NE(repaired, nullptr);
    expect_value(*repaired, 45);
    EXPECT_EQ(invocations(), 2);
    EXPECT_EQ(libraries().size(), 1u);
}

INSTANTIATE_TEST_SUITE_P(BadArtifacts, UnusableJitCache, testing::Values("corrupt", "missing-symbol"),
                        [](const auto& info) { return info.param == std::string_view("corrupt") ? "truncated" : "wrong_symbol"; });

TEST_F(JitCache, CompilerFailureLeavesNoEntryAndCanBeRetried) {
    test_helpers::ScopedEnvironment failing("LOGGOPSIM_TEST_COMPILER_MODE", "fail");
    EXPECT_EQ(compile(), nullptr);
    EXPECT_TRUE(std::filesystem::is_empty(cache_));
    test_helpers::ScopedEnvironment working("LOGGOPSIM_TEST_COMPILER_MODE", "normal");
    const auto repaired = compile();
    ASSERT_NE(repaired, nullptr);
    expect_value(*repaired, 45);
    EXPECT_EQ(invocations(), 2);
}

TEST_F(JitCache, UnusableCachePathFallsBackToTheInterpreter) {
    // A file where the directory should be is deterministic even when run as root.
    std::ofstream(cache_) << "leave this file intact";
    test_helpers::ScopedEnvironment requested("LOGGOPSIM_DECODER", "jit");
    const auto schema = layout();
    const std::array destinations{FieldDestination{OutputStruct::Node, 0, 8, 0}};
    const Decoder decoder(schema, {require_field<std::uint64_t>(std::string{"value"})}, destinations);
    EXPECT_EQ(decoder.get_runner_name(), "plan");
    std::array<std::byte, 16> record{};
    record[0] = std::byte{3};
    std::uint64_t result = 0;
    std::byte* targets[] = {reinterpret_cast<std::byte*>(&result)};
    decoder.decode_into(std::span(record).first(1), record.size(), targets);
    EXPECT_EQ(result, 45u);
    EXPECT_TRUE(std::filesystem::is_regular_file(cache_));
}

} // namespace
