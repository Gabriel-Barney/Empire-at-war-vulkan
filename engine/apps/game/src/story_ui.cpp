#include "alamo/game/story_ui.hpp"

#include <algorithm>
#include <iterator>
#include <span>

#include "alamo/core/ascii.hpp"

namespace alamo::game {

commands::EventData story_ui_event(const sim::StoryUiNotification& n) {
    std::vector<uint8_t> b;
    const size_t len = std::min(n.name.size(), kStoryUiMaxName);
    b.reserve(5 + len);
    b.push_back(static_cast<uint8_t>(static_cast<uint8_t>(n.kind) | (n.tactical ? kStoryUiTacticalBit : 0u)));
    const auto o = static_cast<uint32_t>(n.object);
    for (int k = 0; k < 4; ++k) b.push_back(static_cast<uint8_t>((o >> (8 * k)) & 0xFFu));
    for (size_t i = 0; i < len; ++i) b.push_back(static_cast<uint8_t>(n.name[i]));
    return commands::ev::port_extension(kStoryUiSubtype, std::span<const uint8_t>(b));
}

commands::EventData story_esc_event() { return commands::ev::port_extension(kStoryEscSubtype, {}); }

bool read_story_ui_event(const commands::EventData& ev, sim::StoryUiNotification& n) {
    if (ev.type() != commands::EventType::PortExtension || ev.u("subtype") != kStoryUiSubtype) return false;
    const std::span<const uint8_t> b = ev.bytes_view("data");
    const auto kind = static_cast<uint8_t>(b.empty() ? 0u : b[0] & ~kStoryUiTacticalBit);
    if (b.size() < 5 || b.size() > 5 + kStoryUiMaxName || kind >= static_cast<uint8_t>(sim::StoryUiEvent::Count)) return false;
    n = {};
    n.kind = static_cast<sim::StoryUiEvent>(kind);
    n.tactical = (b[0] & kStoryUiTacticalBit) != 0;
    uint32_t o = 0;
    for (int k = 0; k < 4; ++k) o |= static_cast<uint32_t>(b[static_cast<size_t>(1 + k)]) << (8 * k);
    n.object = static_cast<int32_t>(o);
    n.name.reserve(b.size() - 5);
    for (size_t i = 5; i < b.size(); ++i) n.name.push_back(static_cast<char>(b[i]));
    return true;
}

void story_reward_media(std::string_view reward_type, const std::vector<std::string>& params, std::vector<StoryMediaCue>& out) {
    auto param = [&params](size_t i) -> std::string_view {
        if (i >= params.size()) return {};
        std::string_view v = params[i];
        while (!v.empty() && static_cast<unsigned char>(v.front()) <= 0x20) v.remove_prefix(1);
        while (!v.empty() && static_cast<unsigned char>(v.back()) <= 0x20) v.remove_suffix(1);
        return v;
    };
    if (reward_type == "START_MOVIE" && !param(0).empty()) out.push_back({StoryMediaCue::Kind::Movie, std::string(param(0))});
    if (reward_type == "MULTIMEDIA" && !param(7).empty()) out.push_back({StoryMediaCue::Kind::Speech, std::string(param(7))});
    // FIXPASS_GALACTIC §4.2 (capture C2): Reward_Param9 = the talking head; the data also holds "" and "-1" (no head).
    if (reward_type == "MULTIMEDIA" && param(8).size() > 5 && ascii_iends_with(param(8), "_loop"))
        out.push_back({StoryMediaCue::Kind::AdvisorMovie, std::string(param(8))});
    if (reward_type == "STOP_COMMANDBAR_MOVIE") out.push_back({StoryMediaCue::Kind::AdvisorStop, {}});
}

void StoryUiFeed::reset() {
    last_ = {};
    last_battle_.clear();
    battle_ = battle_seeded_ = false;
    out_.clear();
}

void StoryUiFeed::set_battle(bool battle) {
    if (battle != battle_) {
        last_battle_.clear();
        battle_seeded_ = false;
    }
    battle_ = battle;
}

void StoryUiFeed::observe_battle(const std::vector<uint32_t>& selection) {
    std::vector<uint32_t> now = selection;
    std::sort(now.begin(), now.end());
    now.erase(std::unique(now.begin(), now.end()), now.end());
    now.erase(std::remove(now.begin(), now.end(), 0u), now.end());
    if (battle_seeded_)
        for (const uint32_t id : now)
            if (!std::binary_search(last_battle_.begin(), last_battle_.end(), id)) raise(sim::StoryUiEvent::SelectUnit, id, {}, true);
    battle_seeded_ = true;
    last_battle_ = std::move(now);
}

void StoryUiFeed::raise(sim::StoryUiEvent kind, uint32_t object, std::string_view name, bool tactical) {
    sim::StoryUiNotification n;
    n.kind = kind;
    n.tactical = tactical;
    n.object = static_cast<int32_t>(object);
    n.name = std::string(name.substr(0, kStoryUiMaxName));
    out_.push_back(std::move(n));
    ++counts_[static_cast<size_t>(kind)];
}

void StoryUiFeed::observe(const StoryUiView& v) {
    using E = sim::StoryUiEvent;
    StoryUiView now = v;
    std::sort(now.selection.begin(), now.selection.end());
    now.selection.erase(std::unique(now.selection.begin(), now.selection.end()), now.selection.end());
    now.selection.erase(std::remove(now.selection.begin(), now.selection.end(), 0u), now.selection.end());
    if (now.zoomed && now.zoom_planet == 0) now.zoom_planet = now.planet;
    if (!now.zoomed) now.zoom_planet = 0;
    for (const uint32_t id : now.selection)  // newly selected objects (last_.selection is sorted)
        if (!std::binary_search(last_.selection.begin(), last_.selection.end(), id)) raise(E::SelectUnit, id, {});
    if (now.planet != 0 && now.planet != last_.planet) raise(E::SelectPlanet, now.planet, {});
    if (last_.zoomed && (!now.zoomed || now.zoom_planet != last_.zoom_planet) && last_.zoom_planet != 0)
        raise(E::ZoomOutPlanet, last_.zoom_planet, {});
    if (now.zoomed && (!last_.zoomed || now.zoom_planet != last_.zoom_planet) && now.zoom_planet != 0)
        raise(E::ZoomIntoPlanet, now.zoom_planet, {});
    if (now.corruption_planet != 0 && now.corruption_planet != last_.corruption_planet)
        raise(E::OpenCorruption, now.corruption_planet, {});
    last_ = std::move(now);
}

void StoryUiFeed::click_gui(std::string_view component) {
    if (!component.empty()) raise(sim::StoryUiEvent::ClickGui, 0, component, battle_);
}
void StoryUiFeed::speech_done(std::string_view event) {
    if (!event.empty()) raise(sim::StoryUiEvent::SpeechDone, 0, event, battle_);
}
void StoryUiFeed::movie_done(std::string_view movie) {
    if (!movie.empty()) raise(sim::StoryUiEvent::MovieDone, 0, movie);
}
void StoryUiFeed::generic(std::string_view name) {
    if (!name.empty()) raise(sim::StoryUiEvent::Generic, 0, name);
}
void StoryUiFeed::mission_lost(std::string_view name) {
    if (!name.empty()) raise(sim::StoryUiEvent::MissionLost, 0, name);
}

std::vector<sim::StoryUiNotification> StoryUiFeed::take() {
    std::vector<sim::StoryUiNotification> v;
    v.swap(out_);
    return v;
}

uint64_t StoryUiFeed::raised(sim::StoryUiEvent kind) const {
    const auto i = static_cast<size_t>(kind);
    return i < std::size(counts_) ? counts_[i] : 0;
}

uint64_t StoryUiFeed::raised_total() const {
    uint64_t t = 0;
    for (const uint64_t c : counts_) t += c;
    return t;
}

// --- story t13: LinkedBattleOverlay (story_ui.hpp) ---
void LinkedBattleOverlay::begin(bool letterbox, bool input_block, uint32_t battle_frame) {
    reset();
    start_ = battle_frame;
    block_ = input_block;
    letterbox_hold_ = letterbox;
    input_hold_ = input_block;
    blocked_ = input_block;
    if (letterbox) bars_.show();
}

void LinkedBattleOverlay::reset() {
    bars_.remove();
    start_ = 0;
    block_ = letterbox_hold_ = input_hold_ = blocked_ = false;
}

void LinkedBattleOverlay::script_letterbox(bool in) {
    letterbox_hold_ = false;
    if (in) bars_.slide_in();
    else bars_.slide_out();
}

void LinkedBattleOverlay::script_controls() { input_hold_ = false; }

void LinkedBattleOverlay::update(uint32_t battle_frame, bool script_locked, float dt_seconds) {
    const bool late = battle_frame >= start_ && battle_frame - start_ >= kHandOverFrames;
    if (letterbox_hold_ && late) {
        letterbox_hold_ = false;
        bars_.slide_out();
    }
    if (input_hold_ && late) input_hold_ = false;
    blocked_ = block_ && (input_hold_ || script_locked);
    bars_.update(dt_seconds);
}

}  // namespace alamo::game
