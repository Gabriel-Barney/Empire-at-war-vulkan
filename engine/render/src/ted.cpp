#include "alamo/render/ted.hpp"

#include <vector>

#include "alamo/core/file_provider.hpp"

namespace alamo::render {

namespace {
bool from_map(const terrain::MapData& m, MapEnvironments& out, std::string* err) {
    static_cast<terrain::EnvironmentSet&>(out) = m.environments;
    out.space = m.is_space();
    if (out.environments.empty()) {
        if (err) *err = "map has no TEE environment records";
        return false;
    }
    return true;
}
}  // namespace

bool read_map_environments(std::span<const uint8_t> ted, MapEnvironments& out, std::string* err) {
    out = MapEnvironments{};
    terrain::MapData m;
    terrain::ParseOptions po;
    po.environments_only = true;  // a malformed terrain chunk cannot fail the environment read
    if (!terrain::parse_ted(ted, m, err, po)) return false;
    return from_map(m, out, err);
}

bool load_map_environments(const FileProvider& files, std::string_view map, MapEnvironments& out, std::string* err) {
    out = MapEnvironments{};
    terrain::MapData m;
    terrain::ParseOptions po;
    po.environments_only = true;  // a malformed terrain chunk cannot fail the environment read
    if (!terrain::load_map(files, map, m, err, po)) return false;
    return from_map(m, out, err);
}

}  // namespace alamo::render
