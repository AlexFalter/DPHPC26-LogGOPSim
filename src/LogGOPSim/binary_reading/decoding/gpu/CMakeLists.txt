# Optional GPU bulk decoder (bench/gpu/README.md). Only the CUDA driver API and NVRTC are
# needed; the kernels are compiled at run time, so no CUDA compiler or CMake CUDA language is used.
# Pass -DCUDAToolkit_ROOT=/usr/local/cuda if nvcc is not on PATH.
find_package(CUDAToolkit 11.2 REQUIRED)

add_library(trace_schema_gpu STATIC GpuPlan.cpp GpuRuntime.cpp)
target_link_libraries(trace_schema_gpu PUBLIC trace_schema CUDA::cuda_driver CUDA::nvrtc)

add_executable(gpu_decode_bench ${PROJECT_SOURCE_DIR}/../bench/gpu/gpu_decode_bench.cpp)
target_link_libraries(gpu_decode_bench PRIVATE trace_schema_gpu)
