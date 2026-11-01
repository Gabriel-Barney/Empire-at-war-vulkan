// alamo_game_battle_fx_tests (refix t06 / refixf t05, data-free): the tactical super weapon's presentation cues
// (battle_fx.hpp) on a synthetic type set and object store - Firing / AttackAnim at the TSW, PlanetDestroyed at the target
// dummy while it is dead but not flushed, nothing without a dummy or for other events -, the cinematic camera of
// TSW_PLANET_DESTROY.md §11 (improved noise / fBm, ease and accel profiles, the explosion shake for J = 1 / D = 100, its
// ease-out envelope, the gate outside the cinematic, the firing shot's key frames at 0 / 300 / 315 / 765, the cut, input
// cancel, camera save / restore, letterbox), and GameAudioHost::tsw_audio on a synthetic audio tree with the null backend
// (activation voice + power-up SFX, mode 8 music, SFXEvent_Fire, the dummy's Death_SFXEvent_Start_Die).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "alamo/core/file_provider.hpp"
#include "alamo/game/battle_fx.hpp"
#include "alamo/game/game_audio_host.hpp"
#include "alamo/game/game_data.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/object_types.hpp"
#include "alamo/tactical/abilities/special_weapons.hpp"
#ifdef ALAMO_GAME_HAVE_AUDIO
#include "alamo/audio/audio_system.hpp"
#endif

using namespace alamo;
using namespace alamo::game;
namespace ta = alamo::tactical::abilities;

static int g_fail = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)

namespace {

class MemoryProvider final : public FileProvider {
public:
    void add(const std::string& path, const std::string& text) {
        files_[normalize_path(path)] = std::vector<uint8_t>(text.begin(), text.end());
    }
    bool read(std::string_view path, std::vector<uint8_t>& out) const override {
        auto it = files_.find(normalize_path(path));
        if (it == files_.end()) return false;
        out = it->second;
        return true;
    }
    bool exists(std::string_view path) const override { return files_.count(normalize_path(path)) != 0; }
    std::string origin(std::string_view path) const override { return exists(path) ? "mem:" + std::string(path) : ""; }

private:
    std::map<std::string, std::vector<uint8_t>> files_;
};

const char* kTypes = R"(<?xml version="1.0"?>
<Fx>
 <SpaceUnit Name="Fx_Death_Star"><SpaceBehavior>TACTICAL_SUPER_WEAPON</SpaceBehavior>
  <SFXEvent_Fire>Fx_Superlaser</SFXEvent_Fire>
  <TSW_SFXEvent_Activate_Voice>Fx_Activate_Voice</TSW_SFXEvent_Activate_Voice>
  <TSW_SFXEvent_Weapon_Power_Up>Fx_Power_Up</TSW_SFXEvent_Weapon_Power_Up>
  <TSW_MusicEvent_Activation_Start>Fx_TSW_Music</TSW_MusicEvent_Activation_Start></SpaceUnit>
 <SpaceProp Name="Fx_Planet"><Behavior>DUMMY_TACTICAL_SUPER_WEAPON_TARGET</Behavior>
  <Death_SFXEvent_Start_Die>Fx_Planet_Death</Death_SFXEvent_Start_Die>
  <Death_Explosions>Fx_Boom_Default</Death_Explosions></SpaceProp>
 <SpaceProp Name="Fx_Planet_Big"><Behavior>DUMMY_TACTICAL_SUPER_WEAPON_TARGET</Behavior>
  <Death_Explosions>Fx_Boom_Default</Death_Explosions><Death_Explosions>Fx_Boom_Big</Death_Explosions></SpaceProp>
 <SpaceProp Name="Fx_Planet_Bare"><Behavior>DUMMY_TACTICAL_SUPER_WEAPON_TARGET</Behavior></SpaceProp>
 <SpaceProp Name="Fx_Boom_Default"></SpaceProp>
 <SpaceProp Name="Fx_Boom_Big"><Explosion_Jitter_Factor>3.0</Explosion_Jitter_Factor></SpaceProp>
</Fx>
)";
const char* kSfx = R"(<?xml version="1.0"?>
<SFXEvents>
  <SFXEvent Name="Fx_Activate_Voice"><Is_2D>Yes</Is_2D><Is_3D>No</Is_3D><Samples>VOICE.WAV</Samples></SFXEvent>
  <SFXEvent Name="Fx_Power_Up"><Is_2D>Yes</Is_2D><Is_3D>No</Is_3D><Is_GUI>Yes</Is_GUI><Samples>POWER.WAV</Samples></SFXEvent>
  <SFXEvent Name="Fx_Superlaser"><Is_2D>Yes</Is_2D><Is_3D>No</Is_3D><Samples>LASER.WAV</Samples></SFXEvent>
  <SFXEvent Name="Fx_Planet_Death"><Is_2D>Yes</Is_2D><Is_3D>No</Is_3D><Samples>BOOM.WAV</Samples></SFXEvent>
</SFXEvents>
)";
const char* kMusic = R"(<?xml version="1.0"?>
<MusicEvents>
  <MusicEvent Name="Fx_TSW_Music"><Files>TSW.MP3</Files></MusicEvent>
</MusicEvents>
)";

std::unique_ptr<MemoryProvider> make_tree() {
    auto p = std::make_unique<MemoryProvider>();
    p->add("Data\\XML\\GameObjectFiles.xml", "<GameObjectFiles><File>FxTypes.xml</File></GameObjectFiles>");
    p->add("Data\\XML\\FxTypes.xml", kTypes);
    p->add("Data\\XML\\SFXEventFiles.xml", "<SFXEvent_Files><File>SFXEventsFx.xml</File></SFXEvent_Files>");
    p->add("Data\\XML\\SFXEventsFx.xml", kSfx);
    p->add("Data\\XML\\MusicEvents.xml", kMusic);
    p->add("Data\\XML\\SpeechEvents.xml", "<SpeechEvents/>");
    p->add("Data\\XML\\Audio.xml", "<Audio/>");
    p->add("Data\\XML\\FactionFiles.xml", "<Faction_Files></Faction_Files>");
    return p;
}

struct World {
    std::unique_ptr<MemoryProvider> files;
    objects::ObjectTypeDatabase raw;
    objects::GameObjectTypes types;
    std::unique_ptr<objects::ObjectStore> store;
    objects::TypeIndex ds = objects::kNone, planet = objects::kNone;
    objects::ObjectHandle tsw, dummy;
};

std::unique_ptr<World> make_world() {
    auto w = std::make_unique<World>();
    w->files = make_tree();
    CHECK(w->raw.load(*w->files));
    CHECK(w->types.build(w->raw, w->files.get()));
    w->ds = w->types.resolve_name("Fx_Death_Star");
    w->planet = w->types.resolve_name("Fx_Planet");
    CHECK(w->ds != objects::kNone && w->planet != objects::kNone);
    w->store = std::make_unique<objects::ObjectStore>(objects::ModeKind::Space, objects::StoreKind::Primary, w->types);
    objects::SpawnParams p;
    p.type = w->ds;
    p.owner = 1;
    p.position = {100.0f, 200.0f, 300.0f};
    w->tsw = w->store->create(p);
    p.type = w->planet;
    p.owner = 2;
    p.position = {0.0f, 5000.0f, 100.0f};
    w->dummy = w->store->create(p);
    return w;
}

ta::SpecialWeaponEvent ev(ta::SwEventKind k, objects::ObjectId id, objects::ObjectId target = ta::kNoObject) {
    ta::SpecialWeaponEvent e;
    e.kind = k;
    e.id = id;
    e.target = target;
    return e;
}

void test_tsw_cues() {
    auto w = make_world();
    objects::ObjectStore& st = *w->store;
    const objects::ObjectId tsw = st.id(w->tsw), dummy = st.id(w->dummy);
    std::vector<ta::SpecialWeaponEvent> evs;
    evs.push_back(ev(ta::SwEventKind::Ready, tsw));
    evs.push_back(ev(ta::SwEventKind::TswFiring, tsw, dummy));
    evs.push_back(ev(ta::SwEventKind::TswState, tsw));
    evs.push_back(ev(ta::SwEventKind::TswAttackAnim, tsw, dummy));
    ta::SpecialWeaponEvent kill = ev(ta::SwEventKind::TswPlanetDestroyed, tsw, dummy);
    kill.point = {0.0f, 5000.0f, 100.0f};
    kill.value = 0;  // the Death_Explosions pick
    evs.push_back(kill);
    evs.push_back(ev(ta::SwEventKind::TswPlanetDestroyed, tsw));          // no target dummy: no cue (§4 step 3)
    evs.push_back(ev(ta::SwEventKind::TswPlanetDestroyed, tsw, 0x12345));  // unknown object: no cue
    evs.push_back(ev(ta::SwEventKind::TswDebris, tsw, dummy));
    evs.push_back(ev(ta::SwEventKind::TswDone, tsw));

    // The sim kills the dummy at state 9 (dead, flushed at the next store update): the cue still reads its type.
    CHECK(st.destroy(w->dummy));
    std::vector<TswCue> c = tsw_cues(evs, st);
    CHECK(c.size() == 3);
    if (c.size() == 3) {
        CHECK(c[0].kind == TswCue::Kind::Firing && c[0].object == tsw && c[0].type == w->ds && c[0].target == dummy);
        CHECK(c[0].position.x == 100.0f && c[0].position.y == 200.0f && c[0].position.z == 300.0f);
        CHECK(c[1].kind == TswCue::Kind::AttackAnim && c[1].object == tsw && c[1].type == w->ds);
        CHECK(c[2].kind == TswCue::Kind::PlanetDestroyed && c[2].object == dummy && c[2].type == w->planet);
        CHECK(c[2].position.y == 5000.0f && c[2].position.z == 100.0f && c[2].pick == 0);
    }
    // Once flushed the dummy is unknown: no PlanetDestroyed cue (a dummy gone before state 9).
    st.flush();
    c = tsw_cues(evs, st);
    CHECK(c.size() == 2);
    CHECK(tsw_cues({}, st).empty());
}

float len(tactical::Vec3 v) { return std::sqrt(tactical::dot(v, v)); }
bool close(float a, float b, float eps) { return std::fabs(a - b) <= eps; }
bool close(tactical::Vec3 a, tactical::Vec3 b, float eps) { return len(a - b) <= eps; }

// §11.1 N / Fbm: the reference permutation, zero on the lattice, golden values (double-precision reimplementation of the
// doc's algorithm), the fBm sum rule.
void test_noise() {
    const std::span<const uint8_t, 256> p = perlin_permutation();
    bool seen[256] = {};
    for (uint8_t v : p) seen[v] = true;
    bool all = true;
    for (bool b : seen) all = all && b;
    CHECK(all);  // a permutation of 0..255
    CHECK(p[0] == 151 && p[1] == 160 && p[2] == 137 && p[3] == 91 && p[4] == 90 && p[5] == 15 && p[255] == 180);
    CHECK(improved_noise(0.0f, 0.0f, 0.0f) == 0.0f && improved_noise(3.0f, -7.0f, 300.0f) == 0.0f);
    CHECK(close(improved_noise(0.5f, 0.25f, 0.75f), -0.374267578125f, 1e-5f));
    CHECK(close(improved_noise(3.7f, 1.0f, 100.0f), -0.1512f, 1e-5f));
    CHECK(close(improved_noise(110.0f, 0.4f, 303.1416015625f), 0.30854728642661067f, 1e-5f));
    CHECK(close(improved_noise(1.25f, 2.5f, -0.5f), 0.0703125f, 1e-5f));
    CHECK(close(noise_fbm(0.5f, 0.25f, 0.75f), -0.2872752308176323f, 1e-4f));
    CHECK(close(noise_fbm(3.7f, 1.0f, 100.0f), -0.2545033971585878f, 1e-4f));
    CHECK(close(noise_fbm(110.0f, 0.4f, 303.1416015625f), 0.46907232250870595f, 1e-4f));
    const float x = 2.3f, y = 0.7f, z = 100.0f;
    const float sum = improved_noise(x, y, z) + improved_noise(1.9f * x, 1.9f * y, 1.9f * z) / 1.9f +
                      improved_noise(6.859f * x, 6.859f * y, 6.859f * z) / 3.61f;
    CHECK(close(noise_fbm(x, y, z), sum, 1e-3f));
    float peak = 0.0f;  // the fBm stays within the sum of the octave weights
    for (int i = 0; i < 2000; ++i) peak = std::max(peak, std::fabs(noise_fbm(0.37f * static_cast<float>(i), 1.3f, 100.0f)));
    CHECK(peak > 0.2f && peak < 1.0f + 1.0f / 1.9f + 1.0f / 3.61f);
}

void test_profiles() {
    // Ease(t, 0, 0.25): 1 up to 0.75, smooth fall to 0 at 1; 0 outside.
    CHECK(shake_ease(-0.1f, 0.0f, 0.25f) == 0.0f);
    CHECK(shake_ease(0.0f, 0.0f, 0.25f) == 1.0f && shake_ease(0.5f, 0.0f, 0.25f) == 1.0f && shake_ease(0.75f, 0.0f, 0.25f) == 1.0f);
    CHECK(close(shake_ease(0.875f, 0.0f, 0.25f), 0.5f, 1e-6f));
    CHECK(close(shake_ease(0.9f, 0.0f, 0.25f), 0.352f, 1e-5f));
    CHECK(shake_ease(1.0f, 0.0f, 0.25f) == 0.0f && shake_ease(1.5f, 0.0f, 0.25f) == 0.0f);
    float prev = 1.0f;
    bool falling = true;
    for (int i = 0; i <= 100; ++i) {
        const float e = shake_ease(0.75f + 0.0025f * static_cast<float>(i), 0.0f, 0.25f);
        falling = falling && e <= prev + 1e-6f;
        prev = e;
    }
    CHECK(falling);
    CHECK(close(shake_ease(0.125f, 0.25f, 0.0f), 0.5f, 1e-6f));  // ease in: the mirror
    // Accel: 0 -> 1, continuous at the phase changes, monotone.
    for (const auto& ab : {std::pair{0.5f, 0.25f}, std::pair{0.5f, 0.0f}, std::pair{0.0f, 0.25f}, std::pair{0.25f, 0.25f}}) {
        CHECK(shot_accel(0.0f, ab.first, ab.second) == 0.0f);
        CHECK(close(shot_accel(1.0f, ab.first, ab.second), 1.0f, 1e-5f));
        float last = 0.0f;
        bool mono = true;
        for (int i = 1; i <= 200; ++i) {
            const float v = shot_accel(0.005f * static_cast<float>(i), ab.first, ab.second);
            mono = mono && v >= last - 1e-6f && v - last < 0.02f;
            last = v;
        }
        CHECK(mono);
    }
    CHECK(close(shot_accel(0.5f, 0.5f, 0.25f), 0.4f, 1e-6f));    // v a / 2, v = 2 / 1.25
    CHECK(close(shot_accel(0.75f, 0.5f, 0.25f), 0.8f, 1e-6f));
    CHECK(close(shot_accel(0.5f, 0.5f, 0.0f), 1.0f / 3.0f, 1e-6f));
}

// §11.1 parameters and evaluation for the TSW planet kill (J = 1, D = 100).
void test_explosion_shake() {
    const ShakeEffect e = explosion_shake(1.0f, kTswShakeDistance, 50);
    CHECK(e.x_amplitude == 5.0f && e.y_amplitude == 5.0f);
    CHECK(close(e.roll_amplitude, 0.3f, 1e-7f));
    CHECK(e.frequency == 330.0f && e.noise_offset == 1.0f && e.ease_in == 0.0f && e.ease_out == 0.25f);
    CHECK(explosion_shake(1.0f, 100.0f, 0).noise_offset == 0.0f && explosion_shake(1.0f, 100.0f, 100).noise_offset == 2.0f);
    const ShakeEffect big = explosion_shake(3.0f, 50.0f, 1);
    CHECK(big.x_amplitude == 30.0f && close(big.roll_amplitude, 1.8f, 1e-6f) && big.frequency == 990.0f && close(big.noise_offset, 0.02f, 1e-7f));

    const tactical::Vec3 R{1.0f, 0.0f, 0.0f}, U{0.0f, 0.0f, 1.0f};
    CameraShake s;
    CHECK(s.empty());
    s.start(e, 10);  // 30 frames from shot clock 10
    CHECK(s.count() == 1);
    // Before the start and from t = 1 on: nothing (t = 1 has the envelope at 0).
    for (const int32_t c : {0, 9, 40, 41, 100}) {
        const CameraShake::Sample z = s.evaluate(c, R, U);
        CHECK(len(z.offset) == 0.0f && z.roll_deg == 0.0f);
    }
    // Exact values: full strength at t = 1/3 (clock 20), the ease-out at t = 0.9 (clock 37).
    {
        const float t = 10.0f / 30.0f, sv = 330.0f * t;
        const CameraShake::Sample a = s.evaluate(20, R, U);
        CHECK(close(a.offset.x, noise_fbm(sv, 1.0f, 0.0f) * 5.0f, 1e-5f));
        CHECK(close(a.offset.z, noise_fbm(sv, 1.0f, 100.0f) * 5.0f, 1e-5f));
        CHECK(a.offset.y == 0.0f);
        CHECK(close(a.roll_deg, noise_fbm(sv, 1.0f, 303.1416015625f) * 0.3f, 1e-6f));
        const float t2 = 27.0f / 30.0f, s2 = 330.0f * t2, env = shake_ease(t2, 0.0f, 0.25f);
        CHECK(close(env, 0.352f, 1e-5f));
        const CameraShake::Sample b = s.evaluate(37, R, U);
        CHECK(close(b.offset.x, noise_fbm(s2, 1.0f, 0.0f) * 5.0f * env, 1e-5f));
        CHECK(close(b.roll_deg, noise_fbm(s2, 1.0f, 303.1416015625f) * 0.3f * env, 1e-6f));
    }
    // Bounded by amplitude x (1 + 1/1.9 + 1/3.61) per axis; the last quarter fades to nothing.
    const float lim = 5.0f * (1.0f + 1.0f / 1.9f + 1.0f / 3.61f);
    float peak = 0.0f;
    for (int32_t c = 10; c <= 40; ++c) {
        const CameraShake::Sample v = s.evaluate(c, R, U);
        CHECK(std::fabs(v.offset.x) <= lim && std::fabs(v.offset.z) <= lim && std::fabs(v.roll_deg) <= 0.3f * lim / 5.0f);
        if (c < 33) peak = std::max(peak, len(v.offset));
    }
    CHECK(peak > 1.0f);
    // Several shakes add up.
    CameraShake two;
    two.start(e, 10);
    two.start(e, 10);
    CHECK(close(two.evaluate(20, R, U).offset, s.evaluate(20, R, U).offset * 2.0f, 1e-5f));
    // Application: eye = pos + 0.1 offset, look point = pos + 1000 d + offset, roll added.
    const tactical::Vec3 pos{0.0f, 0.0f, 0.0f}, look{0.0f, 500.0f, 0.0f};
    const CinematicPose plain = apply_shake(pos, look, 0.0f, CameraShake{}, 20);
    CHECK(close(plain.eye, pos, 0.0f) && close(plain.look, look, 0.0f) && plain.roll_deg == 0.0f);
    CHECK(close(plain.up, {0.0f, 0.0f, 1.0f}, 1e-6f));
    tactical::Vec3 r, u;
    camera_axes(pos, look, 0.0f, r, u);
    CHECK(close(r, {1.0f, 0.0f, 0.0f}, 1e-6f) && close(u, {0.0f, 0.0f, 1.0f}, 1e-6f));  // looking +Y: right +X, up +Z
    const CameraShake::Sample sm = s.evaluate(20, r, u);
    const CinematicPose shaken = apply_shake(pos, look, 0.0f, s, 20);
    CHECK(close(shaken.eye, sm.offset * 0.1f, 1e-5f));
    CHECK(close(shaken.look, tactical::Vec3{0.0f, 1000.0f, 0.0f} + sm.offset, 1e-3f));
    CHECK(close(shaken.roll_deg, sm.roll_deg, 1e-6f));
    s.reset();
    CHECK(s.empty());
}

// Explosion_Jitter_Factor (default 1), the Death_Explosions pick, the gate outside the cinematic (no tracker, no draw).
void test_shake_types() {
    auto w = make_world();
    const objects::GameObjectTypes& ty = w->types;
    const objects::TypeIndex boom = ty.resolve_name("Fx_Boom_Default"), big = ty.resolve_name("Fx_Boom_Big");
    CHECK(boom != objects::kNone && big != objects::kNone);
    CHECK(explosion_jitter_factor(ty, boom) == 1.0f);
    CHECK(explosion_jitter_factor(ty, big) == 3.0f);
    CHECK(explosion_jitter_factor(ty, objects::kNone) == 1.0f);
    CHECK(tsw_explosion_type(ty, w->planet, 0) == boom);
    CHECK(tsw_explosion_type(ty, w->planet, 1) == objects::kNone);
    CHECK(tsw_explosion_type(ty, ty.resolve_name("Fx_Planet_Big"), 1) == big);
    CHECK(tsw_explosion_type(ty, ty.resolve_name("Fx_Planet_Bare"), 0) == objects::kNone);  // no entry: no shake
    CHECK(tsw_explosion_type(ty, w->planet, -1) == objects::kNone);

    CameraShake s;
    UnsyncedRandom rng(1234), ref(1234);
    CHECK(!cinematic_shake(s, false, true, 1.0f, kTswShakeDistance, 348, rng));  // no cinematic: the RTS camera never shakes
    CHECK(!cinematic_shake(s, true, false, 1.0f, kTswShakeDistance, 348, rng));  // not a land / space mode
    CHECK(s.empty());
    CHECK(rng.range(0, 100) == ref.range(0, 100));  // the gate draws nothing
    CHECK(cinematic_shake(s, true, true, 1.0f, kTswShakeDistance, 348, rng));
    CHECK(s.count() == 1);
    CHECK(len(s.evaluate(350, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}).offset) > 0.0f);
    CHECK(s.evaluate(347, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}).roll_deg == 0.0f);
    CHECK(s.evaluate(378, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}).roll_deg == 0.0f);
    // The unsynced generator: inclusive range, all values reachable, lo >= hi -> lo.
    UnsyncedRandom g(7);
    bool lo = false, hi = false, in = true;
    for (int i = 0; i < 20000; ++i) {
        const int32_t v = g.range(0, 100);
        in = in && v >= 0 && v <= 100;
        lo = lo || v == 0;
        hi = hi || v == 100;
    }
    CHECK(in && lo && hi);
    CHECK(g.range(5, 5) == 5 && g.range(9, 3) == 9);
}

// §11.2 key frames: TSW at T0 with the identity frame, E = (10, 20, 30), Ranged_Target_Z_Adjust 6, dummy far away.
void test_firing_shot() {
    TswShotInputs in;
    in.tsw_position = {100.0f, 200.0f, 300.0f};
    in.dummy_position = {100.0f, 5200.0f, 300.0f};
    in.half_extents = {10.0f, 20.0f, 30.0f};
    in.z_adjust = 6.0f;
    in.have_station = true;
    in.station_position = {100.0f, -800.0f, 300.0f};  // M - T = (0, -6000, 0): P4 2000 beyond M, away from the planet
    ObjectFrame f;
    f.valid = true;
    f.position = in.tsw_position;
    const tactical::Vec3 T0 = in.tsw_position, Z{0.0f, 0.0f, 6.0f};
    const tactical::Vec3 P1 = T0 + tactical::Vec3{20.0f, -40.0f, 60.0f} + Z, P2 = T0 + tactical::Vec3{15.0f, -30.0f, 45.0f} + Z;
    const tactical::Vec3 P3 = T0 + tactical::Vec3{150.0f, -100.0f, 150.0f} + Z, P4{100.0f, -800.0f - 8000.0f, 300.0f};
    const tactical::Vec3 L1 = T0 + tactical::Vec3{0.0f, -20.0f, 4.5f} + Z, Mid{100.0f, 2700.0f, 300.0f};

    TswFiringShot shot;
    CHECK(!shot.active());
    shot.begin(in, 1000, 400, 300);
    CHECK(shot.active() && shot.fire_frame() == 1000);
    CHECK(shot.eye_keys().size() == 4 && shot.look_keys().size() == 2);
    CHECK(shot.eye_keys()[1].frame == 300 && shot.eye_keys()[2].frame == 315 && shot.eye_keys()[3].frame == 765);
    // The shot clock is the tactical frame count since Fire; the timer ends at 765.
    CHECK(shot.clock(1000) == 0 && shot.clock(1315) == 315 && shot.clock(999) == 0);
    CHECK(!shot.expired(1764) && shot.expired(1765));
    const float eps = 1e-3f;
    CHECK(close(shot.eye(0, f), P1, eps));
    CHECK(close(shot.eye(150, f), P1 + (P2 - P1) * 0.4f, eps));  // Accel(0.5, 0.5, 0.25) = 0.4
    CHECK(close(shot.eye(225, f), P1 + (P2 - P1) * 0.8f, eps));  // decelerating over the last quarter
    CHECK(close(shot.eye(300, f), P2, eps));
    CHECK(close(shot.eye(314, f), P2, eps));                     // hold
    CHECK(close(shot.eye(315, f), P3, eps));                     // the cut
    CHECK(len(shot.eye(315, f) - shot.eye(314, f)) > 100.0f);
    CHECK(close(shot.eye(540, f), P3 + (P4 - P3) * (1.0f / 3.0f), 0.05f));  // Accel(0.5, 0.5, 0)
    CHECK(close(shot.eye(765, f), P4, eps));
    CHECK(close(shot.eye(900, f), P4, eps));
    CHECK(close(shot.look(0, f), L1, eps) && close(shot.look(200, f), L1, eps) && close(shot.look(314, f), L1, eps));
    CHECK(close(shot.look(315, f), Mid, eps) && close(shot.look(765, f), Mid, eps));
    // Attached keys follow the TSW: moved and turned (model +X -> world +Y, +Y -> world -X).
    ObjectFrame g = f;
    g.position = T0 + tactical::Vec3{50.0f, 0.0f, 0.0f};
    CHECK(close(shot.eye(0, g), P1 + tactical::Vec3{50.0f, 0.0f, 0.0f}, eps));
    CHECK(close(shot.look(315, g), Mid, eps));  // L2 is a world key, fixed at Fire
    ObjectFrame r = f;
    r.axis[0] = {0.0f, 1.0f, 0.0f};
    r.axis[1] = {-1.0f, 0.0f, 0.0f};
    CHECK(close(shot.eye(0, r), T0 + tactical::Vec3{40.0f, 20.0f, 60.0f} + Z, eps));
    // The TSW gone (no frame): its last frame is kept.
    CHECK(close(shot.eye(0, ObjectFrame{}), T0 + tactical::Vec3{40.0f, 20.0f, 60.0f} + Z, eps));
    shot.end();
    CHECK(!shot.active());

    // No `Space Station Position` marker: the eye holds P3 (attached) from 315 to the end.
    in.have_station = false;
    TswFiringShot ns;
    ns.begin(in, 0, 0, 0);
    CHECK(ns.eye_keys().size() == 3);
    CHECK(close(ns.eye(315, f), P3, eps) && close(ns.eye(600, f), P3, eps) && close(ns.eye(765, f), P3, eps));
    CHECK(close(ns.eye(600, g), P3 + tactical::Vec3{50.0f, 0.0f, 0.0f}, eps));
}

// §11.2 input skip, camera save / restore, letterbox.
void test_shot_input_and_restore() {
    TswShotInputs in;
    in.half_extents = {10.0f, 10.0f, 10.0f};
    TswFiringShot shot;
    shot.begin(in, 0, 400, 300);  // 800 x 600: 1/20 = 40 x 30 px
    CHECK(!shot.input_cancels(400, 300, 800, 600, false));
    CHECK(!shot.input_cancels(439, 329, 800, 600, false));  // a small move is swallowed
    CHECK(!shot.input_cancels(361, 271, 800, 600, false));
    CHECK(shot.input_cancels(440, 300, 800, 600, false));   // beyond 1/20 of the width
    CHECK(shot.input_cancels(400, 330, 800, 600, false));   // beyond 1/20 of the height
    CHECK(shot.input_cancels(400, 300, 800, 600, true));    // any button / wheel event
    // Restore: the camera saved once at cinematic mode on comes back at the end, whatever moved it meanwhile.
    struct Cam {
        float x = 0.0f, znear = 10.0f;
    };
    SavedCamera<Cam> saved;
    Cam cam{5.0f, 10.0f};
    CHECK(!saved.restore(cam) && cam.x == 5.0f);
    saved.save_once(cam);
    cam.x = 99.0f;
    saved.save_once(cam);  // once: a second Fire keeps the first copy
    cam.znear = 100.0f;
    CHECK(saved.saved());
    CHECK(saved.restore(cam) && cam.x == 5.0f && cam.znear == 10.0f);
    CHECK(!saved.saved());
    // Letterbox: 0.5 s in, out at the same rate from where it is, removal at once.
    Letterbox lb;
    CHECK(!lb.visible());
    lb.slide_in();
    lb.update(0.25f);
    CHECK(close(lb.level(), 0.5f, 1e-6f));
    lb.update(0.5f);
    CHECK(lb.level() == 1.0f);
    lb.slide_out();
    lb.update(0.1f);
    CHECK(close(lb.level(), 0.8f, 1e-6f));
    lb.update(0.0f);
    lb.update(-1.0f);
    CHECK(close(lb.level(), 0.8f, 1e-6f));
    lb.update(1.0f);
    CHECK(lb.level() == 0.0f && !lb.visible());
    lb.slide_in();
    lb.update(0.1f);
    lb.remove();
    CHECK(!lb.visible());
}

#ifdef ALAMO_GAME_HAVE_AUDIO
bool logged(const audio::AudioSystem& a, const std::string& event) {
    for (const audio::LogEntry& e : a.log())
        if (e.event == event && e.op == audio::LogOp::SfxStart) return true;
    return false;
}

void test_tsw_audio() {
    auto w = make_world();
    GameData data;
    data.files = make_tree();
    GameAudioHost h;
    std::string err;
    CHECK(h.init(data, AudioBackendKind::Null, 11, &err));
    CHECK(h.active());
    if (!h.active()) return;
    h.music(MusicScene::Battle, "", true);
    objects::ObjectStore& st = *w->store;
    const objects::ObjectId tsw = st.id(w->tsw), dummy = st.id(w->dummy);
    std::vector<ta::SpecialWeaponEvent> evs{ev(ta::SwEventKind::TswFiring, tsw, dummy)};
    h.tsw_audio(tsw_cues(evs, st), w->types);
    CHECK(h.stats().tsw_activations == 1);
    CHECK(h.system()->current_music() == "Fx_TSW_Music" && h.system()->music_mode() == audio::music_mode::kSuperWeapon);
    CHECK(logged(*h.system(), "Fx_Activate_Voice"));
    CHECK(logged(*h.system(), "Fx_Power_Up"));

    evs = {ev(ta::SwEventKind::TswAttackAnim, tsw, dummy)};
    h.tsw_audio(tsw_cues(evs, st), w->types);
    CHECK(h.stats().tsw_fires == 1 && logged(*h.system(), "Fx_Superlaser"));

    CHECK(st.destroy(w->dummy));
    ta::SpecialWeaponEvent kill = ev(ta::SwEventKind::TswPlanetDestroyed, tsw, dummy);
    kill.point = {0.0f, 5000.0f, 100.0f};
    evs = {kill};
    h.tsw_audio(tsw_cues(evs, st), w->types);
    CHECK(h.stats().tsw_planet_deaths == 1 && logged(*h.system(), "Fx_Planet_Death"));

    // A cue with no type is skipped.
    TswCue bad;
    bad.kind = TswCue::Kind::PlanetDestroyed;
    const std::vector<TswCue> bads{bad};
    h.tsw_audio(bads, w->types);
    CHECK(h.stats().tsw_planet_deaths == 1);
}
#endif

void test_inactive() {
    auto w = make_world();
    GameData data;
    GameAudioHost h;
    CHECK(h.init(data, AudioBackendKind::Off, 1));
    const std::vector<ta::SpecialWeaponEvent> evs{ev(ta::SwEventKind::TswFiring, w->store->id(w->tsw))};
    h.tsw_audio(tsw_cues(evs, *w->store), w->types);
    CHECK(h.stats().tsw_activations == 0);
}

}  // namespace

int main() {
    test_tsw_cues();
    test_noise();
    test_profiles();
    test_explosion_shake();
    test_shake_types();
    test_firing_shot();
    test_shot_input_and_restore();
    test_inactive();
#ifdef ALAMO_GAME_HAVE_AUDIO
    test_tsw_audio();
#endif
    std::printf(g_fail == 0 ? "game battle fx tests passed\n" : "%d game battle fx check(s) FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
