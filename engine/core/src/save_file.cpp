#include "alamo/core/save_file.hpp"

#include <fstream>
#include <stdexcept>

#include "alamo/core/compress.hpp"
#include "alamo/core/sim_crc.hpp"

namespace alamo {
namespace {
constexpr uint8_t kBodyStored = 0, kBodyLz = 1;
constexpr uint32_t kMaxBodyBytes = 1u << 30;  // a plausibility cap on the unpacked size (allocation)

bool fail(std::string* err, std::string msg) {
    if (err) *err = std::move(msg);
    return false;
}

bool valid_name(std::string_view n) {
    if (n.empty()) return false;
    for (const char c : n)
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') return false;
    return true;
}

uint64_t digest_of(std::span<const uint8_t> bytes) {
    StateHasher h;
    h.bytes(bytes.data(), bytes.size());
    return h.digest();
}
}  // namespace

void SaveFile::add(std::string name, uint32_t version, const ByteWriter& w) { add(std::move(name), version, w.bytes()); }

void SaveFile::add(std::string name, uint32_t version, std::vector<uint8_t> data) {
    if (!valid_name(name)) throw std::invalid_argument("SaveFile::add: bad chunk name '" + name + "'");
    if (find(name)) throw std::invalid_argument("SaveFile::add: duplicate chunk '" + name + "'");
    chunks_.push_back(SaveChunk{std::move(name), version, std::move(data)});
}

const SaveChunk* SaveFile::find(std::string_view name) const {
    for (const auto& c : chunks_)
        if (c.name == name) return &c;
    return nullptr;
}

std::vector<uint8_t> SaveFile::encode() const {
    ByteWriter body;
    body.u32(static_cast<uint32_t>(chunks_.size()));
    for (const auto& c : chunks_) {
        body.str(c.name).u32(c.version).u32(static_cast<uint32_t>(c.data.size()));
        body.raw(c.data);
    }
    if (body.size() > kMaxBodyBytes) throw std::length_error("SaveFile::encode: more than 1 GiB of chunks");
    const std::vector<uint8_t> packed = lz_compress(body.bytes());
    const bool lz = packed.size() < body.size();
    const std::vector<uint8_t>& stored = lz ? packed : body.bytes();
    ByteWriter w;
    w.u32(kSaveMagic).u32(kSaveFormatVersion).u64(tick).str(description);
    w.u8(lz ? kBodyLz : kBodyStored).u32(static_cast<uint32_t>(body.size())).u32(static_cast<uint32_t>(stored.size()));
    w.raw(stored);
    std::vector<uint8_t> out = w.bytes();
    ByteWriter tail;
    tail.u64(digest_of(out));
    out.insert(out.end(), tail.bytes().begin(), tail.bytes().end());
    return out;
}

bool SaveFile::decode(std::span<const uint8_t> bytes, SaveFile& out, std::string* err) {
    if (bytes.size() < 8) return fail(err, "save file truncated");
    const auto file = bytes.first(bytes.size() - 8);
    ByteReader tail(bytes.last(8));
    if (tail.u64() != digest_of(file)) return fail(err, "save file checksum mismatch");
    ByteReader r(file);
    if (r.u32() != kSaveMagic) return fail(err, "not an engine save file");
    const uint32_t version = r.u32();
    if (!r.ok() || version != kSaveFormatVersion)
        return fail(err, "unsupported save format version " + std::to_string(version));
    SaveFile f;
    f.tick = r.u64();
    f.description = r.str();
    const uint8_t encoding = r.u8();
    const uint32_t body_size = r.u32();
    const uint32_t stored_size = r.u32();
    const std::span<const uint8_t> stored = r.raw(stored_size);
    if (!r.ok() || !r.done()) return fail(err, "save file truncated or malformed");
    if (body_size > kMaxBodyBytes) return fail(err, "save file body too large");
    std::vector<uint8_t> unpacked;
    std::span<const uint8_t> body = stored;
    if (encoding == kBodyLz) {
        if (!lz_decompress(stored, body_size, unpacked)) return fail(err, "save file body does not unpack");
        body = unpacked;
    } else if (encoding != kBodyStored || stored_size != body_size) {
        return fail(err, "save file body encoding unknown or malformed");
    }
    ByteReader b(body);
    const uint32_t count = b.u32();
    for (uint32_t i = 0; i < count && b.ok(); ++i) {
        SaveChunk c;
        c.name = b.str();
        c.version = b.u32();
        const uint32_t len = b.u32();
        const auto data = b.raw(len);
        if (!b.ok()) break;
        if (!valid_name(c.name) || f.find(c.name)) return fail(err, "bad or duplicate chunk name '" + c.name + "'");
        c.data.assign(data.begin(), data.end());
        f.chunks_.push_back(std::move(c));
    }
    if (!b.ok() || !b.done()) return fail(err, "save file truncated or malformed");
    out = std::move(f);
    return true;
}

bool SaveFile::write_file(const std::string& path, std::string* err) const {
    const std::vector<uint8_t> bytes = encode();
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return fail(err, "cannot write " + path);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!f) return fail(err, "write failed: " + path);
    return true;
}

bool SaveFile::read_file(const std::string& path, SaveFile& out, std::string* err) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return fail(err, "cannot open " + path);
    const std::streamoff size = f.tellg();
    if (size < 0) return fail(err, "cannot read " + path);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!f) return fail(err, "cannot read " + path);
    return decode(bytes, out, err);
}

}  // namespace alamo
