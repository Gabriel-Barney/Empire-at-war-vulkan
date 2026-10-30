// Presentation side of the Lua bridge (roadmap L1 stage 2, family PRESENTATION): camera, cinematics,
// fades, letterbox, fog, audio and UI calls made by scripts.
//
// The simulation never depends on presentation. Script calls of this family do not touch sim state;
// they append a PresentationEvent (sim frame + arguments) to a PresentationLog and update a small
// PresentationState model. Later steps consume both: R1 (sim -> render snapshot), P1 (input and
// camera), P3 (audio). Nothing here is fed to a sim CrcRegistry (rule 3: presentation is not sim state).
//
// Determinism: events are stamped with the calling VM's sim frame (LuaVm::set_frame) and a
// per-log sequence number; arguments are recorded by value (numbers as Lua doubles, objects by id),
// so two runs of the same scripts produce identical logs on every machine (digest()). Durations
// given to scripts in seconds are converted to whole sim frames at the fixed 30 Hz step
// (docs/re/MAIN_LOOP.md: "The simulation step is always 1/30 s"): frames = ceil(seconds * 30).
//
// Blocking: in the original only two calls of this family return a block status that scripts wait on
// with PGBase's BlockOnCommand: Play_Bink_Movie (BinkMovieBlockStatus) and Play_Lightning_Effect
// (LightningEffectBlockStatus) (docs/re/LUA_API.md, BlockingStatus.IsFinished overrides; shipped
// scripts: BlockOnCommand(Play_Bink_Movie(...)), BlockOnCommand(Play_Lightning_Effect(...))). Fades,
// letterbox and camera transitions return nothing; scripts wait for them with Sleep(t). Here a block
// finishes at a fixed sim frame (start + duration frames), or earlier when the host / a script stops
// it (Stop_Bink_Movie) - never on render timing, so a script resumes on the same frame everywhere.
//
// Ownership: a LuaVm records into the log attached with attach_presentation(); without one it
// creates a private log on first use (owned by the VM's Lua state). Owner-thread rule as LuaVm.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

struct lua_State;

namespace alamo {
class ByteWriter;
class ByteReader;
}  // namespace alamo

namespace alamo::lua {

class LuaVm;

inline constexpr uint32_t kSimFramesPerSecond = 30;
// Script seconds -> whole sim frames: ceil(s * 30); <= 0 and NaN -> 0, saturates at 2^31.
// [inferred] the rounding direction (the original runs these effects on render time).
uint32_t seconds_to_frames(double seconds);

// One per implemented binding (Lua name: op_name()).
enum class PresentationOp : uint16_t {
    // camera / cinematics
    StartCinematicCamera,
    EndCinematicCamera,
    StartCinematicMode,
    EndCinematicMode,
    SetCinematicCameraKey,
    SetCinematicTargetKey,
    TransitionCinematicCameraKey,
    TransitionCinematicTargetKey,
    TransitionToTacticalCamera,
    PointCameraAt,
    ScrollCameraTo,
    CameraToFollow,
    RotateCameraBy,
    RotateCameraTo,
    ZoomCamera,
    CinematicZoom,
    SetCinematicEnvironment,
    SetNewEnvironment,
    PromoteToSpaceCinematicLayer,
    DoEndCinematicCleanup,
    // screen
    FadeScreenIn,
    FadeScreenOut,
    FadeOn,
    FadeOff,
    LetterBoxIn,
    LetterBoxOut,
    LetterBoxOn,
    LetterBoxOff,
    LockControls,
    CancelFastForward,
    EnableFog,
    EnableDistanceFog,
    // audio
    AllowLocalizedSfx,
    PlayMusic,
    StopAllMusic,
    ResumeModeBasedMusic,
    StopAllSpeech,
    MasterVolumeRestore,
    WeatherAudioPause,
    SfxAllowAmbientVo,
    SfxAllowEnemySightedVo,
    SfxAllowHudVo,
    SfxAllowLocalizedSfxEvents,
    SfxAllowUnitResponseVo,
    ObjectPlaySfxEvent,
    ObjectStopSfxEvent,
    // movies / effects (blocking)
    PlayBinkMovie,
    StopBinkMovie,
    PlayLightningEffect,
    // UI
    GameMessage,
    RemoveAllText,
    ActivateRetryDialog,
    GuiButtonIcon,
    GuiComponentBlink,
    GuiComponentColor,
    GuiComponentEnable,
    GuiComponentFlash,
    GuiComponentPlayAnim,
    GuiComponentStopAnim,
    GuiComponentStopFlash,
    GuiComponentText,
    GuiComponentVisibility,
    GuiTextColor,
    // object effects (L1 stage 3; R4 particles / R1 visibility consume them)
    HideSubObject,                  // Hide_Sub_Object(object, hide, sub-object name)
    ObjectAttachParticleEffect,     // GameObjectWrapper.Attach_Particle_Effect(type [, bone])
    ObjectCinematicHyperspaceIn,    // GameObjectWrapper.Cinematic_Hyperspace_In(delay frames)
    ObjectInEndCinematic,           // GameObjectWrapper.In_End_Cinematic(bool)
    ResumeHyperspaceIn,             // Resume_Hyperspace_In()
    ObjectShowEmitter,              // GameObjectWrapper.Show_Emitter(name, bool)
    ObjectPlayCinematicEngineFlyby, // GameObjectWrapper.Play_Cinematic_Engine_Flyby(...)
    Count
};
std::string_view op_name(PresentationOp op);  // the Lua name ("Fade_Screen_In", "GameObjectWrapper.Play_SFX_Event")

// A script argument by value.
struct PresentationArg {
    enum class Kind : uint8_t { Nil, Bool, Number, String, Object, Player, Type, Other };
    Kind kind = Kind::Nil;
    bool b = false;
    double number = 0.0;
    int32_t id = 0;    // Object: object id; Player: player id; Type: type index
    std::string text;  // String: the string; Other: Lua type name or engine wrapper class
    friend bool operator==(const PresentationArg&, const PresentationArg&) = default;
};

struct PresentationEvent {
    uint32_t frame = 0;  // sim frame of the call
    uint32_t seq = 0;    // order within the log
    PresentationOp op = PresentationOp::Count;
    std::vector<PresentationArg> args;  // as passed by the script (receiver first for methods)
    uint32_t block = 0;                 // block id for blocking calls, else 0
    friend bool operator==(const PresentationEvent&, const PresentationEvent&) = default;
};

// Linear ramp of a level between two sim frames (fade, letterbox, camera transitions).
struct Ramp {
    double from = 0.0, to = 0.0;
    uint32_t start = 0, end = 0;  // level == to from `end` on
    double at(uint32_t frame) const;
    bool done(uint32_t frame) const { return frame >= end; }
    void set(double level, uint32_t frame) {
        from = level;
        to = level;
        start = frame;
        end = frame;
    }
    void go(double target, uint32_t frame, uint32_t frames);  // from the current level at `frame`
};

enum class CameraMode : uint8_t { Tactical, Cinematic };

struct CameraKey {
    bool set = false;
    std::vector<PresentationArg> args;  // position/object, x/dist, y/pitch, z/yaw, euler, attach, rot, anim
    uint32_t start = 0, end = 0;        // transition frames (start == end: a cut)
};

struct PresentationState {
    // camera
    CameraMode camera = CameraMode::Tactical;
    bool cinematic_mode = false;
    bool cinematic_letterbox_arg = true;  // Start_Cinematic_Camera's optional bool [inferred meaning]
    CameraKey camera_key, target_key;
    Ramp tactical_blend;              // Transition_To_Tactical_Camera: 0 -> 1 over its time
    PresentationArg camera_focus;     // last Point_Camera_At / Scroll_Camera_To / Camera_To_Follow target
    bool following = false;           // Camera_To_Follow
    bool cinematic_environment = false;
    // screen
    Ramp fade;       // 0 = clear, 1 = black
    Ramp letterbox;  // 0 = off, 1 = bars fully in
    bool controls_locked = false;
    bool fast_forward = false;  // host input (set_fast_forward); Cancel_Fast_Forward clears it
    bool fog_enabled = true;          // [inferred] default on
    bool distance_fog_enabled = true; // [inferred] default on
    // audio
    bool localized_sfx = true;
    bool weather_audio_paused = false;
    bool ambient_vo = true, enemy_sighted_vo = true, hud_vo = true, localized_sfx_events = true,
         unit_response_vo = true;
    std::string music;               // Play_Music event; "" = none / stopped
    bool mode_based_music = true;    // Play_Music suspends it, Resume_Mode_Based_Music restores [inferred]
    // movies / UI
    std::string movie;               // playing Bink movie, "" when none
    uint32_t movie_block = 0;
    bool retry_dialog = false;
    // object effects: "objectid|SUBOBJECT" -> hidden (Hide_Sub_Object), object id -> In_End_Cinematic flag,
    // "objectid|EMITTER" -> shown (Show_Emitter). Particles / hyperspace-ins are events only.
    std::map<std::string, bool> hidden_sub_objects;
    std::map<int32_t, bool> in_end_cinematic;
    std::map<std::string, bool> emitters;

    // The original's "cinematic rate lock" (FS +0x37, docs/re/MAIN_LOOP.md §1.5): set while a
    // cinematic camera or cinematic mode is active; it pins pacing to 30 Hz and selects the parity
    // CRC inversion (COMMANDS.md §7.1 step 6). The HOST reads it and passes it to
    // ParityFrameOptions::invert; it is not hashed as presentation state.
    bool cinematic_rate_lock() const { return camera == CameraMode::Cinematic || cinematic_mode; }
};

struct PresentationConfig {
    // Bink movies have no length in game data: a movie block finishes this many frames after the
    // call unless stopped earlier. [inferred] Headless default: 1 frame.
    uint32_t movie_frames = 1;
    // Lightning effect block length by effect name; empty -> lightning_frames. [inferred]
    std::function<uint32_t(std::string_view effect)> lightning_frames_of;
    uint32_t lightning_frames = 30;
};

// OI lua (L1 -> R4 hand-off): GameObjectWrapper.Attach_Particle_Effect(type [, bone]) resolved for a host. Recorded
// next to its ObjectAttachParticleEffect event (same frame / seq); the render host maps it to a particle system on
// the object (the type's particle model) at the bone (ProxyAttachments::sync_effect). Derived from the event, so not
// hashed separately by digest().
struct ParticleEffectRequest {
    uint32_t frame = 0;      // sim frame of the call
    uint32_t seq = 0;        // seq of the ObjectAttachParticleEffect event
    int32_t object = 0;      // object id of the receiver
    int32_t type = -1;       // type index; -1 = the name is not in the VM's type database (or no database)
    std::string type_name;   // the type's name (as written by the script when unresolved)
    std::string bone;        // "" = no bone given (the object's root)
    friend bool operator==(const ParticleEffectRequest&, const ParticleEffectRequest&) = default;
};

class PresentationLog {
public:
    PresentationConfig& config() { return config_; }
    const std::vector<PresentationEvent>& events() const { return events_; }
    // OI lua: Attach_Particle_Effect requests since the last clear_events(), in call order.
    const std::vector<ParticleEffectRequest>& particle_effects() const { return particle_effects_; }
    ParticleEffectRequest& record_particle_effect(ParticleEffectRequest r) { return particle_effects_.emplace_back(std::move(r)); }
    const PresentationState& state() const { return state_; }
    PresentationState& mutable_state() { return state_; }

    double fade_level(uint32_t frame) const { return state_.fade.at(frame); }
    double letterbox_level(uint32_t frame) const { return state_.letterbox.at(frame); }

    // Consumers drain events each frame; state and block bookkeeping are kept.
    void clear_events() {
        events_.clear();
        particle_effects_.clear();  // OI lua
    }
    void set_fast_forward(bool on) { state_.fast_forward = on; }

    // Appends an event (bindings).
    PresentationEvent& record(uint32_t frame, PresentationOp op);

    // Blocks: a new id that finishes at end_frame (or when finish_block() is called).
    uint32_t open_block(uint32_t end_frame);
    void finish_block(uint32_t id, uint32_t frame);
    bool block_finished(uint32_t id, uint32_t frame) const;  // unknown ids are finished

    // Digest of events + state (determinism tests, replays). Not part of any sim CRC.
    uint64_t digest() const;

    // N1 save / load (contract: alamo/core/save_file.hpp). Layout: u32 version (1); u32 next seq, u32 next block id;
    // u32 count + the open / finished blocks in ascending id (u32 id, u32 end frame); the PresentationState (u8 camera,
    // bool cinematic mode, bool letterbox arg, camera key, target key (bool set, u32 count + args, u32 start, u32 end),
    // tactical blend ramp (f64 from, f64 to, u32 start, u32 end), camera focus arg, bool following, bool cinematic
    // environment, fade ramp, letterbox ramp, the 13 switches in digest() order, str music, str movie, u32 movie block,
    // u32 count + hidden sub-objects (str, bool), u32 count + emitters (str, bool), u32 count + in-end-cinematic (i32,
    // bool)); then the undrained events (u32 frame, u32 seq, u16 op, u32 block, u32 count + args) and particle
    // requests (u32 frame, u32 seq, i32 object, i32 type, str type name, str bone). An arg = u8 kind, bool, f64 number
    // (bit pattern), i32 id, str text. Not saved: config() (the host's). Block ids are kept, so the block status
    // tables in a restored VM image stay valid. read(): false and unchanged on bad data; reads its own fields only
    // (the owner checks done()). The log of a VM without attach_presentation lives inside the VM (a raw userdata) and
    // cannot be saved: LuaVm::write refuses such a VM; hosts that save VMs attach a log and save it with this.
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);

private:
    struct Block {
        uint32_t end = 0;
    };
    PresentationConfig config_;
    std::vector<PresentationEvent> events_;
    std::vector<ParticleEffectRequest> particle_effects_;  // OI lua
    PresentationState state_;
    std::map<uint32_t, Block> blocks_;
    uint32_t next_seq_ = 0;
    uint32_t next_block_ = 1;
};

// Routes a VM's presentation calls to `log` (must outlive the VM or be detached with nullptr).
void attach_presentation(LuaVm& vm, PresentationLog* log);
// The VM's log (the attached one, else its private one, created on first use).
PresentationLog& presentation(LuaVm& vm);
namespace detail {
PresentationLog& presentation_of(lua_State* L);
}

}  // namespace alamo::lua
