#include "alamo/ui/selection_blob.hpp"

#include <cstring>

namespace alamo::ui {

std::shared_ptr<assets::ModelAsset> selection_blob_model(const terrain::ShadowBlobMaterial& material) {
    auto m = std::make_shared<assets::ModelAsset>();
    m->name = "SELECTION_BLOB_" + material.name;
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
    // Counter-clockwise seen from +Z (the renderer's outward winding, as render::debug_box_model).
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
    auto tex = [](const char* name, const std::string& file) {
        assets::MaterialParam p;
        p.name = name;
        p.type = assets::ParamType::Texture;
        p.texture = file;
        return p;
    };
    if (!material.shader.empty()) {
        mat.effect = material.shader;
        mat.params.push_back(tex("Texture0", material.texture));
        if (!material.texture2.empty()) mat.params.push_back(tex("Texture1", material.texture2));
    } else {
        mat.params.push_back(tex("BaseTexture", material.texture));
    }
    m->submeshes.push_back(std::move(sm));
    m->bounds = {{-1, -1, 0}, {1, 1, 0}};
    m->sphere = {{0, 0, 0}, 1.4142136f};
    return m;
}

render::Instance selection_blob_instance(const render::GpuModel& model, const SelectionBlob& blob) {
    render::Instance in;
    in.model = &model;
    in.transform = assets::Mat3x4{};
    const float r = blob.radius > 0.0f ? blob.radius : 1.0f;
    in.transform.m[0][0] = r;
    in.transform.m[1][1] = r;
    in.transform.m[2][2] = r;
    in.transform.m[0][3] = blob.center.x;
    in.transform.m[1][3] = blob.center.y;
    in.transform.m[2][3] = blob.center.z + kSpaceBlobLift;
    return in;
}

}  // namespace alamo::ui
