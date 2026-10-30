// Little-endian byte writer/reader used by the command codec and the recording format.
#pragma once
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace alamo::commands::wire {

template <class U>
inline void put(std::vector<uint8_t>& out, U v) {
    for (size_t i = 0; i < sizeof(U); ++i) out.push_back(static_cast<uint8_t>((static_cast<uint64_t>(v) >> (8 * i)) & 0xFFu));
}
inline void put_f32(std::vector<uint8_t>& out, float v) { put(out, std::bit_cast<uint32_t>(v)); }

class Reader {
public:
    explicit Reader(std::span<const uint8_t> d, size_t pos = 0) noexcept : d_(d), pos_(pos) {}
    template <class U>
    U get() noexcept {
        if (!ok_ || d_.size() - pos_ < sizeof(U)) {
            ok_ = false;
            return U{};
        }
        uint64_t v = 0;
        for (size_t i = 0; i < sizeof(U); ++i) v |= static_cast<uint64_t>(d_[pos_ + i]) << (8 * i);
        pos_ += sizeof(U);
        return static_cast<U>(v);
    }
    float f32() noexcept { return std::bit_cast<float>(get<uint32_t>()); }
    bool skip(size_t n) noexcept {
        if (!ok_ || d_.size() - pos_ < n) return ok_ = false;
        pos_ += n;
        return true;
    }
    size_t remaining() const noexcept { return d_.size() - pos_; }
    size_t pos() const noexcept { return pos_; }
    bool ok() const noexcept { return ok_; }
    void fail() noexcept { ok_ = false; }
    std::span<const uint8_t> data() const noexcept { return d_; }

private:
    std::span<const uint8_t> d_;
    size_t pos_ = 0;
    bool ok_ = true;
};

}  // namespace alamo::commands::wire
