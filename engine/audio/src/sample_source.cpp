#include "alamo/audio/sample_source.hpp"

#include <filesystem>

#include "alamo/core/hash.hpp"
#include "alamo/meg/meg.h"
#include "alamo/vfs/vfs.hpp"

namespace alamo::audio {

const char* bus_name(Bus b) {
    switch (b) {
        case Bus::Sfx2D: return "sfx2d";
        case Bus::Sfx3D: return "sfx3d";
        case Bus::Music: return "music";
        case Bus::Speech: return "speech";
    }
    return "?";
}

GameSampleSource::GameSampleSource(const FileProvider& files, GameSampleConfig cfg)
    : files_(&files), cfg_(std::move(cfg)) {
    for (const std::string& p : cfg_.sfx_megs) {
        std::string err;
        auto a = meg::Archive::open(p, meg::Source::Mode::Auto, &err);
        if (!a) {
            warnings_.push_back("sfx archive " + p + ": " + err);
            continue;
        }
        archives_.push_back(std::move(a));
    }
}

GameSampleSource::~GameSampleSource() = default;

std::string GameSampleSource::path_for(const SampleRef& ref) const {
    switch (ref.bus) {
        case Bus::Music: return "Data\\Audio\\Music\\" + ref.file;
        case Bus::Speech: return "Data\\Audio\\Speech\\" + cfg_.language + "\\" + ref.file;
        case Bus::Sfx2D:
        case Bus::Sfx3D: break;
    }
    return "Data\\Audio\\SFX\\" + ref.file;
}

// SFX: the SFX archives by leaf name, then the loose Data\Audio\SFX\<name> [port]. Music and speech:
// streamed through the normal file layer only (AUDIO.md §5), so a speech event naming a file that only
// exists in an SFX archive does not play, as in the original.
const meg::Entry* GameSampleSource::find_in_archives(std::string_view name, const meg::Archive** owner) const {
    for (const auto& a : archives_) {
        if (const meg::Entry* e = a->find(name)) {
            *owner = a.get();
            return e;
        }
    }
    return nullptr;
}

bool GameSampleSource::read(const SampleRef& ref, std::vector<uint8_t>& out) const {
    out.clear();
    if (ref.bus == Bus::Sfx2D || ref.bus == Bus::Sfx3D) {
        const meg::Archive* a = nullptr;
        if (const meg::Entry* e = find_in_archives(ref.file, &a)) return a->read(*e, out);
    }
    return files_->read(path_for(ref), out);
}

bool GameSampleSource::exists(const SampleRef& ref) const {
    const meg::Archive* a = nullptr;
    if ((ref.bus == Bus::Sfx2D || ref.bus == Bus::Sfx3D) && find_in_archives(ref.file, &a)) return true;
    return files_->exists(path_for(ref));
}

std::string GameSampleSource::origin(const SampleRef& ref) const {
    const meg::Archive* a = nullptr;
    if (ref.bus == Bus::Sfx2D || ref.bus == Bus::Sfx3D)
        if (const meg::Entry* e = find_in_archives(ref.file, &a)) return a->path() + "#" + e->name;
    return files_->origin(path_for(ref));
}
std::vector<std::string> GameSampleSource::default_sfx_megs(const vfs::Vfs& vfs, std::string_view language) {
    const std::string names[3] = {"sfx2d_" + std::string(language) + ".meg", "sfx2d_non_localized.meg",
                                  "sfx3d_non_localized.meg"};
    std::vector<std::string> out;
    for (const std::string& n : names) {
        auto res = vfs.resolve("Data\\Audio\\SFX\\" + n);
        // Loose layers only: a MEG inside a MEG is not a host file.
        if (res && res->layer != vfs::Vfs::Resolution::Meg) out.push_back(res->location);
    }
    if (!vfs.eaw_root().empty()) {
        for (const std::string& n : names) {
            std::filesystem::path p = path_from_utf8(vfs.eaw_root()) / "Data" / "Audio" / "SFX" / n;
            std::error_code ec;
            if (std::filesystem::exists(p, ec)) {
                const std::string s = path_to_utf8(p);
                bool dup = false;
                for (const std::string& o : out) dup = dup || iequals(o, s);
                if (!dup) out.push_back(s);
            }
        }
    }
    return out;
}

}  // namespace alamo::audio
