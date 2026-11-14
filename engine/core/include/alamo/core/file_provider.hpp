// Read-only file access used by every loader. The real implementation is the VFS (engine/vfs),
// which applies the verified Data\ resolution order. DirectoryProvider is a plain loose-folder
// provider for tests and tools.
//
// Path convention: game-relative, e.g. "Data\\XML\\Planets.xml". Lookup is ASCII
// case-insensitive, '/' == '\\', and a leading ".\\" is ignored (the original passes
// ".\\Data\\XML\\..." in places).
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace alamo {

// Upper-case, '/' -> '\\', strip leading ".\\" and duplicate separators.
std::string normalize_path(std::string_view path);
// Asset name -> path: adds `dir` when `name` has no '\\' or '/', and `ext` when its last part has no '.';
// then normalize_path. ("ei_trooper", "Data\\Art\\Models\\", ".alo") -> "DATA\\ART\\MODELS\\EI_TROOPER.ALO".
std::string with_default_dir_ext(std::string_view name, std::string_view dir, std::string_view ext);

class FileProvider {
public:
    virtual ~FileProvider() = default;
    // Whole-file read. Returns false if not found (out is left empty).
    virtual bool read(std::string_view path, std::vector<uint8_t>& out) const = 0;
    virtual bool exists(std::string_view path) const = 0;
    // Human-readable origin of the file that would be returned ("" if not found). For diagnostics.
    virtual std::string origin(std::string_view path) const = 0;
};

// Whole-file read from the host file system (no VFS rules; long paths OK). False on error.
// Windows: opened with FILE_SHARE_READ | WRITE | DELETE, so it never blocks an atomic replace.
bool read_host_file(const std::string& path, std::vector<uint8_t>& out);  // UTF-8
bool read_host_file(const char* path, std::vector<uint8_t>& out);         // UTF-8
// The std::filesystem::path overload is in alamo/core/host_fs.hpp.

}  // namespace alamo
