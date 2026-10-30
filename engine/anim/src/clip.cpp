#include "alamo/anim/clip.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <exception>
#include <stdexcept>

#include "alamo/core/file_provider.hpp"
#include "alamo/core/hash.hpp"
#include "alamo/formats/ala.hpp"
#include "alamo/formats/alo.hpp"
#include "alamo/formats/chunk.hpp"

namespace alamo::anim {

namespace {

Mat34 to_mat(const alamo::Mat34& m) {
    Mat34 o;
    std::memcpy(o.m, m.m, sizeof o.m);
    return o;
}

Float3 to_f3(const alamo::Vec3& v) { return {v.x, v.y, v.z}; }

// "EI_TROOPER" -> "DATA\ART\MODELS\EI_TROOPER.ALO"; names with a separator are only normalised.
std::string model_path(std::string_view name) {
    std::string n = normalize_path(name);
    if (n.find('\\') == std::string::npos) n = "DATA\\ART\\MODELS\\" + n;
    const size_t slash = n.find_last_of('\\');
    if (n.find('.', slash == std::string::npos ? 0 : slash) == std::string::npos) n += ".ALO";
    return n;
}

std::string model_dir(std::string_view model) {
    const std::string p = model_path(model);
    const size_t slash = p.find_last_of('\\');
    return slash == std::string::npos ? std::string() : p.substr(0, slash + 1);
}

// Raw i16[4] of mini 0x11 per bone (ala::BoneTrack only keeps it normalised), in bone order.
std::vector<std::array<int16_t, 4>> default_rotations(std::span<const uint8_t> bytes) {
    std::vector<std::array<int16_t, 4>> out;
    auto top = top_level_chunks(bytes);
    auto root = top.next();
    if (!root || root->id != 0x1000 || !root->has_children) return out;
    auto cur = root->children();
    while (auto ch = cur.next()) {
        if (ch->id != 0x1002) continue;
        std::array<int16_t, 4> q{0, 0, 0, 32767};
        auto bc = ch->children();
        while (auto b = bc.next()) {
            if (b->id != 0x1003) continue;
            auto mc = b->minis();
            while (auto m = mc.next())
                if (m->id == 0x11 && m->data.size() == 8) std::memcpy(q.data(), m->data.data(), 8);
        }
        out.push_back(q);
    }
    return out;
}

}  // namespace

// ---- Skeleton ----------------------------------------------------------------------------------------

int32_t Skeleton::find(std::string_view name) const {
    for (size_t i = 0; i < names.size(); ++i)
        if (iequals(names[i], name)) return static_cast<int32_t>(i);
    return -1;
}

bool skeleton_from_alo(std::span<const uint8_t> bytes, Skeleton& out, std::string* error) {
    try {
        const alo::AloFile f = alo::parse(bytes);
        if (f.kind != alo::AloFile::Kind::Model) throw std::runtime_error("particle system, not a model");
        const alo::Skeleton& s = f.model.skeleton;
        Skeleton k;
        k.names = s.names;
        k.parents = s.parents;
        k.billboard_mode = s.billboard_mode;
        k.billboard_mode.resize(s.size(), 0);
        k.bind_local.reserve(s.size());
        for (size_t b = 0; b < s.size(); ++b) {
            if (s.parents[b] >= static_cast<int32_t>(b)) throw std::runtime_error("bone parent is not an earlier bone");
            k.bind_local.push_back(b < s.local.size() ? to_mat(s.local[b]) : Mat34{});
        }
        out = std::move(k);
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

Skeleton skeleton_from_arrays(std::span<const std::string> names, std::span<const int32_t> parents, std::span<const uint32_t> billboard_mode,
                              std::span<const Mat34> bind_local) {
    Skeleton k;
    const size_t n = names.size();
    k.names.assign(names.begin(), names.end());
    k.parents.assign(n, -1);
    k.billboard_mode.assign(n, 0);
    k.bind_local.assign(n, Mat34{});
    for (size_t i = 0; i < n; ++i) {
        if (i < parents.size() && parents[i] < static_cast<int32_t>(i)) k.parents[i] = parents[i];
        if (i < billboard_mode.size()) k.billboard_mode[i] = billboard_mode[i];
        if (i < bind_local.size()) k.bind_local[i] = bind_local[i];
    }
    return k;
}

// ---- Clip --------------------------------------------------------------------------------------------

void Clip::sample(uint32_t track, uint32_t a, uint32_t b, float t, Quat& rot, Float3& trans) const {
    const ClipTrack& tr = tracks[track];
    const int16_t* ra = &rot_raw[static_cast<size_t>(tr.rot_first) * 4];
    const int16_t* rb = ra;
    if (tr.rot_animated) {
        ra = &rot_raw[(static_cast<size_t>(tr.rot_first) + a) * 4];
        rb = &rot_raw[(static_cast<size_t>(tr.rot_first) + b) * 4];
    }
    rot = nlerp_fast(dequantize_rotation(ra), dequantize_rotation(rb), t);
    if (tr.trans_animated) {
        const Float3 ta = dequantize_translation(tr.trans_offset, tr.trans_scale, &trans_raw[(static_cast<size_t>(tr.trans_first) + a) * 3]);
        const Float3 tb = dequantize_translation(tr.trans_offset, tr.trans_scale, &trans_raw[(static_cast<size_t>(tr.trans_first) + b) * 3]);
        trans = lerp(ta, tb, t);
    } else {
        trans = tr.trans_offset;
    }
}

bool Clip::visible(uint32_t track, uint32_t frame) const {
    const ClipTrack& tr = tracks[track];
    if (!tr.has_visibility) return true;
    const size_t byte = static_cast<size_t>(tr.vis_first) + (frame >> 3);
    return byte < vis_bits.size() && ((vis_bits[byte] >> (frame & 7u)) & 1u) != 0;
}

bool parse_clip(std::span<const uint8_t> bytes, std::string_view name, Clip& out, std::string* error) {
    try {
        const ala::Animation a = ala::parse(bytes);
        if (!(a.fps == a.fps)) throw std::runtime_error("fps is NaN");
        const std::vector<std::array<int16_t, 4>> defaults = default_rotations(bytes);
        Clip c;
        c.name = normalize_path(name);
        c.name_crc = name_crc(c.name);
        c.frame_count = a.num_frames;
        c.fps = a.fps;
        c.bone_count = a.num_bones;
        const size_t nf = a.num_frames;
        const size_t vis_bytes = (nf + 7) / 8;
        constexpr size_t kMaxRawElements = size_t(64) << 20;  // guard against absurd block indexing
        c.tracks.reserve(a.bones.size());
        for (size_t i = 0; i < a.bones.size(); ++i) {
            const ala::BoneTrack& src = a.bones[i];
            ClipTrack t;
            t.bone_name = src.name;
            t.file_bone_index = src.bone_index;
            t.overlay_excluded = src.overlay_excluded != 0;
            t.trans_offset = to_f3(src.translation_offset);
            t.trans_scale = to_f3(src.translation_scale);
            t.rot_animated = a.is_rotation_animated(i) && nf > 0;
            t.trans_animated = a.is_translation_animated(i) && nf > 0;
            // Rotation keys.
            t.rot_first = static_cast<uint32_t>(c.rot_raw.size() / 4);
            if (a.layout == ala::Layout::Block) {
                if (t.rot_animated) {
                    for (size_t f = 0; f < nf; ++f) {
                        const size_t at = f * a.rotation_block + src.rotation_index;
                        if (at + 4 > a.rotation_data.size()) throw std::runtime_error("rotation index out of data");
                        c.rot_raw.insert(c.rot_raw.end(), a.rotation_data.begin() + static_cast<std::ptrdiff_t>(at),
                                         a.rotation_data.begin() + static_cast<std::ptrdiff_t>(at + 4));
                    }
                } else {
                    const std::array<int16_t, 4> q = i < defaults.size() ? defaults[i] : std::array<int16_t, 4>{0, 0, 0, 32767};
                    c.rot_raw.insert(c.rot_raw.end(), q.begin(), q.end());
                }
            } else {
                if (t.rot_animated) {
                    if (src.rotation_data.size() < nf * 4) throw std::runtime_error("per-bone rotation track too short");
                    c.rot_raw.insert(c.rot_raw.end(), src.rotation_data.begin(), src.rotation_data.begin() + static_cast<std::ptrdiff_t>(nf * 4));
                } else if (src.rotation_data.size() >= 4) {
                    c.rot_raw.insert(c.rot_raw.end(), src.rotation_data.begin(), src.rotation_data.begin() + 4);
                } else {
                    const int16_t id[4] = {0, 0, 0, 32767};
                    c.rot_raw.insert(c.rot_raw.end(), id, id + 4);
                }
            }
            // Translation keys.
            t.trans_first = static_cast<uint32_t>(c.trans_raw.size() / 3);
            if (t.trans_animated) {
                if (a.layout == ala::Layout::Block) {
                    for (size_t f = 0; f < nf; ++f) {
                        const size_t at = f * a.translation_block + src.translation_index;
                        if (at + 3 > a.translation_data.size()) throw std::runtime_error("translation index out of data");
                        c.trans_raw.insert(c.trans_raw.end(), a.translation_data.begin() + static_cast<std::ptrdiff_t>(at),
                                           a.translation_data.begin() + static_cast<std::ptrdiff_t>(at + 3));
                    }
                } else {
                    if (src.translation_data.size() < nf * 3) throw std::runtime_error("per-bone translation track too short");
                    c.trans_raw.insert(c.trans_raw.end(), src.translation_data.begin(),
                                       src.translation_data.begin() + static_cast<std::ptrdiff_t>(nf * 3));
                }
            }
            // Visibility bits.
            t.vis_first = static_cast<uint32_t>(c.vis_bits.size());
            if (!src.visibility.empty() && src.visibility.size() >= vis_bytes && vis_bytes > 0) {
                t.has_visibility = true;
                c.vis_bits.insert(c.vis_bits.end(), src.visibility.begin(), src.visibility.begin() + static_cast<std::ptrdiff_t>(vis_bytes));
            }
            if (c.rot_raw.size() > kMaxRawElements || c.trans_raw.size() > kMaxRawElements)
                throw std::runtime_error("animation decodes to too many keys");
            c.tracks.push_back(std::move(t));
        }
        out = std::move(c);
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

std::string clip_file_name(std::string_view base, int32_t type, uint32_t index) {
    char nn[16];
    std::snprintf(nn, sizeof nn, "%02u", index);
    std::string s(base);
    s += '_';
    s += anim_type_suffix(type);
    s += '_';
    s += nn;
    s += ".ALA";
    return normalize_name(s);
}

std::string model_base_name(std::string_view model) {
    const size_t slash = model.find_last_of("\\/");
    std::string_view stem = slash == std::string_view::npos ? model : model.substr(slash + 1);
    const size_t dot = stem.find_last_of('.');
    if (dot != std::string_view::npos) stem = stem.substr(0, dot);
    return normalize_name(stem);
}

// ---- ClipCache ---------------------------------------------------------------------------------------

std::shared_ptr<const Clip> ClipCache::get(const FileProvider& files, const std::string& path) {
    const std::string key = normalize_path(path);
    {
        std::lock_guard<std::mutex> lock(mu_);
        const auto it = clips_.find(key);
        if (it != clips_.end()) return it->second;
    }
    std::shared_ptr<const Clip> made;
    std::vector<uint8_t> bytes;
    if (files.read(key, bytes)) {
        auto c = std::make_shared<Clip>();
        if (parse_clip(bytes, key, *c)) made = std::move(c);
    }
    std::lock_guard<std::mutex> lock(mu_);
    auto [it, inserted] = clips_.try_emplace(key, made);
    if (inserted && !made) ++failures_;
    return it->second;  // first finisher wins; equal content either way
}

size_t ClipCache::size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return clips_.size();
}

size_t ClipCache::failures() const {
    std::lock_guard<std::mutex> lock(mu_);
    return failures_;
}

// ---- AnimModel ---------------------------------------------------------------------------------------

bool AnimModel::has_any_clip() const {
    for (const auto& v : clips)
        if (!v.empty()) return true;
    return false;
}

void bind_clips(AnimModel& m) {
    DiscoveryStats st;
    st.failed = m.stats.failed;
    const size_t bones = m.has_skeleton ? m.skeleton.size() : 0;
    for (auto& list : m.clips) {
        for (ClipRef& r : list) {
            ++st.clips;
            r.track_bone.clear();
            r.bound = r.clip && m.has_skeleton && bones == static_cast<size_t>(r.clip->bone_count) + 1;
            if (!r.bound) {
                ++st.mismatched;
                continue;
            }
            ++st.bound;
            r.track_bone.resize(r.clip->tracks.size(), -1);
            for (size_t i = 0; i < r.clip->tracks.size(); ++i) {
                const ClipTrack& t = r.clip->tracks[i];
                size_t bone = i + 1;
                if (t.file_bone_index >= 1 && t.file_bone_index < bones) bone = t.file_bone_index;
                if (bone >= bones) continue;
                r.track_bone[i] = static_cast<int16_t>(bone);
                if (!iequals(m.skeleton.names[bone], t.bone_name)) ++st.track_name_mismatch;
            }
        }
    }
    m.stats = st;
}

void discover_clips(const FileProvider& files, std::string_view dir, std::string_view base, AnimModel& out, ClipCache* cache) {
    ClipCache local;
    ClipCache& cc = cache ? *cache : local;
    out.base = normalize_name(base);
    uint32_t failed = 0;
    for (int32_t type = 0; type < static_cast<int32_t>(kAnimTypeCount); ++type) {
        auto& list = out.clips[static_cast<size_t>(type)];
        list.clear();
        for (uint32_t nn = 0; nn < 1000; ++nn) {
            const std::string path = normalize_path(std::string(dir) + clip_file_name(out.base, type, nn));
            if (!files.exists(path)) break;  // stop at the first missing index (§A.2)
            ClipRef r;
            r.clip = cc.get(files, path);
            if (!r.clip) {
                // A present but malformed file: the original's loader would fail on it too; keep the index
                // contiguous by stopping here. [port]
                ++failed;
                break;
            }
            list.push_back(std::move(r));
        }
    }
    out.stats.failed = failed;
    bind_clips(out);
}

bool load_anim_model(const FileProvider& files, std::string_view model, std::string_view anim_override, AnimModel& out, ClipCache* cache,
                     std::string* error) {
    out = AnimModel{};
    out.model = model_path(model);
    std::vector<uint8_t> bytes;
    bool ok = true;
    if (!files.read(out.model, bytes)) {
        if (error) *error = "not found: " + out.model;
        ok = false;
    } else {
        ok = skeleton_from_alo(bytes, out.skeleton, error);
    }
    out.has_skeleton = ok;
    const std::string base = anim_override.empty() ? model_base_name(model) : model_base_name(anim_override);
    discover_clips(files, model_dir(model), base, out, cache);
    return ok;
}

}  // namespace alamo::anim
