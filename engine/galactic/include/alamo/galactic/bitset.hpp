// Small fixed-size bitset indexed by PlanetIndex (per-player world state masks).
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace alamo::galactic {

class Bitset {
public:
    Bitset() = default;
    explicit Bitset(size_t bits, bool value = false) { resize(bits, value); }

    void resize(size_t bits, bool value = false) {
        bits_ = bits;
        words_.assign((bits + 63) / 64, value ? ~0ull : 0ull);
        trim();
    }
    void fill(bool value) { resize(bits_, value); }
    size_t size() const { return bits_; }
    bool empty() const { return bits_ == 0; }
    bool test(size_t i) const { return (words_[i >> 6] >> (i & 63)) & 1ull; }
    void set(size_t i, bool value = true) {
        const uint64_t m = 1ull << (i & 63);
        if (value)
            words_[i >> 6] |= m;
        else
            words_[i >> 6] &= ~m;
    }
    std::span<const uint64_t> words() const { return words_; }
    friend bool operator==(const Bitset&, const Bitset&) = default;

private:
    void trim() {
        if (bits_ & 63) words_.back() &= (1ull << (bits_ & 63)) - 1;
    }
    size_t bits_ = 0;
    std::vector<uint64_t> words_;
};

}  // namespace alamo::galactic
