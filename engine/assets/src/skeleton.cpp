#include "alamo/assets/skeleton.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <stdexcept>

#include "alamo/assets/model.hpp"
#include "alamo/core/file_provider.hpp"
#include "alamo/core/hash.hpp"
#include "alamo/formats/ala.hpp"

namespace alamo::assets {

// ---- Skeleton -------------------------------------------------------------------------------

void Skeleton::finalize() {
    const std::size_t n = names.size();
    if (parents.size() != n || bind_local.size() != n)
        throw std::runtime_error("skeleton arrays have different sizes");
    visible.resize(n, 1);
    billboard_mode.resize(n, 0);
    bind_world.assign(n, Mat3x4::identity());
    inverse_bind_world.assign(n, Mat3x4::identity());
    for (std::size_t b = 0; b < n; ++b) {
        const std::int32_t p = parents[b];
        if (p >= 0 && static_cast<std::size_t>(p) >= b)
            throw std::runtime_error("bone " + std::to_string(b) + " has parent " + std::to_string(p) +
                                     " that is not an earlier bone");
        bind_world[b] = p < 0 ? bind_local[b] : bind_world[static_cast<std::size_t>(p)] * bind_local[b];
        inverse_bind_world[b] = bind_world[b].inverse();
    }
    lookup_.clear();
    lookup_.reserve(n);
    for (std::size_t b = 0; b < n; ++b) lookup_.push_back({name_crc(names[b]), static_cast<std::uint32_t>(b)});
    std::sort(lookup_.begin(), lookup_.end(), [](const NameKey& a, const NameKey& b) {
        return a.crc != b.crc ? a.crc < b.crc : a.bone < b.bone;
    });
}

std::int32_t Skeleton::find(std::string_view name) const {
    const std::uint32_t crc = name_crc(name);
    auto it = std::lower_bound(lookup_.begin(), lookup_.end(), crc,
                               [](const NameKey& k, std::uint32_t c) { return k.crc < c; });
    for (; it != lookup_.end() && it->crc == crc; ++it)
        if (it->bone < names.size() && iequals(names[it->bone], name)) return static_cast<std::int32_t>(it->bone);
    return -1;
}

// ---- Poses ----------------------------------------------------------------------------------

Pose bind_pose(const Skeleton& skel) {
    Pose p;
    p.local = skel.bind_local;
    p.visible = skel.visible;
    p.visible.resize(skel.size(), 1);
    return p;
}

namespace {

template <class T>
const T& key_at(const std::vector<T>& v, std::uint32_t frame) {
    return v.size() == 1 ? v[0] : v[std::min<std::size_t>(frame, v.size() - 1)];
}

Float3 lerp3(const Float3& a, const Float3& b, float t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}

}  // namespace

void sample(const Skeleton& skel, const AnimationClip& clip, float time, bool loop, Pose& out) {
    out.local = skel.bind_local;
    out.visible = skel.visible;
    out.visible.resize(skel.size(), 1);
    if (clip.frame_count == 0 || skel.size() == 0) return;

    const std::uint32_t n = clip.frame_count;
    const float fps = (std::isfinite(clip.fps) && clip.fps > 0) ? clip.fps : 30.f;
    float pos = std::isfinite(time) ? time * fps : 0.f;
    if (std::isnan(pos)) pos = 0.f;  // finite * finite cannot be NaN, but keep the cast below defined
    // Float -> uint32 conversion is only done for 0 <= pos < float(last); `last` itself otherwise
    // (float(n - 1) may round up to 2^32 for huge frame counts).
    const auto frame_of = [](float p, std::uint32_t last) -> std::uint32_t {
        return p < float(last) ? std::min(static_cast<std::uint32_t>(p), last) : last;
    };
    std::uint32_t f0 = 0, f1 = 0;
    float t = 0;
    if (loop) {
        pos = std::fmod(pos, float(n));  // +-inf -> NaN
        if (pos < 0) pos += float(n);
        if (!(pos >= 0 && pos < float(n))) pos = 0;  // NaN or fmod rounding at the boundary
        f0 = frame_of(pos, n - 1);
        f1 = (f0 + 1) % n;
        t = pos - float(f0);
    } else {
        pos = std::clamp(pos, 0.f, float(n - 1));
        f0 = frame_of(pos, n - 1);
        f1 = std::min(f0 + 1, n - 1);
        t = pos - float(f0);
    }
    t = std::clamp(t, 0.f, 1.f);

    for (const auto& tr : clip.tracks) {
        const std::int32_t bone = skel.find(tr.bone_name);
        if (bone < 0) continue;
        const auto b = static_cast<std::size_t>(bone);
        Float3 tt{0, 0, 0}, ss{1, 1, 1};
        Quat rr;
        if (!tr.translation.empty()) tt = lerp3(key_at(tr.translation, f0), key_at(tr.translation, f1), t);
        if (!tr.rotation.empty()) rr = nlerp(key_at(tr.rotation, f0), key_at(tr.rotation, f1), t);
        if (!tr.scale.empty()) ss = lerp3(key_at(tr.scale, f0), key_at(tr.scale, f1), t);
        out.local[b] = Mat3x4::from_trs(tt, rr, ss);
        if (!tr.visible.empty()) out.visible[b] = static_cast<std::uint8_t>(out.visible[b] && key_at(tr.visible, f0));
    }
}

void compute_world(const Skeleton& skel, std::span<const Mat3x4> local, std::span<Mat3x4> world) {
    const std::size_t n = skel.size();
    if (local.size() != n || world.size() != n) return;
    for (std::size_t b = 0; b < n; ++b) {
        const std::int32_t p = skel.parents[b];
        world[b] = (p >= 0 && static_cast<std::size_t>(p) < b) ? world[static_cast<std::size_t>(p)] * local[b] : local[b];
    }
}

void compute_skin_palette(const Skeleton& skel, std::span<const Mat3x4> world,
                          std::span<const std::uint16_t> palette, std::span<Mat3x4> out) {
    const std::size_t n = std::min(palette.size(), out.size());
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t b = palette[i];
        out[i] = (b < world.size() && b < skel.inverse_bind_world.size()) ? world[b] * skel.inverse_bind_world[b]
                                                                          : Mat3x4::identity();
    }
}

// ---- ALA ------------------------------------------------------------------------------------

namespace {

Float3 f3(const alamo::Vec3& v) { return {v.x, v.y, v.z}; }

// Scale is never animated in shipped data; guard against a zero offset collapsing the bone.
Float3 fix_scale(Float3 s) {
    if (s.x == 0.f && s.y == 0.f && s.z == 0.f) return {1, 1, 1};
    return s;
}

// Throws if any track would make ala::Animation's accessors read outside their arrays (e.g. an
// animated index with num_frames == 0). alamo_formats checks this too; we do not rely on it.
void validate_tracks(const ala::Animation& a) {
    const std::size_t nf = a.num_frames;
    auto need = [&](bool ok, std::size_t bone, const char* what) {
        if (!ok)
            throw std::runtime_error("animation bone " + std::to_string(bone) + ": " + what +
                                     " track does not fit the data (" + std::to_string(nf) + " frames)");
    };
    for (std::size_t i = 0; i < a.bones.size(); ++i) {
        const auto& b = a.bones[i];
        if (a.layout == ala::Layout::Block) {
            if (b.translation_index != 0xFFFF)
                need(nf > 0 && std::size_t(b.translation_index) + 3 <= a.translation_block &&
                         a.translation_data.size() >= nf * a.translation_block, i, "translation");
            if (b.rotation_index != 0xFFFF)
                need(nf > 0 && std::size_t(b.rotation_index) + 4 <= a.rotation_block &&
                         a.rotation_data.size() >= nf * a.rotation_block, i, "rotation");
            if (b.scale_index != 0xFFFF)
                need(nf > 0 && std::size_t(b.scale_index) + 3 <= a.scale_block && a.scale_data.size() >= nf * a.scale_block,
                     i, "scale");
        } else {
            if (!b.translation_data.empty()) need(nf > 0 && b.translation_data.size() >= nf * 3, i, "translation");
            if (!b.rotation_data.empty() && b.rotation_data.size() != 4)
                need(nf > 0 && b.rotation_data.size() >= nf * 4, i, "rotation");
            if (!b.scale_data.empty()) need(nf > 0 && b.scale_data.size() >= nf * 3, i, "scale");
        }
        if (!b.visibility.empty()) need(nf > 0 && b.visibility.size() >= (nf + 7) / 8, i, "visibility");
    }
}

bool finite3(const alamo::Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

// Header floats feed every decoded key: a non-finite value would make whole poses NaN (and a
// non-finite fps used to reach a float -> uint32 conversion in sample()).
void validate_floats(const ala::Animation& a) {
    if (!std::isfinite(a.fps)) throw std::runtime_error("animation fps is not finite");
    for (std::size_t i = 0; i < a.bones.size(); ++i) {
        const auto& b = a.bones[i];
        if (!finite3(b.translation_offset) || !finite3(b.translation_scale) || !finite3(b.scale_offset) ||
            !finite3(b.scale_scale))
            throw std::runtime_error("animation bone " + std::to_string(i) + ": non-finite track offset/scale");
    }
}

}  // namespace

// Decoded keys are bones x frames, but in the block layout any number of bones may index the same
// block elements, so the file size does not bound them (a ~1 MB file could otherwise ask for
// ~15 GB). The largest shipped clip decodes to well under 1% of this budget (see test_assets).
constexpr std::uint64_t kMaxDecodedAnimationBytes = std::uint64_t(256) << 20;

bool build_animation(std::span<const std::uint8_t> bytes, std::string_view name, AnimationClip& out,
                     std::string* error) {
    try {
        const ala::Animation a = ala::parse(bytes);
        validate_tracks(a);  // defence in depth: every accessor call below stays in bounds
        validate_floats(a);
        AnimationClip c;
        c.name = std::string(name);
        c.fps = a.fps > 0 ? a.fps : 30.f;
        c.frame_count = a.num_frames;
        const std::uint32_t frames = std::max<std::uint32_t>(a.num_frames, 1);
        {
            // Same track-length decisions as the loop below, summed before anything is allocated.
            std::uint64_t total = 0;
            for (std::size_t i = 0; i < a.bones.size(); ++i) {
                const auto& src = a.bones[i];
                const bool sa = a.layout == ala::Layout::Block ? src.scale_index != 0xFFFF
                                                               : src.scale_data.size() >= std::size_t(frames) * 3;
                total += sizeof(BoneTrack) + src.name.size();
                total += std::uint64_t(a.is_translation_animated(i) ? frames : 1) * sizeof(Float3);
                total += std::uint64_t(a.is_rotation_animated(i) ? frames : 1) * sizeof(Quat);
                total += std::uint64_t(sa ? frames : 1) * sizeof(Float3);
                if (!src.visibility.empty()) total += frames;
                if (total > kMaxDecodedAnimationBytes)
                    throw std::runtime_error("animation decodes to more than " +
                                             std::to_string(kMaxDecodedAnimationBytes >> 20) + " MiB (" +
                                             std::to_string(a.bones.size()) + " bones x " + std::to_string(frames) +
                                             " frames)");
            }
        }
        c.tracks.reserve(a.bones.size());
        for (std::size_t i = 0; i < a.bones.size(); ++i) {
            const auto& src = a.bones[i];
            BoneTrack t;
            t.bone_name = src.name;
            t.name_crc = name_crc(src.name);
            t.file_bone_index = src.bone_index;
            t.overlay_excluded = src.overlay_excluded != 0;
            const std::uint32_t nt = a.is_translation_animated(i) ? frames : 1;
            const std::uint32_t nr = a.is_rotation_animated(i) ? frames : 1;
            bool scale_animated = false;
            if (a.layout == ala::Layout::Block) scale_animated = src.scale_index != 0xFFFF;
            else scale_animated = src.scale_data.size() >= std::size_t(frames) * 3;
            const std::uint32_t ns = scale_animated ? frames : 1;
            t.translation.reserve(nt);
            for (std::uint32_t f = 0; f < nt; ++f) t.translation.push_back(f3(a.translation(i, f)));
            t.rotation.reserve(nr);
            for (std::uint32_t f = 0; f < nr; ++f) {
                const auto q = a.rotation(i, f);
                t.rotation.push_back(normalize(Quat{q.x, q.y, q.z, q.w}));
            }
            t.scale.reserve(ns);
            for (std::uint32_t f = 0; f < ns; ++f) t.scale.push_back(fix_scale(f3(a.scale(i, f))));
            if (!src.visibility.empty()) {
                t.visible.resize(frames);
                bool all = true;
                for (std::uint32_t f = 0; f < frames; ++f) {
                    t.visible[f] = a.visible(i, f) ? 1 : 0;
                    all = all && t.visible[f];
                }
                if (all) t.visible.clear();
            }
            c.tracks.push_back(std::move(t));
        }
        out = std::move(c);
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

bool load_animation(const FileProvider& files, std::string_view path, AnimationClip& out, std::string* error) {
    const std::string p = animation_path(path);
    std::vector<std::uint8_t> bytes;
    if (!files.read(p, bytes)) {
        if (error) *error = "not found: " + p;
        return false;
    }
    return build_animation(bytes, p, out, error);
}

}  // namespace alamo::assets
