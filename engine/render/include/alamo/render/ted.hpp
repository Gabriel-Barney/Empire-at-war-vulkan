// alamo_render - map environments, now read by engine/terrain (the canonical .ted reader). This header
// keeps the render-side names: Environment is terrain::Environment (a superset of the old record, same
// member names / types / defaults), MapEnvironments is the terrain EnvironmentSet plus the space flag
// (no terrain chunk 0x101 -> fills off, RENDER.md 1.2).
#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "alamo/terrain/ted.hpp"

namespace alamo {
class FileProvider;
}

namespace alamo::render {

using Environment = terrain::Environment;
using terrain::decode_environment;
using terrain::map_path;

struct MapEnvironments : terrain::EnvironmentSet {
    bool space = false;  // no terrain chunk 0x101
};
// Environments of a whole .ted image (objects and zones are skipped).
bool read_map_environments(std::span<const uint8_t> ted, MapEnvironments& out, std::string* err = nullptr);
bool load_map_environments(const FileProvider& files, std::string_view map, MapEnvironments& out, std::string* err = nullptr);

}  // namespace alamo::render
