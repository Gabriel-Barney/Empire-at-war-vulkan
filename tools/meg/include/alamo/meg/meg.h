// alamo_meg - clean-room reader for Petroglyph Alamo-engine MEG archives.
// No dependencies beyond the C++20 standard library and the host OS file-mapping API.
//
// Supported container versions (see docs/assets/README.md for byte layouts):
//   V1  - Empire at War / Forces of Corruption retail archives (all shipped files).
//   V2  - 0xFFFFFFFF / 0x3F7D70A4 header, 20-byte records.
//   V3  - 0xFFFFFFFF / 0x3F7D70A4 header + name-table size, 20-byte records (u16 flags first) with
//         per-entry flags. 0x8FFFFFFF id = encrypted archive (detected, not decrypted).
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace alamo::meg {

enum class Version : uint8_t { V1 = 1, V2 = 2, V3 = 3 };

// Engine filename hash: CRC-32/IEEE (reflected poly 0xEDB88320, init and xorout 0xFFFFFFFF)
// over the upper-cased name with '/' normalised to '\\'.
uint32_t crc32(const void* data, size_t len, uint32_t seed = 0);
uint32_t name_crc(std::string_view name);
std::string normalize_name(std::string_view name);  // upper-case + backslashes

struct Entry {
    std::string name;      // as stored (retail archives store upper-case, '\\' separated)
    uint32_t crc = 0;      // name_crc(name); record table is sorted ascending by this
    uint32_t index = 0;    // record index field (== position in retail archives)
    uint32_t size = 0;     // bytes
    uint32_t offset = 0;   // absolute offset of data from start of archive
    uint32_t name_index = 0;
    uint16_t flags = 0;    // V3 only; bit0 = encrypted entry
    bool encrypted() const { return (flags & 1) != 0; }
};

// Read-only byte source. Memory-mapped when possible, falls back to buffered stream reads.
class Source {
public:
    virtual ~Source() = default;
    virtual uint64_t size() const = 0;
    // Returns false on short read / out of range.
    virtual bool read(uint64_t offset, void* dst, size_t len) const = 0;
    // Non-null for mapped sources: zero-copy view of the whole file.
    virtual const uint8_t* data() const { return nullptr; }

    enum class Mode { Auto, Mapped, Stream };
    static std::unique_ptr<Source> open(const std::string& path, Mode mode, std::string* err);
    static std::unique_ptr<Source> from_memory(std::vector<uint8_t> bytes);
};

class Archive {
public:
    // Opens and parses the header + tables. Returns nullptr and sets *err on failure.
    static std::unique_ptr<Archive> open(const std::string& path,
                                         Source::Mode mode = Source::Mode::Auto,
                                         std::string* err = nullptr);
    static std::unique_ptr<Archive> open(std::unique_ptr<Source> src, std::string* err = nullptr);

    Version version() const { return version_; }
    // True for 0x8FFFFFFF-id archives. Their tables are encrypted: entries() will be empty.
    bool encrypted() const { return encrypted_; }
    const std::vector<Entry>& entries() const { return entries_; }
    const std::string& path() const { return path_; }

    // Engine-style lookup: CRC of normalised name, binary search on the CRC-sorted
    // record table, then confirm by case-insensitive name compare (handles collisions).
    const Entry* find(std::string_view name) const;

    // Copies entry bytes. False if encrypted or out of bounds.
    bool read(const Entry& e, std::vector<uint8_t>& out) const;
    // Zero-copy view (only for mapped sources); empty span otherwise.
    std::span<const uint8_t> view(const Entry& e) const;

    // Structural self-check: CRC mismatches, sort order, bounds. Empty == OK.
    std::vector<std::string> verify() const;

private:
    bool parse(std::string* err);
    std::unique_ptr<Source> src_;
    std::string path_;
    Version version_ = Version::V1;
    bool encrypted_ = false;
    std::vector<Entry> entries_;  // in record (CRC-sorted) order
};

// Ordered overlay of archives, mirroring the engine's MegaFile manager: archives mounted
// later shadow earlier ones (megafiles.xml order, then Patch, Patch2, 64Patch).
class FileSystem {
public:
    bool mount(const std::string& path, std::string* err = nullptr);
    void mount(std::unique_ptr<Archive> a) { archives_.push_back(std::move(a)); }
    struct Hit { const Archive* archive; const Entry* entry; };
    // Last-mounted archive containing the name wins.
    bool find(std::string_view name, Hit& out) const;
    bool read(std::string_view name, std::vector<uint8_t>& out) const;
    const std::vector<std::unique_ptr<Archive>>& archives() const { return archives_; }

private:
    std::vector<std::unique_ptr<Archive>> archives_;
};

// Case-insensitive glob ('*', '?'); separators '/' and '\\' compare equal.
bool glob_match(std::string_view pattern, std::string_view text);

}  // namespace alamo::meg
