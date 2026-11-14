// Opens the FoC game data for the data-driven galactic tests (ALAMO_FOC_DATA).
//   1. A folder with loose Data\XML (e.g. extracted/foc_data_text.zip unzipped): read directly.
//   2. Otherwise, when the test exe is built with the vfs module (ALAMO_GALACTIC_TESTS_HAVE_VFS): the folder is treated as
//      a FoC install (<...>\corruption) and read through the VFS (MegaFiles.xml + patch MEGs, EaW fallback). A Steam
//      install keeps no loose XML, so before this the data-driven checks (and their goldens) skipped silently there.
// Both routes resolve the same text files for the campaigns the goldens use (checked: the apps/sim galactic HotM golden
// is identical on the Steam install and on the unzipped text data).
#pragma once
#include <memory>
#include <string>

#include "alamo/core/file_provider.hpp"
#include "alamo/core/host_fs.hpp"
#include "alamo/objects/object_types.hpp"
#ifdef ALAMO_GALACTIC_TESTS_HAVE_VFS
#include "alamo/vfs/vfs.hpp"
#endif

namespace alamo::galactic::testing {

// nullptr when `root` is empty or neither route finds Data\XML. `how` names the route taken.
inline std::unique_ptr<FileProvider> open_foc_data(const std::string& root, std::string& how) {
    how.clear();
    if (root.empty()) return nullptr;
    auto loose = std::make_unique<DirectoryProvider>(root);  // long paths OK
    if (loose->exists(objects::ObjectTypeDatabase::kListPath)) {
        how = "loose Data\\XML";
        return loose;
    }
#ifdef ALAMO_GALACTIC_TESTS_HAVE_VFS
    vfs::VfsConfig cfg;
    cfg.foc_root = root;
    std::string err;
    std::unique_ptr<vfs::Vfs> fs = vfs::Vfs::create(cfg, &err);
    if (fs && fs->exists(objects::ObjectTypeDatabase::kListPath)) {
        how = "VFS";
        return fs;
    }
#endif
    return nullptr;
}

}  // namespace alamo::galactic::testing
