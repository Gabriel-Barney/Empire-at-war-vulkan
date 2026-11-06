// engine/lua presentation family (L1 stage 2): camera / cinematic / fade / letterbox / fog / audio /
// UI bindings write a deterministic PresentationLog + PresentationState; blocking calls
// (Play_Bink_Movie, Play_Lightning_Effect) finish on fixed sim frames; presentation calls never
// change a sim CRC.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "alamo/core/bytes.hpp"
#include "alamo/core/file_provider.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/core/sim_rng.hpp"
#include "alamo/lua/bindings.hpp"
#include "alamo/lua/lua_vm.hpp"
#include "alamo/lua/presentation.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/object_store.hpp"
#include "alamo/objects/object_types.hpp"
#include "vm_internal.hpp"  // the per-family binding tables (duplicate check)

#include <lua.h>

using namespace alamo;
using namespace alamo::lua;
namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __builtin_FILE(), __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)

static std::string run(LuaVm& vm, const std::string& code) {
    std::string err;
    if (!vm.run(code, "test", &err)) return err.empty() ? "error" : err;
    return {};
}

static double gnum(LuaVm& vm, const char* name) {
    lua_State* L = vm.state();
    lua_pushstring(L, name);
    lua_rawget(L, LUA_GLOBALSINDEX);
    const double d = lua_tonumber(L, -1);
    lua_pop(L, 1);
    return d;
}

static bool contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

// PGBase.lua-shaped helpers (same structure as the shipped Sleep / BlockOnCommand / PumpEvents).
static const char* kHelpers =
    "function Pump() coroutine.yield(true) local ev = GetEvent() while ev do ev() ev = GetEvent() end end\n"
    "function Sleep(t) ThreadValue.Set('St', GetCurrentTime()) while GetCurrentTime() - ThreadValue('St') < t do Pump() end end\n"
    "function BlockOnCommand(block) Pump() if not block then return nil end\n"
    "  repeat Pump() until block.IsFinished() == true\n"
    "  Pump() return block.Result() end\n";

// First frame on which a Sleep(t) begun on frame `start` returns (time = frame / 30.0, as step()).
static uint32_t sleep_end_frame(uint32_t start, double t) {
    uint32_t f = start;
    while (f / 30.0 - start / 30.0 < t) ++f;
    return f;
}

static void step(LuaVm& vm, uint32_t frame) {
    vm.set_frame(frame);
    vm.set_time(frame / 30.0);
    vm.service_threads();
}

// A tiny object world (one marker type) shared by the tests.
struct World {
    fs::path root;
    objects::ObjectTypeDatabase raw;
    objects::GameObjectTypes types;
    objects::IdCounter ids;
    std::unique_ptr<objects::ObjectStore> store;
    GameRandom rng{1234u, 99u};

    World() {
        root = fs::temp_directory_path() / "alamo_lua_presentation_test";
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(root / "Data/XML");
        std::ofstream(root / "Data/XML/GameObjectFiles.xml", std::ios::binary) << "<L><File>S.xml</File></L>";
        std::ofstream(root / "Data/XML/S.xml", std::ios::binary)
            << "<R><Prop Name=\"Cam_Marker\"></Prop><GroundInfantry Name=\"Trooper\">"
               "<Tactical_Health>40</Tactical_Health></GroundInfantry></R>";
        DirectoryProvider dp(root.string());
        CHECK(raw.load(dp));
        CHECK(types.build(raw));
        store = std::make_unique<objects::ObjectStore>(objects::ModeKind::Land, objects::StoreKind::Primary, types, &ids);
        objects::SpawnParams sp;
        sp.type = raw.find("Cam_Marker");
        sp.position = {10.0f, 20.0f, 0.0f};
        store->create(sp);  // id 1
        sp.type = raw.find("Trooper");
        sp.owner = 1;
        sp.position = {-5.0f, 3.0f, 0.0f};
        store->create(sp);  // id 2
    }
    ~World() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    LuaHost host() {
        LuaHost h;
        h.store = store.get();
        h.types = &types;
        h.rng = &rng;
        return h;
    }
};

static const char* kFind =
    "local all = Find_All_Objects_Of_Type('Cam_Marker') marker = all[1]\n"
    "trooper = Find_All_Objects_Of_Type('Trooper')[1]\n";

// ------------------------------------------------------------------------------------------

static void test_registry() {
    // Every presentation binding is implemented and no implemented name is duplicated across families.
    for (const char* n : {"Transition_Cinematic_Camera_Key", "Set_Cinematic_Camera_Key", "Set_Cinematic_Target_Key",
                          "Start_Cinematic_Camera", "End_Cinematic_Camera", "Fade_Screen_In", "Fade_Screen_Out",
                          "Letter_Box_In", "Letter_Box_Out", "Lock_Controls", "Point_Camera_At",
                          "Transition_To_Tactical_Camera", "Cancel_Fast_Forward", "Allow_Localized_SFX",
                          "Play_Bink_Movie", "Play_Lightning_Effect", "Enable_Fog", "Enable_Distance_Fog",
                          "GUI_Component_Visibility", "Activate_Retry_Dialog", "Camera_To_Follow"}) {
        bool found = false;
        for (const BindingInfo& b : binding_table())
            if (b.kind == BindingKind::Global && std::string(b.name) == n) found = binding_implemented(b);
        if (!found) std::printf("not implemented: %s\n", n);
        CHECK(found);
    }
    // No presentation entry is also implemented by another family (the first one would win silently).
    for (const detail::ImplEntry& p : detail::impl_presentation())
        for (const auto family : {detail::impl_core(), detail::impl_objects(), detail::impl_players(), detail::impl_story()})
            for (const detail::ImplEntry& o : family)
                if (o.kind == p.kind && std::string(o.cls) == p.cls && std::string(o.name) == p.name) {
                    std::printf("duplicate binding: %s.%s\n", p.cls, p.name);
                    CHECK(false);
                }
    LuaHost h;
    LuaVm vm(h);
    CHECK(vm.resolve_method("GameObjectWrapper", "Play_SFX_Event") == 1);
    CHECK(vm.resolve_method("LuaSFXCommandsClassClass", "Allow_Ambient_VO") == 1);
    for (int i = 0; i < static_cast<int>(PresentationOp::Count); ++i)
        CHECK(op_name(static_cast<PresentationOp>(i)) != "?");
    CHECK(seconds_to_frames(0.0) == 0 && seconds_to_frames(-1.0) == 0 && seconds_to_frames(0.5) == 15 &&
          seconds_to_frames(1.0) == 30 && seconds_to_frames(0.01) == 1 && seconds_to_frames(12.0) == 360);
}

// Stage 3 object effects: recorded with the receiver, state for sub-objects / emitters / end cinematic.
static void test_object_effects() {
    World w;
    LuaHost h = w.host();
    LuaVm vm(h);
    PresentationLog log;
    attach_presentation(vm, &log);
    vm.set_frame(7);
    CHECK(run(vm, std::string(kFind) +
                      "Hide_Sub_Object(trooper, 1, 'lightsaber') Hide_Sub_Object(trooper, 0, 'gun') "
                      "trooper.Attach_Particle_Effect('Rescue_Effect') trooper.Attach_Particle_Effect(Find_Object_Type('Trooper'), 'Bone_A') "
                      "trooper.Cinematic_Hyperspace_In(20) trooper.In_End_Cinematic(true) Resume_Hyperspace_In() "
                      "trooper.Show_Emitter('Engine', false) trooper.Play_Cinematic_Engine_Flyby()") == "");
    const auto& ev = log.events();
    CHECK(ev.size() == 9 && ev[0].op == PresentationOp::HideSubObject && ev[0].frame == 7);
    CHECK(ev[2].op == PresentationOp::ObjectAttachParticleEffect && ev[2].args.size() == 2 && ev[2].args[0].id == 2 &&
          ev[2].args[1].text == "Rescue_Effect");
    CHECK(ev[3].args.size() == 3 && ev[3].args[1].kind == PresentationArg::Kind::Type && ev[3].args[2].text == "Bone_A");
    // OI lua: the resolved requests a render host consumes (object id, type index / name, bone), drained with the events.
    const auto& pe = log.particle_effects();
    CHECK(pe.size() == 2 && pe[0].frame == 7 && pe[0].seq == ev[2].seq && pe[0].object == 2 && pe[0].type == -1 &&
          pe[0].type_name == "Rescue_Effect" && pe[0].bone.empty());
    CHECK(pe.size() == 2 && pe[1].seq == ev[3].seq && pe[1].type == ev[3].args[1].id && pe[1].type_name == "Trooper" &&
          pe[1].bone == "Bone_A");
    CHECK(run(vm, "trooper.Attach_Particle_Effect('TROOPER')") == "" && log.particle_effects().size() == 3 &&
          log.particle_effects()[2].type == ev[3].args[1].id && log.particle_effects()[2].type_name == "Trooper");
    CHECK(ev[4].op == PresentationOp::ObjectCinematicHyperspaceIn && ev[4].args[1].number == 20.0);
    CHECK(ev[6].op == PresentationOp::ResumeHyperspaceIn && ev[8].op == PresentationOp::ObjectPlayCinematicEngineFlyby);
    const PresentationState& s = log.state();
    CHECK(s.hidden_sub_objects.at("2|LIGHTSABER") && !s.hidden_sub_objects.at("2|GUN") && s.in_end_cinematic.at(2) &&
          !s.emitters.at("2|ENGINE"));
    CHECK(contains(run(vm, "Hide_Sub_Object(trooper, 1)"), "LuaHideSubObject -- Expected gameobject pointer parameter 1"));
    CHECK(contains(run(vm, "Hide_Sub_Object(5, 1, 'x')"), "LuaHideSubObject -- Invalid parameter 1, expected a game object"));
    CHECK(contains(run(vm, "Hide_Sub_Object(trooper, 'a', 'x')"), "LuaHideSubObject -- Invalid parameter 2, expected a float"));
    CHECK(contains(run(vm, "trooper.Attach_Particle_Effect()"), "Attach_Particle_Effect -- Invalid number of parameters. Expected 1 or 2, got 0."));
    CHECK(contains(run(vm, "trooper.Attach_Particle_Effect(5)"), "Invalid type for parameter 1. Expected type or type name."));
    CHECK(contains(run(vm, "trooper.Cinematic_Hyperspace_In('x')"), "Cinematic_Hyperspace_In -- invalid type for parameter 1. Expected number."));
    CHECK(contains(run(vm, "trooper.In_End_Cinematic()"), "GameObjectWrapper::Lua_Is_In_End_Cinematic -- Missing required boolean parameter."));
    CHECK(contains(run(vm, "trooper.Show_Emitter('x')"), "Show_Emitter -- invalid number of parameters. Expected 2, got 1."));
}

static void test_argument_contracts() {
    World w;
    LuaHost h = w.host();
    LuaVm vm(h);
    CHECK(run(vm, kFind) == "");
    CHECK(contains(run(vm, "Fade_Screen_In()"), "LuaFadeScreenIn -- Expected float parameter 1"));
    CHECK(contains(run(vm, "Fade_Screen_Out('x')"), "LuaFadeScreenOut -- Invalid parameter 1, expected a float"));
    CHECK(contains(run(vm, "Letter_Box_In({})"), "LuaLetterBoxIn -- Invalid parameter 1, expected a float"));
    CHECK(contains(run(vm, "Lock_Controls()"), "LuaLockControls -- Expected float parameter 1"));
    CHECK(contains(run(vm, "Set_Cinematic_Camera_Key(marker, 1, 2)"), "LuaSetCinematicCameraKey -- Expected 8 parameters"));
    CHECK(contains(run(vm, "Set_Cinematic_Camera_Key(5, 300, 5, 100, 1, 0, 0, 0)"),
                   "LuaSetCinematicCameraKey -- Invalid parameter 1, expected a position."));
    CHECK(contains(run(vm, "Transition_Cinematic_Target_Key(marker, 'x', 0, 0, 0, 0, 0, 0, 0)"),
                   "LuaTransitionCinematicTargetKey -- Invalid parameter 2, expected a float"));
    CHECK(contains(run(vm, "Point_Camera_At(nil)"), "LuaPointCamera -- Expected position parameter 1"));
    CHECK(contains(run(vm, "Start_Cinematic_Camera(3)"), "LuaStartCinematicCamera -- invalid type for parameter 1. Expected bool."));
    CHECK(contains(run(vm, "Enable_Fog(1)"), "LuaEnableFog -- invalid type for parameter 1. Expected bool."));
    CHECK(contains(run(vm, "Enable_Distance_Fog()"), "Enable_Distance_Fog -- invalid number of parameters. Expected 1, got 0."));
    CHECK(contains(run(vm, "Allow_Localized_SFX()"), "LuaAllowLocalizedSFX -- Expected bool parameter 1"));
    CHECK(contains(run(vm, "Play_Music()"), "Play_Music -- invalid number of parameters. Expected 1, got 0."));
    CHECK(contains(run(vm, "Cinematic_Zoom(1)"), "LuaCinematicZoom -- Expected float param 1 time, float param 2 delta"));
    CHECK(contains(run(vm, "GUI_Component_Visibility('b_sell', 1)"),
                   "GUI_Component_Visibility -- invalid type for parameter 2. Expected bool."));
    CHECK(contains(run(vm, "Play_Bink_Movie(3)"), "LuaPlayBinkMovie -- Expected type string for parameter 1"));
    CHECK(contains(run(vm, "Play_Lightning_Effect('Zap', marker)"),
                   "Play_Lightning_Effect -- invalid number of parameters. Expected 3, got 2."));
    CHECK(contains(run(vm, "trooper.Play_SFX_Event(1)"), "GameObjectWrapper::Play_SFX_Event -- invalid type for parameter 1. Expected string."));
    // Failed calls record nothing.
    CHECK(presentation(vm).events().empty());
}

// The canonical cinematic sequence of the shipped story scripts, driven frame by frame.
static const char* kCinematic =
    "function Intro()\n"
    "  Lock_Controls(1) Suspend = nil\n"
    "  Start_Cinematic_Camera()\n"
    "  Letter_Box_In(1)\n"
    "  Fade_Screen_Out(.5)\n"
    "  Set_Cinematic_Camera_Key(marker, 300, 5, 100, 1, 0, 0, 0)\n"
    "  Set_Cinematic_Target_Key(trooper, 0, 0, 0, 0, trooper, 0, 0)\n"
    "  Transition_Cinematic_Camera_Key(marker, 2, 500, 10, 97, 1, marker, 0, 0)\n"
    "  Enable_Fog(false) Allow_Localized_SFX(false) Cancel_Fast_Forward()\n"
    "  SFXManager.Allow_Ambient_VO(false) trooper.Play_SFX_Event('Unit_Select', 2)\n"
    "  Play_Music('Cinematic_Empire_Intro_Music_Event_2') Game_Message('TEXT_X')\n"
    "  Sleep(2)\n"
    "  marks = GetCurrentTime.Frame()\n"
    "  Transition_To_Tactical_Camera(1) Letter_Box_Out(.5) Fade_Screen_In(.5)\n"
    "  Sleep(1)\n"
    "  End_Cinematic_Camera() Lock_Controls(0) Resume_Mode_Based_Music()\n"
    "  marke = GetCurrentTime.Frame()\n"
    "end\n";

struct CinematicRun {
    std::vector<PresentationEvent> events;
    uint64_t digest = 0;
    double sleep_end = 0, end = 0;
};

static CinematicRun run_cinematic(bool check_state) {
    World w;
    LuaHost h = w.host();
    LuaVm vm(h);
    PresentationLog log;
    attach_presentation(vm, &log);
    log.set_fast_forward(true);
    CHECK(run(vm, std::string(kHelpers) + kFind + kCinematic + "Create_Thread('Intro')") == "");
    const uint32_t blend_mid = sleep_end_frame(10, 2.0) + 15;
    for (uint32_t f = 10; f <= 200; ++f) {
        step(vm, f);
        if (!check_state) continue;
        const PresentationState& s = log.state();
        if (f == blend_mid)  // half way through Transition_To_Tactical_Camera(1); still cinematic
            CHECK(s.tactical_blend.at(f) == 0.5 && s.camera == CameraMode::Cinematic && s.controls_locked);
        if (f == 10) {
            // All the first-frame calls happened at frame 10.
            CHECK(s.camera == CameraMode::Cinematic && s.cinematic_rate_lock() && s.controls_locked);
            CHECK(!s.fog_enabled && !s.localized_sfx && !s.fast_forward && !s.ambient_vo);
            CHECK(log.letterbox_level(10) == 0.0 && log.letterbox_level(25) == 0.5 && log.letterbox_level(40) == 1.0);
            CHECK(log.fade_level(10) == 0.0 && log.fade_level(25) == 1.0);
            CHECK(s.camera_key.set && s.camera_key.start == 10 && s.camera_key.end == 70);
            CHECK(s.camera_key.args.size() == 8 && s.camera_key.args[0].kind == PresentationArg::Kind::Object &&
                  s.camera_key.args[0].id == 1 && s.camera_key.args[1].number == 500.0 &&
                  s.camera_key.args[5].kind == PresentationArg::Kind::Object);
            CHECK(s.target_key.set && s.target_key.start == s.target_key.end && s.target_key.args[0].id == 2);
            CHECK(s.music == "Cinematic_Empire_Intro_Music_Event_2" && !s.mode_based_music);
        }
    }
    CinematicRun r;
    r.sleep_end = gnum(vm, "marks");
    r.end = gnum(vm, "marke");
    if (check_state) {
        const PresentationState& s = log.state();
        const uint32_t ms = sleep_end_frame(10, 2.0), me = sleep_end_frame(ms, 1.0);
        CHECK(r.sleep_end == ms && r.end == me && ms >= 70 && ms <= 71);
        // Out/in ramps started at the end of Sleep(2) from the full level.
        CHECK(log.letterbox_level(ms) == 1.0 && log.letterbox_level(ms + 15) == 0.0);
        CHECK(log.fade_level(ms) == 1.0 && log.fade_level(ms + 15) == 0.0);
        CHECK(s.tactical_blend.start == me && s.tactical_blend.end == me);  // reset by End_Cinematic_Camera
        CHECK(s.camera == CameraMode::Tactical && !s.cinematic_rate_lock() && !s.controls_locked);
        CHECK(s.mode_based_music && s.music.empty());
        // Event log: every call, in call order, stamped with its sim frame.
        const auto& ev = log.events();
        CHECK(ev.size() == 20);
        for (size_t i = 0; i < ev.size(); ++i) CHECK(ev[i].seq == i);
        CHECK(ev.front().op == PresentationOp::LockControls && ev.front().frame == 10);
        CHECK(ev[1].op == PresentationOp::StartCinematicCamera && ev[1].args.empty());
        CHECK(ev[3].op == PresentationOp::FadeScreenOut && ev[3].args.size() == 1 && ev[3].args[0].number == 0.5);
        const PresentationEvent* sfx = nullptr;
        for (const auto& e : ev)
            if (e.op == PresentationOp::ObjectPlaySfxEvent) sfx = &e;
        CHECK(sfx && sfx->args.size() == 3 && sfx->args[0].id == 2 && sfx->args[1].text == "Unit_Select");
        CHECK(ev.back().op == PresentationOp::ResumeModeBasedMusic && ev.back().frame == me);
        CHECK(op_name(ev[6].op) == "Transition_Cinematic_Camera_Key");
    }
    r.events = log.events();
    r.digest = log.digest();
    attach_presentation(vm, nullptr);
    return r;
}

static void test_cinematic_sequence() {
    const CinematicRun a = run_cinematic(true);
    const CinematicRun b = run_cinematic(false);
    CHECK(a.events == b.events && a.digest == b.digest && a.digest != 0);
    CHECK(a.sleep_end == b.sleep_end && a.end == b.end);
}

static void test_private_log_and_digest() {
    // Without attach_presentation the VM keeps its own log (destroyed with the VM).
    LuaHost h;
    uint64_t d1 = 0, d2 = 0, d3 = 0;
    {
        LuaVm vm(h);
        vm.set_frame(5);
        CHECK(run(vm, "Fade_Screen_Out(1) Letter_Box_On() Enable_Distance_Fog(false)") == "");
        CHECK(presentation(vm).events().size() == 3 && presentation(vm).events()[0].frame == 5);
        CHECK(presentation(vm).fade_level(20) == 0.5 && presentation(vm).state().letterbox.at(5) == 1.0);
        d1 = presentation(vm).digest();
    }
    {
        LuaVm vm(h);
        vm.set_frame(5);
        CHECK(run(vm, "Fade_Screen_Out(1) Letter_Box_On() Enable_Distance_Fog(false)") == "");
        d2 = presentation(vm).digest();
    }
    {
        LuaVm vm(h);
        vm.set_frame(6);  // same calls one frame later: different log
        CHECK(run(vm, "Fade_Screen_Out(1) Letter_Box_On() Enable_Distance_Fog(false)") == "");
        d3 = presentation(vm).digest();
    }
    CHECK(d1 == d2 && d1 != d3);
}

static void test_blocking() {
    World w;
    LuaHost h = w.host();
    LuaVm vm(h);
    PresentationLog log;
    log.config().movie_frames = 45;
    log.config().lightning_frames_of = [](std::string_view e) { return e == "Hack_Electicity_Zaps" ? 12u : 30u; };
    attach_presentation(vm, &log);
    CHECK(run(vm, std::string(kHelpers) + kFind +
                      "function Movie()\n"
                      "  local b = Play_Bink_Movie('Star_Wars_Intro')\n"
                      "  started = GetCurrentTime.Frame() before = b.IsFinished() and 1 or 0\n"
                      "  res = BlockOnCommand(b)\n"
                      "  movie_done = GetCurrentTime.Frame()\n"
                      "  BlockOnCommand(Play_Lightning_Effect('Hack_Electicity_Zaps', marker, trooper))\n"
                      "  zap_done = GetCurrentTime.Frame()\n"
                      "  Play_Lightning_Effect('Other', marker, trooper)\n"  // not waited on
                      "end\n"
                      "Create_Thread('Movie')") == "");
    for (uint32_t f = 0; f <= 120; ++f) step(vm, f);
    CHECK(gnum(vm, "started") == 0.0 && gnum(vm, "before") == 0.0);
    // Finished on frame 0 + 45; PGBase's BlockOnCommand pumps once more after IsFinished, so the
    // script continues on the next service.
    CHECK(gnum(vm, "movie_done") == 46.0);
    CHECK(gnum(vm, "zap_done") == 46.0 + 12.0 + 1.0);
    CHECK(vm.thread_count() == 0 && vm.errors().empty());
    CHECK(log.state().movie == "Star_Wars_Intro");

    // Stop_Bink_Movie from another thread ends the block early; the waiter resumes on its next service.
    log.config().movie_frames = 1000;
    CHECK(run(vm, "function Long() BlockOnCommand(Play_Bink_Movie('Long')) long_done = GetCurrentTime.Frame() end\n"
                  "function Stopper() Sleep(1) Stop_Bink_Movie() stopped_at = GetCurrentTime.Frame() end\n"
                  "Create_Thread('Long') Create_Thread('Stopper')") == "");
    for (uint32_t f = 200; f <= 300; ++f) step(vm, f);
    const uint32_t stop = sleep_end_frame(200, 1.0);
    CHECK(gnum(vm, "stopped_at") == stop);
    // Long (lower thread id) checks before Stopper runs on frame `stop`: sees it finished on stop + 1.
    CHECK(gnum(vm, "long_done") == stop + 2.0);
    CHECK(log.state().movie.empty() && log.state().movie_block == 0);
    // Blocking events carry their block id.
    uint32_t blocks = 0;
    for (const auto& e : log.events())
        if (e.block) ++blocks;
    CHECK(blocks == 4);
    attach_presentation(vm, nullptr);
}

// Presentation calls change nothing the sim CRC hashes (objects, synced RNG) and draw no randomness.
static void test_sim_crc_unchanged() {
    auto capture = [](bool with_presentation, PresentationLog* out_log) {
        World w;
        LuaHost h = w.host();
        LuaVm vm(h);
        PresentationLog log;
        attach_presentation(vm, &log);
        CrcRegistry reg;
        w.store->register_crc(reg, "objects");
        reg.add("rng", [&w](StateHasher& s) { w.rng.hash(s); });
        const std::string pres =
            with_presentation
                ? "Start_Cinematic_Camera() Lock_Controls(1) Letter_Box_In(1) Fade_Screen_Out(.5) Enable_Fog(false)\n"
                  "Set_Cinematic_Camera_Key(marker, 300, 5, 100, 1, 0, 0, 0) Point_Camera_At(trooper)\n"
                  "trooper.Play_SFX_Event('X') Play_Music('M') Cancel_Fast_Forward()\n"
                  "Play_Bink_Movie('Intro') Play_Lightning_Effect('Z', marker, trooper) GUI_Component_Text('a', 'TEXT_B')\n"
                  "SFXManager.Allow_HUD_VO(false) Transition_To_Tactical_Camera(1) End_Cinematic_Camera()\n"
                : "";
        std::vector<CrcRecord> recs;
        CHECK(run(vm, std::string(kFind) + "r1 = GameRandom(1, 100)\n" + pres + "r2 = GameRandom(1, 100)\n") == "");
        for (uint32_t f = 0; f < 5; ++f) {
            step(vm, f);
            recs.push_back(reg.capture(f));
        }
        if (out_log) *out_log = log;
        attach_presentation(vm, nullptr);
        return std::make_pair(recs, gnum(vm, "r1") * 1000.0 + gnum(vm, "r2"));
    };
    PresentationLog with_log;
    const auto plain = capture(false, nullptr);
    const auto with = capture(true, &with_log);
    CHECK(with_log.events().size() == 16);  // the calls did happen
    CHECK(plain.first == with.first);       // identical per-tick CRC records (ordered + parity)
    CHECK(plain.second == with.second);     // identical random draws around the calls
}

// N1 save / load: a cinematic saved mid-sequence (letterbox / fade ramps running, a movie block open, sub-object /
// emitter / end-cinematic flags set, events and particle requests not drained) together with its VM image continues
// identically after a load into a fresh VM + log; write -> read -> write is byte-identical; bad data is refused.
static void test_save_load() {
    const std::string script = std::string(kHelpers) + kFind + kCinematic +
                               "function Movie() Sleep(0.5) Hide_Sub_Object(trooper, 1, 'gun') trooper.Show_Emitter('Engine', true)\n"
                               "  trooper.In_End_Cinematic(true) trooper.Attach_Particle_Effect('Trooper', 'Bone_A')\n"
                               "  res = BlockOnCommand(Play_Bink_Movie('Mid_Movie')) movie_done = GetCurrentTime.Frame() end\n"
                               "Create_Thread('Intro') Create_Thread('Movie')";
    World w1;
    LuaHost h1 = w1.host();
    LuaVm a(h1);
    PresentationLog la;
    la.config().movie_frames = 60;
    attach_presentation(a, &la);
    la.set_fast_forward(true);
    CHECK(run(a, script) == "");
    for (uint32_t f = 10; f <= 40; ++f) step(a, f);
    const PresentationState& sa = la.state();
    CHECK(sa.camera == CameraMode::Cinematic && sa.controls_locked && sa.movie == "Mid_Movie" && sa.movie_block != 0 &&
          !sa.hidden_sub_objects.empty() && !sa.emitters.empty() && !sa.in_end_cinematic.empty() && la.events().size() > 10 &&
          la.particle_effects().size() == 1);
    ByteWriter wl;
    la.write(wl);
    ByteWriter wv;
    a.write(wv);
    CHECK(a.save_error().empty());

    World w2;
    LuaHost h2 = w2.host();
    LuaVm b(h2);
    PresentationLog lb;
    lb.config().movie_frames = 60;  // configuration is the host's (not saved)
    attach_presentation(b, &lb);
    {
        ByteReader r(wl.bytes());
        CHECK(lb.read(r) && r.done());
        ByteReader rv(wv.bytes());
        CHECK(b.read(rv));
    }
    CHECK(lb.digest() == la.digest() && lb.events() == la.events() && lb.particle_effects() == la.particle_effects());
    ByteWriter again;
    lb.write(again);
    CHECK(again.bytes() == wl.bytes());

    bool same = true;
    for (uint32_t f = 41; f <= 200; ++f) {
        step(a, f);
        step(b, f);
        same = same && la.digest() == lb.digest() && la.fade_level(f) == lb.fade_level(f) && la.letterbox_level(f) == lb.letterbox_level(f);
        if (f % 50 == 0) {  // a consumer draining both logs
            la.clear_events();
            lb.clear_events();
        }
    }
    CHECK(same);
    CHECK(gnum(b, "marke") > 0.0 && gnum(a, "marke") == gnum(b, "marke") && gnum(b, "movie_done") > 0.0 &&
          gnum(a, "movie_done") == gnum(b, "movie_done"));
    CHECK(a.errors().empty() && b.errors().empty() && b.thread_count() == 0);
    std::printf("save: presentation log (%zu bytes) + VM image round trip mid-cinematic; 160 frames identical after load\n",
                wl.bytes().size());

    // Rejection: every truncation and the corruptions below fail and leave the log unchanged.
    const uint64_t before = lb.digest();
    const std::vector<uint8_t>& img = wl.bytes();
    const auto rejects = [&lb, before](std::span<const uint8_t> data) {
        ByteReader r(data);
        return !lb.read(r) && !r.ok() && lb.digest() == before;
    };
    bool trunc = true;
    for (size_t cut = 0; cut < img.size(); ++cut) trunc = trunc && rejects(std::span<const uint8_t>(img.data(), cut));
    CHECK(trunc);
    uint32_t nblocks = 0;
    std::memcpy(&nblocks, img.data() + 12, 4);
    CHECK(nblocks >= 1);
    std::vector<uint8_t> bad = img;
    bad[0] = 9;  // version
    CHECK(rejects(bad));
    bad = img;
    std::memset(bad.data() + 8, 0, 4);  // next block id 0
    CHECK(rejects(bad));
    bad = img;
    std::memcpy(bad.data() + 16, img.data() + 8, 4);  // block id == next block id (never handed out)
    CHECK(rejects(bad));
    bad = img;
    bad[16 + 8 * static_cast<size_t>(nblocks)] = 2;  // camera mode 2
    CHECK(rejects(bad));
    bad = img;
    bad[16 + 8 * static_cast<size_t>(nblocks) + 1] = 7;  // cinematic-mode switch: not a bool
    CHECK(rejects(bad));
    bad = img;
    bad[12] = 0xFF;  // block count past the data
    bad[14] = 0xFF;
    CHECK(rejects(bad));
    attach_presentation(a, nullptr);
    attach_presentation(b, nullptr);
}

int main() {
    test_registry();
    test_argument_contracts();
    test_object_effects();
    test_cinematic_sequence();
    test_private_log_and_digest();
    test_blocking();
    test_sim_crc_unchanged();
    test_save_load();
    if (g_fail) {
        std::printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("alamo_lua_presentation_tests: all passed\n");
    return 0;
}
