// Phase-timing instrumentation for the benchmark harness (bench/README.md, "Phase timing").
// Copied into a NEW worktree as src/LogGOPSim/trace_format/PhaseTiming.hpp by
// bench/instrumentation/apply_phase_timing.py; never part of the baseline.
//
// Enabled at run time by GOAL_PHASE_TIMING=1 (any value except empty or "0"). Then each phase's
// accumulated CLOCK_MONOTONIC time is written to stderr at exit as one line:
//   PHASES {"version":1,"unit":"ns","phases":{"total":123,...},"calls":{...},"notes":{...}}
// Disabled, every hook costs one predictable branch on a cached flag and no clock read.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <utility>

namespace phase_timing {

inline bool enabled() {
    static const bool on = [] {
        const char* value = std::getenv("GOAL_PHASE_TIMING");
        return value && *value && !(value[0] == '0' && value[1] == '\0');
    }();
    return on;
}

inline std::uint64_t now_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<std::uint64_t>(ts.tv_nsec);
}

class Registry {
public:
    ~Registry() {
        if (!enabled()) return;
        std::string line = "PHASES {\"version\":1,\"unit\":\"ns\",\"phases\":{";
        append_map(line, ns_);
        line += "},\"calls\":{";
        append_map(line, calls_);
        line += "},\"notes\":{";
        bool first = true;
        for (const auto& [key, value] : notes_) {
            line += (first ? "\"" : ",\"") + key + "\":\"" + value + "\"";
            first = false;
        }
        line += "}}\n";
        std::fputs(line.c_str(), stderr);
        std::fflush(stderr);
    }
    void add(const char* name, std::uint64_t elapsed) {
        const std::lock_guard lock(mutex_);
        ns_[name] += elapsed;
        ++calls_[name];
    }
    void note(const char* key, std::string value) {
        const std::lock_guard lock(mutex_);
        notes_[key] = std::move(value);
    }

private:
    static void append_map(std::string& line, const std::map<std::string, std::uint64_t>& values) {
        bool first = true;
        for (const auto& [key, value] : values) {
            line += (first ? "\"" : ",\"") + key + "\":" + std::to_string(value);
            first = false;
        }
    }
    std::mutex mutex_;
    std::map<std::string, std::uint64_t> ns_, calls_;
    std::map<std::string, std::string> notes_;
};

inline Registry& registry() {
    static Registry instance;
    return instance;
}

// 0 when disabled, so that add() can ignore it.
inline std::uint64_t start() { return enabled() ? now_ns() : 0; }

inline void add(const char* name, std::uint64_t started) {
    if (!enabled() || started == 0) return;
    registry().add(name, now_ns() - started);
}

inline void note(const char* key, std::string value) {
    if (enabled()) registry().note(key, std::move(value));
}

// Adds the time from construction to destruction to the named phase.
class Scope {
public:
    explicit Scope(const char* name) : name_(name), started_(start()) {}
    ~Scope() { add(name_, started_); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    const char* name_;
    std::uint64_t started_;
};

// Runs function() as the named phase and returns its result (also usable in member initializers).
template<class Function>
decltype(auto) timed(const char* name, Function&& function) {
    const Scope scope(name);
    return std::forward<Function>(function)();
}

} // namespace phase_timing
