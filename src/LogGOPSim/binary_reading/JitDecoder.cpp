#include "binary_reading/JitDecoder.hpp"

#include <atomic>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <spawn.h>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace traceschema {

static const char* const store_types[] = {"", "unsigned char", "unsigned short", "", "unsigned int",
                                          "", "", "", "u64"};

std::string generate_decoder_source(const DecodePlan& plan) {
    std::ostringstream out;
    out << std::hex << "typedef unsigned long long u64;\n"
        << "int decode(const unsigned char* r, unsigned char* const* t) {\n    u64 w, c";
    const auto type_step = plan.get_type_step();
    const auto peer_step = plan.get_peer_step();
    const bool send_check = type_step >= 0 && peer_step >= 0 && plan.get_steps()[peer_step].wildcard;
    if (send_check) out << ", type, peer";
    out << ";\n";
    int index = 0;
    for (const auto& step : plan.get_steps()) {
        out << "    __builtin_memcpy(&w, r + 0x" << step.byte << ", 8);\n";
        if constexpr (std::endian::native == std::endian::big) out << "    w = __builtin_bswap64(w);\n";
        out << "    c = ((w >> 0x" << unsigned(step.shift) << ")";
        if (step.spill) out << " | ((u64)r[0x" << step.byte + 8 << "] << 0x" << 64 - step.shift << ")";
        out << ") & 0x" << step.mask << "ull;\n";
        if (send_check && index == type_step) out << "    type = c;\n";
        if (send_check && index == peer_step) out << "    peer = c;\n";
        if (step.maximum != step.mask) {
            out << "    if (c > 0x" << step.maximum << "ull) ";
            if (step.wildcard)
                out << "{ if (c != 0x" << step.maximum + 1 << "ull) return 0; c = 0x" << step.wildcard_bits
                    << "ull; } else c += 0x" << step.base << "ull;\n";
            else out << "return 0;\n    c += 0x" << step.base << "ull;\n";
        } else if (step.base != 0) out << "    c += 0x" << step.base << "ull;\n";
        out << "    { " << store_types[step.size] << " v = (" << store_types[step.size] << ")c; "
            << "__builtin_memcpy(t[" << unsigned(step.target) << "] + 0x" << step.offset << ", &v, "
            << unsigned(step.size) << "); }\n";
        ++index;
    }
    if (send_check) {
        const auto& type = plan.get_steps()[type_step];
        const auto& peer = plan.get_steps()[peer_step];
        out << "    if (type + 0x" << type.base << "ull == 0x" << unsigned(OperationType::Send)
            << " && peer == 0x" << peer.maximum + 1 << "ull) return 0;\n";
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

JitDecodeFunction load_jit_decoder(const DecodePlan& plan) {
    const char* compiler = std::getenv("LOGGOPSIM_JIT_CC");
    if (!compiler) compiler = "cc";
    const auto source = generate_decoder_source(plan);
    auto key = source + compiler;
    for (const char* flag : compiler_flags) key += flag;
    char name[17];
    std::snprintf(name, sizeof(name), "%016llx", static_cast<unsigned long long>(fnv1a(key)));
    std::error_code error;
    const auto directory = get_cache_directory();
    if (directory.empty()) return nullptr;
    std::filesystem::create_directories(directory, error);
    const auto library = directory / (std::string(name) + ".so");
    if (!std::filesystem::exists(library, error)) {
        static std::atomic<unsigned> sequence{0};
        const auto unique = std::string(name) + "." + std::to_string(getpid()) + "." + std::to_string(sequence++);
        const auto source_path = directory / (unique + ".c");
        const auto temporary = directory / (unique + ".so");
        std::ofstream(source_path) << source;
        const bool compiled = run_compiler(compiler, source_path.string(), temporary.string());
        std::filesystem::remove(source_path, error);
        if (!compiled) {
            std::filesystem::remove(temporary, error);
            return nullptr;
        }
        std::filesystem::rename(temporary, library, error);
        if (error) return nullptr;
    }
    void* handle = dlopen(library.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) return nullptr;
    return reinterpret_cast<JitDecodeFunction>(dlsym(handle, "decode"));
}

} // namespace traceschema
