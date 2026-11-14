// SP5 (docs/SIM_PARALLEL_DESIGN.md §4.3): deferred writes committed in a canonical order.
//
// Inside a parallel phase a job never writes shared state: it appends records to the buffer of the worker range it
// runs (one Lane per ParallelFor range, or per index), each record tagged with a canonical key. commit() merges every
// lane by the key and hands the records to the apply function in key order, so the result never depends on which
// worker ran which range or in what order they finished: the key alone decides. Keys must be unique per record
// (source id + a sequence number within the source make them so: Key::seq).
//
// Typical keys: (phase, source entity id, seq) for spawns / events of a service; (target id, source id, seq) for damage
// records (§5 "Combat").
#pragma once
#include <algorithm>
#include <cstdint>
#include <mutex>
#include <tuple>
#include <utility>
#include <vector>

namespace alamo::tactical::services {

struct Key {
    uint32_t a = 0;    // phase, or the target id (damage)
    uint32_t b = 0;    // source entity id
    uint32_t seq = 0;  // sequence within the source (the caller counts)
    friend bool operator<(const Key& x, const Key& y) { return std::tie(x.a, x.b, x.seq) < std::tie(y.a, y.b, y.seq); }
    friend bool operator==(const Key& x, const Key& y) = default;
};

// Records of type T from many concurrent writers. lane() hands out one vector per caller-chosen lane index (a
// ParallelFor range's begin index, or the entity index): lanes are written by one thread each, so appends need no lock.
template <class T>
class DeferredBuffer {
public:
    struct Entry {
        Key key;
        T value;
    };
    class Lane {
    public:
        void push(Key k, T v) { out_->push_back({k, std::move(v)}); }
        size_t size() const { return out_->size(); }

    private:
        friend class DeferredBuffer;
        explicit Lane(std::vector<Entry>* o) : out_(o) {}
        std::vector<Entry>* out_;
    };

    // Prepare `lanes` empty lanes (sequential; before the parallel phase). Capacity is kept across ticks.
    void begin(uint32_t lanes) {
        if (lanes_.size() < lanes) lanes_.resize(lanes);
        used_ = lanes;
        for (uint32_t i = 0; i < used_; ++i) lanes_[i].clear();
    }
    // Lane i (0 <= i < lanes): one writer at a time.
    Lane lane(uint32_t i) { return Lane(&lanes_[i]); }

    // Merge every lane by key (stable for equal keys within a lane, but keys should be unique) and apply in that order.
    template <class Apply>
    void commit(Apply&& apply) {
        merged_.clear();
        for (uint32_t i = 0; i < used_; ++i)
            for (Entry& e : lanes_[i]) merged_.push_back(std::move(e));
        std::stable_sort(merged_.begin(), merged_.end(), [](const Entry& x, const Entry& y) { return x.key < y.key; });
        for (Entry& e : merged_) apply(e.key, e.value);
        merged_.clear();
        for (uint32_t i = 0; i < used_; ++i) lanes_[i].clear();
    }
    size_t pending() const {
        size_t n = 0;
        for (uint32_t i = 0; i < used_; ++i) n += lanes_[i].size();
        return n;
    }

private:
    std::vector<std::vector<Entry>> lanes_;
    std::vector<Entry> merged_;
    uint32_t used_ = 0;
};

}  // namespace alamo::tactical::services
