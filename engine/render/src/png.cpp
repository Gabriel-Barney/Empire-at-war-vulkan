#include "alamo/render/png.hpp"

#include <algorithm>
#include <array>
#include <fstream>

#include "alamo/core/file_provider.hpp"

namespace alamo::render {

namespace {
const std::array<uint32_t, 256>& crc_table() {
    static const std::array<uint32_t, 256> t = [] {
        std::array<uint32_t, 256> r{};
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            r[n] = c;
        }
        return r;
    }();
    return t;
}

void put_be32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v >> 24));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}

void put_chunk(std::vector<uint8_t>& out, const char type[4], std::span<const uint8_t> data) {
    put_be32(out, static_cast<uint32_t>(data.size()));
    const size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    put_be32(out, crc32(std::span<const uint8_t>(out).subspan(start)));
}
}  // namespace

uint32_t crc32(std::span<const uint8_t> bytes, uint32_t crc) {
    const auto& t = crc_table();
    uint32_t c = crc ^ 0xFFFFFFFFu;
    for (uint8_t b : bytes) c = t[(c ^ b) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

uint32_t adler32(std::span<const uint8_t> bytes, uint32_t adler) {
    uint32_t a = adler & 0xFFFFu, b = adler >> 16;
    for (uint8_t x : bytes) {
        a = (a + x) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

std::vector<uint8_t> encode_png_rgba(uint32_t w, uint32_t h, std::span<const uint8_t> rgba) {
    std::vector<uint8_t> out;
    if (rgba.size() != static_cast<size_t>(w) * h * 4 || w == 0 || h == 0) return out;
    const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    out.insert(out.end(), sig, sig + 8);

    std::vector<uint8_t> ihdr;
    put_be32(ihdr, w);
    put_be32(ihdr, h);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});  // 8-bit, RGBA, deflate, adaptive filter, no interlace
    put_chunk(out, "IHDR", ihdr);

    // Raw scanlines: filter byte 0 + row.
    std::vector<uint8_t> raw;
    const size_t row = static_cast<size_t>(w) * 4;
    raw.reserve((row + 1) * h);
    for (uint32_t y = 0; y < h; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rgba.begin() + static_cast<std::ptrdiff_t>(row * y),
                   rgba.begin() + static_cast<std::ptrdiff_t>(row * (y + 1)));
    }
    // zlib: CMF 0x78 (deflate, 32K window), FLG 0x01 (check bits; 0x7801 % 31 == 0), stored blocks.
    std::vector<uint8_t> z = {0x78, 0x01};
    size_t pos = 0;
    do {
        const size_t n = std::min<size_t>(65535, raw.size() - pos);
        const bool last = pos + n == raw.size();
        z.push_back(last ? 1 : 0);  // BFINAL, BTYPE = 00 (stored)
        z.push_back(static_cast<uint8_t>(n));
        z.push_back(static_cast<uint8_t>(n >> 8));
        z.push_back(static_cast<uint8_t>(~n));
        z.push_back(static_cast<uint8_t>(~n >> 8));
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(pos), raw.begin() + static_cast<std::ptrdiff_t>(pos + n));
        pos += n;
    } while (pos < raw.size());
    put_be32(z, adler32(raw));
    put_chunk(out, "IDAT", z);
    put_chunk(out, "IEND", {});
    return out;
}

bool write_png_rgba(const std::string& path, uint32_t w, uint32_t h, std::span<const uint8_t> rgba) {
    const std::vector<uint8_t> file = encode_png_rgba(w, h, rgba);
    if (file.empty()) return false;
    std::ofstream f(long_path(path_from_utf8(path)), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    return static_cast<bool>(f);
}

}  // namespace alamo::render
