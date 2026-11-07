// Read-only file access used by every loader. The real implementation is the VFS (engine/vfs),
// which applies the verified Data\ resolution order. DirectoryProvider is a plain loose-folder
// provider for tests and tools.
//
// Path convention: game-relative, e.g. "Data\\XML\\Planets.xml". Lookup is ASCII
// case-insensitive, '/' == '\\', and a leading ".\\" is ignored (the original passes
// ".\\Data\\XML\\..." in places).
#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
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

// Loose folder rooted at `root` (the folder that contains "Data"). Indexes the tree once at
// construction so lookups are case-insensitive on every OS.
class DirectoryProvider final : public FileProvider {
public:
    explicit DirectoryProvider(std::string root);
    bool read(std::string_view path, std::vector<uint8_t>& out) const override;
    bool exists(std::string_view path) const override;
    std::string origin(std::string_view path) const override;
    size_t file_count() const { return index_.size(); }
    const std::string& root() const { return root_; }

private:
    std::string root_;
    std::unordered_map<std::string, std::string> index_;  // normalized relative -> real path
};

// UTF-8 std::string <-> std::filesystem::path without the deprecated u8path / char8_t friction.
std::filesystem::path path_from_utf8(std::string_view utf8);
// Generic (forward-slash) form: for logs and game-relative names. Do not feed the result of a "\\?\"
// path back into file calls -- use the std::filesystem::path itself (long_path repairs it if one does).
std::string path_to_utf8(const std::filesystem::path& p);

// Host path usable beyond MAX_PATH (260 characters). Windows: absolute, normalised ('/' -> '\\',
// "." / ".." resolved) and prefixed with "\\?\" ("\\?\UNC\server\share\..." for UNC paths); paths
// already in "\\?\" form are returned with '/' rewritten to '\\' (also the generic forms "//?/..." and
// "/?/..." that a UTF-8 round trip produces); "\\.\" device paths and empty paths are returned
// unchanged. Elsewhere a no-op. Pass the result to every std::filesystem / fstream / Win32 file call
// that may see long paths.
std::filesystem::path long_path(const std::filesystem::path& p);

// Directory listing / recursive delete that also work beyond MAX_PATH. Use these instead of
// std::filesystem::directory_iterator / remove_all on long paths: MinGW libstdc++'s directory
// iterator cannot open "\\?\" paths over 260 characters (it lists the current directory instead,
// and remove_all then recurses forever). Windows: Win32 FindFirstFileExW / DeleteFileW /
// RemoveDirectoryW on long_path(). Symlinks and junctions (name-surrogate reparse points) are removed
// as links, never followed; other reparse points (cloud placeholders) are recursed into. Read-only
// entries are made writable first. A delete blocked by another process (sharing violation, pending
// delete, access denied) is retried with back-off for ~0.1 s, then fails with that error. Elsewhere:
// std::filesystem. list_directory returns `dir / name` for each entry (no "." / ".."), unsorted;
// empty + ec on error.
std::vector<std::filesystem::path> list_directory(const std::filesystem::path& dir, std::error_code& ec);
// Returns the number of entries removed; a missing path is 0 with ec cleared (as std::filesystem).
// Windows: refuses (static_cast<uintmax_t>(-1), errc::invalid_argument, nothing deleted) a volume or
// share root, a drive-relative path ("C:", "C:x"), a component with a trailing '.' or ' ' (Win32
// strips them: "cache." would delete "cache"), a final "." / "..", and "\\.\" device paths.
std::uintmax_t remove_all_long(const std::filesystem::path& p, std::error_code& ec);

// Whole-file read from the host file system (no VFS rules; long paths OK). False on error.
// Windows: opened with FILE_SHARE_READ | WRITE | DELETE, so it never blocks an atomic replace.
bool read_host_file(const std::filesystem::path& path, std::vector<uint8_t>& out);
bool read_host_file(const std::string& path, std::vector<uint8_t>& out);  // UTF-8
bool read_host_file(const char* path, std::vector<uint8_t>& out);         // UTF-8

}  // namespace alamo
