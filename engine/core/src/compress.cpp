// LZ77 + canonical Huffman save compressor (format: alamo/core/compress.hpp). Written for the engine; no code from
// zlib / LZMA / other libraries.
#include "alamo/core/compress.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <stdexcept>

namespace alamo {
namespace {

static_assert(std::endian::native == std::endian::little, "the match finder loads little-endian words");

constexpr uint32_t kMinMatch = 3;                          // shortest length the format codes (repeat distances)
constexpr uint32_t kMinNewMatch = 4;                       // shortest match at a new distance the encoder emits
constexpr uint32_t kMaxMatch = kMinMatch + (1u << 20) - 1;  // length - 3 < 2^20: 42 bucket codes
constexpr uint32_t kWindow = 1u << 24;                     // distance - 1 < 2^24: 50 bucket codes
constexpr uint32_t kMaxCodeLen = 15;
constexpr uint32_t kEndOfBlock = 256;
constexpr uint32_t kLitLenSymbols = 257 + 42;
constexpr uint32_t kDistSymbols = 2 + 50;
constexpr size_t kBlockTokens = size_t{1} << 16;  // tokens per block (each block has its own codes)
constexpr uint32_t kHashBits = 18;
constexpr int kMaxChain = 48;          // candidates tried per position
constexpr uint32_t kNiceLength = 1024;  // a match this long ends the search
constexpr uint32_t kLazyBelow = 64;     // a shorter match is compared with the one at the next position
constexpr uint32_t kNone = 0xFFFFFFFFu;

// ---------------------------------------------------------------- bucket codes

struct Bucket {
    uint32_t code = 0, nbits = 0, extra = 0;
};

Bucket bucket(uint32_t v) {
    if (v < 8) return {v, 0, 0};
    const uint32_t n = static_cast<uint32_t>(std::bit_width(v)) - 1u;  // >= 3
    return {8u + 2u * (n - 3u) + ((v >> (n - 1u)) & 1u), n - 1u, v & ((1u << (n - 1u)) - 1u)};
}
uint32_t bucket_bits(uint32_t code) { return code < 8 ? 0u : 2u + (code - 8u) / 2u; }
uint32_t bucket_base(uint32_t code) {
    if (code < 8) return code;
    const uint32_t n = 3u + (code - 8u) / 2u;
    return (1u << n) | (((code - 8u) & 1u) << (n - 1u));
}

// ---------------------------------------------------------------- bits

class BitWriter {
public:
    explicit BitWriter(std::vector<uint8_t>& out) : out_(out) {}
    void put(uint32_t bits, uint32_t count) {  // count <= 32, bits < 2^count
        acc_ |= static_cast<uint64_t>(bits) << n_;
        n_ += count;
        while (n_ >= 8) {
            out_.push_back(static_cast<uint8_t>(acc_ & 0xFFu));
            acc_ >>= 8;
            n_ -= 8;
        }
    }
    void finish() {  // zero bits to the byte boundary
        if (n_ > 0) out_.push_back(static_cast<uint8_t>(acc_ & 0xFFu));
        acc_ = 0;
        n_ = 0;
    }

private:
    std::vector<uint8_t>& out_;
    uint64_t acc_ = 0;
    uint32_t n_ = 0;
};

// Past the end it reads zero bits and counts them: overrun() then says the stream was too short.
class BitReader {
public:
    explicit BitReader(std::span<const uint8_t> in) : in_(in) {}
    uint32_t get(uint32_t count) {  // count <= 32
        if (count == 0) return 0;
        if (n_ < count) refill();
        const uint32_t v = static_cast<uint32_t>(acc_ & ((uint64_t{1} << count) - 1u));
        acc_ >>= count;
        n_ -= count;
        return v;
    }
    uint32_t peek(uint32_t count) {  // count <= 32
        if (n_ < count) refill();
        return static_cast<uint32_t>(acc_ & ((uint64_t{1} << count) - 1u));
    }
    void skip(uint32_t count) {  // after peek(>= count)
        acc_ >>= count;
        n_ -= count;
    }
    uint64_t consumed_bits() const { return (static_cast<uint64_t>(pos_) + virt_) * 8u - n_; }
    bool overrun() const { return consumed_bits() > static_cast<uint64_t>(in_.size()) * 8u; }

private:
    void refill() {
        while (n_ <= 56) {
            uint64_t b = 0;
            if (pos_ < in_.size()) b = in_[pos_++];
            else ++virt_;
            acc_ |= b << n_;
            n_ += 8;
        }
    }
    std::span<const uint8_t> in_;
    size_t pos_ = 0;
    uint64_t virt_ = 0;
    uint64_t acc_ = 0;
    uint32_t n_ = 0;
};

// ---------------------------------------------------------------- Huffman codes

// Length-limited Huffman code lengths. Deterministic: leaves sorted by (frequency, symbol), a leaf goes before an
// internal node of the same weight; over kMaxCodeLen the frequencies are halved (kept >= 1) and the code rebuilt.
// Unused symbols get 0, a single used symbol 1.
void huffman_lengths(const std::vector<uint32_t>& freq, std::vector<uint8_t>& len) {
    const size_t n = freq.size();
    len.assign(n, 0);
    std::vector<uint32_t> sym;
    for (uint32_t s = 0; s < n; ++s)
        if (freq[s] != 0) sym.push_back(s);
    if (sym.empty()) return;
    if (sym.size() == 1) {
        len[sym[0]] = 1;
        return;
    }
    std::vector<uint64_t> f(freq.begin(), freq.end());
    const size_t m = sym.size();
    std::vector<uint64_t> w(2 * m - 1);
    std::vector<size_t> parent(2 * m - 1);
    std::vector<uint32_t> depth(2 * m - 1);
    for (;;) {
        std::sort(sym.begin(), sym.end(), [&f](uint32_t a, uint32_t b) { return f[a] != f[b] ? f[a] < f[b] : a < b; });
        for (size_t i = 0; i < m; ++i) w[i] = f[sym[i]];
        size_t leaf = 0, node = m;
        for (size_t k = m; k < 2 * m - 1; ++k) {
            size_t pick[2] = {0, 0};
            for (size_t& pk : pick) pk = (leaf < m && (node >= k || w[leaf] <= w[node])) ? leaf++ : node++;
            w[k] = w[pick[0]] + w[pick[1]];
            parent[pick[0]] = parent[pick[1]] = k;
        }
        depth[2 * m - 2] = 0;
        for (size_t k = 2 * m - 2; k-- > 0;) depth[k] = depth[parent[k]] + 1;  // a parent's index exceeds its children's
        uint32_t deepest = 0;
        for (size_t i = 0; i < m; ++i) deepest = std::max(deepest, depth[i]);
        if (deepest <= kMaxCodeLen) {
            for (size_t i = 0; i < m; ++i) len[sym[i]] = static_cast<uint8_t>(depth[i]);
            return;
        }
        for (const uint32_t s : sym) f[s] = (f[s] >> 1) | 1u;
    }
}

// Canonical codes (deflate's assignment), bit-reversed for LSB-first packing. False when the lengths over-subscribe
// the code space (incomplete codes are allowed: their unused patterns decode as errors).
bool canonical_codes(const std::vector<uint8_t>& len, std::vector<uint32_t>& code) {
    uint32_t count[kMaxCodeLen + 1] = {};
    for (const uint8_t l : len) {
        if (l > kMaxCodeLen) return false;
        ++count[l];
    }
    count[0] = 0;
    uint32_t left = 1;
    for (uint32_t b = 1; b <= kMaxCodeLen; ++b) {
        left <<= 1;
        if (count[b] > left) return false;
        left -= count[b];
    }
    uint32_t next[kMaxCodeLen + 1] = {};
    uint32_t c = 0;
    for (uint32_t b = 1; b <= kMaxCodeLen; ++b) {
        c = (c + count[b - 1]) << 1;
        next[b] = c;
    }
    code.assign(len.size(), 0);
    for (size_t s = 0; s < len.size(); ++s) {
        const uint32_t l = len[s];
        if (l == 0) continue;
        const uint32_t v = next[l]++;
        uint32_t r = 0;
        for (uint32_t i = 0; i < l; ++i) r |= ((v >> i) & 1u) << (l - 1u - i);
        code[s] = r;
    }
    return true;
}

void put_lengths(BitWriter& bw, const std::vector<uint8_t>& len) {
    uint32_t prev = 0;
    for (const uint8_t l : len) {
        if (l == prev) {
            bw.put(0, 1);
        } else {
            bw.put(1, 1);
            bw.put(l, 4);
            prev = l;
        }
    }
}

bool get_lengths(BitReader& br, std::vector<uint8_t>& len) {
    uint32_t prev = 0;
    for (uint8_t& l : len) {
        if (br.get(1) != 0) prev = br.get(4);
        l = static_cast<uint8_t>(prev);
    }
    return !br.overrun();
}

class HuffDecoder {
public:
    bool build(const std::vector<uint8_t>& len) {
        std::vector<uint32_t> code;
        if (!canonical_codes(len, code)) return false;
        table_.assign(size_t{1} << kMaxCodeLen, 0);
        for (size_t s = 0; s < len.size(); ++s) {
            const uint32_t l = len[s];
            if (l == 0) continue;
            const auto e = static_cast<uint16_t>(s | (l << 9));
            for (uint32_t i = code[s]; i < (1u << kMaxCodeLen); i += 1u << l) table_[i] = e;
        }
        return true;
    }
    // The next symbol, or -1 for a bit pattern without a code.
    int decode(BitReader& br) const {
        const uint16_t e = table_[br.peek(kMaxCodeLen)];
        if (e == 0) return -1;
        br.skip(static_cast<uint32_t>(e >> 9));
        return e & 0x1FF;
    }

private:
    std::vector<uint16_t> table_;  // 2^15 entries: symbol | length << 9 (0: no code)
};

// ---------------------------------------------------------------- tokens

// len 0: a literal (the byte in `dist`); otherwise a match: dist 0 / 1 = the last / the previous distance,
// else the new distance + 1.
struct Token {
    uint32_t len = 0, dist = 0;
};

void emit_block(BitWriter& bw, const std::vector<Token>& toks, bool last) {
    std::vector<uint32_t> lf(kLitLenSymbols, 0), df(kDistSymbols, 0);
    for (const Token& t : toks) {
        if (t.len == 0) {
            ++lf[t.dist];
            continue;
        }
        ++lf[257 + bucket(t.len - kMinMatch).code];
        ++df[t.dist < 2 ? t.dist : 2 + bucket(t.dist - 2).code];
    }
    ++lf[kEndOfBlock];
    std::vector<uint8_t> ll, dl;
    std::vector<uint32_t> lc, dc;
    huffman_lengths(lf, ll);
    huffman_lengths(df, dl);
    canonical_codes(ll, lc);
    canonical_codes(dl, dc);
    bw.put(last ? 1u : 0u, 1);
    put_lengths(bw, ll);
    put_lengths(bw, dl);
    for (const Token& t : toks) {
        if (t.len == 0) {
            bw.put(lc[t.dist], ll[t.dist]);
            continue;
        }
        const Bucket lb = bucket(t.len - kMinMatch);
        bw.put(lc[257 + lb.code], ll[257 + lb.code]);
        if (lb.nbits) bw.put(lb.extra, lb.nbits);
        if (t.dist < 2) {
            bw.put(dc[t.dist], dl[t.dist]);
        } else {
            const Bucket db = bucket(t.dist - 2);
            bw.put(dc[2 + db.code], dl[2 + db.code]);
            if (db.nbits) bw.put(db.extra, db.nbits);
        }
    }
    bw.put(lc[kEndOfBlock], ll[kEndOfBlock]);
}

// ---------------------------------------------------------------- match finder (hash chains over 4-byte prefixes)

class MatchFinder {
public:
    explicit MatchFinder(std::span<const uint8_t> in) : in_(in), n_(static_cast<uint32_t>(in.size())) {
        head_.assign(size_t{1} << kHashBits, kNone);
        uint32_t ring = 1;
        while (ring < n_ && ring < kWindow) ring <<= 1;
        prev_.assign(ring, kNone);
        mask_ = ring - 1;
    }

    // Adds every position below p to the chains (each once, in order).
    void insert_to(uint32_t p) {
        const uint32_t end = std::min(p, n_ >= 4 ? n_ - 3 : 0u);
        for (; next_ < end; ++next_) {
            const uint32_t h = hash(next_);
            prev_[next_ & mask_] = head_[h];
            head_[h] = next_;
        }
        next_ = std::max(next_, p);
    }

    // Length of the common prefix of in[a..] and in[b..] (a < b), at most max (b + max <= size).
    uint32_t match_len(uint32_t a, uint32_t b, uint32_t max) const {
        uint32_t l = 0;
        while (l + 8 <= max) {
            uint64_t x = 0, y = 0;
            std::memcpy(&x, in_.data() + a + l, 8);
            std::memcpy(&y, in_.data() + b + l, 8);
            if (const uint64_t d = x ^ y; d != 0) return l + static_cast<uint32_t>(std::countr_zero(d)) / 8u;
            l += 8;
        }
        while (l < max && in_[a + l] == in_[b + l]) ++l;
        return l;
    }

    // The longest match at p (at most max long) among the chain's candidates; 0 when none reaches kMinNewMatch.
    uint32_t longest(uint32_t p, uint32_t max, uint32_t& dist) const {
        if (max < kMinNewMatch) return 0;
        uint32_t best = kMinNewMatch - 1;
        uint32_t cand = head_[hash(p)];
        for (int chain = kMaxChain; cand != kNone && chain > 0; --chain) {
            const uint32_t d = p - cand;
            if (d > kWindow) break;
            if (in_[cand + best] == in_[p + best]) {
                const uint32_t l = match_len(cand, p, max);
                if (l > best) {
                    best = l;
                    dist = d;
                    if (l >= kNiceLength || l == max) break;
                }
            }
            cand = prev_[cand & mask_];
        }
        return best >= kMinNewMatch ? best : 0;
    }

private:
    uint32_t hash(uint32_t p) const {
        uint32_t v = 0;
        std::memcpy(&v, in_.data() + p, 4);
        return (v * 2654435761u) >> (32 - kHashBits);
    }
    std::span<const uint8_t> in_;
    uint32_t n_ = 0;
    uint32_t next_ = 0;  // next position to insert
    uint32_t mask_ = 0;
    std::vector<uint32_t> head_;
    std::vector<uint32_t> prev_;  // ring of chain links, indexed by position & mask_
};

// A parse step: a literal (len 0) or a match, scored as the bits it saves over literals (estimated code costs).
struct Choice {
    uint32_t len = 0, dist = 0;  // Token fields
    int64_t score = 0;
};

Choice choose(const MatchFinder& mf, uint32_t p, uint32_t n, uint32_t rep0, uint32_t rep1) {
    const uint32_t max = std::min(kMaxMatch, n - p);
    Choice best;
    auto consider = [&best](uint32_t len, uint32_t dist, int64_t cost) {
        const int64_t s = static_cast<int64_t>(len) * 8 - cost;
        if (s > best.score) best = {len, dist, s};
    };
    if (max < kMinMatch) return best;
    if (p >= rep0) {
        const uint32_t l = mf.match_len(p - rep0, p, max);
        if (l >= kMinMatch) consider(l, 0, 6 + bucket(l - kMinMatch).nbits);
    }
    if (rep1 != rep0 && p >= rep1) {
        const uint32_t l = mf.match_len(p - rep1, p, max);
        if (l >= kMinMatch) consider(l, 1, 8 + bucket(l - kMinMatch).nbits);
    }
    uint32_t d = 0;
    const uint32_t l = mf.longest(p, max, d);
    if (l >= kMinNewMatch && d != rep0 && d != rep1)
        consider(l, d + 1, 10 + bucket(l - kMinMatch).nbits + static_cast<uint32_t>(std::bit_width(d)));
    return best;
}

}  // namespace

std::vector<uint8_t> lz_compress(std::span<const uint8_t> in) {
    if (in.size() >= 0xFFFFFFFFu) throw std::length_error("lz_compress: input of 4 GiB or more");
    const auto n = static_cast<uint32_t>(in.size());
    std::vector<uint8_t> out;
    BitWriter bw(out);
    MatchFinder mf(in);
    std::vector<Token> toks;
    toks.reserve(kBlockTokens);
    uint32_t rep0 = 1, rep1 = 1;
    uint32_t p = 0;
    Choice ahead;               // choose() at p + 1, kept when the lazy step moves there
    uint32_t ahead_at = kNone;
    while (p < n) {
        mf.insert_to(p);
        Choice c = ahead_at == p ? ahead : choose(mf, p, n, rep0, rep1);
        ahead_at = kNone;
        if (c.len != 0 && c.len < kLazyBelow && p + 1 < n) {
            mf.insert_to(p + 1);
            ahead = choose(mf, p + 1, n, rep0, rep1);
            ahead_at = p + 1;
            if (ahead.score > c.score) c = Choice{};  // a literal here, the better match next
        }
        if (c.len == 0) {
            toks.push_back({0, in[p]});
            ++p;
        } else {
            toks.push_back({c.len, c.dist});
            if (c.dist == 1) {
                std::swap(rep0, rep1);
            } else if (c.dist >= 2) {
                rep1 = rep0;
                rep0 = c.dist - 1;
            }
            p += c.len;
            ahead_at = kNone;
        }
        if (toks.size() >= kBlockTokens && p < n) {
            emit_block(bw, toks, false);
            toks.clear();
        }
    }
    emit_block(bw, toks, true);
    bw.finish();
    return out;
}

bool lz_decompress(std::span<const uint8_t> in, size_t size, std::vector<uint8_t>& out) {
    out.assign(size, 0);
    BitReader br(in);
    HuffDecoder lit, dist;
    std::vector<uint8_t> ll(kLitLenSymbols), dl(kDistSymbols);
    uint32_t rep0 = 1, rep1 = 1;
    size_t w = 0;
    for (bool last = false; !last;) {
        if (br.overrun()) return false;
        last = br.get(1) != 0;
        if (!get_lengths(br, ll) || !get_lengths(br, dl) || !lit.build(ll) || !dist.build(dl)) return false;
        for (;;) {
            const int s = lit.decode(br);
            if (s < 0) return false;
            if (s < 256) {
                if (w >= size) return false;
                out[w++] = static_cast<uint8_t>(s);
                continue;
            }
            if (s == static_cast<int>(kEndOfBlock)) break;
            const uint32_t lcode = static_cast<uint32_t>(s) - 257u;
            const uint32_t len = kMinMatch + bucket_base(lcode) + br.get(bucket_bits(lcode));
            const int ds = dist.decode(br);
            if (ds < 0) return false;
            uint32_t d = rep0;
            if (ds == 1) {
                std::swap(rep0, rep1);
                d = rep0;
            } else if (ds >= 2) {
                const uint32_t dcode = static_cast<uint32_t>(ds) - 2u;
                d = 1u + bucket_base(dcode) + br.get(bucket_bits(dcode));
                rep1 = rep0;
                rep0 = d;
            }
            if (d > w || len > size - w) return false;
            uint8_t* dst = out.data() + w;
            const uint8_t* src = dst - d;
            if (d >= len) {
                std::memcpy(dst, src, len);
            } else {
                for (uint32_t k = 0; k < len; ++k) dst[k] = src[k];
            }
            w += len;
        }
    }
    if (w != size || br.overrun()) return false;
    // Zero padding to the byte boundary, then the end of the data.
    const auto pad = static_cast<uint32_t>((8u - br.consumed_bits() % 8u) % 8u);
    if (br.get(pad) != 0) return false;
    return br.consumed_bits() == static_cast<uint64_t>(in.size()) * 8u;
}

}  // namespace alamo
