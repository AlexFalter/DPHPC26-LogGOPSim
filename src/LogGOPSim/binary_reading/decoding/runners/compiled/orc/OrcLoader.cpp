#include "binary_reading/decoding/runners/compiled/orc/OrcLoader.hpp"

#include <cstdlib>
#include <dlfcn.h>
#include <mutex>
#include <string>
#include <unordered_map>

namespace traceschema {

CompiledCode compile_with_orc(const DecodePlan& plan) {
#ifdef LOGGOPSIM_ORC_LIBRARY
    const char* const override_path = std::getenv("LOGGOPSIM_ORC_LIBRARY");
    const std::string path = override_path ? override_path : LOGGOPSIM_ORC_LIBRARY;
    // Keep each plugin loaded once. The returned owner also keeps it alive if
    // static cache destruction happens before the last reader is destroyed.
    static std::mutex mutex;
    static std::unordered_map<std::string, std::shared_ptr<void>> plugins;
    std::shared_ptr<void> plugin;
    {
        const std::lock_guard lock(mutex);
        auto& cached = plugins[path];
        if (!cached) {
            void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
            if (!handle) { plugins.erase(path); return {}; }
            cached = std::shared_ptr<void>(handle, [](void* handle) { dlclose(handle); });
        }
        plugin = cached;
    }
    using Compile = int (*)(const DecodePlan*, CompiledCode*);
    const auto compile = reinterpret_cast<Compile>(dlsym(plugin.get(), "loggopsim_compile_with_orc_v2"));
    if (!compile) {
        const std::lock_guard lock(mutex);
        plugins.erase(path);
        return {};
    }
    CompiledCode code;
    if (!compile(&plan, &code)) return {};
    struct Lifetime {
        std::shared_ptr<void> plugin; // Destroy executable code before unloading its implementation.
        std::shared_ptr<void> executable;
    };
    code.owner = std::make_shared<Lifetime>(Lifetime{std::move(plugin), std::move(code.owner)});
    return code;
#else
    (void)plan;
    return {};
#endif
}

bool has_orc_compiler() {
#ifdef LOGGOPSIM_ORC_LIBRARY
    return true;
#else
    return false;
#endif
}

} // namespace traceschema
