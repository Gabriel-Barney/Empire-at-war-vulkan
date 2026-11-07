// "Chunks of mini-chunks" files: terrain material sets (.tem) and environment sets (.tee).
//
//   .tem: chunk 0x02 (children) -> 64 x chunk 0x03 (one terrain texture slot each; unused slots
//         have empty texture names)
//   .tee: sequence of leaf chunks 0x06 (one lighting environment each), terminated by an empty
//         chunk 0x05
//
// The generic Record tree is exact. The typed field meanings live in the engine reader
// (engine/terrain ted.hpp: TerrainMaterial, Environment); this library keeps only the raw records.
#pragma once

#include "alamo/formats/chunk.hpp"

#include <optional>
#include <string>
#include <vector>

namespace alamo::minifile {

struct Record {
    std::uint32_t chunk_id = 0;
    std::vector<std::uint8_t> mini_ids;
    std::vector<std::vector<std::uint8_t>> mini_data;
    const std::vector<std::uint8_t>* get(std::uint8_t id) const;
    std::optional<float> f32(std::uint8_t id) const;
    std::optional<std::uint32_t> u32(std::uint8_t id) const;   // 1-, 2- or 4-byte payloads
    std::optional<Vec3> vec3(std::uint8_t id) const;
    std::optional<std::string> str(std::uint8_t id) const;
    // Mini-chunks that look like printable NUL-terminated strings (e.g. texture names).
    std::vector<std::string> strings() const;
};

struct File {
    std::vector<Record> records;   // in file order, flattened
};

File parse(Bytes data);
File load(const std::filesystem::path& path);

}  // namespace alamo::minifile
