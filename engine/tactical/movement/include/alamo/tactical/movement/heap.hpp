// Open list of every movement planner (docs/re/MOVEMENT.md §1.4 step 6, §2.1, §7): a 1-based
// binary min-heap on f with the retail tie rules, which must be replicated exactly for
// bit-identical paths:
//   * sift-up stops when parent <= child (a pushed node never rises above an equal key);
//   * sift-down takes the left child first, the right child replaces it only if STRICTLY smaller,
//     and the swap happens only when that child is strictly smaller than the moved element;
//   * an optional capacity: a push into a full heap is silently dropped (space planner).
#pragma once
#include <cstdint>
#include <vector>

namespace alamo::tactical::movement {

template <class T>
class RetailHeap {
public:
    struct Item {
        float f;
        T value;
    };

    explicit RetailHeap(uint32_t capacity = 0) : cap_(capacity) { a_.resize(1); }
    void clear() { a_.resize(1); }
    void set_capacity(uint32_t c) { cap_ = c; }  // 0 = unbounded
    bool empty() const { return a_.size() <= 1; }
    uint32_t size() const { return static_cast<uint32_t>(a_.size() - 1); }
    const Item& top() const { return a_[1]; }

    // False when dropped (full).
    bool push(float f, const T& v) {
        if (cap_ != 0 && size() >= cap_) return false;
        a_.push_back(Item{f, v});
        size_t i = a_.size() - 1;
        while (i > 1) {
            const size_t p = i >> 1;
            if (a_[p].f <= a_[i].f) break;
            Item t = a_[p];
            a_[p] = a_[i];
            a_[i] = t;
            i = p;
        }
        return true;
    }

    Item pop() {
        Item out = a_[1];
        a_[1] = a_.back();
        a_.pop_back();
        const size_t n = a_.size() - 1;
        size_t i = 1;
        for (;;) {
            const size_t l = i * 2;
            if (l > n) break;
            size_t c = l;
            if (l + 1 <= n && a_[l + 1].f < a_[l].f) c = l + 1;
            if (!(a_[c].f < a_[i].f)) break;
            Item t = a_[c];
            a_[c] = a_[i];
            a_[i] = t;
            i = c;
        }
        return out;
    }

private:
    std::vector<Item> a_;
    uint32_t cap_;
};

}  // namespace alamo::tactical::movement
