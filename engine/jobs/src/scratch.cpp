#include "alamo/jobs/scratch.hpp"

#include <algorithm>
#include <cstdint>

namespace alamo::jobs {
namespace {
constexpr size_t kBlockAlign = 64;

size_t align_up(size_t v, size_t a) { return (v + (a - 1)) & ~(a - 1); }
}  // namespace

ScratchArena::Block ScratchArena::make_block(size_t size) {
    auto* p = static_cast<std::byte*>(::operator new(size, std::align_val_t{kBlockAlign}));
    return Block{p, size};
}

void ScratchArena::free_block(Block b) noexcept {
    ::operator delete(b.data, std::align_val_t{kBlockAlign});
}

ScratchArena::ScratchArena(size_t initial_block_size) {
    blocks_.push_back(make_block(std::max<size_t>(initial_block_size, 4096)));
}

ScratchArena::~ScratchArena() {
    for (const Block& b : blocks_) free_block(b);
}

void* ScratchArena::alloc(size_t bytes, size_t align) {
    if (align < 1) align = 1;
    for (;;) {
        Block& b = blocks_[cur_];
        // Align the absolute address (blocks are 64-aligned, so offset alignment suffices up to
        // 64; larger alignments are handled through the address).
        const auto base = reinterpret_cast<uintptr_t>(b.data);
        const size_t start = align_up(static_cast<size_t>(base + off_), align) - static_cast<size_t>(base);
        if (start + bytes <= b.size) {
            off_ = start + bytes;
            high_water_ = std::max(high_water_, bytes_in_use());
            return b.data + start;
        }
        // Move to the next block. Everything after cur_ is free (stack discipline), so a block
        // that is too small is replaced.
        const size_t need = bytes + align;
        const size_t next = cur_ + 1;
        if (next < blocks_.size() && blocks_[next].size < need) {
            free_block(blocks_[next]);
            blocks_.erase(blocks_.begin() + static_cast<std::ptrdiff_t>(next));
        }
        if (next >= blocks_.size() || blocks_[next].size < need) {
            const size_t grow = std::max(blocks_.back().size * 2, need);
            blocks_.insert(blocks_.begin() + static_cast<std::ptrdiff_t>(next), make_block(grow));
        }
        cur_ = next;
        off_ = 0;
    }
}

void ScratchArena::rewind(Marker m) noexcept {
    cur_ = m.block;
    off_ = m.offset;
    if (cur_ == 0 && off_ == 0 && blocks_.size() > 1) coalesce();
}

void ScratchArena::reset() noexcept { rewind(Marker{}); }

void ScratchArena::coalesce() noexcept {
    size_t total = 0;
    for (const Block& b : blocks_) total += b.size;
    Block merged{};
    try {
        merged = make_block(total);
    } catch (...) {
        return;  // keep the chain; still correct
    }
    for (const Block& b : blocks_) free_block(b);
    blocks_.clear();
    blocks_.push_back(merged);  // capacity >= 1 retained by clear(): no throw
}

size_t ScratchArena::bytes_in_use() const noexcept {
    size_t used = off_;
    for (size_t i = 0; i < cur_; ++i) used += blocks_[i].size;
    return used;
}

size_t ScratchArena::capacity() const noexcept {
    size_t total = 0;
    for (const Block& b : blocks_) total += b.size;
    return total;
}

ScratchArena& thread_scratch() {
    thread_local ScratchArena arena;
    return arena;
}

}  // namespace alamo::jobs
