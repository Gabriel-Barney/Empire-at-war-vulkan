// Little-endian byte encoding for saves / sim state (SimRng, SimRngStreams) and whole-file helpers
// for the CRC log files. (The command wire format lives in engine/commands.)
#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace alamo {

class ByteWriter {
public:
    ByteWriter& u8(uint8_t v);
    ByteWriter& u16(uint16_t v);
    ByteWriter& u32(uint32_t v);
    ByteWriter& u64(uint64_t v);
    ByteWriter& i32(int32_t v) { return u32(static_cast<uint32_t>(v)); }
    ByteWriter& i64(int64_t v) { return u64(static_cast<uint64_t>(v)); }
    ByteWriter& boolean(bool v) { return u8(v ? 1 : 0); }
    ByteWriter& f32(float v);
    ByteWriter& f64(double v);
    // u32 length, then the bytes (N1 saves).
    ByteWriter& str(std::string_view s);
    // Bytes as they are (no length; the reader must know it, e.g. from a preceding u32).
    ByteWriter& raw(std::span<const uint8_t> data);
    const std::vector<uint8_t>& bytes() const noexcept { return buf_; }
    size_t size() const noexcept { return buf_.size(); }

private:
    std::vector<uint8_t> buf_;
};

// Reads past the end return 0 and clear ok(); a consumer rejects the data when !ok() or when
// bytes remain (done() false) after reading its fields.
class ByteReader {
public:
    explicit ByteReader(std::span<const uint8_t> data) noexcept : d_(data) {}
    uint8_t u8() noexcept;
    uint16_t u16() noexcept;
    uint32_t u32() noexcept;
    uint64_t u64() noexcept;
    int32_t i32() noexcept { return static_cast<int32_t>(u32()); }
    int64_t i64() noexcept { return static_cast<int64_t>(u64()); }
    bool boolean() noexcept;  // a byte other than 0 / 1 clears ok()
    float f32() noexcept;
    double f64() noexcept;
    std::string str();                          // ByteWriter::str; a length past the end clears ok()
    std::span<const uint8_t> raw(size_t n) noexcept;  // the next n bytes (empty + !ok() when short)
    // Clears ok() (a consumer found an invalid value, e.g. an out-of-range enum or count).
    void fail() noexcept { ok_ = false; }
    bool ok() const noexcept { return ok_; }
    bool done() const noexcept { return pos_ == d_.size(); }
    size_t remaining() const noexcept { return d_.size() - pos_; }

private:
    uint64_t take(size_t n) noexcept;
    std::span<const uint8_t> d_;
    size_t pos_ = 0;
    bool ok_ = true;
};

// Whole-file helpers (binary mode, so text is byte-identical on every platform/CRT).
bool read_text_file(const std::string& path, std::string& out, std::string* err = nullptr);
bool write_text_file(const std::string& path, std::string_view text, std::string* err = nullptr);

}  // namespace alamo
