#pragma once

#include "SimulationEvents.hpp"
#include "LogGOPSimAdapter.hpp"

#include <cstdio>
#include <memory>
#include <vector>

// Ready-node state for one rank; the adapter handles the binary representation of either format.
class RankSchedule {
public:
    RankSchedule(traceschema::LogGOPSimAdapter& reader, std::uint32_t rank)
        : reader_(&reader), rank_(rank), ready_nodes_(reader.read_root_nodes(rank)) {}

    std::uint32_t GetNumNodes() const { return reader_->get_node_count(rank_); }
    // Recycle both vectors' capacity between ready batches.
    void GetExecutableNodes_DSN(std::vector<LogGOPSimNode>& output) {
        output.clear();
        output.swap(ready_nodes_);
    }

    using nodelist_t = std::vector<graph_node_properties>;
    void GetExecutableNodes(nodelist_t* output) {
        for (const auto& node : ready_nodes_) {
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
            output->push_back(event);
        }
        ready_nodes_.clear();
    }

    // Starting and finishing a node need only its lists of dependents, not its other fields.
    void MarkNodeAsStarted(std::uint32_t node_index) {
        release_dependents(reader_->read_dependents(rank_, node_index, traceschema::DependencyEvent::Start));
    }

    void MarkNodeAsDone(std::uint32_t node_index) {
        release_dependents(reader_->read_dependents(rank_, node_index, traceschema::DependencyEvent::Finish));
    }

private:
    void release_dependents(const traceschema::DependencyListView& dependents) {
        for (const auto index : dependents)
            if (reader_->decrement_dependency_count(rank_, index) == 0)
                ready_nodes_.push_back(reader_->read_node(rank_, index));
    }
    traceschema::LogGOPSimAdapter* reader_;
    std::uint32_t rank_;
    std::vector<LogGOPSimNode> ready_nodes_;
};

class SimulationSchedule {
private:
    std::unique_ptr<traceschema::LogGOPSimAdapter> trace_adapter_;

public:
    using schedules_t = std::vector<RankSchedule>;
    schedules_t schedules;

    // The plan runner decoding a self-describing schedule, or "legacy".
    [[nodiscard]] std::string_view get_runner_name() const { return trace_adapter_->get_runner_name(); }

    SimulationSchedule(const char* filename, bool save_mem)
        : trace_adapter_(std::make_unique<traceschema::LogGOPSimAdapter>(filename, save_mem)) {
        schedules.reserve(trace_adapter_->get_rank_count());
        for (std::uint32_t rank = 0; rank < trace_adapter_->get_rank_count(); ++rank)
            schedules.emplace_back(*trace_adapter_, rank);
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
};
