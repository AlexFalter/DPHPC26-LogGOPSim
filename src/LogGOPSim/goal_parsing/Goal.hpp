#pragma once

#include "trace_format/FieldIdentity.hpp"
#include "trace_format/FieldTypes.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

// In-memory rank graph constructed by GoalReader; no simulator or binary I/O.
struct Node {
    static constexpr std::uint32_t any_source = std::numeric_limits<std::uint32_t>::max();
    static constexpr std::uint32_t any_tag = std::numeric_limits<std::uint32_t>::max();

    std::vector<traceschema::FieldValue> custom_values; // TOML declaration order.

    uint64_t Size;
    std::vector<Node *> DependOnMe;
    std::vector<Node *> StartDependOnMe;
    uint32_t DependenciesCnt;
    uint32_t offset;
    uint32_t Peer;
    uint32_t Tag;
    uint8_t Proc;
    uint8_t Nic;
    char Type;

    [[nodiscard]] bool is_root() const { return DependenciesCnt == 0; }
};

class Graph {

  private:
    std::vector<Node *> allNodes;

    uint32_t num_edges;
    uint32_t offset_cntr;

    void addDependent(Node *node, std::vector<Node *> &dependents) {
        if (num_edges == std::numeric_limits<uint32_t>::max())
            throw std::invalid_argument("Legacy dependency count exceeds uint32_t");
        dependents.push_back(node);
        ++node->DependenciesCnt;
        ++num_edges;
    }

  public:
    ~Graph() {
        for (std::vector<Node *>::iterator it = allNodes.begin(); it != allNodes.end(); it++) {
            delete *it;
        }
    }

    Graph() {
        num_edges = 0;
        offset_cntr = 0;
    }

    const std::vector<Node *> &GetNodes() const { return allNodes; }
    std::uint32_t GetDependencyCount() const { return num_edges; }
    std::uint32_t GetRootCount() const {
        return static_cast<std::uint32_t>(
            std::ranges::count_if(allNodes, [](const Node *node) { return node && node->is_root(); }));
    }

    inline Node *addNode() {

        /**
                add a new node to the graph
        */

        if (allNodes.size() >= std::numeric_limits<uint32_t>::max())
            throw std::invalid_argument("Legacy rank node count exceeds uint32_t");
        Node *N = new Node;
        N->DependenciesCnt = 0;
        N->offset = offset_cntr;
        offset_cntr++;
        allNodes.push_back(N);

        return N;
    }

    // a waits for b to finish.
    void addDependency(Node *a, Node *b) { addDependent(a, b->DependOnMe); }

    // a waits for b to start.
    void addStartDependency(Node *a, Node *b) { addDependent(a, b->StartDependOnMe); }

    void write_as_dot() {
        /**
                Produces a dot representation of the graph. This is usefull for debugging purposes.
        */

        FILE *fd = fopen("graph.dot", "w");
        assert(fd != NULL);
        fprintf(fd, "digraph mygraph {\n");
        fprintf(fd, "graph [rankdir=LR];\n");
        fprintf(fd, "node [shape=record];\n");

        for (std::vector<Node *>::iterator it = allNodes.begin(); it != allNodes.end(); it++) {
            char typestr[5];
            if ((**it).Type == std::to_underlying(traceschema::OperationType::Send))
                strcpy(typestr, "Send");
            else if ((**it).Type == std::to_underlying(traceschema::OperationType::Receive))
                strcpy(typestr, "Recv");
            else if ((**it).Type == std::to_underlying(traceschema::OperationType::Calculation))
                strcpy(typestr, "Calc");
            else
                strcpy(typestr, "Unkn");
            fprintf(fd,
                    "%i [label=\"<f0> Type: %s | <f1> Peer: %i | <f2> Size: %lu | <f3> Tag: %i | <f4> Proc: "
                    "%i | <f5> Nic: %i \"]\n",
                    (**it).offset, typestr, (**it).Peer, (unsigned long int)(**it).Size, (**it).Tag,
                    (**it).Proc, (**it).Nic);
        }

        for (std::vector<Node *>::iterator it = allNodes.begin(); it != allNodes.end(); it++) {
            for (std::vector<Node *>::iterator dit = (**it).DependOnMe.begin();
                 dit != (**it).DependOnMe.end(); dit++) {
                fprintf(fd, "%i:f0 -> %i:f0\n", (**it).offset, (**dit).offset);
            }
            for (std::vector<Node *>::iterator dit = (**it).StartDependOnMe.begin();
                 dit != (**it).StartDependOnMe.end(); dit++) {
                fprintf(fd, "%i:f0 -> %i:f0 [arrowhead=diamond]\n", (**it).offset, (**dit).offset);
            }
        }

        fprintf(fd, "} \n");
    }
};

typedef Node *goalop_t;

class Goal {

  private:
    Graph graph;
    uint32_t rank = 0;
    uint32_t num_ranks = 0;

  public:
    std::uint32_t GetRank() const { return rank; }
    std::uint32_t GetNumRanks() const { return num_ranks; }
    const std::vector<Node *> &GetNodes() const { return graph.GetNodes(); }
    std::uint32_t GetDependencyCount() const { return graph.GetDependencyCount(); }
    std::uint32_t GetRootCount() const { return graph.GetRootCount(); }

    goalop_t Send(uint32_t dest, uint64_t size, uint32_t tag, uint8_t cpu, uint8_t nic) {

        Node *n = graph.addNode();

        n->Type = std::to_underlying(traceschema::OperationType::Send);
        n->Peer = dest;
        n->Tag = tag;
        n->Proc = cpu;
        n->Nic = nic;
        n->Size = size;

        return n;
    }

    goalop_t Recv(uint32_t src, uint64_t size, uint32_t tag, uint8_t cpu, uint8_t nic) {

        Node *n = graph.addNode();

        n->Type = std::to_underlying(traceschema::OperationType::Receive);
        n->Peer = src;
        n->Tag = tag;
        n->Proc = cpu;
        n->Nic = nic;
        n->Size = size;

        return n;
    }

    goalop_t Calc(uint64_t size, uint8_t cpu, uint8_t nic) {

        Node *n = graph.addNode();

        n->Type = std::to_underlying(traceschema::OperationType::Calculation);
        n->Peer = 0; // this optype has not real peer, i just set it so it is clearly defined
        n->Tag = 0;  // this optype has not real tag, i just set it so it is clearly defined
        n->Proc = cpu;
        n->Nic = nic;
        n->Size = size;

        return n;
    }

    void StartDependency(goalop_t src, goalop_t dest) {
        // a can not be executed before b is started
        graph.addStartDependency(src, dest);
    }

    void Dependency(goalop_t src, goalop_t dest) {
        // a can not be executed before b is finished
        graph.addDependency(src, dest);
    }

    void SetRank(uint32_t r) { rank = r; }

    void SetNumRanks(uint32_t nr) { num_ranks = nr; }
};
