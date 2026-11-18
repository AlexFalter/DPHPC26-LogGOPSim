#ifndef LOGGOPSIM_GPURUNTIME_HPP
#define LOGGOPSIM_GPURUNTIME_HPP

#include <cuda.h>

#include <cstddef>
#include <filesystem>
#include <string>

// The CUDA side of the GPU decoder: device context, NVRTC compilation with an on-disk cache,
// module loading. Uses only the driver API and NVRTC, so no CUDA compiler is needed at build time.
namespace traceschema::gpu {

// Throws std::runtime_error naming what failed.
void check(CUresult result, const char* what);

// The device's primary context, current on the calling thread while this object lives.
class Context {
public:
    explicit Context(int ordinal = 0);
    ~Context();
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    CUdevice device = 0;
    CUcontext context = nullptr;
    int major = 0, minor = 0;
    std::string name;
    std::size_t total_memory = 0;
    bool pageable_memory_access = false; // the GPU can read ordinary (e.g. mmap'ed) host memory
    bool can_map_host_memory = false;
    bool integrated = false;
};

// A compiled module: a cubin for the device's architecture, or PTX when NVRTC does not know it.
struct CompiledModule {
    std::string image;
    bool cubin = false;
    std::string target;    // e.g. sm_75 or compute_90
    std::string key;       // cache key (hex)
    double compile_ms = 0; // NVRTC time, 0 on a cache hit
    bool cache_hit = false;
};

// $LOGGOPSIM_GPU_CACHE, else $XDG_CACHE_HOME/loggopsim-gpu, else ~/.cache/loggopsim-gpu.
[[nodiscard]] std::filesystem::path get_cache_directory();

// Compiles source with NVRTC for the device. With a cache directory, an image is looked up by a
// hash of source, target, options and NVRTC version, and stored after a miss.
[[nodiscard]] CompiledModule compile(const std::string& source, const Context& context,
                                     const std::filesystem::path& cache_directory);

[[nodiscard]] std::string get_nvrtc_version();
[[nodiscard]] int get_driver_version();

class Module {
public:
    explicit Module(const CompiledModule& compiled);
    ~Module();
    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;

    [[nodiscard]] CUfunction get_function(const char* name) const;
    // Device address and size of a __constant__ or __device__ variable.
    [[nodiscard]] CUdeviceptr get_global(const char* name, std::size_t* bytes = nullptr) const;
    [[nodiscard]] double get_load_ms() const { return load_ms_; }

private:
    CUmodule module_ = nullptr;
    double load_ms_ = 0;
};

// Device memory owned by this object.
class DeviceBuffer {
public:
    DeviceBuffer() = default;
    explicit DeviceBuffer(std::size_t bytes);
    ~DeviceBuffer();
    DeviceBuffer(DeviceBuffer&& other) noexcept : pointer_(other.pointer_), bytes_(other.bytes_) {
        other.pointer_ = 0;
        other.bytes_ = 0;
    }
    DeviceBuffer& operator=(DeviceBuffer&& other) noexcept;
    DeviceBuffer(const DeviceBuffer&) = delete;
    DeviceBuffer& operator=(const DeviceBuffer&) = delete;

    [[nodiscard]] CUdeviceptr get() const { return pointer_; }
    [[nodiscard]] std::size_t size() const { return bytes_; }

private:
    CUdeviceptr pointer_ = 0;
    std::size_t bytes_ = 0;
};

} // namespace traceschema::gpu
#endif
