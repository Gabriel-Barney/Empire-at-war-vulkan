#include "alamo/render/objects.hpp"

#include <vector>

#include "alamo/core/ascii.hpp"
#include "alamo/data/values.hpp"
#include "alamo/vfs/vfs.hpp"

namespace alamo::render {

std::string find_object_model(const FileProvider& files, std::string_view object_type) {
    if (object_type.empty()) return {};
    const auto* vfs = dynamic_cast<const vfs::Vfs*>(&files);
    if (!vfs) return {};
    const std::string want = "NAME=\"" + ascii_upper(object_type) + "\"";
    std::vector<uint8_t> bytes;
    for (const std::string& path : vfs->list("Data\\XML\\*.XML")) {
        if (!files.read(path, bytes)) continue;
        const std::string text = ascii_upper(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        const size_t at = text.find(want);
        if (at == std::string::npos) continue;
        // The element ends at the next object definition (next Name=" attribute) at the latest.
        const size_t next = text.find("NAME=\"", at + want.size());
        const size_t end = next == std::string::npos ? text.size() : next;
        for (const char* tag : {"<SPACE_MODEL_NAME>", "<LAND_MODEL_NAME>", "<GALACTIC_MODEL_NAME>"}) {
            const size_t t = text.find(tag, at);
            if (t == std::string::npos || t > end) continue;
            const size_t v0 = t + std::string_view(tag).size();
            const size_t v1 = text.find('<', v0);
            if (v1 == std::string::npos) continue;
            // Value from the original bytes (case kept).
            std::string value(data::trim(std::string_view(reinterpret_cast<const char*>(bytes.data()) + v0, v1 - v0)));
            if (!value.empty()) return value;
        }
    }
    return {};
}

}  // namespace alamo::render
