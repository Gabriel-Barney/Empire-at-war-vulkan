#include "alamo/assets/asset_cache.hpp"

#include <algorithm>
#include <mutex>
#include <unordered_map>

#include "alamo/core/file_provider.hpp"
#include "alamo/core/hash.hpp"
#include "alamo/core/log.hpp"

namespace alamo::assets {

namespace {

template <class T>
struct Slot {
    std::string key;
    std::once_flag once;
    std::shared_ptr<const T> value;  // nullptr = failed
    std::string error;
};

// Name-keyed table of once-slots. The table mutex is held only to find/insert a slot; the load
// itself runs under the slot's once_flag, so different keys load concurrently.
template <class T>
class Table {
public:
    std::shared_ptr<Slot<T>> slot(const std::string& key) {
        const std::uint32_t crc = name_crc(key);
        std::lock_guard<std::mutex> lock(mu_);
        auto& bucket = map_[crc];
        for (const auto& s : bucket)
            if (s->key == key) return s;
        auto s = std::make_shared<Slot<T>>();
        s->key = key;
        bucket.push_back(s);
        ++count_;
        return s;
    }
    std::size_t count() const {
        std::lock_guard<std::mutex> lock(mu_);
        return count_;
    }

private:
    mutable std::mutex mu_;
    std::unordered_map<std::uint32_t, std::vector<std::shared_ptr<Slot<T>>>> map_;
    std::size_t count_ = 0;
};

}  // namespace

struct AssetCache::Impl {
    const FileProvider& files;
    ModelLoadOptions model_options;
    Table<ModelAsset> models;
    Table<AnimationClip> anims;
    Table<std::string> texture_names;  // written name -> resolved path (nullptr = not found)
    Table<TextureAsset> textures;      // keyed by resolved path
    std::shared_ptr<const TextureAsset> fallback[3];

    mutable std::mutex fail_mu;
    std::vector<Failure> fails;  // append order varies with threading; failures() sorts

    Impl(const FileProvider& f, ModelLoadOptions o) : files(f), model_options(std::move(o)) {
        fallback[0] = std::make_shared<const TextureAsset>(make_fallback_texture(TextureRole::Color));
        fallback[1] = std::make_shared<const TextureAsset>(make_fallback_texture(TextureRole::Normal));
        fallback[2] = std::make_shared<const TextureAsset>(make_fallback_texture(TextureRole::White));
    }

    void record(const char* kind, const std::string& key, const std::string& reason) {
        log::warnf("assets", "%s %s: %s", kind, key.c_str(), reason.c_str());
        std::lock_guard<std::mutex> lock(fail_mu);
        fails.push_back({kind, key, reason});
    }
};

AssetCache::AssetCache(const FileProvider& files, ModelLoadOptions model_options)
    : impl_(std::make_unique<Impl>(files, std::move(model_options))) {}
AssetCache::~AssetCache() = default;

std::shared_ptr<const ModelAsset> AssetCache::model(std::string_view name) {
    const std::string key = model_path(name);
    auto s = impl_->models.slot(key);
    std::call_once(s->once, [&] {
        try {
            auto m = std::make_shared<ModelAsset>();
            std::string err;
            if (load_model(impl_->files, key, *m, impl_->model_options, &err)) s->value = std::move(m);
            else s->error = err;
        } catch (const std::exception& e) {  // e.g. bad_alloc; never let call_once rethrow
            s->error = e.what();
        }
        if (!s->value) impl_->record("model", key, s->error);
    });
    return s->value;
}

std::shared_ptr<const AnimationClip> AssetCache::animation(std::string_view name) {
    const std::string key = animation_path(name);
    auto s = impl_->anims.slot(key);
    std::call_once(s->once, [&] {
        try {
            auto a = std::make_shared<AnimationClip>();
            std::string err;
            if (load_animation(impl_->files, key, *a, &err)) s->value = std::move(a);
            else s->error = err;
        } catch (const std::exception& e) {
            s->error = e.what();
        }
        if (!s->value) impl_->record("animation", key, s->error);
    });
    return s->value;
}

std::shared_ptr<const TextureAsset> AssetCache::texture(std::string_view name, TextureRole role) {
    // Two levels: written name -> resolved file (first existing texture_lookup() candidate), then
    // resolved file -> decoded texture. Both depend only on the file set, never on request order.
    const std::string key = normalize_path(name);
    auto n = impl_->texture_names.slot(key);
    std::call_once(n->once, [&] {
        try {
            for (const auto& c : texture_lookup(key)) {
                if (!impl_->files.exists(c.path)) continue;
                n->value = std::make_shared<const std::string>(c.path);  // already normalised
                break;
            }
            if (!n->value) n->error = "not found: " + key;
        } catch (const std::exception& e) {
            n->error = e.what();
        }
        if (!n->value) impl_->record("texture", key, n->error);
    });
    if (!n->value) return impl_->fallback[static_cast<std::size_t>(role)];

    const std::string& file = *n->value;
    auto s = impl_->textures.slot(file);
    std::call_once(s->once, [&] {
        try {
            auto t = std::make_shared<TextureAsset>();
            std::vector<std::uint8_t> bytes;
            std::string err;
            if (!impl_->files.read(file, bytes)) {
                s->error = "read failed";
            } else if (!decode_texture(std::move(bytes), *t, &err)) {
                s->error = err;
            } else {
                t->name = file;
                for (const auto& w : t->warnings) log::warnf("assets", "texture %s: %s", t->name.c_str(), w.c_str());
                s->value = std::move(t);
            }
        } catch (const std::exception& e) {
            s->error = e.what();
        }
        if (!s->value) impl_->record("texture", file, s->error);
    });
    if (s->value) return s->value;
    return impl_->fallback[static_cast<std::size_t>(role)];
}

std::vector<AssetCache::Failure> AssetCache::failures() const {
    std::vector<Failure> out;
    {
        std::lock_guard<std::mutex> lock(impl_->fail_mu);
        out = impl_->fails;
    }
    std::sort(out.begin(), out.end(), [](const Failure& a, const Failure& b) {
        return a.kind != b.kind ? a.kind < b.kind : a.key < b.key;
    });
    return out;
}

AssetCache::Stats AssetCache::stats() const {
    Stats s;
    s.models = impl_->models.count();
    s.animations = impl_->anims.count();
    s.textures = impl_->texture_names.count();
    s.texture_files = impl_->textures.count();
    std::lock_guard<std::mutex> lock(impl_->fail_mu);
    s.failed = impl_->fails.size();
    return s;
}

}  // namespace alamo::assets
