#include "alamo/jobs/task_graph.hpp"

#include <functional>
#include <queue>
#include <stdexcept>

namespace alamo::jobs {

TaskGraph::NodeId TaskGraph::add(std::string name, std::function<void()> fn, Kind kind) {
    nodes_.push_back(Node{std::move(name), std::move(fn), kind, {}});
    dirty_ = true;
    return static_cast<NodeId>(nodes_.size() - 1);
}

void TaskGraph::precede(NodeId before, NodeId after) {
    if (before >= nodes_.size() || after >= nodes_.size())
        throw std::out_of_range("TaskGraph::precede: bad node id");
    nodes_[before].succ.push_back(after);
    dirty_ = true;
}

void TaskGraph::compile() {
    if (!dirty_) return;
    const size_t n = nodes_.size();
    succ_.assign(n, {});
    indegree_.assign(n, 0);
    int64_t prev_commit = -1;
    for (size_t i = 0; i < n; ++i) {
        succ_[i] = nodes_[i].succ;
        if (nodes_[i].kind == Kind::Commit) {
            if (prev_commit >= 0) succ_[static_cast<size_t>(prev_commit)].push_back(static_cast<NodeId>(i));
            prev_commit = static_cast<int64_t>(i);
        }
    }
    for (size_t i = 0; i < n; ++i)
        for (NodeId s : succ_[i]) ++indegree_[s];

    // Kahn with a min-heap: deterministic order, lowest id first.
    std::vector<uint32_t> deg = indegree_;
    std::priority_queue<NodeId, std::vector<NodeId>, std::greater<NodeId>> ready;
    for (size_t i = 0; i < n; ++i)
        if (deg[i] == 0) ready.push(static_cast<NodeId>(i));
    topo_.clear();
    while (!ready.empty()) {
        const NodeId id = ready.top();
        ready.pop();
        topo_.push_back(id);
        for (NodeId s : succ_[id])
            if (--deg[s] == 0) ready.push(s);
    }
    if (topo_.size() != n) {
        std::string msg = "TaskGraph: dependency cycle among:";
        for (size_t i = 0; i < n; ++i)
            if (deg[i] != 0) msg += " '" + nodes_[i].name + "'";
        throw std::logic_error(msg);
    }
    remaining_ = std::make_unique<std::atomic<uint32_t>[]>(n);
    dirty_ = false;
}

const std::vector<TaskGraph::NodeId>& TaskGraph::topological_order() {
    compile();
    return topo_;
}

void TaskGraph::execute_node(NodeId id) {
    if (!failed_.load(std::memory_order_relaxed)) {
        try {
            if (nodes_[id].fn) nodes_[id].fn();
        } catch (...) {
            std::lock_guard lk(error_mutex_);
            if (!error_ || id < error_node_) {
                error_ = std::current_exception();
                error_node_ = id;
            }
            failed_.store(true, std::memory_order_relaxed);
        }
    }
    release(id);
}

void TaskGraph::start_parallel(NodeId id) {
    try {
        js_->submit(counter_, &node_job, this, id);
    } catch (...) {
        execute_node(id);  // could not queue (allocation failure): run it right here instead
    }
}

void TaskGraph::release(NodeId id) {
    JobSystem* const js = js_;  // run() clears js_ once everything finished
    const size_t total = nodes_.size();
    bool wake = false;
    for (NodeId s : succ_[id]) {
        if (remaining_[s].fetch_sub(1, std::memory_order_acq_rel) == 1) {
            if (nodes_[s].kind == Kind::Parallel) {
                start_parallel(s);
            } else {
                ready_commit_.store(s);
                wake = true;
            }
        }
    }
    // After releasing: finished == size implies nothing left to submit. seq_cst, paired with
    // the caller's help_while condition.
    const uint32_t done = finished_.fetch_add(1) + 1;
    if (wake || done == total) js->notify_waiters();
}

void TaskGraph::node_job(void* ctx, uint32_t id, uint32_t) {
    static_cast<TaskGraph*>(ctx)->execute_node(id);
}

void TaskGraph::run(JobSystem& js) {
    compile();
    const auto n = static_cast<uint32_t>(nodes_.size());
    if (n == 0) return;
    js_ = &js;
    failed_.store(false);
    error_ = nullptr;
    error_node_ = 0;

    if (js.serial()) {
        for (NodeId id : topo_) {
            if (failed_.load()) break;
            try {
                if (nodes_[id].fn) nodes_[id].fn();
            } catch (...) {
                error_ = std::current_exception();
                error_node_ = id;
                failed_.store(true);
            }
        }
    } else {
        for (uint32_t i = 0; i < n; ++i) remaining_[i].store(indegree_[i], std::memory_order_relaxed);
        finished_.store(0);
        ready_commit_.store(-1);
        for (uint32_t i = 0; i < n; ++i) {
            if (indegree_[i] != 0) continue;
            if (nodes_[i].kind == Kind::Parallel) start_parallel(i);
            else ready_commit_.store(i);  // at most one: commits are chained
        }
        for (;;) {
            const int64_t c = ready_commit_.exchange(-1);
            if (c >= 0) {
                execute_node(static_cast<NodeId>(c));
                continue;
            }
            if (finished_.load() == n) break;
            js.help_while([this, n] { return ready_commit_.load() < 0 && finished_.load() < n; });
        }
        js.wait(counter_);  // node jobs fully returned (they touch `this` after finishing)
    }
    js_ = nullptr;
    if (error_) std::rethrow_exception(std::exchange(error_, nullptr));
}

}  // namespace alamo::jobs
