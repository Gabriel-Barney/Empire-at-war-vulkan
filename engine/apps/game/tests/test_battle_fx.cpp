// alamo_game_battle_fx_tests (refix t06, data-free): the tactical super weapon's presentation cues (battle_fx.hpp) on a
// synthetic type set and object store - Firing / AttackAnim at the TSW, PlanetDestroyed at the target dummy while it is
// dead but not flushed, nothing without a dummy or for other events -, the camera shake's envelope / falloff / cap, and
// GameAudioHost::tsw_audio on a synthetic audio tree with the null backend (activation voice + power-up SFX, mode 8
// music, SFXEvent_Fire, the dummy's Death_SFXEvent_Start_Die).
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
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
  <Death_SFXEvent_Start_Die>Fx_Planet_Death</Death_SFXEvent_Start_Die></SpaceProp>
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
        CHECK(c[0].kind == TswCue::Kind::Firing && c[0].object == tsw && c[0].type == w->ds);
        CHECK(c[0].position.x == 100.0f && c[0].position.y == 200.0f && c[0].position.z == 300.0f);
        CHECK(c[1].kind == TswCue::Kind::AttackAnim && c[1].object == tsw && c[1].type == w->ds);
        CHECK(c[2].kind == TswCue::Kind::PlanetDestroyed && c[2].object == dummy && c[2].type == w->planet);
        CHECK(c[2].position.y == 5000.0f && c[2].position.z == 100.0f);
    }
    // Once flushed the dummy is unknown: no PlanetDestroyed cue (a dummy gone before state 9).
    st.flush();
    c = tsw_cues(evs, st);
    CHECK(c.size() == 2);
    CHECK(tsw_cues({}, st).empty());
}

float len(tactical::Vec3 v) { return std::sqrt(tactical::dot(v, v)); }

void test_camera_shake() {
    const tactical::Vec3 eye{0.0f, -1000.0f, 1000.0f}, target{0.0f, 0.0f, 0.0f};
    const float dist = len(eye - target);
    CameraShake s;
    CHECK(!s.active() && len(s.offset(eye, target)) == 0.0f);
    s.start({0.0f, 5000.0f, 0.0f}, kTswPlanetShake);
    CHECK(s.active() && s.count() == 1);
    // Bounded by magnitude x distance x |(1, 1, 0.6)|, non-zero while it runs, decays to zero.
    const float bound = kTswPlanetShake.magnitude * dist * std::sqrt(2.36f) * 1.001f;
    float peak = 0.0f, late = 0.0f;
    for (int i = 0; i < 89; ++i) {  // 89 frames of 1/30 s < 3 s
        s.update(1.0 / 30.0);
        const float o = len(s.offset(eye, target));
        CHECK(o <= bound);
        peak = std::max(peak, o);
        if (i >= 80) late = std::max(late, o);
    }
    CHECK(s.active());
    CHECK(peak > 0.25f * kTswPlanetShake.magnitude * dist);
    CHECK(late < 0.05f * peak);  // the squared fade
    s.update(1.0 / 30.0);
    s.update(1.0 / 30.0);
    CHECK(!s.active() && len(s.offset(eye, target)) == 0.0f);

    // Same inputs, same offsets (no RNG); the eye-target distance scales the offset.
    CameraShake a, b;
    a.start({}, kTswPlanetShake);
    b.start({}, kTswPlanetShake);
    a.update(0.1);
    b.update(0.1);
    const tactical::Vec3 oa = a.offset(eye, target), ob = b.offset(eye, target);
    CHECK(oa.x == ob.x && oa.y == ob.y && oa.z == ob.z);
    const tactical::Vec3 far_eye{0.0f, -2000.0f, 2000.0f};
    CHECK(std::fabs(len(a.offset(far_eye, target)) - 2.0f * len(oa)) <= 1e-3f * len(oa) + 1e-6f);

    // Radius falloff: nothing beyond the radius, full strength at the source.
    CameraShakeParams local{1.0f, 0.02f, 8.0f, 500.0f};
    CameraShake r;
    r.start({0.0f, 0.0f, 0.0f}, local);
    r.update(0.05);
    CHECK(len(r.offset(eye, target)) > 0.0f);
    CHECK(len(r.offset(eye + tactical::Vec3{600.0f, 0.0f, 0.0f}, tactical::Vec3{600.0f, 0.0f, 0.0f})) == 0.0f);

    // Cap: a ninth shake drops the oldest; zero-length / zero-magnitude shakes are ignored; dt <= 0 changes nothing.
    CameraShake m;
    for (size_t i = 0; i < CameraShake::kMaxShakes + 3; ++i) m.start({}, kTswPlanetShake);
    CHECK(m.count() == CameraShake::kMaxShakes);
    m.start({}, CameraShakeParams{0.0f, 0.02f, 8.0f, 0.0f});
    m.start({}, CameraShakeParams{1.0f, 0.0f, 8.0f, 0.0f});
    CHECK(m.count() == CameraShake::kMaxShakes);
    m.update(0.0);
    m.update(-1.0);
    CHECK(m.count() == CameraShake::kMaxShakes);
    m.reset();
    CHECK(!m.active());
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
    test_camera_shake();
    test_inactive();
#ifdef ALAMO_GAME_HAVE_AUDIO
    test_tsw_audio();
#endif
    std::printf(g_fail == 0 ? "game battle fx tests passed\n" : "%d game battle fx check(s) FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
