#include "alamo/core/bytes.hpp"

#include <bit>
#include <fstream>
#include <iterator>

namespace alamo {
namespace {
bool fail(std::string* err, std::string msg) {
    if (err) *err = std::move(msg);
    return false;
}
}  // namespace

ByteWriter& ByteWriter::u8(uint8_t v) {
    buf_.push_back(v);
    return *this;
}
// Multi-byte fields: little-endian bytes assembled locally, appended with one insert (one capacity check).
ByteWriter& ByteWriter::u16(uint16_t v) {
    const uint8_t b[2] = {static_cast<uint8_t>(v & 0xFFu), static_cast<uint8_t>((v >> 8) & 0xFFu)};
    buf_.insert(buf_.end(), b, b + 2);
    return *this;
}
ByteWriter& ByteWriter::u32(uint32_t v) {
    uint8_t b[4];
    for (int i = 0; i < 4; ++i) b[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFFu);
    buf_.insert(buf_.end(), b, b + 4);
    return *this;
}
ByteWriter& ByteWriter::u64(uint64_t v) {
    uint8_t b[8];
    for (int i = 0; i < 8; ++i) b[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFFu);
    buf_.insert(buf_.end(), b, b + 8);
    return *this;
}
ByteWriter& ByteWriter::f32(float v) { return u32(std::bit_cast<uint32_t>(v)); }
ByteWriter& ByteWriter::f64(double v) { return u64(std::bit_cast<uint64_t>(v)); }
ByteWriter& ByteWriter::str(std::string_view s) {
    u32(static_cast<uint32_t>(s.size()));
    buf_.insert(buf_.end(), s.begin(), s.end());
    return *this;
}
ByteWriter& ByteWriter::raw(std::span<const uint8_t> data) {
    buf_.insert(buf_.end(), data.begin(), data.end());
    return *this;
}

uint64_t ByteReader::take(size_t n) noexcept {
    if (!ok_ || d_.size() - pos_ < n) {
        ok_ = false;
        return 0;
    }
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i) v |= static_cast<uint64_t>(d_[pos_ + i]) << (8 * i);
    pos_ += n;
    return v;
}
uint8_t ByteReader::u8() noexcept { return static_cast<uint8_t>(take(1)); }
uint16_t ByteReader::u16() noexcept { return static_cast<uint16_t>(take(2)); }
uint32_t ByteReader::u32() noexcept { return static_cast<uint32_t>(take(4)); }
uint64_t ByteReader::u64() noexcept { return take(8); }
float ByteReader::f32() noexcept { return std::bit_cast<float>(u32()); }
double ByteReader::f64() noexcept { return std::bit_cast<double>(u64()); }
bool ByteReader::boolean() noexcept {
    const uint8_t v = u8();
    if (v > 1) ok_ = false;
    return v == 1;
}
std::span<const uint8_t> ByteReader::raw(size_t n) noexcept {
    if (!ok_ || d_.size() - pos_ < n) {
        ok_ = false;
        return {};
    }
    const auto s = d_.subspan(pos_, n);
    pos_ += n;
    return s;
}
std::string ByteReader::str() {
    const uint32_t n = u32();
    const auto s = raw(n);
    return std::string(s.begin(), s.end());
}

bool read_file_bytes(const std::string& path, std::vector<uint8_t>& out, std::string* err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return fail(err, "cannot open " + path);
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

bool read_text_file(const std::string& path, std::string& out, std::string* err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return fail(err, "cannot open " + path);
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

bool write_text_file(const std::string& path, std::string_view text, std::string* err) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return fail(err, "cannot write " + path);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!f) return fail(err, "write failed: " + path);
    return true;
}

}  // namespace alamo
