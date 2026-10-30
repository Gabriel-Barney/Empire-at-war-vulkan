// Minimal inflate (RFC 1951) + PNG decoder. See png_decode.hpp.
#include "png_decode.hpp"

#include <array>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>

namespace alamo::reftest {
namespace {

struct BitReader {
    std::span<const uint8_t> d;
    size_t pos = 0;
    uint32_t bitbuf = 0;
    int bitcnt = 0;
    bool overflow = false;
    uint32_t bits(int n) {
        while (bitcnt < n) {
            uint32_t b = 0;
            if (pos < d.size()) b = d[pos++];
            else overflow = true;
            bitbuf |= b << bitcnt;
            bitcnt += 8;
        }
        const uint32_t v = bitbuf & ((n == 32) ? 0xFFFFFFFFu : ((1u << n) - 1u));
        bitbuf >>= n;
        bitcnt -= n;
        return v;
    }
    void align() {
        bitbuf = 0;
        bitcnt = 0;
    }
};

// Canonical Huffman table (puff-style: counts per length + symbols sorted by code).
struct Huffman {
    std::array<uint16_t, 16> count{};
    std::array<uint16_t, 320> symbol{};
    bool build(const uint8_t* lengths, int n) {
        count.fill(0);
        for (int i = 0; i < n; ++i) ++count[lengths[i]];
        count[0] = 0;
        int left = 1;
        for (int len = 1; len < 16; ++len) {
            left <<= 1;
            left -= count[static_cast<size_t>(len)];
            if (left < 0) return false;  // over-subscribed
        }
        std::array<uint16_t, 16> offs{};
        for (int len = 1; len < 15; ++len)
            offs[static_cast<size_t>(len + 1)] = static_cast<uint16_t>(offs[static_cast<size_t>(len)] + count[static_cast<size_t>(len)]);
        for (int i = 0; i < n; ++i)
            if (lengths[i]) symbol[offs[lengths[i]]++] = static_cast<uint16_t>(i);
        return true;
    }
    int decode(BitReader& br) const {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; ++len) {
            code |= static_cast<int>(br.bits(1));
            const int c = count[static_cast<size_t>(len)];
            if (code - c < first) return symbol[static_cast<size_t>(index + (code - first))];
            index += c;
            first += c;
            first <<= 1;
            code <<= 1;
            if (br.overflow) return -1;
        }
        return -1;
    }
};

constexpr uint16_t kLenBase[] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr uint8_t kLenExtra[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr uint16_t kDistBase[] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                  193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr uint8_t kDistExtra[] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool codes(BitReader& br, std::vector<uint8_t>& out, const Huffman& lit, const Huffman& dist) {
    for (;;) {
        const int sym = lit.decode(br);
        if (sym < 0 || br.overflow) return false;
        if (sym < 256) {
            out.push_back(static_cast<uint8_t>(sym));
        } else if (sym == 256) {
            return true;
        } else {
            const int s = sym - 257;
            if (s >= 29) return false;
            const size_t len = kLenBase[s] + br.bits(kLenExtra[s]);
            const int ds = dist.decode(br);
            if (ds < 0 || ds >= 30) return false;
            const size_t d = kDistBase[ds] + br.bits(kDistExtra[ds]);
            if (d > out.size()) return false;
            const size_t start = out.size() - d;
            for (size_t i = 0; i < len; ++i) out.push_back(out[start + i]);
        }
        if (out.size() > (512u << 20)) return false;
    }
}

}  // namespace

bool zlib_inflate(std::span<const uint8_t> in, std::vector<uint8_t>& out) {
    out.clear();
    if (in.size() < 2 || (in[0] & 0x0F) != 8 || ((in[0] << 8) | in[1]) % 31 != 0) return false;
    BitReader br{in.subspan(2)};
    Huffman fixed_lit, fixed_dist;
    {
        uint8_t l[288];
        for (int i = 0; i < 144; ++i) l[i] = 8;
        for (int i = 144; i < 256; ++i) l[i] = 9;
        for (int i = 256; i < 280; ++i) l[i] = 7;
        for (int i = 280; i < 288; ++i) l[i] = 8;
        fixed_lit.build(l, 288);
        uint8_t d[30];
        std::memset(d, 5, sizeof d);
        fixed_dist.build(d, 30);
    }
    int last = 0;
    do {
        last = static_cast<int>(br.bits(1));
        const uint32_t type = br.bits(2);
        if (br.overflow) return false;
        if (type == 0) {
            br.align();
            if (br.pos + 4 > br.d.size()) return false;
            const uint32_t len = br.d[br.pos] | (uint32_t(br.d[br.pos + 1]) << 8);
            br.pos += 4;
            if (br.pos + len > br.d.size()) return false;
            out.insert(out.end(), br.d.begin() + static_cast<std::ptrdiff_t>(br.pos),
                       br.d.begin() + static_cast<std::ptrdiff_t>(br.pos + len));
            br.pos += len;
        } else if (type == 1) {
            if (!codes(br, out, fixed_lit, fixed_dist)) return false;
        } else if (type == 2) {
            const int nlen = static_cast<int>(br.bits(5)) + 257, ndist = static_cast<int>(br.bits(5)) + 1,
                      ncode = static_cast<int>(br.bits(4)) + 4;
            if (nlen > 286 || ndist > 30) return false;
            static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            uint8_t lengths[320] = {};
            for (int i = 0; i < ncode; ++i) lengths[order[i]] = static_cast<uint8_t>(br.bits(3));
            Huffman lencode;
            if (!lencode.build(lengths, 19)) return false;
            int idx = 0;
            std::memset(lengths, 0, sizeof lengths);
            while (idx < nlen + ndist) {
                int sym = lencode.decode(br);
                if (sym < 0) return false;
                if (sym < 16) {
                    lengths[idx++] = static_cast<uint8_t>(sym);
                } else {
                    uint8_t len = 0;
                    int rep = 0;
                    if (sym == 16) {
                        if (idx == 0) return false;
                        len = lengths[idx - 1];
                        rep = 3 + static_cast<int>(br.bits(2));
                    } else if (sym == 17) {
                        rep = 3 + static_cast<int>(br.bits(3));
                    } else {
                        rep = 11 + static_cast<int>(br.bits(7));
                    }
                    if (idx + rep > nlen + ndist) return false;
                    while (rep--) lengths[idx++] = len;
                }
            }
            Huffman lit, dist;
            if (!lit.build(lengths, nlen) || !dist.build(lengths + nlen, ndist)) return false;
            if (!codes(br, out, lit, dist)) return false;
        } else {
            return false;
        }
    } while (!last);
    return true;
}

bool decode_png(std::span<const uint8_t> f, Image& img, std::string* err) {
    auto fail = [&](const char* m) {
        if (err) *err = m;
        return false;
    };
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (f.size() < 8 || std::memcmp(f.data(), sig, 8) != 0) return fail("not a PNG");
    auto be32 = [&](size_t o) {
        return (uint32_t(f[o]) << 24) | (uint32_t(f[o + 1]) << 16) | (uint32_t(f[o + 2]) << 8) | f[o + 3];
    };
    size_t p = 8;
    uint32_t w = 0, h = 0;
    uint8_t depth = 0, ctype = 0, interlace = 0;
    std::vector<uint8_t> idat, palette, trns;
    while (p + 12 <= f.size()) {
        const uint32_t len = be32(p);
        if (p + 12 + size_t(len) > f.size()) return fail("truncated chunk");
        const char* type = reinterpret_cast<const char*>(&f[p + 4]);
        const uint8_t* d = &f[p + 8];
        if (std::memcmp(type, "IHDR", 4) == 0 && len >= 13) {
            w = (uint32_t(d[0]) << 24) | (uint32_t(d[1]) << 16) | (uint32_t(d[2]) << 8) | d[3];
            h = (uint32_t(d[4]) << 24) | (uint32_t(d[5]) << 16) | (uint32_t(d[6]) << 8) | d[7];
            depth = d[8];
            ctype = d[9];
            interlace = d[12];
        } else if (std::memcmp(type, "PLTE", 4) == 0) {
            palette.assign(d, d + len);
        } else if (std::memcmp(type, "tRNS", 4) == 0) {
            trns.assign(d, d + len);
        } else if (std::memcmp(type, "IDAT", 4) == 0) {
            idat.insert(idat.end(), d, d + len);
        } else if (std::memcmp(type, "IEND", 4) == 0) {
            break;
        }
        p += 12 + size_t(len);
    }
    if (!w || !h || w > 16384 || h > 16384) return fail("bad size");
    if (depth != 8 || interlace != 0) return fail("only 8-bit non-interlaced PNGs");
    size_t ch = 0;
    switch (ctype) {
    case 0: ch = 1; break;
    case 2: ch = 3; break;
    case 3: ch = 1; break;
    case 4: ch = 2; break;
    case 6: ch = 4; break;
    default: return fail("colour type");
    }
    std::vector<uint8_t> raw;
    if (!zlib_inflate(idat, raw)) return fail("inflate");
    const size_t stride = size_t(w) * ch;
    if (raw.size() < (stride + 1) * h) return fail("short image data");
    std::vector<uint8_t> cur(stride), prev(stride, 0);
    img.width = w;
    img.height = h;
    img.rgba.assign(size_t(w) * h * 4, 255);
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* line = &raw[y * (stride + 1)];
        const uint8_t filter = line[0];
        for (size_t x = 0; x < stride; ++x) {
            const int a = x >= size_t(ch) ? cur[x - ch] : 0, b = prev[x], c = x >= size_t(ch) ? prev[x - ch] : 0;
            int v = line[1 + x];
            switch (filter) {
            case 0: break;
            case 1: v += a; break;
            case 2: v += b; break;
            case 3: v += (a + b) / 2; break;
            case 4: {
                const int pp = a + b - c, pa = std::abs(pp - a), pb = std::abs(pp - b), pc = std::abs(pp - c);
                v += (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                break;
            }
            default: return fail("filter");
            }
            cur[x] = static_cast<uint8_t>(v);
        }
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t* o = &img.rgba[(size_t(y) * w + x) * 4];
            const uint8_t* s = &cur[size_t(x) * ch];
            switch (ctype) {
            case 0: o[0] = o[1] = o[2] = s[0]; break;
            case 2: o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; break;
            case 3:
                if (size_t(s[0]) * 3 + 2 < palette.size()) {
                    o[0] = palette[s[0] * 3u];
                    o[1] = palette[s[0] * 3u + 1];
                    o[2] = palette[s[0] * 3u + 2];
                }
                if (s[0] < trns.size()) o[3] = trns[s[0]];
                break;
            case 4: o[0] = o[1] = o[2] = s[0]; o[3] = s[1]; break;
            default: std::memcpy(o, s, 4); break;
            }
        }
        std::swap(cur, prev);
    }
    return true;
}

bool read_png(const std::string& path, Image& out, std::string* err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (err) *err = "cannot open " + path;
        return false;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return decode_png(bytes, out, err);
}

}  // namespace alamo::reftest
