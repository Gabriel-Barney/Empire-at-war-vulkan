// Task graph for one tick: named systems with dependency edges.
//
// - Parallel nodes run as jobs as soon as all their predecessors finished (maximum parallelism
//   allowed by the edges). Edges are the only ordering guarantee between parallel nodes: two
//   nodes that touch the same data must be ordered by an edge.
// - Commit nodes run on the thread that called run(), one at a time, and in registration order
//   relative to each other (the graph adds implicit commit_i -> commit_{i+1} edges). They are the
//   deterministic serial phases (apply proposals, insert into shared containers, draw RNG).
//   They may overlap with parallel nodes that are not ordered against them by an edge.
// - Serial mode (JobSystem with 0 workers): every node runs on the caller in a fixed topological
//   order (Kahn, lowest node id first), handy for repro / debugging.
// - Exceptions: once a node throws, nodes that have not started are skipped; run() rethrows the
//   exception of the lowest-id failing node.
// - The graph is built once and run every tick; run() allocates nothing after the first run.
//   Not re-entrant: one run() at a time per graph.
#pragma once
#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "alamo/jobs/job_system.hpp"

namespace alamo::jobs {

class TaskGraph {
public:
    using NodeId = uint32_t;
    enum class Kind : uint8_t { Parallel, Commit };

    TaskGraph() = default;
    TaskGraph(const TaskGraph&) = delete;
    TaskGraph& operator=(const TaskGraph&) = delete;

    NodeId add(std::string name, std::function<void()> fn, Kind kind = Kind::Parallel);
    NodeId add_commit(std::string name, std::function<void()> fn) {
        return add(std::move(name), std::move(fn), Kind::Commit);
    }
    // `after` runs only once `before` finished.
    void precede(NodeId before, NodeId after);

    // Throws std::logic_error on a cycle (message names the nodes involved).
    void run(JobSystem& js);

    // Deterministic topological order (also used in serial mode). Throws on a cycle.
    const std::vector<NodeId>& topological_order();

    size_t size() const noexcept { return nodes_.size(); }
    const std::string& name(NodeId id) const { return nodes_.at(id).name; }
    Kind kind(NodeId id) const { return nodes_.at(id).kind; }

private:
    struct Node {
        std::string name;
        std::function<void()> fn;
        Kind kind;
        std::vector<NodeId> succ;  // explicit edges
    };

    void compile();
    void execute_node(NodeId id);
    void release(NodeId id);
    void start_parallel(NodeId id);
    static void node_job(void* ctx, uint32_t id, uint32_t);

    std::vector<Node> nodes_;
    bool dirty_ = true;
    // compiled
    std::vector<std::vector<NodeId>> succ_;  // explicit + implicit commit chain
    std::vector<uint32_t> indegree_;
    std::vector<NodeId> topo_;
    // per-run state
    std::unique_ptr<std::atomic<uint32_t>[]> remaining_;
    std::atomic<uint32_t> finished_{0};
    std::atomic<int64_t> ready_commit_{-1};
    std::atomic<bool> failed_{false};
    std::mutex error_mutex_;
    std::exception_ptr error_;
    NodeId error_node_ = 0;
    JobSystem* js_ = nullptr;
    JobCounter counter_;
};

}  // namespace alamo::jobs
