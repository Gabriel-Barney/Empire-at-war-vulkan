#include "alamo/audio/lua_audio.hpp"

#include "alamo/audio/game_audio.hpp"

namespace alamo::audio {
namespace {

using lua::PresentationArg;
using lua::PresentationEvent;
using lua::PresentationOp;

const PresentationArg* first(const PresentationEvent& e, PresentationArg::Kind k) {
    for (const PresentationArg& a : e.args)
        if (a.kind == k) return &a;
    return nullptr;
}

LuaAudioResult apply(AudioSystem& audio, GameAudio* game, const std::vector<PresentationEvent>& events,
                     uint32_t* next_seq, const ObjectPositionFn& position) {
    LuaAudioResult r;
    uint32_t from = next_seq ? *next_seq : 0;
    for (const PresentationEvent& e : events) {
        if (e.seq < from) continue;
        if (next_seq) *next_seq = e.seq + 1;
        const PresentationArg* s = first(e, PresentationArg::Kind::String);
        const PresentationArg* b = first(e, PresentationArg::Kind::Bool);
        const PresentationArg* o = first(e, PresentationArg::Kind::Object);
        switch (e.op) {
            case PresentationOp::PlayMusic:
                if (!s) break;
                audio.play_music(s->text, music_mode::kLua, false, true);  // forced, no resume
                ++r.applied;
                continue;
            case PresentationOp::StopAllMusic:
                audio.stop_all_music();
                ++r.applied;
                continue;
            case PresentationOp::StopAllSpeech:
                audio.stop_all_speech();
                ++r.applied;
                continue;
            case PresentationOp::AllowLocalizedSfx:
            case PresentationOp::SfxAllowLocalizedSfxEvents:
                if (!b) break;
                audio.allow_localized_sfx(b->b);
                ++r.applied;
                continue;
            case PresentationOp::SfxAllowHudVo:
                if (!b) break;
                audio.allow_hud_vo(b->b);
                ++r.applied;
                continue;
            case PresentationOp::SfxAllowUnitResponseVo:
                if (!b) break;
                audio.allow_unit_response_vo(b->b);
                ++r.applied;
                continue;
            case PresentationOp::SfxAllowAmbientVo:
                if (!b) break;
                audio.allow_ambient_vo(b->b);
                ++r.applied;
                continue;
            case PresentationOp::ObjectPlaySfxEvent: {
                if (!s || !o) break;
                SfxPlay p;
                p.kind = SfxPlay::Kind::Object;
                p.owner = static_cast<uint64_t>(static_cast<uint32_t>(o->id));
                if (position) {
                    if (auto pos = position(o->id)) p.position = *pos;
                }
                audio.play_sfx(s->text, p);
                ++r.applied;
                continue;
            }
            case PresentationOp::ObjectStopSfxEvent:
                if (!s || !o) break;
                audio.stop_object_sfx(static_cast<uint64_t>(static_cast<uint32_t>(o->id)), s->text);
                ++r.applied;
                continue;
            case PresentationOp::ResumeModeBasedMusic:
                audio.resume_mode_based_music(true);  // the Lua call forces
                ++r.applied;
                continue;
            case PresentationOp::MasterVolumeRestore:
                audio.master_volume_restore();
                ++r.applied;
                continue;
            case PresentationOp::SfxAllowEnemySightedVo:
                if (!game) {
                    ++r.unhandled;
                    continue;
                }
                if (!b) break;
                game->allow_enemy_sighted_vo(b->b);
                ++r.applied;
                continue;
            case PresentationOp::WeatherAudioPause:
                if (!game) {
                    ++r.unhandled;
                    continue;
                }
                if (!b) break;
                game->weather_audio_pause(b->b);
                ++r.applied;
                continue;
            default:
                ++r.ignored;
                continue;
        }
        ++r.bad_args;  // a handled op whose arguments were missing
    }
    return r;
}

}  // namespace

LuaAudioResult apply_presentation_audio(AudioSystem& audio, const std::vector<PresentationEvent>& events,
                                        uint32_t* next_seq, const ObjectPositionFn& position) {
    return apply(audio, nullptr, events, next_seq, position);
}

LuaAudioResult apply_presentation_audio(GameAudio& game, const std::vector<PresentationEvent>& events,
                                        uint32_t* next_seq, const ObjectPositionFn& position) {
    return apply(game.audio(), &game, events, next_seq, position);
}

}  // namespace alamo::audio
