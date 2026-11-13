#include "alamo/game/hud_feeds.hpp"
#ifdef ALAMO_GAME_HAVE_CONTROLS
#include <algorithm>
#include <cmath>

#include "alamo/galactic/economy.hpp"
#include "alamo/objects/type_fields.hpp"

namespace alamo::game {

float day_cycle_fraction(int32_t mode_frame, int32_t cycle_start, int32_t cycle_frames) {
    if (cycle_frames <= 0) return -1.0f;
    const float f = static_cast<float>(mode_frame - cycle_start) / static_cast<float>(cycle_frames);
    return std::clamp(f, 0.0f, 1.0f);
}

float galactic_cycle_fraction(const galactic::GalacticSim& g) {
    return day_cycle_fraction(g.frame(), g.last_cycle_frame(), galactic::econ::fiscal_period_frames(g.rules().constants.fiscal_cycle_seconds));
}

ui::HudOrderButtons order_buttons(std::span<const OrderUnitState> selection, int32_t local_player, bool controls_locked) {
    ui::HudOrderButtons b;
    b.controls_locked = controls_locked;
    if (controls_locked) return b;  // the scan is skipped (HUD_GAPS §5.1)
    for (const OrderUnitState& u : selection) {
        if (u.owner != local_player || local_player < 0) continue;  // enemy / allied units in the selection are ignored
        b.can_attack = b.can_attack || u.can_attack;
        b.can_move = b.can_move || u.can_move;
    }
    return b;
}

ui::HudBombingRunButton bombing_run_button(bool land, bool controls_locked, bool available, float fraction) {
    ui::HudBombingRunButton b;
    if (!land) return b;
    b.present = true;
    const bool ready = !controls_locked && available;
    b.disabled = !ready;
    if (!ready) b.dial = fraction > 0.0f ? std::min(fraction, 1.0f) : 1.0f;
    return b;
}

BombingRunStyle bombing_run_style(const galactic::FactionRules& faction) {
    BombingRunStyle s;
    s.blob_material = std::string(faction.xml_text("Bombing_Run_Shadow_Blob_Material_Name"));
    if (const std::string_view v = faction.xml_text("Bombing_Run_Blob_Size"); !v.empty()) s.blob_size = static_cast<float>(objects::c_atof(v));
    return s;
}

const char* hardpoint_type_name(uint8_t kind) {
    static const char* const kNames[] = {"HARD_POINT_ENGINE",        "HARD_POINT_SHIELD_GENERATOR", "HARD_POINT_GRAVITY_WELL",
                                         "HARD_POINT_FIGHTER_BAY",   "HARD_POINT_TRACTOR_BEAM",     "HARD_POINT_WEAPON_LASER",
                                         "HARD_POINT_WEAPON_MISSILE", "HARD_POINT_WEAPON_TORPEDO",  "HARD_POINT_WEAPON_ION_CANNON",
                                         "HARD_POINT_WEAPON_MASS_DRIVER", "HARD_POINT_WEAPON_SPECIAL", "HARD_POINT_ENABLE_SPECIAL_ABILITY",
                                         "HARD_POINT_DUMMY_ART"};
    return kind < std::size(kNames) ? kNames[kind] : "";
}

ui::Vec3 hardpoint_world(ui::Vec3 position, ui::Vec3 facing, ui::Vec3 offset) {
    // Yaw only (ships bank / pitch little; presentation): forward = the facing's ground direction, left = +90 deg.
    float fx = facing.x, fy = facing.y;
    const float len = std::sqrt(fx * fx + fy * fy);
    if (len > 1e-6f) {
        fx /= len;
        fy /= len;
    } else {
        fx = 1.0f;
        fy = 0.0f;
    }
    ui::Vec3 out = position;
    out.x += fx * offset.x - fy * offset.y;
    out.y += fy * offset.x + fx * offset.y;
    out.z += offset.z;
    return out;
}

std::vector<ui::HeroEntry> galactic_hero_entries(const galactic::GalacticSim& g, int32_t player,
                                                 const std::function<std::string(uint32_t type)>& icon,
                                                 std::span<const uint32_t> selection, size_t max_entries,
                                                 const std::function<int32_t(uint32_t type)>& ranking) {
    std::vector<ui::HeroEntry> out;
    if (player < 0) return out;
    const objects::ObjectStore& st = g.store();
    auto selected = [&selection](uint32_t id) { return std::binary_search(selection.begin(), selection.end(), id); };
    std::vector<uint32_t> seen;  // one entry per hero type (§3.2 step 1)
    // Is_Named_Hero of the type, or of a company's first member (heroes travel as *_Team companies).
    auto named = [&](uint32_t t) { return is_named_hero(g, t); };
    auto keys = [&](ui::HeroEntry& e, uint32_t t) {  // §4 sort keys
        const uint32_t bar = hero_bar_type(g.rules(), t);
        e.ranking = ranking ? ranking(bar) : 0;
        e.type_name = bar < g.rules().types.size() ? g.rules().types[bar].name : std::string();
    };
    // FIXPASS_GALACTIC §2.2 click targets: planet = the planet up the hero's parent chain (an orbiting fleet's planet, the
    // surface's planet; kNoObject in hyperspace), container = its fleet (kNoObject on a surface).
    auto add_object = [&](objects::ObjectHandle h, uint32_t container_id, uint32_t planet_id) {
        // Own heroes only; galactic stealth (hidden(), e.g. Boba Fett) hides them from enemies, not from their owner's roster.
        if (out.size() >= max_entries || !st.alive(h) || st.owner(h) != player) return;
        const uint32_t t = static_cast<uint32_t>(st.type(h));
        if (!named(t) || std::find(seen.begin(), seen.end(), t) != seen.end()) return;
        std::string ic = icon ? icon(t) : std::string();
        if (ic.empty()) return;
        seen.push_back(t);
        ui::HeroEntry e;
        e.object = static_cast<uint32_t>(st.id(h));
        e.icon = std::move(ic);
        e.selected = selected(e.object) || (container_id != 0 && selected(container_id));
        e.planet = planet_id;
        e.container = container_id != 0 ? container_id : commands::kNoObject;
        keys(e, t);
        out.push_back(std::move(e));
    };
    auto planet_object = [&](PlanetIndex pi) {
        return pi < g.planets().size() && st.exists(g.planets()[pi].object) ? static_cast<uint32_t>(st.id(g.planets()[pi].object))
                                                                             : commands::kNoObject;
    };
    // Fleets (orbit and transit), then planet surfaces, in index order [port: the original's sort key is unread].
    for (const galactic::FleetState& fl : g.fleets()) {
        if (!fl.alive || fl.owner != player) continue;
        const uint32_t fid = st.exists(fl.object) ? static_cast<uint32_t>(st.id(fl.object)) : 0u;
        const uint32_t at = fl.moving ? commands::kNoObject : planet_object(fl.planet);
        for (const objects::ObjectHandle m : fl.contents) add_object(m, fid, at);
    }
    for (PlanetIndex pi = 0; pi < g.planets().size(); ++pi)
        for (const objects::ObjectHandle m : g.planets()[pi].surface) add_object(m, 0, planet_object(pi));
    // §3.2 step 2: named heroes being built (queue entries; the head's timer gives the dial) or awaiting respawn.
    const int32_t now = g.frame();
    const float respawn_frames = 30.0f * g.rules().special.default_hero_respawn_time;
    for (PlanetIndex pi = 0; pi < g.planets().size() && out.size() < max_entries; ++pi) {
        for (uint32_t q = 0; q < 2 && out.size() < max_entries; ++q) {
            const std::span<const galactic::BuildEntry> queue = g.queue_of(pi, q);
            for (size_t i = 0; i < queue.size() && out.size() < max_entries; ++i) {
                const galactic::BuildEntry& b = queue[i];
                if (b.player != player || !named(b.type)) continue;
                std::string ic = icon ? icon(b.type) : std::string();
                if (ic.empty()) continue;
                ui::HeroEntry e;
                e.icon = std::move(ic);
                e.respawn = (i == 0 && b.duration > 0) ? std::clamp(static_cast<float>(now - b.start) / static_cast<float>(b.duration), 0.0f, 1.0f) : 0.0f;
                keys(e, b.type);
                out.push_back(std::move(e));
            }
        }
        for (const galactic::RespawnEntry& r : g.respawns_pending(pi)) {
            if (out.size() >= max_entries) break;
            if (r.owner != player || !named(r.type)) continue;
            std::string ic = icon ? icon(r.type) : std::string();
            if (ic.empty()) continue;
            ui::HeroEntry e;
            e.icon = std::move(ic);
            // [port] the respawn list keeps only the due frame: start = due - Default_Hero_Respawn_Time.
            e.respawn = respawn_frames > 0.0f
                            ? std::clamp(1.0f - static_cast<float>(r.due - now) / respawn_frames, 0.0f, 1.0f)
                            : 0.0f;
            keys(e, r.type);
            out.push_back(std::move(e));
        }
    }
    return out;
}

bool is_named_hero(const galactic::GalacticSim& g, uint32_t type) {
    return type < g.rules().types.size() && (g.rules().types[type].named_hero || g.rules().special.type(type).named_hero);
}

uint32_t hero_bar_type(const galactic::GalacticRules& rules, uint32_t type) {
    if (type >= rules.types.size()) return type;
    const auto named = [&rules](uint32_t t) { return t < rules.types.size() && rules.types[t].named_hero; };
    const auto& company = rules.types[type];
    if (named(company.company_transport)) return company.company_transport;
    if (!company.company_units.empty() && named(company.company_units.front())) return company.company_units.front();
    return type;
}

std::vector<objects::ObjectHandle> conflict_units(const galactic::GalacticSim& g, const galactic::ConflictInfo& c, int32_t player) {
    std::vector<objects::ObjectHandle> out;
    const objects::ObjectStore& st = g.store();
    if (c.planet >= g.planets().size() || player < 0) return out;
    const galactic::PlanetState& p = g.planets()[c.planet];
    if (c.kind == galactic::ConflictKind::Space) {
        for (const uint32_t fi : g.fleets_at(c.planet, player))
            for (const objects::ObjectHandle m : g.fleets()[fi].contents)
                if (st.alive(m)) out.push_back(m);
        if (p.starbase_owner == player && st.alive(p.starbase)) out.push_back(p.starbase);
    } else if (player == c.attacker) {
        for (const objects::ObjectHandle m : c.units)
            if (st.alive(m)) out.push_back(m);
    } else {
        for (const objects::ObjectHandle m : p.surface)
            if (st.alive(m) && st.owner(m) == player) out.push_back(m);
    }
    return out;
}

BattleForceSummary battle_force_summary(const galactic::GalacticSim& g, std::span<const objects::ObjectHandle> units,
                                        const std::function<std::string(uint32_t type)>& icon, uint32_t ships_per_stack) {
    BattleForceSummary out;
    const objects::ObjectStore& st = g.store();
    std::string lead;
    for (const objects::ObjectHandle h : units) {
        if (!st.alive(h)) continue;
        ++out.units;
        const uint32_t t = static_cast<uint32_t>(st.type(h));
        if (out.hero) continue;
        std::string ic = icon ? icon(t) : std::string();
        if (ic.empty()) continue;
        if (is_named_hero(g, t)) {
            out.icon = std::move(ic);
            out.hero = true;
        } else if (lead.empty()) {
            lead = std::move(ic);
        }
    }
    if (!out.hero) out.icon = std::move(lead);
    const uint32_t per = ships_per_stack ? ships_per_stack : 1u;
    out.stacks = std::min(5u, (out.units + per - 1u) / per);
    return out;
}

int32_t ar_result_magnitude(bool win, uint32_t n_own, uint32_t n_other) {
    const uint64_t a = win ? n_other : n_own, b = win ? n_own : n_other;
    return a > 3u * b ? 0 : a > b ? 1 : 2;
}

ui::HudBuildOption::Block galactic_build_block(commands::RejectReason why) {
    using B = ui::HudBuildOption::Block;
    if (why == commands::RejectReason::None) return B::None;
    if (why == commands::RejectReason::QueueFull) return B::QueueFull;
    if (why == galactic::kRejectPopulationCap) return B::PopCap;
    if (why == commands::RejectReason::CannotAfford) return B::Funds;
    return B::Prerequisite;
}

std::vector<ui::GalacticBuildOption> galactic_build_options(const galactic::GalacticSim& g, int32_t player, PlanetIndex planet,
                                                            const std::function<std::string(uint32_t type)>& icon) {
    std::vector<ui::GalacticBuildOption> out;
    if (planet >= g.planets().size() || player < 0 || static_cast<size_t>(player) >= g.players().size()) return out;
    for (const uint32_t t : g.buildable(planet, player)) {
        if (t >= g.rules().types.size() || t > 0xFFFFu) continue;
        const galactic::TypeInfo& ti = g.rules().types[t];
        ui::GalacticBuildOption o;
        o.type_index = static_cast<uint16_t>(t);
        o.icon = icon ? icon(t) : std::string();
        o.cost = g.build_cost_for(player, planet, t);
        o.block = galactic_build_block(g.can_enqueue(player, planet, t));
        o.enabled = o.block == ui::HudBuildOption::Block::None;
        for (uint32_t q = 0; q < 2; ++q)
            for (const galactic::BuildEntry& b : g.queue_of(planet, q))
                if (b.player == player && b.type == t) ++o.queued;
        o.structure = galactic::is_ground_structure_type(ti) || galactic::is_orbital_structure_type(ti) || galactic::is_star_base_type(ti);
        o.production_disabled = g.production_disabled(player, t);
        // GALACTIC_DIALOGS §5.1: the space tab = space-queue units, starbases and orbital structures.
        o.space = g.queue_kind(t) == galactic::kSpaceQueue || galactic::is_star_base_type(ti) || galactic::is_orbital_structure_type(ti);
        out.push_back(std::move(o));
    }
    return out;
}

}  // namespace alamo::game
#endif
