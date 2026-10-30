// alamo_render_jam_tests - Phase 4 R5 t05 sensor jamming visual (docs/re/ABILITY_VISUALS.md section 3). No GPU, no game data.
//   * jam_blob_visible: allies always; others only with Show_Blob_To_Enemies and not fogged; dying / contained / faded jammers
//     and non-positive radii draw nothing
//   * jam_blobs over an InterpolatedFrame: centre = the jammer object's position, half size = radius (side 2 r), material and
//     Blob_Color carried over, ascending jammer id, missing objects skipped
//   * jam_blob_model / jam_blob_instance: unit quad with the MeshAlpha effect, BaseTexture + Diffuse, scale r, lift in z
//   * snapshot plumbing: SnapshotJamming::is_jammed, digest sensitivity (and unchanged when empty), interpolate() copies it
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "alamo/core/log.hpp"
#include "alamo/render/jam_fx.hpp"
#include "alamo/render/sim_snapshot.hpp"

using namespace alamo;

static int g_fail = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)

static render::SnapshotObject object(objects::ObjectId id, int32_t owner, float x, float y, float z, uint8_t flags = 0, float fade = 1.0f) {
    render::SnapshotObject o;
    o.id = id;
    o.owner = owner;
    o.position = {x, y, z};
    o.flags = flags;
    o.fade = fade;
    return o;
}

static render::SnapshotJammer jammer(objects::ObjectId id, int32_t owner, float radius, bool allied, bool enemies = false) {
    render::SnapshotJammer j;
    j.id = id;
    j.owner = owner;
    j.radius = radius;
    j.allied = allied;
    j.show_to_enemies = enemies;
    j.blob_material = "Sensor_Jamming_Blob";
    j.blob_color[0] = 200.0f / 255.0f;
    j.blob_color[1] = 50.0f / 255.0f;
    j.blob_color[2] = 200.0f / 255.0f;
    j.blob_color[3] = 128.0f / 255.0f;
    return j;
}

static void test_visibility() {
    const render::SnapshotObject plain = object(1, 0, 0, 0, 0);
    const render::SnapshotObject fogged = object(1, 1, 0, 0, 0, render::kSnapFogHidden);
    // Allies always see it (also a fogged / stealthed own jammer: the fog rule only guards the enemy case).
    CHECK(render::jam_blob_visible(jammer(1, 0, 300, true), &plain));
    CHECK(render::jam_blob_visible(jammer(1, 0, 300, true), &fogged));
    // Enemies: only with Show_Blob_To_Enemies, and not while the jammer is fogged for the local player.
    CHECK(!render::jam_blob_visible(jammer(1, 1, 300, false), &plain));
    CHECK(render::jam_blob_visible(jammer(1, 1, 300, false, true), &plain));
    CHECK(!render::jam_blob_visible(jammer(1, 1, 300, false, true), &fogged));
    // Missing / dying / contained / corpse / faded jammers and bad radii draw nothing.
    CHECK(!render::jam_blob_visible(jammer(1, 0, 300, true), nullptr));
    const render::SnapshotObject dying = object(1, 0, 0, 0, 0, render::kSnapDying);
    CHECK(!render::jam_blob_visible(jammer(1, 0, 300, true), &dying));
    const render::SnapshotObject contained = object(1, 0, 0, 0, 0, render::kSnapContained);
    const render::SnapshotObject corpse = object(1, 0, 0, 0, 0, render::kSnapCorpse);
    const render::SnapshotObject faded = object(1, 0, 0, 0, 0, 0, 0.0f);
    CHECK(!render::jam_blob_visible(jammer(1, 0, 300, true), &contained));
    CHECK(!render::jam_blob_visible(jammer(1, 0, 300, true), &corpse));
    CHECK(!render::jam_blob_visible(jammer(1, 0, 300, true), &faded));
    CHECK(!render::jam_blob_visible(jammer(1, 0, 0, true), &plain));
    CHECK(!render::jam_blob_visible(jammer(1, 0, -5, true), &plain));
    CHECK(!render::jam_blob_visible(jammer(1, 0, std::nanf(""), true), &plain));
}

static render::InterpolatedFrame make_frame() {
    render::InterpolatedFrame f;
    f.objects = {object(2, 0, 10, 20, 5), object(5, 1, -30, 40, 0), object(9, 1, 1, 2, 3, render::kSnapFogHidden), object(12, 0, 7, 7, 7)};
    f.jamming.jammers = {jammer(2, 0, 300, true),                 // ally: drawn
                         jammer(5, 1, 1000, false),               // enemy, no Show_Blob_To_Enemies: hidden
                         jammer(9, 1, 150, false, true),          // enemy shown, but fogged: hidden
                         jammer(11, 0, 100, true),                // object missing: skipped
                         jammer(12, 0, 50, true, true)};          // ally: drawn
    f.jamming.jammed = {5, 12};
    return f;
}

static void test_blobs() {
    const render::InterpolatedFrame f = make_frame();
    const std::vector<render::JamBlob> blobs = render::jam_blobs(f);
    CHECK(blobs.size() == 2);
    if (blobs.size() == 2) {
        CHECK(blobs[0].jammer == 2 && blobs[1].jammer == 12);  // ascending jammer id
        CHECK(blobs[0].center.x == 10.0f && blobs[0].center.y == 20.0f && blobs[0].center.z == 5.0f);
        CHECK(blobs[0].half_size == 300.0f && blobs[1].half_size == 50.0f);  // side 2 x radius
        CHECK(blobs[0].material == "Sensor_Jamming_Blob");
        CHECK(std::fabs(blobs[0].color[0] - 200.0f / 255.0f) < 1e-6f && std::fabs(blobs[0].color[3] - 128.0f / 255.0f) < 1e-6f);
    }
    CHECK(render::jam_blobs(render::InterpolatedFrame{}).empty());
    // Is_Jammed list.
    CHECK(f.jamming.is_jammed(5) && f.jamming.is_jammed(12) && !f.jamming.is_jammed(2) && !f.jamming.is_jammed(99));
}

static void test_model() {
    terrain::ShadowBlobMaterial mat;
    mat.name = "Sensor_Jamming_Blob";
    mat.texture = "i_circle00.tga";
    mat.mode = terrain::RenderMode::Alpha;
    const float color[4] = {200.0f / 255.0f, 50.0f / 255.0f, 200.0f / 255.0f, 128.0f / 255.0f};
    const std::shared_ptr<assets::ModelAsset> m = render::jam_blob_model(mat, color);
    CHECK(m && m->submeshes.size() == 1 && m->meshes.size() == 1 && m->skeleton.names.size() == 1);
    if (m && m->submeshes.size() == 1) {
        const assets::Submesh& sm = m->submeshes[0];
        CHECK(sm.vertex_count == 4 && sm.index_count == 6 && sm.vertices.size() == 4 * 64);
        CHECK(sm.material.effect == "MeshAlpha.fx");
        const assets::MaterialParam* tex = sm.material.find("BaseTexture");
        const assets::MaterialParam* dif = sm.material.find("Diffuse");
        CHECK(tex && tex->type == assets::ParamType::Texture && tex->texture == "i_circle00.tga");
        CHECK(dif && dif->type == assets::ParamType::Float4 && std::fabs(dif->value.w - color[3]) < 1e-6f && std::fabs(dif->value.x - color[0]) < 1e-6f);
        // Unit square: the instance scale gives the side 2 r.
        assets::StaticVertex v0{}, v2{};
        std::memcpy(&v0, sm.vertices.data(), sizeof v0);
        std::memcpy(&v2, sm.vertices.data() + 2 * 64, sizeof v2);
        CHECK(v0.position[0] == -1.0f && v0.position[1] == -1.0f && v2.position[0] == 1.0f && v2.position[1] == 1.0f);
    }
    // A material without a texture falls back to the vanilla blob texture.
    terrain::ShadowBlobMaterial bare;
    bare.name = "x";
    const std::shared_ptr<assets::ModelAsset> m2 = render::jam_blob_model(bare, color);
    CHECK(m2 && m2->submeshes[0].material.find("BaseTexture") && m2->submeshes[0].material.find("BaseTexture")->texture == render::kJamBlobFallbackTexture);

    render::GpuModel gm;
    render::JamBlob b;
    b.center = {10, 20, 5};
    b.half_size = 300.0f;
    const render::Instance in = render::jam_blob_instance(gm, b);
    CHECK(in.model == &gm);
    CHECK(in.transform.m[0][0] == 300.0f && in.transform.m[1][1] == 300.0f && in.transform.m[2][2] == 300.0f);
    CHECK(in.transform.m[0][3] == 10.0f && in.transform.m[1][3] == 20.0f && in.transform.m[2][3] == 5.0f + render::kJamBlobLift);
    CHECK(in.light_scale.w == 1.0f);
}

static void test_snapshot() {
    render::SimSnapshot a;
    a.objects = {object(2, 0, 10, 20, 5)};
    const uint64_t base = a.digest();
    render::SimSnapshot b = a;
    CHECK(b.digest() == base);  // an empty jamming section leaves the digest as it was
    b.jamming.jammed = {2};
    const uint64_t with_jammed = b.digest();
    CHECK(with_jammed != base);
    render::SimSnapshot c = a;
    c.jamming.jammers = {jammer(2, 0, 300, true)};
    const uint64_t with_jammer = c.digest();
    CHECK(with_jammer != base && with_jammer != with_jammed);
    render::SimSnapshot d = c;
    d.jamming.jammers[0].radius = 301.0f;
    CHECK(d.digest() != with_jammer);
    d = c;
    d.jamming.jammers[0].show_to_enemies = true;
    CHECK(d.digest() != with_jammer);
    d = c;
    d.jamming.jammers[0].blob_color[3] = 0.25f;
    CHECK(d.digest() != with_jammer);
    CHECK(c.digest() == with_jammer);  // deterministic
    render::SimSnapshot e = a;  // R5 stealth radar filter ids: hashed, ordered lookup
    e.jamming.stealthed = {2};
    CHECK(e.digest() != base && e.digest() != with_jammed && e.jamming.is_stealthed(2) && !e.jamming.is_stealthed(3));

    // interpolate() carries the section through, in both modes.
    for (render::InterpMode mode : {render::InterpMode::Original, render::InterpMode::Smooth}) {
        render::InterpolatedFrame f;
        render::interpolate(&a, c, 0.5f, mode, f);
        CHECK(f.jamming.jammers.size() == 1 && f.jamming.jammers[0].id == 2 && f.jamming.jammers[0].radius == 300.0f);
        render::interpolate(nullptr, b, 1.0f, mode, f);
        CHECK(f.jamming.jammers.empty() && f.jamming.jammed.size() == 1 && f.jamming.is_jammed(2));
    }
    // The particle variant is an ordinary attached effect.
    static_assert(render::kEffectJamming != render::kEffectLua && render::kEffectJamming >= render::kEffectAbilityBase + 76);
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    log::set_min_level(log::Level::Error);
    test_visibility();
    test_blobs();
    test_model();
    test_snapshot();
    if (g_fail) {
        std::printf("alamo_render_jam_tests: %d FAILED\n", g_fail);
        return 1;
    }
    std::printf("alamo_render_jam_tests: all passed\n");
    return 0;
}
