#pragma once

#include "SimulationEvents.hpp"
#include "LogGOPSimAdapter.hpp"

#include <cstdio>
#include <limits>
#include <memory>
#include <vector>

// How a rank's ready nodes and dependency counts are kept during the simulation.
enum class ScheduleMode {
    // Ready nodes are stored decoded; counts are decremented in the mapped file.
    Eager,
    // Ready nodes are stored as indices and decoded once, when the simulator takes them; counts are
    // kept in memory, so the mapped file is never written and its pages are never copied.
    Lazy,
};

// Ready-node state for one rank; the adapter handles the binary representation of either format.
class RankSchedule {
public:
    RankSchedule(traceschema::LogGOPSimAdapter& reader, std::uint32_t rank,
                 ScheduleMode mode = ScheduleMode::Eager)
        : reader_(&reader), rank_(rank), mode_(mode) {
        if (mode_ == ScheduleMode::Eager) {
            ready_nodes_ = reader.read_root_nodes(rank);
            return;
        }
        // Each count is read from the file on its first decrement: entries hold the remaining
        // count + 1, and 0 means "not read yet".
        counters_.resize(reader.get_node_count(rank));
        ready_indices_.reserve(reader.get_root_count(rank));
        for (std::uint32_t position = 0; position < reader.get_root_count(rank); ++position) {
            const auto root = reader.read_root(rank, position);
            if (!ready_indices_.empty() && root <= ready_indices_.back())
                throw std::invalid_argument("Root indices must be ordered and distinct");
            if (reader.read_dependency_count(rank, root) != 0)
                throw std::invalid_argument("Root node must have zero dependencies");
            ready_indices_.push_back(root);
        }
    }

    std::uint32_t GetNumNodes() const { return reader_->get_node_count(rank_); }
    // Recycle both vectors' capacity between ready batches.
    void GetExecutableNodes_DSN(std::vector<LogGOPSimNode>& output) {
        output.clear();
        if (mode_ == ScheduleMode::Eager) {
            output.swap(ready_nodes_);
            return;
        }
        for (const auto index : ready_indices_) output.push_back(reader_->read_node(rank_, index));
        ready_indices_.clear();
    }

    using nodelist_t = std::vector<graph_node_properties>;
    void GetExecutableNodes(nodelist_t* output) {
        if (mode_ == ScheduleMode::Eager) {
            for (const auto& node : ready_nodes_) output->push_back(to_event(node));
            ready_nodes_.clear();
            return;
        }
        for (const auto index : ready_indices_) output->push_back(to_event(reader_->read_node(rank_, index)));
        ready_indices_.clear();
    }

    // Starting and finishing a node need only its lists of dependents, not its other fields.
    void MarkNodeAsStarted(std::uint32_t node_index) {
        release_dependents(reader_->read_dependents(rank_, node_index, traceschema::DependencyEvent::Start));
    }

    void MarkNodeAsDone(std::uint32_t node_index) {
        release_dependents(reader_->read_dependents(rank_, node_index, traceschema::DependencyEvent::Finish));
    }

private:
    static graph_node_properties to_event(const LogGOPSimNode& node) {
        graph_node_properties event{};
        event.target = node.Peer;
        event.size = node.Size;
        event.tag = node.Tag;
        event.proc = node.Proc;
        event.nic = node.Nic;
        switch (static_cast<traceschema::OperationType>(node.Type)) {
        case traceschema::OperationType::Send: event.type = OP_SEND; break;
        case traceschema::OperationType::Receive: event.type = OP_RECV; break;
        case traceschema::OperationType::Calculation: event.type = OP_LOCOP; break;
        default: throw std::invalid_argument("Invalid schedule operation type");
        }
        event.node_index = node.node_index;
        return event;
    }

    void release_dependents(const traceschema::DependencyListView& dependents) {
        if (mode_ == ScheduleMode::Eager) {
            for (const auto index : dependents)
                if (reader_->decrement_dependency_count(rank_, index) == 0)
                    ready_nodes_.push_back(reader_->read_node(rank_, index));
            return;
        }
        for (const auto index : dependents) {
            auto& counter = counters_[index];
            if (counter == 0) {
                const auto count = reader_->read_dependency_count(rank_, index);
                if (count >= std::numeric_limits<std::uint32_t>::max())
                    throw std::invalid_argument("Dependency count exceeds uint32_t");
                counter = static_cast<std::uint32_t>(count) + 1;
            }
            if (counter == 1) throw std::invalid_argument("Dependency counter is already zero");
            if (--counter == 1) ready_indices_.push_back(index);
        }
    }

    traceschema::LogGOPSimAdapter* reader_;
    std::uint32_t rank_;
    ScheduleMode mode_;
    std::vector<LogGOPSimNode> ready_nodes_;   // Eager
    std::vector<std::uint32_t> ready_indices_; // Lazy
    std::vector<std::uint32_t> counters_;      // Lazy: remaining count + 1, 0 = not read yet
};

class SimulationSchedule {
private:
    std::unique_ptr<traceschema::LogGOPSimAdapter> trace_adapter_;

public:
    using schedules_t = std::vector<RankSchedule>;
    schedules_t schedules;

    // The plan runner decoding a self-describing schedule, or "legacy".
    [[nodiscard]] std::string_view get_runner_name() const { return trace_adapter_->get_runner_name(); }

    SimulationSchedule(const char* filename, bool save_mem, ScheduleMode mode = ScheduleMode::Eager)
        : trace_adapter_(create_adapter(filename, save_mem, mode)) {
        schedules.reserve(trace_adapter_->get_rank_count());
        for (std::uint32_t rank = 0; rank < trace_adapter_->get_rank_count(); ++rank)
            schedules.emplace_back(*trace_adapter_, rank, mode);
        if (schedules.empty()) throw std::invalid_argument("Simulation requires at least one rank");
        if (save_mem) {
            // The shared mapping writes counter updates back to the file; mark it as consumed.
            trace_adapter_->invalidate_shared_input();
            std::printf("The schedule will be invalid after this simulation!\n");
        }
    }

    std::uint32_t GetNumCPU() const {
        return static_cast<std::uint32_t>(trace_adapter_->get_resource_count(traceschema::FieldId::Cpu));
    }
    std::uint32_t GetNumNIC() const {
        return static_cast<std::uint32_t>(trace_adapter_->get_resource_count(traceschema::FieldId::Nic));
    }

private:
    // Lazy mode never writes the file, so a shared mapping would only invalidate it. Checked
    // before mapping, so a rejected combination leaves the file untouched.
    static std::unique_ptr<traceschema::LogGOPSimAdapter> create_adapter(
        const char* filename, bool save_mem, ScheduleMode mode) {
        if (save_mem && mode == ScheduleMode::Lazy)
            throw std::invalid_argument("--save-mem cannot be combined with --lazy-decoding");
        return std::make_unique<traceschema::LogGOPSimAdapter>(filename, save_mem);
    }
};
