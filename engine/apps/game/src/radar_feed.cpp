#include "alamo/game/radar_feed.hpp"
#include "alamo/objects/game_object_types.hpp"

#include <algorithm>

#include "alamo/core/ascii.hpp"
#include "alamo/data/values.hpp"
#ifdef ALAMO_SIM_HAVE_STORY
#include "alamo/lua/story_model.hpp"
#endif

namespace alamo::game {

std::vector<ui::RadarField> radar_asteroid_fields(std::span<const tactical::abilities::SpaceField> fields) {
    std::vector<ui::RadarField> out;
    for (const tactical::abilities::SpaceField& f : fields) {
        if (!(f.kinds & (tactical::abilities::kFieldAsteroid | tactical::abilities::kFieldImpassable)) || !(f.radius > 0.0f)) continue;
        out.push_back({f.x, f.y, f.radius});
    }
    return out;
}

void RadarFeed::reset() {
    progress_.clear();
    death_pings_ = transition_pings_ = 0;
}

void RadarFeed::deaths(std::span<const RadarDeathNote> notes, const ui::RadarSettings& settings, ui::RadarEvents& events,
                       const std::string& event) {
    if (event.empty()) return;
    for (const RadarDeathNote& n : notes) {
        if (!n.visible || !n.visible_on_radar) continue;
        events.add(settings, event, n.x, n.y);
        ++death_pings_;
    }
}

void RadarFeed::capture_points(std::span<const RadarCapturePointNote> points, const ui::RadarSettings& settings, ui::RadarEvents& events) {
    for (const RadarCapturePointNote& p : points) {
        const auto it = progress_.find(p.id);
        const float before = it == progress_.end() ? 0.0f : it->second;
        // Progress leaves 0 towards a controller other than the owner (0x501980: "from 0 play ... Begin_Control_Transition").
        if (before <= 0.0f && p.progress > 0.0f && p.pending >= 0 && p.pending != p.owner && p.visible && !p.begin_event.empty()) {
            events.add(settings, p.begin_event, p.x, p.y);
            ++transition_pings_;
        }
        progress_[p.id] = p.progress;
    }
}

void RadarFeed::blips(std::span<const RadarBlipNote> notes, ui::RadarEvents& events) {
    for (const RadarBlipNote& n : notes)
        if (n.valid) events.set_marker(n.name, n.x, n.y);
    for (const std::string& name : events.marker_names()) {
        const bool listed = std::any_of(notes.begin(), notes.end(), [&](const RadarBlipNote& n) { return ascii_iequals(n.name, name); });
        if (!listed) events.remove_marker(name);
    }
}

void RadarFeed::battle_tick(const sim::TacticalBattle& battle, const objects::ObjectStore& store, const objects::GameObjectTypes& types,
                            std::span<const ui::HudTypeInfo> hud_types, int32_t local_player, const lua::StoryModel* story,
                            const ui::RadarSettings& settings, ui::RadarEvents& events) {
    std::vector<RadarDeathNote> deaths_now;
    for (const tactical::combat::DeathEvent& d : battle.combat().deaths()) {
        const objects::ObjectHandle h = store.find(static_cast<objects::ObjectId>(d.id));
        if (!store.exists(h)) continue;
        const objects::TypeIndex t = store.type(h);
        RadarDeathNote n;
        n.x = store.position(h).x;
        n.y = store.position(h).y;
        n.visible = !battle.fog_object_hidden(local_player, static_cast<objects::ObjectId>(d.id), false);
        n.visible_on_radar = t >= hud_types.size() || hud_types[t].visible_on_radar;
        deaths_now.push_back(n);
    }
    deaths(deaths_now, settings, events);
    if (const tactical::garrison::CapturePointSystem* cps = battle.captures()) {
        std::vector<RadarCapturePointNote> pts;
        for (size_t i = 0; i < cps->point_count(); ++i) {
            const objects::ObjectId id = cps->point_at(i);
            const objects::ObjectHandle h = store.find(id);
            if (!store.exists(h)) continue;
            RadarCapturePointNote n;
            n.id = id;
            n.owner = cps->owner(id);
            n.pending = cps->pending(id);
            n.progress = cps->progress(id);
            n.x = store.position(h).x;
            n.y = store.position(h).y;
            n.visible = !battle.fog_object_hidden(local_player, id, false);
            const objects::TypeIndex t = store.type(h);
            if (t < types.size()) n.begin_event = std::string(data::trim(types.get_string(t, "Begin_Control_Transition_Radar_Event")));
            pts.push_back(std::move(n));
        }
        capture_points(pts, settings, events);
    }
#ifdef ALAMO_SIM_HAVE_STORY
    std::vector<RadarBlipNote> notes;
    if (story) {
        for (const lua::StoryBlip& b : story->blips()) {
            RadarBlipNote n;
            n.name = b.name;
            const objects::ObjectHandle h = b.object_id >= 0 ? store.find(static_cast<objects::ObjectId>(b.object_id)) : objects::ObjectHandle{};
            n.valid = store.exists(h) && store.alive(h);
            if (n.valid) {
                n.x = store.position(h).x;
                n.y = store.position(h).y;
            }
            notes.push_back(std::move(n));
        }
    }
    blips(notes, events);
#else
    (void)story;
#endif
}

}  // namespace alamo::game
