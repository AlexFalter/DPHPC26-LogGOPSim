#include "binary_reading/decoding/runners/compiled/c/CCompiler.hpp"

#include <mutex>
#include <unordered_map>

#include <atomic>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <spawn.h>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace traceschema {

static const char* const store_types[] = {"", "unsigned char", "unsigned short", "", "unsigned int",
                                          "", "", "", "u64"};

// C source specialized to one plan; it contains only numeric constants from the schema.
static std::string generate_decoder_source(const DecodePlan& plan) {
    std::ostringstream out;
    out << std::hex << "typedef unsigned long long u64;\n"
        << "int decode(const unsigned char* r, unsigned char* const* t) {\n    u64 w, c";
    const auto steps = plan.get_steps();
    const auto type_step = plan.get_type_step();
    const auto peer_step = plan.get_peer_step();
    const bool send_check = type_step && peer_step && steps[*peer_step].source.wildcard;
    if (send_check) out << ", type, peer";
    out << ";\n";
    for (std::size_t index = 0; index < steps.size(); ++index) {
        const auto& [source, destination] = steps[index];
        out << "    __builtin_memcpy(&w, r + 0x" << source.first_byte << ", 8);\n";
        if constexpr (std::endian::native == std::endian::big) out << "    w = __builtin_bswap64(w);\n";
        out << "    c = ((w >> 0x" << unsigned(source.shift) << ")";
        if (source.spill) out << " | ((u64)r[0x" << source.first_byte + 8 << "] << 0x" << 64 - source.shift << ")";
        out << ") & 0x" << source.mask << "ull;\n";
        if (send_check && index == type_step) out << "    type = c;\n";
        if (send_check && index == peer_step) out << "    peer = c;\n";
        if (source.maximum_offset != source.mask) {
            out << "    if (c > 0x" << source.maximum_offset << "ull) ";
            if (source.wildcard)
                out << "{ if (c != 0x" << source.maximum_offset + 1 << "ull) return 0; c = 0x"
                    << destination.wildcard_value << "ull; } else c += 0x" << source.base << "ull;\n";
            else out << "return 0;\n    c += 0x" << source.base << "ull;\n";
        } else if (source.base != 0) out << "    c += 0x" << source.base << "ull;\n";
        out << "    { " << store_types[destination.size] << " v = (" << store_types[destination.size] << ")c; "
            << "__builtin_memcpy(t[" << unsigned(std::to_underlying(destination.target)) << "] + 0x"
            << destination.offset << ", &v, " << unsigned(destination.size) << "); }\n";
    }
    if (send_check) {
        const auto& type = steps[*type_step].source;
        const auto& peer = steps[*peer_step].source;
        out << "    if (type + 0x" << type.base << "ull == 0x" << unsigned(OperationType::Send)
            << " && peer == 0x" << peer.maximum_offset + 1 << "ull) return 0;\n";
    }
    out << "    return 1;\n}\n";
    return out.str();
}

static std::uint64_t fnv1a(std::string_view text) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (const unsigned char c : text) hash = (hash ^ c) * 0x100000001b3ull;
    return hash;
}

static std::filesystem::path get_cache_directory() {
    if (const char* path = std::getenv("LOGGOPSIM_JIT_CACHE")) return path;
    if (const char* path = std::getenv("XDG_CACHE_HOME")) return std::filesystem::path(path) / "loggopsim-jit";
    if (const char* home = std::getenv("HOME")) return std::filesystem::path(home) / ".cache" / "loggopsim-jit";
    return {}; // A shared directory such as /tmp would let other users plant libraries.
}

static const char* const compiler_flags[] = {"-O2", "-shared", "-fPIC", "-x", "c"};

static bool run_compiler(const char* compiler, const std::string& source, const std::string& library) {
    const char* argv[] = {compiler, compiler_flags[0], compiler_flags[1], compiler_flags[2], compiler_flags[3],
                          compiler_flags[4], "-o", library.c_str(), source.c_str(), nullptr};
    pid_t pid;
    if (posix_spawnp(&pid, compiler, nullptr, nullptr, const_cast<char**>(argv), environ) != 0) return false;
    int status;
    return waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static const char* get_compiler() {
    const char* const compiler = std::getenv("LOGGOPSIM_JIT_CC");
    return compiler ? compiler : "cc";
}

bool has_c_compiler() {
    const std::string compiler = get_compiler();
    if (compiler.find('/') != std::string::npos) return access(compiler.c_str(), X_OK) == 0;
    const char* const path = std::getenv("PATH");
    for (std::string_view directories = path ? path : ""; !directories.empty();) {
        const auto colon = directories.find(':');
        const auto directory = directories.substr(0, colon);
        if (!directory.empty() && access((std::string(directory) + "/" + compiler).c_str(), X_OK) == 0) return true;
        directories = colon == std::string_view::npos ? std::string_view{} : directories.substr(colon + 1);
    }
    return false;
}

CompiledCode compile_with_c(const DecodePlan& plan) {
    const char* const compiler = get_compiler();
    const auto source = generate_decoder_source(plan);
    auto key = source + compiler;
    for (const char* flag : compiler_flags) key += flag;
    char name[17];
    std::snprintf(name, sizeof(name), "%016llx", static_cast<unsigned long long>(fnv1a(key)));
    std::error_code error;
    const auto directory = get_cache_directory();
    if (directory.empty()) return {};
    std::filesystem::create_directories(directory, error);
    const auto library = directory / (std::string(name) + ".so");
    const auto load = [&]() -> CompiledCode {
        static std::mutex mutex;
        static std::unordered_map<std::string, std::weak_ptr<void>> loaded;
        const std::lock_guard lock(mutex);
        auto& cached = loaded[library.string()];
        auto owner = cached.lock();
        if (!owner) {
            void* handle = dlopen(library.c_str(), RTLD_NOW | RTLD_LOCAL);
            if (!handle) return {};
            owner = std::shared_ptr<void>(handle, [](void* library) { dlclose(library); });
            cached = owner;
        }
        const auto function = reinterpret_cast<CompiledRunner::Function>(dlsym(owner.get(), "decode"));
        if (!function) return {};
        return {function, std::move(owner)};
    };
    if (std::filesystem::exists(library, error))
        if (const auto cached = load()) return cached;

    // A stale, truncated or incompatible entry must not permanently disable JIT.
    // Build beside it and replace it atomically only after successful compilation.
    static std::atomic<unsigned> sequence{0};
    const auto unique = std::string(name) + "." + std::to_string(getpid()) + "." + std::to_string(sequence++);
    const auto source_path = directory / (unique + ".c");
    const auto temporary = directory / (unique + ".so");
    std::ofstream(source_path) << source;
    const bool compiled = run_compiler(compiler, source_path.string(), temporary.string());
    std::filesystem::remove(source_path, error);
    if (!compiled) {
        std::filesystem::remove(temporary, error);
        return {};
    }
    std::filesystem::rename(temporary, library, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return {};
    }
    return load();
}

} // namespace traceschema
