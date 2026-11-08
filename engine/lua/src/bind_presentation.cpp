// Binding family: camera, cinematics, fades, fog, audio and UI (L1 stage 2).
//
// Every binding here records a PresentationEvent (sim frame + arguments) into the VM's
// PresentationLog and updates PresentationState (alamo/lua/presentation.hpp). None of them reads or
// writes sim state, draws random numbers or changes anything a sim CRC hashes.
// Argument contracts and messages: docs/re/LUA_API.md "Binding map" (the original's own strings).
// Behaviour the docs do not give is marked [inferred].
// Exception (story t12): Resume_Hyperspace_In also clears the host's reinforcement hold through
// TacticalScriptHooks::resume_hyperspace_in (script_gaps.hpp) when a tactical host installed it; likewise (cu8g t03)
// Set_New_Environment changes the mode's environment through TacticalScriptHooks::set_new_environment.
#include <string>

#include "alamo/lua/object_bridge.hpp"
#include "alamo/core/ascii.hpp"
#include "alamo/lua/presentation.hpp"
#include "alamo/lua/script_gaps.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "vm_internal.hpp"

namespace alamo::lua {

using detail::Call;
using Arg = PresentationArg;

namespace {

PresentationLog& log_of(Call& c) { return detail::presentation_of(c.L); }
uint32_t frame_of(const Call& c) { return c.vm->frame; }

bool is_bool(const Call& c, int i) { return c.has(i) && lua_type(c.L, c.idx(i)) == LUA_TBOOLEAN; }
bool boolean(const Call& c, int i) { return lua_toboolean(c.L, c.idx(i)) != 0; }

const detail::Ud* engine_ud(const Call& c, int i) {
    if (!c.has(i) || lua_type(c.L, c.idx(i)) != LUA_TUSERDATA) return nullptr;
    return static_cast<const detail::Ud*>(lua_touserdata(c.L, c.idx(i)));
}

// "Position of some sort": a game object (markers, units), a PositionWrapper or an AI target.
// [inferred] dead objects are accepted (their last id is recorded).
bool is_position(const Call& c, int i) {
    const detail::Ud* ud = engine_ud(c, i);
    return ud && (ud->cls == detail::cls_game_object() || ud->cls == detail::cls_position() || ud->cls == detail::cls_target());
}

bool is_object(const Call& c, int i) {
    const detail::Ud* ud = engine_ud(c, i);
    return ud && ud->cls == detail::cls_game_object();
}

Arg to_arg(const Call& c, int i) {
    Arg a;
    if (!c.has(i)) return a;
    switch (lua_type(c.L, c.idx(i))) {
        case LUA_TNIL: break;
        case LUA_TBOOLEAN:
            a.kind = Arg::Kind::Bool;
            a.b = boolean(c, i);
            break;
        case LUA_TNUMBER:
            a.kind = Arg::Kind::Number;
            a.number = c.number(i);
            break;
        case LUA_TSTRING:
            a.kind = Arg::Kind::String;
            a.text = std::string(c.string(i));
            break;
        case LUA_TUSERDATA: {
            const detail::Ud* ud = engine_ud(c, i);
            if (ud->cls == detail::cls_game_object()) {
                a.kind = Arg::Kind::Object;
                a.id = ud->id;
            } else if (ud->cls == detail::cls_player()) {
                a.kind = Arg::Kind::Player;
                a.id = ud->id;
            } else if (ud->cls == detail::cls_type()) {
                a.kind = Arg::Kind::Type;
                a.id = static_cast<int32_t>(ud->value);
            } else {
                a.kind = Arg::Kind::Other;
                a.text = ud->cls < detail::class_names().size() ? detail::class_names()[ud->cls] : "userdata";
                a.id = ud->id;
            }
            break;
        }
        default:
            a.kind = Arg::Kind::Other;
            a.text = lua_typename(c.L, lua_type(c.L, c.idx(i)));
            break;
    }
    return a;
}

Arg self_arg(const Call& c) {
    Arg a;
    a.kind = Arg::Kind::Object;
    a.id = c.self->id;
    return a;
}

// Records the call with all its arguments (receiver first for GameObjectWrapper methods).
PresentationEvent& record(Call& c, PresentationOp op, bool with_self = false) {
    PresentationEvent& e = log_of(c).record(frame_of(c), op);
    if (with_self) e.args.push_back(self_arg(c));
    for (int i = 1; i <= c.nargs; ++i) e.args.push_back(to_arg(c, i));
    return e;
}

PresentationState& state(Call& c) { return log_of(c).mutable_state(); }

// ---------------------------------------------------------------- block statuses
//
// The original returns a BinkMovieBlockStatus / LightningEffectBlockStatus userdata; PGBase's
// BlockOnCommand only calls block.IsFinished() and block.Result() on it. Here the status is a Lua
// table with those two functions (self-contained in this family: the shared BlockingStatus userdata
// class is not owned here). IsFinished compares the VM's sim frame with the block's end frame.
// Result: nil (both originals share the trivial 0x17d070 Result). [inferred] the table form.

int block_is_finished(lua_State* L) {
    const auto* vm = static_cast<const LuaVm::Impl*>(lua_touserdata(L, lua_upvalueindex(1)));
    const auto id = static_cast<uint32_t>(lua_tonumber(L, lua_upvalueindex(2)));
    lua_pushboolean(L, detail::presentation_of(L).block_finished(id, vm->frame) ? 1 : 0);
    return 1;
}

int block_result(lua_State* L) {
    lua_pushnil(L);
    return 1;
}

int push_block(Call& c, uint32_t id) {
    lua_newtable(c.L);
    lua_pushstring(c.L, "IsFinished");
    lua_pushlightuserdata(c.L, c.vm);
    lua_pushnumber(c.L, static_cast<lua_Number>(id));
    lua_pushcclosure(c.L, block_is_finished, 2);
    lua_rawset(c.L, -3);
    lua_pushstring(c.L, "Result");
    lua_pushcfunction(c.L, block_result);
    lua_rawset(c.L, -3);
    return 1;
}

// ---------------------------------------------------------------- argument checks

// One required float: "<Cls> -- Expected float parameter 1" / "<Cls> -- Invalid parameter 1, expected a float".
bool need_float1(Call& c, const char* cls) {
    if (!c.has(1)) {
        c.script_error_stop("%s -- Expected float parameter 1", cls);
        return false;
    }
    if (!c.is_number(1)) {
        c.script_error_stop("%s -- Invalid parameter 1, expected a float", cls);
        return false;
    }
    return true;
}

bool need_float2(Call& c, const char* cls) {
    if (c.nargs < 2) {
        c.script_error_stop("%s -- Expected float parameter 1, float parameter 2", cls);
        return false;
    }
    for (int i = 1; i <= 2; ++i)
        if (!c.is_number(i)) {
            c.script_error_stop("%s -- Invalid parameter %d, expected a float", cls, i);
            return false;
        }
    return true;
}

// One required bool, the Lua* family form.
bool need_bool1(Call& c, const char* cls) {
    if (!c.has(1)) {
        c.script_error_stop("%s -- Expected bool parameter 1", cls);
        return false;
    }
    if (!is_bool(c, 1)) {
        c.script_error_stop("%s -- invalid type for parameter 1. Expected bool.", cls);
        return false;
    }
    return true;
}

// The *Class family form: exact count, then per-parameter types.
bool need_count(Call& c, const char* name, int n) {
    if (c.nargs != n) {
        c.script_error_stop("%s -- invalid number of parameters. Expected %d, got %d.", name, n, c.nargs);
        return false;
    }
    return true;
}

bool need_string(Call& c, const char* name, int i, const char* what = "string") {
    if (!c.is_string(i)) {
        c.script_error_stop("%s -- invalid type for parameter %d. Expected %s.", name, i, what);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- camera / cinematics

int g_start_cinematic_camera(Call& c) {
    if (c.has(1) && !lua_isnil(c.L, c.idx(1)) && !is_bool(c, 1))
        return c.script_error_stop("LuaStartCinematicCamera -- invalid type for parameter 1. Expected bool.");
    record(c, PresentationOp::StartCinematicCamera);
    PresentationState& s = state(c);
    s.camera = CameraMode::Cinematic;
    s.cinematic_letterbox_arg = is_bool(c, 1) ? boolean(c, 1) : true;
    s.tactical_blend.set(0.0, frame_of(c));
    return 0;
}

int g_end_cinematic_camera(Call& c) {
    record(c, PresentationOp::EndCinematicCamera);
    PresentationState& s = state(c);
    s.camera = CameraMode::Tactical;
    s.camera_key = {};
    s.target_key = {};
    s.tactical_blend.set(0.0, frame_of(c));
    return 0;
}

int g_start_cinematic_mode(Call& c) {
    record(c, PresentationOp::StartCinematicMode);
    state(c).cinematic_mode = true;
    return 0;
}

int g_end_cinematic_mode(Call& c) {
    record(c, PresentationOp::EndCinematicMode);
    state(c).cinematic_mode = false;
    return 0;
}

// Set_Cinematic_*_Key(position, x|dist, y|pitch, z|yaw, euler, attach_object|0, use_object_rotation,
// animation): a cut. Transition_Cinematic_*_Key: same with the time (seconds) as parameter 2.
// q6 t11 (LUA_API.md): Set_Cinematic_Target_Key takes 8 or more arguments (FoC Underworld Act I M03 passes 9; the
// message says 8); an invalid position is a warning and the binding returns (the complete warning list).
int set_key(Call& c, const char* cls, PresentationOp op, bool camera, bool transition) {
    const int expected = transition ? 9 : 8;
    const bool at_least = !camera && !transition;
    if (at_least ? c.nargs < expected : c.nargs != expected) return c.script_error_stop("%s -- Expected %d parameters", cls, expected);
    if (!is_position(c, 1)) return c.script_warning("%s -- Invalid parameter 1, expected a position.", cls);
    if (!c.is_number(2)) return c.script_error_stop("%s -- Invalid parameter 2, expected a float", cls);
    record(c, op);
    PresentationState& s = state(c);
    CameraKey& k = camera ? s.camera_key : s.target_key;
    k.set = true;
    k.args.clear();
    for (int i = 1; i <= c.nargs; ++i)
        if (!(transition && i == 2)) k.args.push_back(to_arg(c, i));
    k.start = frame_of(c);
    k.end = k.start + (transition ? seconds_to_frames(c.number(2)) : 0u);
    return 0;
}

int g_set_camera_key(Call& c) {
    return set_key(c, "LuaSetCinematicCameraKey", PresentationOp::SetCinematicCameraKey, true, false);
}
int g_set_target_key(Call& c) {
    return set_key(c, "LuaSetCinematicTargetKey", PresentationOp::SetCinematicTargetKey, false, false);
}
int g_transition_camera_key(Call& c) {
    return set_key(c, "LuaTransitionCinematicCameraKey", PresentationOp::TransitionCinematicCameraKey, true, true);
}
int g_transition_target_key(Call& c) {
    return set_key(c, "LuaTransitionCinematicTargetKey", PresentationOp::TransitionCinematicTargetKey, false, true);
}

// [inferred] blends the cinematic camera back to the tactical one over the time; the camera stays
// in cinematic mode until End_Cinematic_Camera (shipped scripts: Transition_To_Tactical_Camera(t)
// Sleep(t) ... End_Cinematic_Camera()).
int g_transition_to_tactical(Call& c) {
    if (!need_float1(c, "LuaTransitionToTacticalCamera")) return 0;
    record(c, PresentationOp::TransitionToTacticalCamera);
    Ramp& r = state(c).tactical_blend;
    r.set(0.0, frame_of(c));
    r.go(1.0, frame_of(c), seconds_to_frames(c.number(1)));
    return 0;
}

int focus(Call& c, const char* msg, PresentationOp op) {
    if (!is_position(c, 1)) return c.script_warning("%s", msg);  // q6 t11: a warning (LUA_API.md, the warning list)
    record(c, op);
    PresentationState& s = state(c);
    s.camera_focus = to_arg(c, 1);
    s.following = false;
    return 0;
}

int g_point_camera_at(Call& c) {
    return focus(c, "LuaPointCamera -- Expected position parameter 1", PresentationOp::PointCameraAt);
}
int g_scroll_camera_to(Call& c) {
    return focus(c, "LuaScrollCameraTo -- Expected position parameter 1", PresentationOp::ScrollCameraTo);
}

// No contract strings: [inferred] Camera_To_Follow(object) follows it, no / nil argument stops.
int g_camera_to_follow(Call& c) {
    record(c, PresentationOp::CameraToFollow);
    PresentationState& s = state(c);
    s.following = is_object(c, 1);
    if (s.following) s.camera_focus = to_arg(c, 1);
    return 0;
}

int g_rotate_camera_by(Call& c) {
    if (!need_float2(c, "LuaRotateCameraBy")) return 0;
    record(c, PresentationOp::RotateCameraBy);
    return 0;
}
int g_rotate_camera_to(Call& c) {
    if (!need_float2(c, "LuaRotateCameraTo")) return 0;
    record(c, PresentationOp::RotateCameraTo);
    return 0;
}
int g_zoom_camera(Call& c) {
    if (!need_float2(c, "LuaZoomCamera")) return 0;
    record(c, PresentationOp::ZoomCamera);
    return 0;
}

int g_cinematic_zoom(Call& c) {
    if (c.nargs < 2) return c.script_error_stop("LuaCinematicZoom -- Expected float param 1 time, float param 2 delta");
    if (!c.is_number(1)) return c.script_error_stop("LuaCinematicZoom -- Invalid parameter 1, expected a float");
    if (!c.is_number(2)) return c.script_error_stop("LuaCinematicZoom -- Invalid parameter 2, expected a float");
    record(c, PresentationOp::CinematicZoom);
    return 0;
}

int g_set_cinematic_environment(Call& c) {
    if (!need_bool1(c, "LuaSetCinematicEnvironment")) return 0;
    record(c, PresentationOp::SetCinematicEnvironment);
    state(c).cinematic_environment = boolean(c, 1);
    return 0;
}

// Set_New_Environment(int) (0x5448a0; LUA_API.md contract strings; no shipped call sites): recorded, then (cu8g t03)
// the tactical host's TacticalScriptHooks::set_new_environment with the int argument (the mode clamps it,
// CU8F_T03_T05_RE §2.2). [port] the number is truncated toward zero like every int argument (detail::to_int).
int g_set_new_environment(Call& c) {
    if (!c.has(1)) return c.script_error_stop("LuaSetNewEnvironment -- Expected int parameter 1");
    if (!c.is_number(1)) return c.script_error_stop("LuaSetNewEnvironment -- invalid type for parameter 1. Expected int.");
    record(c, PresentationOp::SetNewEnvironment);
    if (ObjectBridge* b = c.vm->owner ? attached_object_bridge(*c.vm->owner) : nullptr; b && b->tactical && b->tactical->set_new_environment)
        b->tactical->set_new_environment(detail::to_int(c.number(1)));
    return 0;
}

int g_promote_to_space_cinematic_layer(Call& c) {
    if (!c.has(1)) return c.script_error_stop("LuaPromoteToSpaceCinematicLayer -- Expected gameobject parameter 1");
    if (!is_object(c, 1)) return c.script_error_stop("LuaPromoteToSpaceCinematicLayer -- Invalid parameter 1, expected a game object");
    record(c, PresentationOp::PromoteToSpaceCinematicLayer);
    return 0;
}

// [inferred] recorded only: what the original resets is not documented.
int g_do_end_cinematic_cleanup(Call& c) {
    record(c, PresentationOp::DoEndCinematicCleanup);
    return 0;
}

// ---------------------------------------------------------------- screen

// Fades / letterbox ramp from the current level at the call frame. [inferred] linear.
int ramp_to(Call& c, const char* cls, PresentationOp op, Ramp PresentationState::*ramp, double target) {
    if (!need_float1(c, cls)) return 0;
    record(c, op);
    (state(c).*ramp).go(target, frame_of(c), seconds_to_frames(c.number(1)));
    return 0;
}

int g_fade_screen_in(Call& c) {
    return ramp_to(c, "LuaFadeScreenIn", PresentationOp::FadeScreenIn, &PresentationState::fade, 0.0);
}
int g_fade_screen_out(Call& c) {
    return ramp_to(c, "LuaFadeScreenOut", PresentationOp::FadeScreenOut, &PresentationState::fade, 1.0);
}
int g_letter_box_in(Call& c) {
    return ramp_to(c, "LuaLetterBoxIn", PresentationOp::LetterBoxIn, &PresentationState::letterbox, 1.0);
}
int g_letter_box_out(Call& c) {
    return ramp_to(c, "LuaLetterBoxOut", PresentationOp::LetterBoxOut, &PresentationState::letterbox, 0.0);
}

// [inferred] the On/Off forms are instant (no parameters in the original).
int g_fade_on(Call& c) {
    record(c, PresentationOp::FadeOn);
    state(c).fade.set(1.0, frame_of(c));
    return 0;
}
int g_fade_off(Call& c) {
    record(c, PresentationOp::FadeOff);
    state(c).fade.set(0.0, frame_of(c));
    return 0;
}
int g_letter_box_on(Call& c) {
    record(c, PresentationOp::LetterBoxOn);
    state(c).letterbox.set(1.0, frame_of(c));
    return 0;
}
int g_letter_box_off(Call& c) {
    record(c, PresentationOp::LetterBoxOff);
    state(c).letterbox.set(0.0, frame_of(c));
    return 0;
}

// Lock_Controls(1) / Lock_Controls(0). [inferred] any non-zero value locks.
int g_lock_controls(Call& c) {
    if (!need_float1(c, "LuaLockControls")) return 0;
    record(c, PresentationOp::LockControls);
    state(c).controls_locked = c.number(1) != 0.0;
    return 0;
}

int g_cancel_fast_forward(Call& c) {
    record(c, PresentationOp::CancelFastForward);
    state(c).fast_forward = false;
    return 0;
}

int g_enable_fog(Call& c) {
    if (!need_bool1(c, "LuaEnableFog")) return 0;
    record(c, PresentationOp::EnableFog);
    state(c).fog_enabled = boolean(c, 1);
    return 0;
}

int g_enable_distance_fog(Call& c) {
    if (!need_count(c, "Enable_Distance_Fog", 1)) return 0;
    if (!is_bool(c, 1)) return c.script_error_stop("Enable_Distance_Fog -- invalid type for parameter 1. Expected boolean.");
    record(c, PresentationOp::EnableDistanceFog);
    state(c).distance_fog_enabled = boolean(c, 1);
    return 0;
}

// ---------------------------------------------------------------- audio

int g_allow_localized_sfx(Call& c) {
    if (!need_bool1(c, "LuaAllowLocalizedSFX")) return 0;
    record(c, PresentationOp::AllowLocalizedSfx);
    state(c).localized_sfx = boolean(c, 1);
    return 0;
}

int g_weather_audio_pause(Call& c) {
    if (!need_bool1(c, "LuaWeatherAudioPause")) return 0;
    record(c, PresentationOp::WeatherAudioPause);
    state(c).weather_audio_paused = boolean(c, 1);
    return 0;
}

// [inferred] Play_Music overrides the mode-based music until Resume_Mode_Based_Music.
int g_play_music(Call& c) {
    if (!need_count(c, "Play_Music", 1) || !need_string(c, "Play_Music", 1)) return 0;
    record(c, PresentationOp::PlayMusic);
    PresentationState& s = state(c);
    s.music = std::string(c.string(1));
    s.mode_based_music = false;
    return 0;
}

int g_stop_all_music(Call& c) {
    record(c, PresentationOp::StopAllMusic);
    state(c).music.clear();
    return 0;
}

int g_resume_mode_based_music(Call& c) {
    record(c, PresentationOp::ResumeModeBasedMusic);
    PresentationState& s = state(c);
    s.music.clear();
    s.mode_based_music = true;
    return 0;
}

int g_stop_all_speech(Call& c) {
    record(c, PresentationOp::StopAllSpeech);
    return 0;
}

int g_master_volume_restore(Call& c) {
    record(c, PresentationOp::MasterVolumeRestore);
    return 0;
}

// SFXManager.Allow_*(bool): no contract strings in the docs. [inferred] message form.
int sfx_flag(Call& c, const char* name, PresentationOp op, bool PresentationState::*flag) {
    if (!is_bool(c, 1)) return c.script_error_stop("LuaSFXCommandsClassClass::%s -- Expected bool parameter 1", name);
    record(c, op);
    state(c).*flag = boolean(c, 1);
    return 0;
}
int m_allow_ambient_vo(Call& c) {
    return sfx_flag(c, "Allow_Ambient_VO", PresentationOp::SfxAllowAmbientVo, &PresentationState::ambient_vo);
}
int m_allow_enemy_sighted_vo(Call& c) {
    return sfx_flag(c, "Allow_Enemy_Sighted_VO", PresentationOp::SfxAllowEnemySightedVo,
                    &PresentationState::enemy_sighted_vo);
}
int m_allow_hud_vo(Call& c) {
    return sfx_flag(c, "Allow_HUD_VO", PresentationOp::SfxAllowHudVo, &PresentationState::hud_vo);
}
int m_allow_localized_sfx_events(Call& c) {
    return sfx_flag(c, "Allow_Localized_SFXEvents", PresentationOp::SfxAllowLocalizedSfxEvents,
                    &PresentationState::localized_sfx_events);
}
int m_allow_unit_response_vo(Call& c) {
    return sfx_flag(c, "Allow_Unit_Reponse_VO", PresentationOp::SfxAllowUnitResponseVo,
                    &PresentationState::unit_response_vo);
}

int object_sfx(Call& c, const char* method, PresentationOp op) {
    if (c.nargs < 1 || c.nargs > 2)
        return c.script_error_stop("GameObjectWrapper::%s -- invalid number of parameters. Expected 1 or 2, got %d.", method, c.nargs);
    if (!c.is_string(1)) return c.script_error_stop("GameObjectWrapper::%s -- invalid type for parameter 1. Expected string.", method);
    if (c.nargs == 2 && !c.is_number(2))
        return c.script_error_stop("GameObjectWrapper::%s -- invalid type for parameter 2. Expected number.", method);
    record(c, op, true);
    return 0;
}
int m_play_sfx_event(Call& c) { return object_sfx(c, "Play_SFX_Event", PresentationOp::ObjectPlaySfxEvent); }
int m_stop_sfx_event(Call& c) { return object_sfx(c, "Stop_SFX_Event", PresentationOp::ObjectStopSfxEvent); }

// ---------------------------------------------------------------- object effects (L1 stage 3)
//
// Visual only: recorded for R1 (visibility of sub-objects / emitters) and R4 (particles); none of them
// touches sim state. [inferred] Hide_Sub_Object's float: non-zero hides.

int g_hide_sub_object(Call& c) {
    if (c.nargs != 3)
        return c.script_error_stop("LuaHideSubObject -- Expected gameobject pointer parameter 1, float parameter 2, string parameter 3");
    if (!is_object(c, 1)) return c.script_error_stop("LuaHideSubObject -- Invalid parameter 1, expected a game object");
    if (!c.is_number(2)) return c.script_error_stop("LuaHideSubObject -- Invalid parameter 2, expected a float");
    if (!c.is_string(3)) return c.script_error_stop("LuaHideSubObject -- invalid type for parameter 3. Expected string.");
    record(c, PresentationOp::HideSubObject);
    const int32_t id = engine_ud(c, 1)->id;
    state(c).hidden_sub_objects[std::to_string(id) + "|" + ascii_upper(c.string(3))] = c.number(2) != 0.0;
    return 0;
}

int m_attach_particle_effect(Call& c) {
    if (c.nargs < 1 || c.nargs > 2)
        return c.script_error_stop("GameObjectWrapper::Attach_Particle_Effect -- Invalid number of parameters. Expected 1 or 2, got %d.",
                      c.nargs);
    const detail::Ud* t = engine_ud(c, 1);
    if (!c.is_string(1) && !(t && t->cls == detail::cls_type()))
        return c.script_error_stop("GameObjectWrapper::Attach_Particle_Effect -- Invalid type for parameter 1. Expected type or type name.");
    if (c.nargs == 2 && !c.is_string(2))
        return c.script_error_stop("GameObjectWrapper::Attach_Particle_Effect -- Invalid type for parameter 2. Expected string.");
    const PresentationEvent& e = record(c, PresentationOp::ObjectAttachParticleEffect, true);
    // OI lua: the resolved request for the render host (presentation.hpp ParticleEffectRequest).
    ParticleEffectRequest r;
    r.frame = e.frame;
    r.seq = e.seq;
    r.object = c.self->id;
    const objects::GameObjectTypes* types = c.vm->host.types;
    if (t && t->cls == detail::cls_type()) {
        if (types && t->value < types->raw().type_count()) {
            r.type = static_cast<int32_t>(t->value);
            r.type_name = std::string(types->raw().types().name[t->value]);
        }
    } else {
        r.type_name = std::string(c.string(1));
        if (types) {
            const objects::TypeIndex ti = types->raw().find(r.type_name);
            if (ti != objects::kNone) {
                r.type = static_cast<int32_t>(ti);
                r.type_name = std::string(types->raw().types().name[ti]);
            }
        }
    }
    if (c.nargs == 2) r.bone = std::string(c.string(2));
    log_of(c).record_particle_effect(std::move(r));
    return 0;
}

int m_cinematic_hyperspace_in(Call& c) {
    if (c.nargs < 1)
        return c.script_error_stop("GameObjectWrapper::Cinematic_Hyperspace_In -- invalid number of parameters. Expected interger value for "
                      "delay frames");
    if (!c.is_number(1)) return c.script_error_stop("GameObjectWrapper::Cinematic_Hyperspace_In -- invalid type for parameter 1. Expected number.");
    record(c, PresentationOp::ObjectCinematicHyperspaceIn, true);
    return 0;
}

int m_in_end_cinematic(Call& c) {
    if (!is_bool(c, 1)) return c.script_error_stop("GameObjectWrapper::Lua_Is_In_End_Cinematic -- Missing required boolean parameter.");
    record(c, PresentationOp::ObjectInEndCinematic, true);
    state(c).in_end_cinematic[c.self->id] = boolean(c, 1);
    return 0;
}

int g_resume_hyperspace_in(Call& c) {
    record(c, PresentationOp::ResumeHyperspaceIn);
    if (ObjectBridge* b = c.vm->owner ? attached_object_bridge(*c.vm->owner) : nullptr; b && b->tactical && b->tactical->resume_hyperspace_in)
        b->tactical->resume_hyperspace_in();  // story t12
    return 0;
}

int m_show_emitter(Call& c) {
    if (c.nargs != 2)
        return c.script_error_stop("GameObjectWrapper::Show_Emitter -- invalid number of parameters. Expected 2, got %d.", c.nargs);
    if (!c.is_string(1)) return c.script_error_stop("GameObjectWrapper::Show_Emitter -- invalid type for parameter 1. Expected string.");
    if (!is_bool(c, 2)) return c.script_error_stop("GameObjectWrapper::Show_Emitter -- invalid type for parameter 2. Expected boolean.");
    record(c, PresentationOp::ObjectShowEmitter, true);
    state(c).emitters[std::to_string(c.self->id) + "|" + ascii_upper(c.string(1))] = boolean(c, 2);
    return 0;
}

int m_play_cinematic_engine_flyby(Call& c) {  // no contract string: arguments recorded as given
    record(c, PresentationOp::ObjectPlayCinematicEngineFlyby, true);
    return 0;
}

// ---------------------------------------------------------------- movies / effects (blocking)

int g_play_bink_movie(Call& c) {
    if (!c.has(1)) return c.script_error_stop("LuaPlayBinkMovie -- Expected type name for parameter 1");
    if (!c.is_string(1)) return c.script_error_stop("LuaPlayBinkMovie -- Expected type string for parameter 1");
    PresentationLog& log = log_of(c);
    PresentationEvent& e = record(c, PresentationOp::PlayBinkMovie);
    PresentationState& s = log.mutable_state();
    if (s.movie_block) log.finish_block(s.movie_block, frame_of(c));  // [inferred] one movie at a time
    e.block = log.open_block(frame_of(c) + log.config().movie_frames);
    s.movie = std::string(c.string(1));
    s.movie_block = e.block;
    return push_block(c, e.block);
}

int g_stop_bink_movie(Call& c) {
    PresentationLog& log = log_of(c);
    record(c, PresentationOp::StopBinkMovie);
    PresentationState& s = log.mutable_state();
    if (s.movie_block) log.finish_block(s.movie_block, frame_of(c));
    s.movie.clear();
    s.movie_block = 0;
    return 0;
}

int g_play_lightning_effect(Call& c) {
    const char* n = "Play_Lightning_Effect";
    if (!need_count(c, n, 3) || !need_string(c, n, 1)) return 0;
    for (int i = 2; i <= 3; ++i)
        if (!engine_ud(c, i)) return c.script_error_stop("%s -- invalid type for parameter %d. Expected position of some sort.", n, i);
    PresentationLog& log = log_of(c);
    PresentationEvent& e = record(c, PresentationOp::PlayLightningEffect);
    const PresentationConfig& cfg = log.config();
    const uint32_t frames = cfg.lightning_frames_of ? cfg.lightning_frames_of(c.string(1)) : cfg.lightning_frames;
    e.block = log.open_block(frame_of(c) + frames);
    return push_block(c, e.block);
}

// ---------------------------------------------------------------- UI

int g_game_message(Call& c) {
    if (!need_count(c, "Game_Message", 1) || !need_string(c, "Game_Message", 1)) return 0;
    record(c, PresentationOp::GameMessage);
    return 0;
}

int g_remove_all_text(Call& c) {
    record(c, PresentationOp::RemoveAllText);
    return 0;
}

// [inferred] shows the mission-failed retry dialog; no parameters.
int g_activate_retry_dialog(Call& c) {
    record(c, PresentationOp::ActivateRetryDialog);
    state(c).retry_dialog = true;
    return 0;
}

// Owned by other families: GameObjectWrapper.Highlight / Highlight_Small (objects: per-object script
// state), Add_Planet_Highlight / Remove_Planet_Highlight (story).

// GUI_*: (component name, ...). Count and the documented parameter types are checked; the rest is
// recorded for the UI step.
struct GuiSpec {
    const char* name;
    PresentationOp op;
    int count;
    int typed;         // 0 = no second typed parameter
    char type;         // 'b' bool, 'f' float, 's' string
    const char* what;  // expected-type text
};

int gui(Call& c, const GuiSpec& g) {
    if (!need_count(c, g.name, g.count) || !need_string(c, g.name, 1)) return 0;
    if (g.typed) {
        const bool ok = g.type == 'b' ? is_bool(c, g.typed) : g.type == 'f' ? c.is_number(g.typed) : c.is_string(g.typed);
        if (!ok) return c.script_error_stop("%s -- invalid type for parameter %d. Expected %s.", g.name, g.typed, g.what);
    }
    record(c, g.op);
    return 0;
}

#define ALAMO_GUI(fn, name, op, count, typed, type, what) \
    int fn(Call& c) { return gui(c, GuiSpec{name, PresentationOp::op, count, typed, type, what}); }
ALAMO_GUI(g_gui_button_icon, "GUI_Button_Icon", GuiButtonIcon, 6, 3, 'f', "float")
ALAMO_GUI(g_gui_blink, "GUI_Component_Blink", GuiComponentBlink, 4, 2, 'b', "bool")
ALAMO_GUI(g_gui_color, "GUI_Component_Color", GuiComponentColor, 5, 2, 'f', "float")
ALAMO_GUI(g_gui_enable, "GUI_Component_Enable", GuiComponentEnable, 2, 2, 'b', "bool")
ALAMO_GUI(g_gui_flash, "GUI_Component_Flash", GuiComponentFlash, 4, 2, 'b', "bool")
ALAMO_GUI(g_gui_play_anim, "GUI_Component_Play_Anim", GuiComponentPlayAnim, 4, 2, 's', "string for animation name")
ALAMO_GUI(g_gui_stop_anim, "GUI_Component_Stop_Anim", GuiComponentStopAnim, 1, 0, 's', "")
ALAMO_GUI(g_gui_stop_flash, "GUI_Component_Stop_Flash", GuiComponentStopFlash, 1, 0, 's', "")
ALAMO_GUI(g_gui_text, "GUI_Component_Text", GuiComponentText, 2, 2, 's', "text id string")
ALAMO_GUI(g_gui_visibility, "GUI_Component_Visibility", GuiComponentVisibility, 2, 2, 'b', "bool")
ALAMO_GUI(g_gui_text_color, "GUI_Text_Color", GuiTextColor, 5, 2, 'f', "float")
#undef ALAMO_GUI

constexpr BindingKind G = BindingKind::Global;
constexpr BindingKind M = BindingKind::Method;

constexpr detail::ImplEntry kImpl[] = {
    // camera / cinematics
    {G, "", "Start_Cinematic_Camera", g_start_cinematic_camera},
    {G, "", "End_Cinematic_Camera", g_end_cinematic_camera},
    {G, "", "Start_Cinematic_Mode", g_start_cinematic_mode},
    {G, "", "End_Cinematic_Mode", g_end_cinematic_mode},
    {G, "", "Set_Cinematic_Camera_Key", g_set_camera_key},
    {G, "", "Set_Cinematic_Target_Key", g_set_target_key},
    {G, "", "Transition_Cinematic_Camera_Key", g_transition_camera_key},
    {G, "", "Transition_Cinematic_Target_Key", g_transition_target_key},
    {G, "", "Transition_To_Tactical_Camera", g_transition_to_tactical},
    {G, "", "Point_Camera_At", g_point_camera_at},
    {G, "", "Scroll_Camera_To", g_scroll_camera_to},
    {G, "", "Camera_To_Follow", g_camera_to_follow},
    {G, "", "Rotate_Camera_By", g_rotate_camera_by},
    {G, "", "Rotate_Camera_To", g_rotate_camera_to},
    {G, "", "Zoom_Camera", g_zoom_camera},
    {G, "", "Cinematic_Zoom", g_cinematic_zoom},
    {G, "", "Set_Cinematic_Environment", g_set_cinematic_environment},
    {G, "", "Set_New_Environment", g_set_new_environment},
    {G, "", "Promote_To_Space_Cinematic_Layer", g_promote_to_space_cinematic_layer},
    {G, "", "Do_End_Cinematic_Cleanup", g_do_end_cinematic_cleanup},
    // screen
    {G, "", "Fade_Screen_In", g_fade_screen_in},
    {G, "", "Fade_Screen_Out", g_fade_screen_out},
    {G, "", "Fade_On", g_fade_on},
    {G, "", "Fade_Off", g_fade_off},
    {G, "", "Letter_Box_In", g_letter_box_in},
    {G, "", "Letter_Box_Out", g_letter_box_out},
    {G, "", "Letter_Box_On", g_letter_box_on},
    {G, "", "Letter_Box_Off", g_letter_box_off},
    {G, "", "Lock_Controls", g_lock_controls},
    {G, "", "Cancel_Fast_Forward", g_cancel_fast_forward},
    {G, "", "Enable_Fog", g_enable_fog},
    {G, "", "Enable_Distance_Fog", g_enable_distance_fog},
    // audio
    {G, "", "Allow_Localized_SFX", g_allow_localized_sfx},
    {G, "", "Play_Music", g_play_music},
    {G, "", "Stop_All_Music", g_stop_all_music},
    {G, "", "Resume_Mode_Based_Music", g_resume_mode_based_music},
    {G, "", "Stop_All_Speech", g_stop_all_speech},
    {G, "", "Master_Volume_Restore", g_master_volume_restore},
    {G, "", "Weather_Audio_Pause", g_weather_audio_pause},
    {M, "LuaSFXCommandsClassClass", "Allow_Ambient_VO", m_allow_ambient_vo},
    {M, "LuaSFXCommandsClassClass", "Allow_Enemy_Sighted_VO", m_allow_enemy_sighted_vo},
    {M, "LuaSFXCommandsClassClass", "Allow_HUD_VO", m_allow_hud_vo},
    {M, "LuaSFXCommandsClassClass", "Allow_Localized_SFXEvents", m_allow_localized_sfx_events},
    {M, "LuaSFXCommandsClassClass", "Allow_Unit_Reponse_VO", m_allow_unit_response_vo},
    {M, "GameObjectWrapper", "Play_SFX_Event", m_play_sfx_event},
    {M, "GameObjectWrapper", "Stop_SFX_Event", m_stop_sfx_event},
    // object effects
    {G, "", "Hide_Sub_Object", g_hide_sub_object},
    {G, "", "Resume_Hyperspace_In", g_resume_hyperspace_in},
    {M, "GameObjectWrapper", "Attach_Particle_Effect", m_attach_particle_effect},
    {M, "GameObjectWrapper", "Cinematic_Hyperspace_In", m_cinematic_hyperspace_in},
    {M, "GameObjectWrapper", "In_End_Cinematic", m_in_end_cinematic},
    {M, "GameObjectWrapper", "Show_Emitter", m_show_emitter},
    {M, "GameObjectWrapper", "Play_Cinematic_Engine_Flyby", m_play_cinematic_engine_flyby},
    // movies / effects
    {G, "", "Play_Bink_Movie", g_play_bink_movie},
    {G, "", "Stop_Bink_Movie", g_stop_bink_movie},
    {G, "", "Play_Lightning_Effect", g_play_lightning_effect},
    // UI
    {G, "", "Game_Message", g_game_message},
    {G, "", "Remove_All_Text", g_remove_all_text},
    {G, "", "Activate_Retry_Dialog", g_activate_retry_dialog},
    {G, "", "GUI_Button_Icon", g_gui_button_icon},
    {G, "", "GUI_Component_Blink", g_gui_blink},
    {G, "", "GUI_Component_Color", g_gui_color},
    {G, "", "GUI_Component_Enable", g_gui_enable},
    {G, "", "GUI_Component_Flash", g_gui_flash},
    {G, "", "GUI_Component_Play_Anim", g_gui_play_anim},
    {G, "", "GUI_Component_Stop_Anim", g_gui_stop_anim},
    {G, "", "GUI_Component_Stop_Flash", g_gui_stop_flash},
    {G, "", "GUI_Component_Text", g_gui_text},
    {G, "", "GUI_Component_Visibility", g_gui_visibility},
    {G, "", "GUI_Text_Color", g_gui_text_color},
};

}  // namespace

std::span<const detail::ImplEntry> detail::impl_presentation() { return kImpl; }

// N1: the IsFinished / Result closures of a block status table (push_block); a script waiting in BlockOnCommand keeps one.
std::span<const detail::NamedCFunction> detail::runtime_cfunctions_presentation() {
    static const NamedCFunction fns[] = {{"~presentation.block_is_finished", block_is_finished},
                                         {"~presentation.block_result", block_result}};
    return fns;
}

}  // namespace alamo::lua
