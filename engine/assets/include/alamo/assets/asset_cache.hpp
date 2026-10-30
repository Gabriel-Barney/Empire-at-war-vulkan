// alamo_assets - thread-safe asset cache keyed by case-insensitive name.
//
// Keys: the normalised VFS path (alamo::normalize_path), hashed with alamo::name_crc; CRC
// collisions are resolved by comparing the full normalised key. Each key is loaded exactly once
// (per-key once semantics): concurrent requests for the same key block until the first loader
// finishes and then share the result; different keys load in parallel. A failed load is cached
// too (no retry storm) and recorded in failures().
//
// Lifetime: the FileProvider must outlive the cache. Returned shared_ptrs stay valid after the
// cache is destroyed.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/assets/model.hpp"
#include "alamo/assets/skeleton.hpp"
#include "alamo/assets/texture.hpp"

namespace alamo {
class FileProvider;
}

namespace alamo::assets {

class AssetCache {
public:
    // model_options apply to every model() load (e.g. the renderer's effect vertex-type override,
    // ModelLoadOptions::vertex_type_override; it is called on the loading threads).
    explicit AssetCache(const FileProvider& files, ModelLoadOptions model_options = {});
    ~AssetCache();
    AssetCache(const AssetCache&) = delete;
    AssetCache& operator=(const AssetCache&) = delete;

    // nullptr if missing or malformed (see failures()). `name` as for model_path().
    std::shared_ptr<const ModelAsset> model(std::string_view name);
    // nullptr if missing or malformed. `name` as for animation_path().
    std::shared_ptr<const AnimationClip> animation(std::string_view name);
    // Never nullptr: a missing or broken texture returns a shared fallback for `role`
    // (TextureAsset::is_fallback = true). Lookup order as load_texture(), but the candidate is
    // chosen with FileProvider::exists() and the decoded texture is cached by the RESOLVED file:
    // each written name (normalised) maps to the first existing candidate, and every name that
    // resolves to the same file (`W.TGA`, `W.DDS`, `Data\Art\Textures\w.tga` -> one file) shares a
    // single decode / GPU copy. Deviation: the original keys textures by the lower-case base name
    // cut at the first '.' (SPACE_AND_OPEN.md B.2 step 1), so `x.tga` and `x.dds` always share an
    // entry and the first request decides which file it holds; with concurrent loads that would
    // depend on thread timing. Keying by the resolved file gives the same sharing whenever the
    // names find the same file and stays independent of request order. A missing name is recorded
    // in failures() under the written name, a broken file under its resolved path (once).
    std::shared_ptr<const TextureAsset> texture(std::string_view name, TextureRole role = TextureRole::Color);

    struct Failure {
        std::string kind;  // "model", "animation", "texture"
        std::string key;   // normalised key
        std::string reason;
    };
    // Every failed load so far, sorted by (kind, key). Independent of load order / threading.
    std::vector<Failure> failures() const;

    struct Stats {
        std::size_t models = 0, animations = 0, textures = 0;  // distinct keys (texture names) requested
        std::size_t failed = 0;
        std::size_t texture_files = 0;  // distinct resolved texture files (decodes)
    };
    Stats stats() const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace alamo::assets
