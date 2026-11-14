// Where audio files come from.
//
//   SFX samples   : bare leaf names (EGL_MDU_1.WAV) inside Data\Audio\SFX\sfx2d_<language>.meg,
//                   sfx2d_non_localized.meg and sfx3d_non_localized.meg (docs/assets/CATALOG.md: these
//                   archives store bare names and are NOT listed in MegaFiles.xml, so the VFS does not
//                   mount them). FoC's archives (mod copies first, through the VFS) are searched before
//                   EaW's GameData ones. Search order [port]: FoC (mod copy first) 2D-localized, 2D, 3D, then the
//                   same for EaW (fallback for EaW-only names, AUDIO.md §5), whatever the event kind;
//                   then the loose Data\Audio\SFX\<name>.
//   Music         : Data\Audio\Music\<file>           (music.meg)
//   Speech        : Data\Audio\Speech\<Language>\<file> (<language>speech.meg)
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/core/file_provider.hpp"

namespace alamo::vfs {
class Vfs;
}
namespace alamo::meg {
class Archive;
struct Entry;
}

namespace alamo::audio {

enum class Bus : uint8_t { Sfx2D = 0, Sfx3D = 1, Music = 2, Speech = 3 };

struct SampleRef {
    Bus bus = Bus::Sfx2D;
    std::string file;  // as written in the XML (case-insensitive)
};

class SampleSource {
public:
    virtual ~SampleSource() = default;
    virtual bool read(const SampleRef& ref, std::vector<uint8_t>& out) const = 0;
    virtual bool exists(const SampleRef& ref) const = 0;
    virtual std::string origin(const SampleRef& ref) const = 0;  // diagnostics; "" if not found
};

struct GameSampleConfig {
    std::string language = "English";  // speech folder and sfx2d_<language>.meg
    // Host paths of the SFX archives in search order. default_sfx_megs() fills it from a Vfs.
    std::vector<std::string> sfx_megs;
};

class GameSampleSource final : public SampleSource {
public:
    // `files` must outlive this object. Archives that fail to open are skipped (see warnings()).
    GameSampleSource(const FileProvider& files, GameSampleConfig cfg);
    ~GameSampleSource() override;

    bool read(const SampleRef& ref, std::vector<uint8_t>& out) const override;
    bool exists(const SampleRef& ref) const override;
    std::string origin(const SampleRef& ref) const override;

    // FoC side through the VFS (mod -> FoC loose), then <EaW>\Data\Audio\SFX. Missing ones are skipped.
    static std::vector<std::string> default_sfx_megs(const vfs::Vfs& vfs, std::string_view language);

    std::string path_for(const SampleRef& ref) const;  // game-relative path used for Music/Speech
    size_t archive_count() const { return archives_.size(); }
    const std::vector<std::string>& warnings() const { return warnings_; }

private:
    const meg::Entry* find_in_archives(std::string_view name, const meg::Archive** owner) const;

    const FileProvider* files_;
    GameSampleConfig cfg_;
    std::vector<std::unique_ptr<meg::Archive>> archives_;
    std::vector<std::string> warnings_;
};

}  // namespace alamo::audio
