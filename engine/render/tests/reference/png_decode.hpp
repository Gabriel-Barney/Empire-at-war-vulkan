// Minimal PNG reader for the reference images (8-bit greyscale / RGB / RGBA / palette, any
// zlib/deflate stream, all five filter types, no interlacing). Test-only, no third-party code.
#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace alamo::reftest {

struct Image {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba;  // width * height * 4, top row first
    const uint8_t* px(uint32_t x, uint32_t y) const { return &rgba[(static_cast<size_t>(y) * width + x) * 4]; }
};

// zlib stream (2-byte header + deflate + adler32) -> bytes. False on malformed data.
bool zlib_inflate(std::span<const uint8_t> in, std::vector<uint8_t>& out);
bool decode_png(std::span<const uint8_t> file, Image& out, std::string* err = nullptr);
bool read_png(const std::string& path_utf8, Image& out, std::string* err = nullptr);

}  // namespace alamo::reftest
