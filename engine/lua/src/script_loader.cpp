// Script lookup through the FileProvider (VFS) and the root list of the original.
#include <string>

#include "alamo/core/file_provider.hpp"
#include "alamo/lua/script_loader.hpp"

namespace alamo::lua {

namespace {

constexpr std::string_view kRoots[] = {
    "Data\\Scripts\\AI\\",        "Data\\Scripts\\AI\\SpaceMode\\", "Data\\Scripts\\AI\\LandMode\\",
    "Data\\Scripts\\AI\\StoryArc\\", "Data\\Scripts\\Interventions\\", "Data\\Scripts\\Evaluators\\",
    "Data\\Scripts\\GameObject\\", "Data\\Scripts\\Story\\",       "Data\\Scripts\\Library\\",
    "Data\\Scripts\\FreeStore\\", "Data\\Scripts\\Miscellaneous\\",
};

bool ends_with_icase(std::string_view s, std::string_view suffix) {
    if (s.size() < suffix.size()) return false;
    for (size_t i = 0; i < suffix.size(); ++i) {
        char a = s[s.size() - suffix.size() + i], b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

}  // namespace

std::span<const std::string_view> script_roots() { return kRoots; }

bool is_bytecode(std::span<const uint8_t> bytes) {
    return bytes.size() >= 4 && bytes[0] == 0x1B && bytes[1] == 'L' && bytes[2] == 'u' && bytes[3] == 'a';
}

std::string read_script(const FileProvider& files, std::string_view name, std::vector<uint8_t>& out) {
    out.clear();
    std::string n(name);
    for (char& ch : n)
        if (ch == '/') ch = '\\';
    while (n.size() >= 2 && n[0] == '.' && n[1] == '\\') n.erase(0, 2);
    const bool has_ext = ends_with_icase(n, ".lua") || ends_with_icase(n, ".lc");
    for (const std::string_view root : kRoots) {
        if (has_ext) {
            const std::string p = std::string(root) + n;
            if (files.read(p, out)) return p;
            continue;
        }
        for (const char* ext : {".lua", ".lc"}) {
            const std::string p = std::string(root) + n + ext;
            if (files.read(p, out)) return p;
        }
    }
    out.clear();
    return {};
}

}  // namespace alamo::lua
