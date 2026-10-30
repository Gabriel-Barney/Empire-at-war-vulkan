#include "alamo/render/jam_fx.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "alamo/core/log.hpp"

namespace alamo::render {

namespace {

std::string lower(std::string_view s) {
    std::string o(s);
    for (char& c : o)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return o;
}

uint8_t byte_of(float f) {
    const float c = std::isfinite(f) ? std::clamp(f, 0.0f, 1.0f) : 1.0f;
    return static_cast<uint8_t>(std::lround(c * 255.0f));
}

std::string model_key(const std::string& material, const float (&color)[4]) {
    std::string k = lower(material);
    k.push_back('|');
    for (float c : color) {
        k += std::to_string(static_cast<unsigned>(byte_of(c)));
        k.push_back(',');
    }
    return k;
}

}  // namespace

bool jam_blob_visible(const SnapshotJammer& j, const SnapshotObject* jammer) {
    if (!jammer) return false;
    if (!(j.radius > 0.0f) || !std::isfinite(j.radius)) return false;
    if ((jammer->flags & (kSnapDying | kSnapContained | kSnapCorpse)) != 0 || !(jammer->fade > 0.0f)) return false;
    if (j.allied) return true;
    return j.show_to_enemies && (jammer->flags & kSnapFogHidden) == 0;
}

std::vector<JamBlob> jam_blobs(const InterpolatedFrame& frame) {
    std::vector<JamBlob> out;
    for (const SnapshotJammer& j : frame.jamming.jammers) {
        const auto it = std::lower_bound(frame.objects.begin(), frame.objects.end(), j.id,
                                         [](const SnapshotObject& o, objects::ObjectId v) { return o.id < v; });
        const SnapshotObject* o = it != frame.objects.end() && it->id == j.id ? &*it : nullptr;
        if (!jam_blob_visible(j, o)) continue;
        JamBlob b;
        b.jammer = j.id;
        b.center = o->position;
        b.half_size = j.radius;
        b.material = j.blob_material;
        for (size_t k = 0; k < 4; ++k) b.color[k] = j.blob_color[k];
        out.push_back(std::move(b));
    }
    return out;
}

std::shared_ptr<assets::ModelAsset> jam_blob_model(const terrain::ShadowBlobMaterial& material, const float (&color)[4]) {
    auto m = std::make_shared<assets::ModelAsset>();
    m->name = "JAM_BLOB_" + material.name;
    assets::Skeleton& sk = m->skeleton;
    sk.names = {"ROOT"};
    sk.parents = {-1};
    sk.visible = {1};
    sk.billboard_mode = {0};
    sk.bind_local = {assets::Mat3x4{}};
    sk.finalize();
    assets::MeshInfo mi;
    mi.name = "BLOB";
    mi.attach_bone = 0;
    mi.first_submesh = 0;
    mi.submesh_count = 1;
    m->meshes.push_back(mi);
    assets::Submesh sm;
    sm.mesh = 0;
    sm.vertex_format = "alD3dVertNU2";
    sm.layout = assets::VertexLayout::Static;
    sm.attach_bone = 0;
    // Counter-clockwise seen from +Z (the renderer's outward winding, as ui::selection_blob_model).
    const float xs[4] = {-1, 1, 1, -1}, ys[4] = {-1, -1, 1, 1};
    sm.vertex_count = 4;
    sm.vertices.resize(4 * 64);
    for (int k = 0; k < 4; ++k) {
        assets::StaticVertex v{};
        v.position[0] = xs[k];
        v.position[1] = ys[k];
        v.position[2] = 0.0f;
        v.normal[2] = 1.0f;
        v.tangent[0] = 1.0f;
        v.binormal[1] = 1.0f;
        v.uv0[0] = 0.5f * (xs[k] + 1.0f);
        v.uv0[1] = 0.5f * (1.0f - ys[k]);
        v.color[0] = v.color[1] = v.color[2] = v.color[3] = 255;
        std::memcpy(sm.vertices.data() + static_cast<size_t>(k) * 64, &v, 64);
    }
    const uint16_t idx[6] = {0, 1, 2, 0, 2, 3};
    sm.index_type = assets::IndexType::U16;
    sm.index_count = 6;
    sm.indices.resize(sizeof idx);
    std::memcpy(sm.indices.data(), idx, sizeof idx);
    sm.bounds = {{-1, -1, 0}, {1, 1, 0}};
    sm.sphere = {{0, 0, 0}, 1.4142136f};
    assets::MaterialRef& mat = sm.material;
    mat.effect = "MeshAlpha.fx";  // ALPHA blend, MODULATE2X texture x lit diffuse (ABILITY_VISUALS 3: the blob material is ALPHA)
    assets::MaterialParam tex;
    tex.name = "BaseTexture";
    tex.type = assets::ParamType::Texture;
    tex.texture = material.texture.empty() ? std::string(kJamBlobFallbackTexture) : material.texture;
    mat.params.push_back(std::move(tex));
    assets::MaterialParam diffuse;
    diffuse.name = "Diffuse";
    diffuse.type = assets::ParamType::Float4;
    diffuse.value = {color[0], color[1], color[2], color[3]};
    mat.params.push_back(std::move(diffuse));
    m->submeshes.push_back(std::move(sm));
    m->bounds = {{-1, -1, 0}, {1, 1, 0}};
    m->sphere = {{0, 0, 0}, 1.4142136f};
    return m;
}

Instance jam_blob_instance(const GpuModel& model, const JamBlob& blob) {
    Instance in;
    in.model = &model;
    in.transform = assets::Mat3x4{};
    const float r = blob.half_size > 0.0f ? blob.half_size : 1.0f;
    in.transform.m[0][0] = r;
    in.transform.m[1][1] = r;
    in.transform.m[2][2] = r;
    in.transform.m[0][3] = blob.center.x;
    in.transform.m[1][3] = blob.center.y;
    in.transform.m[2][3] = blob.center.z + kJamBlobLift;
    return in;
}

uint32_t JamBlobModels::build(Renderer& renderer, const TextureSource& textures, const InterpolatedFrame& frame,
                              const std::function<float(float, float)>& ground, std::vector<Instance>& out) {
    uint32_t n = 0;
    for (JamBlob& b : jam_blobs(frame)) {
        const std::string key = model_key(b.material, b.color);
        auto it = models_.find(key);
        if (it == models_.end()) {
            auto e = std::make_unique<Entry>();
            terrain::ShadowBlobMaterial mat;
            mat.name = b.material;
            mat.mode = terrain::RenderMode::Alpha;
            const std::string want = lower(b.material);
            for (const terrain::ShadowBlobMaterial& m : materials_)
                if (lower(m.name) == want) {
                    mat = m;
                    break;
                }
            std::string err;
            e->ok = renderer.upload_model(jam_blob_model(mat, b.color), textures, e->model, nullptr, &err);
            if (!e->ok) log::warnf("render", "jam blob '%s': %s", b.material.c_str(), err.c_str());
            it = models_.emplace(key, std::move(e)).first;
        }
        Entry& e = *it->second;
        if (!e.ok) continue;
        renderer.make_resident(e.model);
        if (ground) b.center.z = ground(b.center.x, b.center.y);
        out.push_back(jam_blob_instance(e.model, b));
        ++n;
    }
    return n;
}

void JamBlobModels::shutdown(Renderer& renderer) {
    for (auto& [key, e] : models_)
        if (e && e->ok) renderer.destroy_model(e->model);
    models_.clear();
}

}  // namespace alamo::render
