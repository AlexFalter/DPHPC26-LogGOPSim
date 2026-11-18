#include "binary_reading/decoding/gpu/GpuRuntime.hpp"

#include <nvrtc.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <unistd.h>
#include <utility>
#include <vector>

namespace traceschema::gpu {

void check(CUresult result, const char* what) {
    if (result == CUDA_SUCCESS) return;
    const char* name = nullptr;
    const char* text = nullptr;
    cuGetErrorName(result, &name);
    cuGetErrorString(result, &text);
    throw std::runtime_error(std::string(what) + ": " + (name ? name : "?") + " (" + (text ? text : "") + ")");
}

static void check_nvrtc(nvrtcResult result, const char* what) {
    if (result != NVRTC_SUCCESS) throw std::runtime_error(std::string(what) + ": " + nvrtcGetErrorString(result));
}

static double elapsed_ms(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

// ---------------------------------------------------------------------------------------------

Context::Context(int ordinal) {
    check(cuInit(0), "cuInit");
    check(cuDeviceGet(&device, ordinal), "cuDeviceGet");
    check(cuDevicePrimaryCtxRetain(&context, device), "cuDevicePrimaryCtxRetain");
    check(cuCtxSetCurrent(context), "cuCtxSetCurrent");
    const auto attribute = [&](CUdevice_attribute which) {
        int value = 0;
        check(cuDeviceGetAttribute(&value, which, device), "cuDeviceGetAttribute");
        return value;
    };
    major = attribute(CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR);
    minor = attribute(CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR);
    pageable_memory_access = attribute(CU_DEVICE_ATTRIBUTE_PAGEABLE_MEMORY_ACCESS) != 0;
    can_map_host_memory = attribute(CU_DEVICE_ATTRIBUTE_CAN_MAP_HOST_MEMORY) != 0;
    integrated = attribute(CU_DEVICE_ATTRIBUTE_INTEGRATED) != 0;
    char buffer[256] = {};
    check(cuDeviceGetName(buffer, sizeof buffer, device), "cuDeviceGetName");
    name = buffer;
    check(cuDeviceTotalMem(&total_memory, device), "cuDeviceTotalMem");
}

Context::~Context() {
    if (context) cuDevicePrimaryCtxRelease(device);
}

// ---------------------------------------------------------------------------------------------

std::filesystem::path get_cache_directory() {
    if (const char* path = std::getenv("LOGGOPSIM_GPU_CACHE")) return path;
    if (const char* path = std::getenv("XDG_CACHE_HOME")) return std::filesystem::path(path) / "loggopsim-gpu";
    if (const char* home = std::getenv("HOME")) return std::filesystem::path(home) / ".cache" / "loggopsim-gpu";
    return {};
}

std::string get_nvrtc_version() {
    int major = 0, minor = 0;
    check_nvrtc(nvrtcVersion(&major, &minor), "nvrtcVersion");
    return std::to_string(major) + "." + std::to_string(minor);
}

int get_driver_version() {
    int version = 0;
    check(cuDriverGetVersion(&version), "cuDriverGetVersion");
    return version;
}

static std::uint64_t fnv1a(std::string_view text) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (const unsigned char c : text) hash = (hash ^ c) * 0x100000001b3ull;
    return hash;
}

// sm_XY if NVRTC supports the device's architecture, else PTX for the newest one below it, which
// the driver compiles when the module is loaded.
static std::pair<std::string, bool> choose_target(const Context& context) {
    int count = 0;
    check_nvrtc(nvrtcGetNumSupportedArchs(&count), "nvrtcGetNumSupportedArchs");
    std::vector<int> architectures(static_cast<std::size_t>(count));
    check_nvrtc(nvrtcGetSupportedArchs(architectures.data()), "nvrtcGetSupportedArchs");
    const int device = context.major * 10 + context.minor;
    if (std::ranges::find(architectures, device) != architectures.end()) return {"sm_" + std::to_string(device), true};
    int best = 0;
    for (const int architecture : architectures)
        if (architecture <= device) best = std::max(best, architecture);
    if (best == 0) throw std::runtime_error("NVRTC supports no architecture at or below the device's");
    return {"compute_" + std::to_string(best), false};
}

static const char* const compile_options[] = {"-std=c++17", "--extra-device-vectorization"};

static CompiledModule compile_uncached(const std::string& source, const std::string& target, bool cubin) {
    const auto start = std::chrono::steady_clock::now();
    nvrtcProgram program;
    check_nvrtc(nvrtcCreateProgram(&program, source.c_str(), "lgs_decode.cu", 0, nullptr, nullptr), "nvrtcCreateProgram");
    const std::string architecture = "--gpu-architecture=" + target;
    std::vector<const char*> options{architecture.c_str()};
    options.insert(options.end(), std::begin(compile_options), std::end(compile_options));
    const auto result = nvrtcCompileProgram(program, static_cast<int>(options.size()), options.data());
    if (result != NVRTC_SUCCESS) {
        std::size_t size = 0;
        nvrtcGetProgramLogSize(program, &size);
        std::string log(size, '\0');
        nvrtcGetProgramLog(program, log.data());
        nvrtcDestroyProgram(&program);
        throw std::runtime_error("NVRTC failed for " + target + ":\n" + log);
    }
    CompiledModule compiled;
    std::size_t size = 0;
    if (cubin) {
        check_nvrtc(nvrtcGetCUBINSize(program, &size), "nvrtcGetCUBINSize");
        compiled.image.resize(size);
        check_nvrtc(nvrtcGetCUBIN(program, compiled.image.data()), "nvrtcGetCUBIN");
    } else {
        check_nvrtc(nvrtcGetPTXSize(program, &size), "nvrtcGetPTXSize");
        compiled.image.resize(size); // includes the terminating zero cuModuleLoadData needs
        check_nvrtc(nvrtcGetPTX(program, compiled.image.data()), "nvrtcGetPTX");
    }
    nvrtcDestroyProgram(&program);
    compiled.cubin = cubin;
    compiled.target = target;
    compiled.compile_ms = elapsed_ms(start);
    return compiled;
}

CompiledModule compile(const std::string& source, const Context& context, const std::filesystem::path& cache_directory) {
    const auto [target, cubin] = choose_target(context);
    std::string key_text = source + "\n" + target + "\n" + get_nvrtc_version();
    for (const char* option : compile_options) key_text += std::string("\n") + option;
    char key[17];
    std::snprintf(key, sizeof key, "%016llx", static_cast<unsigned long long>(fnv1a(key_text)));

    std::filesystem::path file;
    std::error_code error;
    if (!cache_directory.empty()) {
        std::filesystem::create_directories(cache_directory, error);
        file = cache_directory / (std::string(key) + (cubin ? ".cubin" : ".ptx"));
        std::ifstream in(file, std::ios::binary);
        if (in) {
            CompiledModule cached;
            cached.image.assign(std::istreambuf_iterator<char>(in), {});
            if (!cached.image.empty()) {
                cached.cubin = cubin;
                cached.target = target;
                cached.key = key;
                cached.cache_hit = true;
                return cached;
            }
        }
    }
    auto compiled = compile_uncached(source, target, cubin);
    compiled.key = key;
    if (!file.empty()) { // write beside the entry, then rename, so readers never see a partial file
        static std::atomic<unsigned> sequence{0};
        const auto temporary = cache_directory / (std::string(key) + "." + std::to_string(getpid()) + "." +
                                                  std::to_string(sequence++) + ".tmp");
        {
            std::ofstream out(temporary, std::ios::binary);
            out.write(compiled.image.data(), static_cast<std::streamsize>(compiled.image.size()));
        }
        std::filesystem::rename(temporary, file, error);
        if (error) std::filesystem::remove(temporary, error);
    }
    return compiled;
}

// ---------------------------------------------------------------------------------------------

Module::Module(const CompiledModule& compiled) {
    const auto start = std::chrono::steady_clock::now();
    check(cuModuleLoadData(&module_, compiled.image.data()), "cuModuleLoadData");
    load_ms_ = elapsed_ms(start);
}

Module::~Module() {
    if (module_) cuModuleUnload(module_);
}

CUfunction Module::get_function(const char* name) const {
    CUfunction function;
    check(cuModuleGetFunction(&function, module_, name), name);
    return function;
}

CUdeviceptr Module::get_global(const char* name, std::size_t* bytes) const {
    CUdeviceptr pointer;
    std::size_t size = 0;
    check(cuModuleGetGlobal(&pointer, &size, module_, name), name);
    if (bytes) *bytes = size;
    return pointer;
}

// ---------------------------------------------------------------------------------------------

DeviceBuffer::DeviceBuffer(std::size_t bytes) : bytes_(bytes) {
    if (bytes != 0) check(cuMemAlloc(&pointer_, bytes), "cuMemAlloc");
}

DeviceBuffer::~DeviceBuffer() {
    if (pointer_) cuMemFree(pointer_);
}

DeviceBuffer& DeviceBuffer::operator=(DeviceBuffer&& other) noexcept {
    if (this != &other) {
        if (pointer_) cuMemFree(pointer_);
        pointer_ = std::exchange(other.pointer_, 0);
        bytes_ = std::exchange(other.bytes_, 0);
    }
    return *this;
}

} // namespace traceschema::gpu
