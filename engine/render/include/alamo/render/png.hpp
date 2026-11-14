// alamo_render - minimal PNG writer (8-bit RGBA, zlib "stored" deflate blocks, no compression,
// no third-party code). Used for test captures (logs/render_*.png).
#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace alamo::render {

// Chunk CRCs are alamo::crc32 (core/hash.hpp: CRC-32/IEEE, init / xorout 0xFFFFFFFF).
uint32_t adler32(std::span<const uint8_t> bytes, uint32_t adler = 1);

// rgba: width*height*4 bytes, top row first. Returns the whole file.
std::vector<uint8_t> encode_png_rgba(uint32_t width, uint32_t height, std::span<const uint8_t> rgba);
bool write_png_rgba(const std::string& path_utf8, uint32_t width, uint32_t height, std::span<const uint8_t> rgba);

}  // namespace alamo::render
