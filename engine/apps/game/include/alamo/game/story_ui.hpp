// L1/S4 wave 1 host side: the local player's UI actions -> sim::StorySession::notify_ui (STORY_SELECT_UNIT,
// SELECT_PLANET, ZOOM_INTO / ZOOM_OUT_PLANET, CLICK_GUI, SPEECH_DONE, MOVIE_DONE, GENERIC, MISSION_LOST, OPEN_CORRUPTION).
//   StoryUiFeed     data-free: turns one frame's observed galactic UI state (selection, selected planet, camera zoom,
//                   open corruption panel) into notifications on the transitions, plus the one-shot events the host
//                   reports (GUI click, speech / movie end, battle-end screen closed, mission lost). Presentation only.
//   story_ui_event  [port] a notification as a port-only command (PortExtension subtype kStoryUiSubtype): the original
//                   dispatches these synchronously from the UI (STORY.md 4.1), but a fired story event changes the
//                   hashed story state, so the port sends them through the command stream (recorded, replayed, applied
//                   at the command phase by GameSession::port_event -> notify_ui with the issuing player). Payload: kind
//                   u8 (bit 7 kStoryUiTacticalBit = StoryUiNotification::tactical; recordings made before the flag never
//                   set it), object u32 little endian, then the name bytes (at most kStoryUiMaxName).
// Battle screen (tactical: the sim routes these to the attached tactical plot set, header of story_session.hpp):
//   SelectUnit       observe_battle(): the same newly-selected rule on the battle store's ids (its own last selection;
//                    a set_battle change forgets it). The first observation after set_battle(true) raises nothing: the
//                    selection carried over the tactical push (galactic ids) is not a battle selection [port].
//   ClickGui / SpeechDone  raised while set_battle(true) carry `tactical` (MovieDone / Generic / MissionLost never do).
// Story reward media (story_reward_media): what the host plays for a fired story event's reward - START_MOVIE
//   (Reward_Param1 = Movies.xml name: the full-screen movie slot, whose end raises MOVIE_DONE) and MULTIMEDIA
//   (Reward_Param8 = speech event: GameAudioHost::speech, whose end raises SPEECH_DONE).
// Transition rules (StoryUiFeed::observe, galactic screen only; [inferred] the original's dispatch points are not
// specified beyond the event types):
//   SelectUnit       every object id that is in the selection now and was not in the last observed one, ascending
//                    id (re-selecting the same set raises nothing; a changed set raises the new members only).
//   SelectPlanet     the selected planet changed to a non-zero planet.
//   ZoomIntoPlanet   the camera became zoomed, or stayed zoomed onto another planet: object = the zoom planet (else
//                    the selected planet).
//   ZoomOutPlanet    the camera stopped being zoomed: object = the planet it was zoomed onto.
//   OpenCorruption   the corruption panel opened (or switched planet): object = its planet.
// story t13 (LINK_TACTICAL presentation, BT §2.9.1 P10 / P11; sim state CampaignBattles::linked_presentation):
//   LinkedBattleOverlay  what a story-linked battle opens with, until the battle's story script takes over:
//                    P10 with P9 != 1 (LinkedPresentation::letterbox) -> the letterbox bars of the overlay 0x140a28c10 are
//                    fully in from the first battle frame; P11 (presentation_b, the cinematic input-block flag 0x140a28c91,
//                    UI.md §1.3 / §4) -> the world gets no mouse input and the selection circles are hidden. The same
//                    overlay draws the scripts' Letter_Box_In / _On (in) and _Out / _Off (out) in every battle (TSW §11.2
//                    [M]: one bar overlay, the same height constant). [port][L] the hand-over: a script letterbox op ends the
//                    letterbox hold; Lock_Controls / Start_ or End_Cinematic_Mode / _Camera ends the input hold, after which
//                    the P11 block follows the scripts' own lock (Lock_Controls on, cinematic camera or mode on). Either
//                    hold also ends kHandOverFrames battle frames after the start, so a battle whose script never takes
//                    over is not left letterboxed or without input. The script's own letterbox times are not modelled
//                    (the 0.5 s slide). The other P10 globals (0x140a28c93, 0x140b27ee0 / ee4, 0x140a28c20) have no
//                    known effect (BT §2.9.9 item 1): not shown.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/commands/command.hpp"
#include "alamo/game/battle_fx.hpp"
#include "alamo/sim/story_session.hpp"

namespace alamo::game {

inline constexpr uint16_t kStoryUiSubtype = 0x5355;  // "SU"
inline constexpr size_t kStoryUiMaxName = 255;
inline constexpr uint8_t kStoryUiTacticalBit = 0x80;
commands::EventData story_ui_event(const sim::StoryUiNotification& n);
// False when `ev` is not a well-formed story UI event (n.player is left at -1: the issuer fills it).
bool read_story_ui_event(const commands::EventData& ev, sim::StoryUiNotification& n);
// bkc1 t01 (BKC1_RE_ANSWERS Q1): the local player's Esc during a story cinematic camera as a port-only command (no
// payload). Retail calls Story_Handle_Esc from the message pump between ticks; the port records the key and applies it at
// the next command phase (GameSession::port_event -> sim::StorySession::handle_esc), so replays repeat it.
inline constexpr uint16_t kStoryEscSubtype = 0x5345;  // "SE"
commands::EventData story_esc_event();

// A presentation cue of a fired story reward (story_reward_media).
struct StoryMediaCue {
    // Movie = full-screen START_MOVIE; AdvisorMovie = MULTIMEDIA's *_Loop talking head; AdvisorStop = STOP_COMMANDBAR_MOVIE.
    enum class Kind : uint8_t { Movie, Speech, AdvisorMovie, AdvisorStop };
    Kind kind = Kind::Movie;
    std::string name;
};
// Appends the cues of one fired reward (type upper-case as in StoryFired; params[0] = Reward_Param1).
void story_reward_media(std::string_view reward_type, const std::vector<std::string>& params, std::vector<StoryMediaCue>& out);

// One frame of the galactic UI as the story sees it (object ids of the galactic store, 0 = none).
struct StoryUiView {
    std::vector<uint32_t> selection;  // selected objects (any order; 0 entries ignored)
    uint32_t planet = 0;              // selected planet
    bool zoomed = false;              // the galactic camera is zoomed onto a planet (GalacticCamera::zoomed)
    uint32_t zoom_planet = 0;         // the planet the camera zoomed onto (0 = unknown: `planet`)
    uint32_t corruption_planet = 0;   // the open corruption panel's planet (0 = closed)
};

class StoryUiFeed {
public:
    // Forgets the observed state (a new session): the next observe() compares with an empty view.
    void reset();
    void observe(const StoryUiView& v);
    // Battle screen: `battle` = a battle with a tactical story runs (changing it forgets the battle selection).
    void set_battle(bool battle);
    bool battle() const { return battle_; }
    // The battle's selection (battle-store ids, any order): SelectUnit (tactical) for the newly selected ones.
    void observe_battle(const std::vector<uint32_t>& selection);
    // One-shot events (name = GUI component / speech event / movie / GENERIC name / mission plots file).
    void click_gui(std::string_view component);
    void speech_done(std::string_view event);
    void movie_done(std::string_view movie);
    void generic(std::string_view name);
    void mission_lost(std::string_view name);
    // The notifications raised since the last call, in raise order (player = -1: the command's issuer).
    std::vector<sim::StoryUiNotification> take();
    uint64_t raised(sim::StoryUiEvent kind) const;
    uint64_t raised_total() const;

private:
    void raise(sim::StoryUiEvent kind, uint32_t object, std::string_view name, bool tactical = false);
    StoryUiView last_;
    std::vector<uint32_t> last_battle_;  // sorted, unique, no 0
    bool battle_ = false;
    bool battle_seeded_ = false;  // the first observe_battle of a battle only records the selection
    std::vector<sim::StoryUiNotification> out_;
    uint64_t counts_[static_cast<size_t>(sim::StoryUiEvent::Count)] = {};
};

// story t13: see the header comment. Presentation only (wall-time slide, no sim reads beyond the frame number).
class LinkedBattleOverlay {
public:
    static constexpr uint32_t kHandOverFrames = 150;  // [port] 5 s of battle frames at 30 Hz
    // Battle start: `letterbox` = LinkedPresentation::letterbox, `input_block` = presentation_b of a linked battle (both
    // false for any other battle); `battle_frame` = TacticalBattle::frame() now.
    void begin(bool letterbox, bool input_block, uint32_t battle_frame);
    void reset();  // no battle: bars off, nothing held
    // This frame's story-script ops, in order: a letterbox op (in = Letter_Box_In / _On) and a control op (Lock_Controls,
    // Start_ / End_Cinematic_Mode, Start_ / End_Cinematic_Camera).
    void script_letterbox(bool in);
    void script_controls();
    // Once per frame after the sim: the battle frame, the scripts' lock (Lock_Controls on, cinematic camera or mode on) and
    // the wall time for the slide.
    void update(uint32_t battle_frame, bool script_locked, float dt_seconds);
    float letterbox_level() const { return bars_.level(); }
    bool input_blocked() const { return blocked_; }
    bool letterbox_held() const { return letterbox_hold_; }
    bool input_held() const { return input_hold_; }

private:
    Letterbox bars_;
    uint32_t start_ = 0;
    bool block_ = false;  // P11 for this battle
    bool letterbox_hold_ = false, input_hold_ = false, blocked_ = false;
};

}  // namespace alamo::game
