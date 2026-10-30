// Bridge from the Lua presentation log (engine/lua presentation.hpp, roadmap L1 presentation) to the
// audio player. Built only when the lua module is configured (ALAMO_AUDIO_HAVE_LUA).
//
// Handled ops (args as recorded: receiver first for methods):
//   Play_Music(name)                      -> play_music(name, mode 12, no resume, forced) (AUDIO.md §2.3)
//   Stop_All_Music()                      -> stop_all_music (closes both streams)
//   Resume_Mode_Based_Music()             -> resume_mode_based_music(forced)
//   Stop_All_Speech()                     -> stop_all_speech
//   Allow_Localized_SFX(b), SFXManager.Allow_Localized_SFXEvents(b) -> allow_localized_sfx
//   SFXManager.Allow_HUD_VO / Allow_Unit_Reponse_VO / Allow_Ambient_VO(b) -> the matching gate
//   GameObject.Play_SFX_Event(name)       -> play_sfx attached to the object (initial position from the callback)
//   GameObject.Stop_SFX_Event(name)       -> stop_object_sfx (fade 0 [port]: AUDIO.md §1.5 gives no fade time for it)
//   Master_Volume_Restore()               -> master_volume_restore (§4 saved level)
// With the GameAudio overload also:
//   Weather_Audio_Pause(b)                -> AmbienceManager::set_paused
//   SFXManager.Allow_Enemy_Sighted_VO(b)  -> GameAudio::allow_enemy_sighted_vo
// (the AudioSystem overload counts these two as `unhandled`).
#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "alamo/audio/audio_system.hpp"
#include "alamo/lua/presentation.hpp"

namespace alamo::audio {

// Object id -> world position (nullopt: the object is gone / unknown; the SFX plays unpositioned).
using ObjectPositionFn = std::function<std::optional<Vec3>(int32_t object_id)>;

struct LuaAudioResult {
    uint32_t applied = 0;
    uint32_t ignored = 0;    // not an audio op
    uint32_t unhandled = 0;  // audio op with no player behaviour yet
    uint32_t bad_args = 0;
};

// Applies the events with seq >= *next_seq, in log order, and advances *next_seq past them.
LuaAudioResult apply_presentation_audio(AudioSystem& audio, const std::vector<lua::PresentationEvent>& events,
                                        uint32_t* next_seq, const ObjectPositionFn& position = {});
class GameAudio;
// Same, through the game host's GameAudio (handles every audio op).
LuaAudioResult apply_presentation_audio(GameAudio& game, const std::vector<lua::PresentationEvent>& events,
                                        uint32_t* next_seq, const ObjectPositionFn& position = {});

}  // namespace alamo::audio
