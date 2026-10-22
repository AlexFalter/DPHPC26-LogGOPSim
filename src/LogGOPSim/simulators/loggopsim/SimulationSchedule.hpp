#pragma once

#include "SimulationEvents.hpp"
#include "LogGOPSimAdapter.hpp"
#include "trace_format/BinaryIO.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <span>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace legacy_trace {
template<class Integer>
Integer read(const char* bytes) {
    Integer value;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}
} // namespace legacy_trace

class SerializedGraph {

	private:

	char* mapping_start; // argh. this should be void* but c++ is anal on void*-arithmetic
	uint32_t num_root_nodes;
	uint32_t num_nodes;
	uint32_t num_ranks_in_schedule;
	uint32_t my_rank;

    traceschema::LogGOPSimAdapter* trace_adapter_ = nullptr;
    std::size_t rank_size_ = 0;
    static constexpr std::size_t legacy_record_bytes = 39;

    char* legacy_record(std::uint32_t index) const {
        if (index >= num_nodes) throw std::invalid_argument("Node index outside legacy rank");
        return mapping_start + 2 * sizeof(std::uint32_t)
            + std::size_t{num_root_nodes} * sizeof(std::uint32_t) + std::size_t{index} * legacy_record_bytes;
    }

    traceschema::NodeIndexView legacy_dependents(std::uint32_t begin, std::uint32_t count) const {
        const auto appendix_offset = 2 * sizeof(std::uint32_t)
            + std::size_t{num_root_nodes} * sizeof(std::uint32_t) + std::size_t{num_nodes} * legacy_record_bytes;
        const auto entry_count = (rank_size_ - appendix_offset) / sizeof(std::uint32_t);
        if (begin > entry_count || count > entry_count - begin)
            throw std::invalid_argument("Legacy dependency list exceeds appendix");
        const auto* data = reinterpret_cast<const std::byte*>(mapping_start + appendix_offset)
            + std::size_t{begin} * sizeof(std::uint32_t);
        return traceschema::NodeIndexView::native(
            {data, std::size_t{count} * sizeof(std::uint32_t)}, num_nodes);
    }

    std::uint32_t decrement_dependency_count(std::uint32_t index) {
        if (trace_adapter_) return static_cast<std::uint32_t>(trace_adapter_->decrement_dependency_count(my_rank, index));
        auto* bytes = legacy_record(index);
        auto count = legacy_trace::read<std::uint32_t>(bytes);
        if (count == 0) throw std::invalid_argument("Dependency counter is already zero");
        --count;
        std::memcpy(bytes, &count, sizeof(count));
        return count;
    }

    void release_dependents(const traceschema::NodeIndexView& dependents) {
        for (const auto index : dependents)
            if (decrement_dependency_count(index) == 0) executableNodes.push_back(get_node_by_offset(index));
    }

	std::vector<LogGOPSimNode> executableNodes;



    void add_root_nodes() {
        if (trace_adapter_) {
            executableNodes = trace_adapter_->read_root_nodes(my_rank);
            return;
        }
        for (std::uint32_t index = 0; index < num_root_nodes; ++index) {
            const auto root = legacy_trace::read<std::uint32_t>(mapping_start + 2 * sizeof(std::uint32_t)
                    + std::size_t{index} * sizeof(std::uint32_t));
            executableNodes.push_back(get_node_by_offset(root));
        }
    }

    LogGOPSimNode get_node_by_offset(std::uint32_t index) {
        if (trace_adapter_) return trace_adapter_->read_node(my_rank, index);
        const char* position = legacy_record(index);
        const auto read = [&]<class Integer>() {
            const auto value = legacy_trace::read<Integer>(position);
            position += sizeof(Integer);
            return value;
        };
        LogGOPSimNode node{};
        node.offset = index;
        node.DependenciesCnt = read.operator()<std::uint32_t>();
        node.Type = read.operator()<char>();
        node.Peer = read.operator()<std::uint32_t>();
        node.Size = read.operator()<std::uint64_t>();
        node.Tag = read.operator()<std::uint32_t>();
        node.Proc = read.operator()<std::uint8_t>();
        node.Nic = read.operator()<std::uint8_t>();
        const auto completion_count = read.operator()<std::uint32_t>();
        const auto completion_begin = read.operator()<std::uint32_t>();
        const auto start_count = read.operator()<std::uint32_t>();
        const auto start_begin = read.operator()<std::uint32_t>();
        node.DependOnMe = legacy_dependents(completion_begin, completion_count);
        node.StartDependOnMe = legacy_dependents(start_begin, start_count);
        return node;
    }

	public:

	uint32_t GetNumNodes() {
		return this->num_nodes;
	}


	void write_as_dot(char *filename) {
		/**
			Produces a dot representation of the graph. This is usefull for debugging purposes.
		*/

		std::vector<LogGOPSimNode> allNodes;
		std::vector<LogGOPSimNode> executableNodes;

		executableNodes = GetExecutableNodes_DSN();
		while (executableNodes.size() > 0) {
			for (uint32_t cnt=0; cnt < executableNodes.size(); cnt++) {
				allNodes.push_back(executableNodes[cnt]);
				MarkNodeAsStarted_DSN(executableNodes[cnt]);
				MarkNodeAsDone_DSN(executableNodes[cnt]);
			}
			executableNodes.clear();
			executableNodes = GetExecutableNodes_DSN();
		}

		FILE* fd = fopen(filename, "w");
		assert(fd != NULL);
		fprintf(fd, "digraph mygraph {\n");
		fprintf(fd, "graph [rankdir=LR];\n");
		fprintf(fd, "node [shape=record];\n");

			for (std::vector<LogGOPSimNode>::iterator it = allNodes.begin(); it != allNodes.end(); it++) {
				char typestr[5];
				if ((*it).Type == std::to_underlying(traceschema::OperationType::Send)) strcpy(typestr, "Send");
				else if ((*it).Type == std::to_underlying(traceschema::OperationType::Receive)) strcpy(typestr, "Recv");
				else if ((*it).Type == std::to_underlying(traceschema::OperationType::Calculation)) strcpy(typestr, "Calc");
				else strcpy(typestr, "Unkn");
				fprintf(fd, "%i [label=\"<f0> Type: %s | <f1> Peer: %i | <f2> Size: %llu | <f3> Tag: %i | <f4> Proc: %i | <f5> Nic: %i \"]\n",
							 (*it).offset,    typestr,        (*it).Peer, (unsigned long long) (*it).Size,  (*it).Tag  , (*it).Proc,     (*it).Nic);
			}

			for (std::vector<LogGOPSimNode>::iterator it = allNodes.begin(); it != allNodes.end(); it++) {
				for (auto dit = (*it).DependOnMe.begin(); dit != (*it).DependOnMe.end(); dit++) {
					fprintf(fd, "%i:f0 -> %i:f0\n", (*it).offset, (*dit));
				}
				for (auto dit = (*it).StartDependOnMe.begin(); dit != (*it).StartDependOnMe.end(); dit++) {
					fprintf(fd, "%i:f0 -> %i:f0 [arrowhead=diamond]\n", (*it).offset, (*dit));
				}
			}

		fprintf(fd, "} \n");

	}

    SerializedGraph(char* map_start, size_t map_length, std::uint32_t rank)
        : mapping_start(map_start), my_rank(rank) {
        num_ranks_in_schedule = legacy_trace::read<std::uint32_t>(map_start);
        const auto table_entry = map_start + sizeof(std::uint32_t) + 2 * sizeof(std::uint8_t)
            + std::size_t{rank} * 2 * sizeof(std::uint64_t);
        const auto begin = legacy_trace::read<std::uint64_t>(table_entry);
        const auto end = legacy_trace::read<std::uint64_t>(table_entry + sizeof(std::uint64_t));
        const auto table_end = sizeof(std::uint32_t) + 2 * sizeof(std::uint8_t)
            + std::uint64_t{num_ranks_in_schedule} * 2 * sizeof(std::uint64_t);
        if (begin < table_end || begin > end || end > map_length || end - begin < 2 * sizeof(std::uint32_t))
            throw std::invalid_argument("Invalid legacy rank section bounds");
        mapping_start += begin;
        rank_size_ = end - begin;
        num_nodes = legacy_trace::read<std::uint32_t>(mapping_start);
        num_root_nodes = legacy_trace::read<std::uint32_t>(mapping_start + sizeof(std::uint32_t));
        const auto roots_bytes = std::uint64_t{num_root_nodes} * sizeof(std::uint32_t);
        const auto records_bytes = std::uint64_t{num_nodes} * legacy_record_bytes;
        if (num_root_nodes > num_nodes || 2 * sizeof(std::uint32_t) + roots_bytes + records_bytes > rank_size_)
            throw std::invalid_argument("Truncated legacy rank records");
        add_root_nodes();
    }

    SerializedGraph(traceschema::LogGOPSimAdapter& reader, std::uint32_t rank)
        : mapping_start(nullptr), num_root_nodes(reader.get_root_count(rank)),
          num_nodes(reader.get_node_count(rank)), num_ranks_in_schedule(reader.get_rank_count()),
          my_rank(rank), trace_adapter_(&reader) {
        add_root_nodes();
    }

    void MarkNodeAsStarted_DSN(const LogGOPSimNode& node) { MarkNodeAsStarted(node.offset); }
    void MarkNodeAsDone_DSN(const LogGOPSimNode& node) { MarkNodeAsDone(node.offset); }

	std::vector<LogGOPSimNode> GetExecutableNodes_DSN() {

		std::vector<LogGOPSimNode> ret;

		for (uint32_t cnt=0; cnt<executableNodes.size(); cnt++) {
			ret.push_back(executableNodes[cnt]);
		}
		executableNodes.clear();
		return ret;
	}

  typedef std::vector<graph_node_properties> nodelist_t;
	void GetExecutableNodes(nodelist_t *ret_ptr) {
		nodelist_t& ret = *ret_ptr;

		for (uint32_t cnt=0; cnt<executableNodes.size(); cnt++) {
			graph_node_properties gp;
			gp.target = executableNodes[cnt].Peer;
			gp.size = executableNodes[cnt].Size;
			gp.tag = executableNodes[cnt].Tag;
			gp.proc = executableNodes[cnt].Proc;
			gp.nic = executableNodes[cnt].Nic;
			if (executableNodes[cnt].Type == std::to_underlying(traceschema::OperationType::Send)) gp.type = OP_SEND;
			else if (executableNodes[cnt].Type == std::to_underlying(traceschema::OperationType::Receive)) gp.type = OP_RECV;
			else if (executableNodes[cnt].Type == std::to_underlying(traceschema::OperationType::Calculation)) gp.type = OP_LOCOP;
			gp.offset = executableNodes[cnt].offset;
			ret.push_back(gp);
		}
		executableNodes.clear();
	}

    void MarkNodeAsStarted(std::uint32_t offset) {
        const auto node = get_node_by_offset(offset);
        release_dependents(node.StartDependOnMe);
    }

    void MarkNodeAsDone(std::uint32_t offset) {
        const auto node = get_node_by_offset(offset);
        release_dependents(node.DependOnMe);
    }

};

// The same mmap owner serves legacy and self-describing files.
class MappedSchedule {
public:
    MappedSchedule(const char* filename, bool shared) {
        const int fd = open(filename, shared ? O_RDWR : O_RDONLY);
        if (fd < 0) throw std::runtime_error("Cannot open simulation schedule");
        struct stat info{};
        if (fstat(fd, &info) != 0 || info.st_size <= 0 || !std::in_range<std::size_t>(info.st_size)) {
            close(fd);
            throw std::invalid_argument("Invalid schedule file size");
        }
        size_ = static_cast<std::size_t>(info.st_size);
        data_ = mmap(nullptr, size_, PROT_READ | PROT_WRITE, shared ? MAP_SHARED : MAP_PRIVATE, fd, 0);
        close(fd);
        if (data_ == MAP_FAILED) throw std::runtime_error("Cannot mmap simulation schedule");
    }
    ~MappedSchedule() { munmap(data_, size_); }
    MappedSchedule(const MappedSchedule&) = delete;
    MappedSchedule& operator=(const MappedSchedule&) = delete;
    std::span<std::byte> bytes() const { return {static_cast<std::byte*>(data_), size_}; }

private:
    void* data_;
    std::size_t size_;
};

class SimulationSchedule {
private:
    MappedSchedule mapping_;
    std::unique_ptr<traceschema::LogGOPSimAdapter> trace_adapter_;
    std::uint32_t cpu_count_;
    std::uint32_t nic_count_;

public:
    using schedules_t = std::vector<SerializedGraph>;
    schedules_t schedules;

    SimulationSchedule(const char* filename, bool save_mem) : mapping_(filename, save_mem) {
        const auto file = mapping_.bytes();
        if (file.size() < sizeof(std::uint64_t)) throw std::invalid_argument("Truncated schedule magic");
        auto* bytes = reinterpret_cast<char*>(file.data());
        const auto magic = legacy_trace::read<std::uint64_t>(bytes);
        if (magic == traceschema::invalid_schedule_magic ||
            traceschema::read_little_endian<std::uint64_t>(file.first(sizeof(std::uint64_t))) == traceschema::invalid_schedule_magic)
            throw std::invalid_argument("Schedule was invalidated by a prior simulation run");
        if (traceschema::has_binary_header_magic(file)) {
            trace_adapter_ = std::make_unique<traceschema::LogGOPSimAdapter>(file);
            cpu_count_ = static_cast<std::uint32_t>(trace_adapter_->get_resource_count(traceschema::FieldId::Cpu));
            nic_count_ = static_cast<std::uint32_t>(trace_adapter_->get_resource_count(traceschema::FieldId::Nic));
            schedules.reserve(trace_adapter_->get_rank_count());
            for (std::uint32_t rank = 0; rank < trace_adapter_->get_rank_count(); ++rank)
                schedules.emplace_back(*trace_adapter_, rank);
        } else if (magic == traceschema::legacy_format_magic) {
            const auto prefix_size = sizeof(std::uint64_t) + sizeof(std::uint32_t) + 2 * sizeof(std::uint8_t);
            if (file.size() < prefix_size) throw std::invalid_argument("Truncated legacy schedule header");
            const auto rank_count = legacy_trace::read<std::uint32_t>(bytes + sizeof(std::uint64_t));
            const auto table_size = std::uint64_t{rank_count} * 2 * sizeof(std::uint64_t);
            if (table_size > file.size() - prefix_size) throw std::invalid_argument("Truncated legacy rank table");
            cpu_count_ = static_cast<std::uint8_t>(file[12]) + 1u;
            nic_count_ = static_cast<std::uint8_t>(file[13]) + 1u;
            schedules.reserve(rank_count);
            for (std::uint32_t rank = 0; rank < rank_count; ++rank)
                schedules.emplace_back(bytes + sizeof(std::uint64_t), file.size() - sizeof(std::uint64_t), rank);
        } else throw std::invalid_argument("Unrecognized schedule format");
        if (schedules.empty()) throw std::invalid_argument("Simulation requires at least one rank");
        if (save_mem) {
            traceschema::write_little_endian(std::uint64_t{traceschema::invalid_schedule_magic}, file.first(sizeof(std::uint64_t)));
            std::printf("The schedule will be invalid after this simulation!\n");
        }
    }

    std::uint32_t GetNumCPU() const { return cpu_count_; }
    std::uint32_t GetNumNIC() const { return nic_count_; }
};
