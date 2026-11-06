// N1 save / load of the galactic simulation (contract: alamo/core/save_file.hpp): GalacticSim ("galactic.state")
// and ConflictManager ("galactic.conflicts").
//
// GalacticSim layout (little-endian, version kGalacticSaveVersion):
//   u32 version
//   definition fingerprint (checked, not restored): u32 planet rows, u32 routes, u32 type count, u32 player count
//   mode: i32 frame, pause, last_cycle_frame, next_cycle_frame, cycle_counter; bool linked_battle_pending;
//         i32 neutral_player, local_player; bool auto_reveal, reveal_sfx, initial_build_locks, emit_events, slot_choices
//   players (player order): PlayerState field by field + f32 last_income
//   u32 relation count (0 or P x P) + u8 each
//   planets (row order): bool is_planet + u32 type (checked against the session), then every other PlanetState field
//         except the definition-derived position and links (visibility lists, income breakdown, both build queues,
//         starbase / structures / surface handles, influence ids, orbit fleet indices, respawn list, (v3) i32 x 2 build
//         queue phases)
//   u32 fleet count + every FleetState (dead ones too: fleet indices are stable references) incl. path and slots
//   u32 + i32 stealthed ids; u32 + victory claims; (v2) i32 victory hold; u32 + pending S4 events (GalacticEvent,
//   emission order)
// Object references: a handle whose object exists (live or dead-not-flushed) is written as its ObjectId and resolved
// with ObjectStore::find on read (the store is restored first; an unknown id fails the read). A handle that refers to
// no object any more (stale, or null) is written as kNoObjectId + its raw value, which read keeps as the same stale
// handle when the restored store does not have that slot live (else null), so stale-handle behaviour is unchanged.
//
// Rebuilt on read, not saved: fleet_by_slot_ / planet_by_slot_ (from the restored handles, fleet / row order as at
// creation), derived_ (per-tick scratch, written before it is read), last_spawn_ (set before every read), the path
// scratch. Definitions (map, graph, planet positions and route links, rules) come from start() with the same setup.
// Not saved (the host's): the income / production hooks and their ids, the conflict hook pointer, the ParallelFor.
//
// ConflictManager layout (version kConflictSaveVersion): u32 planet rows (checked); both queues; the state machine
// (state, active kind, tick, timer start, force, retreating, pause held, choice); the auto-resolve in flight (input
// combatants - piece names are rebuilt from the type index -, sides, cargo, and the parallel object lists); the last
// auto-resolve result; the active BattleRequest; the per-planet random maps; the conflict log. Not saved: the config
// and the host callbacks (decide, start_battle, pause, pick_map, on_survival_roll), re-attached by the host.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "alamo/galactic/campaign.hpp"
#include "alamo/galactic/conflicts.hpp"

namespace alamo::galactic {

namespace {

constexpr uint32_t kGalacticSaveVersion = 3;  // v2 (story t11): + i32 victory hold; v3 (A2k t04): + queue phases; reads v1
constexpr uint32_t kConflictSaveVersion = 2;  // v2: BattleRequest::linked (story t10)

// Element count, bounded by the bytes left (each element takes at least `min_bytes`), so corrupt counts never
// allocate.
bool get_count(ByteReader& r, uint32_t& n, size_t min_bytes) {
    n = r.u32();
    return r.ok() && static_cast<uint64_t>(n) * static_cast<uint64_t>(min_bytes) <= static_cast<uint64_t>(r.remaining());
}

void put_handle(ByteWriter& w, const objects::ObjectStore& s, ObjectHandle h) {
    if (s.exists(h)) {
        w.i32(s.id(h));
        return;
    }
    w.i32(objects::kNoObjectId);
    w.u64(h.value);
}

bool get_handle(ByteReader& r, const objects::ObjectStore& s, ObjectHandle& out) {
    const int32_t id = r.i32();
    if (!r.ok()) return false;
    if (id != objects::kNoObjectId) {
        out = s.find(id);
        return static_cast<bool>(out);  // the save names an object the restored store does not have
    }
    const ObjectHandle raw{r.u64()};
    if (!r.ok()) return false;
    out = (raw.slot() < s.capacity() && !s.exists(raw)) ? raw : ObjectHandle{};
    return true;
}

void put_handles(ByteWriter& w, const objects::ObjectStore& s, const std::vector<ObjectHandle>& v) {
    w.u32(static_cast<uint32_t>(v.size()));
    for (ObjectHandle h : v) put_handle(w, s, h);
}

bool get_handles(ByteReader& r, const objects::ObjectStore& s, std::vector<ObjectHandle>& v) {
    uint32_t n = 0;
    if (!get_count(r, n, 4)) return false;
    v.assign(n, ObjectHandle{});
    for (ObjectHandle& h : v)
        if (!get_handle(r, s, h)) return false;
    return true;
}

void put_u32s(ByteWriter& w, const std::vector<uint32_t>& v) {
    w.u32(static_cast<uint32_t>(v.size()));
    for (uint32_t x : v) w.u32(x);
}

bool get_u32s(ByteReader& r, std::vector<uint32_t>& v) {
    uint32_t n = 0;
    if (!get_count(r, n, 4)) return false;
    v.assign(n, 0u);
    for (uint32_t& x : v) x = r.u32();
    return r.ok();
}

void put_vec3(ByteWriter& w, const Vec3& v) { w.f32(v.x).f32(v.y).f32(v.z); }

Vec3 get_vec3(ByteReader& r) {
    Vec3 v;
    v.x = r.f32();
    v.y = r.f32();
    v.z = r.f32();
    return v;
}

bool get_str(ByteReader& r, std::string& s) {
    s = r.str();
    return r.ok();
}

}  // namespace

// ---- GalacticSim --------------------------------------------------------------------------------------------------------

void GalacticSim::write(ByteWriter& w) const {
    const objects::ObjectStore& st = *store_;
    w.u32(kGalacticSaveVersion);
    w.u32(static_cast<uint32_t>(planets_.size()));
    w.u32(static_cast<uint32_t>(map_.routes.size()));
    w.u32(static_cast<uint32_t>(rules_->types.size()));
    w.u32(static_cast<uint32_t>(players_.size()));

    w.i32(frame_).i32(pause_).i32(last_cycle_frame_).i32(next_cycle_frame_).i32(cycle_counter_);
    w.boolean(linked_battle_pending_);
    w.i32(neutral_player_).i32(local_player_);
    w.boolean(auto_reveal_).boolean(reveal_sfx_).boolean(initial_build_locks_).boolean(emit_events_).boolean(slot_choices_);

    for (size_t i = 0; i < players_.size(); ++i) {
        const PlayerState& p = players_[i];
        w.u32(p.faction).boolean(p.human).boolean(p.ai).u32(p.difficulty);
        w.f32(p.account.credits).f32(p.account.cap);
        w.i32(p.tech_level).f32(p.tech_cost_mult).boolean(p.cheat);
        put_u32s(w, p.bm_purchased);
        w.boolean(p.bm_tutorial).boolean(p.sabotage_tutorial);
        for (int32_t c : p.corruptions_completed) w.i32(c);
        put_u32s(w, p.locked_types);
        put_u32s(w, p.unlocked_types);
        put_u32s(w, p.production_disabled);
        w.i32(p.max_tech_level);
        w.u32(static_cast<uint32_t>(p.built.size()));
        for (const auto& [type, count] : p.built) w.u32(type).i32(count);
        w.f32(i < last_income_.size() ? last_income_[i] : 0.0f);
    }
    w.u32(static_cast<uint32_t>(relations_.size()));
    for (uint8_t rel : relations_) w.u8(rel);

    for (const PlanetState& p : planets_) {
        w.boolean(p.is_planet).u32(p.type);
        put_handle(w, st, p.object);
        w.boolean(p.initialised).i32(p.owner).f32(p.control).f32(p.target_control).i32(p.leaning);
        w.i32(p.base_value).f32(p.black_market).boolean(p.black_market_influences).boolean(p.destroyed);
        w.u8(static_cast<uint8_t>(p.corruption)).i32(p.corrupting_player);
        w.boolean(p.corruption_protected).boolean(p.corruption_removing);
        w.u32(p.corruption_start).u32(p.corruption_end).u32(p.corruption_changed);
        w.u8(static_cast<uint8_t>(p.corruption_pending)).i32(p.corruption_pending_player);
        w.boolean(p.corruption_particles);
        for (float v : p.corruption_pairs) w.f32(v);
        w.boolean(p.revealed).i32(p.reveal_delay);
        w.u32(static_cast<uint32_t>(p.visibility.size()));
        for (const std::vector<VisibilityModifier>& l : p.visibility) {
            w.u32(static_cast<uint32_t>(l.size()));
            for (const VisibilityModifier& m : l) {
                w.u32(m.value).u32(m.expiry).u32(m.name_hash);
                w.i32(m.key.object).u32(m.key.serial).u32(m.key.sub).u32(m.key.extra);
            }
        }
        w.f32(p.piracy_pool).i32(p.siphon_player).f32(p.siphon_fraction).i32(p.siphon_until).f32(p.siphon_amount);
        w.i32(p.halt_until).boolean(p.min_income_set).boolean(p.max_income_set).f32(p.min_income).f32(p.max_income);
        w.f32(p.build_time_bonus).f32(p.build_cost_bonus).boolean(p.hidden).boolean(p.in_battle);
        w.u32(static_cast<uint32_t>(p.income.size()));
        for (const IncomeEntry& e : p.income) {
            w.u8(e.kind).f32(e.value).i32(e.expiry);
            w.u32(e.ability).i32(e.source).u32(e.tag).i32(e.next_eval).u32(e.def);
        }
        for (const std::vector<BuildEntry>& q : p.queue) {
            w.u32(static_cast<uint32_t>(q.size()));
            for (const BuildEntry& e : q) {
                w.u32(e.type).f32(e.build_time).i32(e.start).i32(e.duration).f32(e.remaining).i32(e.player).f32(e.cost);
            }
        }
        w.i32(p.starbase_level).i32(p.starbase_owner);
        put_handle(w, st, p.starbase);
        put_handles(w, st, p.structures);
        put_handles(w, st, p.surface);
        w.u32(static_cast<uint32_t>(p.influence.size()));
        for (int32_t id : p.influence) w.i32(id);
        put_u32s(w, p.orbit);
        w.i32(p.sabotage_count);
        w.u32(static_cast<uint32_t>(p.respawns.size()));
        for (const RespawnEntry& e : p.respawns) w.i32(e.due).u32(e.type).i32(e.owner).u8(e.flag);
        w.i32(p.queue_phase[0]).i32(p.queue_phase[1]);
    }

    w.u32(static_cast<uint32_t>(fleets_.size()));
    for (const FleetState& f : fleets_) {
        w.boolean(f.alive);
        put_handle(w, st, f.object);
        w.i32(f.owner);
        w.u32(static_cast<uint32_t>(f.contents.size()));
        for (size_t i = 0; i < f.contents.size(); ++i) {
            put_handle(w, st, f.contents[i]);
            w.u8(i < f.carried.size() ? f.carried[i] : 0);
        }
        w.u32(f.planet).u32(f.from).u32(f.to);
        put_u32s(w, f.path);
        w.u32(f.path_index).f32(f.base_speed).f32(f.hop_speed);
        put_vec3(w, f.position);
        put_vec3(w, f.facing);
        w.boolean(f.moving);
        w.u8(static_cast<uint8_t>(f.slot)).u32(f.want_planet).u8(static_cast<uint8_t>(f.want_slot));
    }

    w.u32(static_cast<uint32_t>(stealthed_.size()));
    for (int32_t id : stealthed_) w.i32(id);
    w.u32(static_cast<uint32_t>(claims_.size()));
    for (const VictoryClaim& c : claims_) w.i32(c.player).u8(c.type).i32(c.frame).i32(c.countdown);
    w.i32(victory_hold_);
    w.u32(static_cast<uint32_t>(events_.size()));
    for (const GalacticEvent& e : events_) {
        w.i32(e.frame).u8(static_cast<uint8_t>(e.kind)).i32(e.player).i32(e.other_player);
        w.u32(e.planet).u32(e.other_planet).u32(e.fleet).u32(e.type).i32(e.object).i32(e.value);
    }
}

bool GalacticSim::read(ByteReader& r) {
    const objects::ObjectStore& st = *store_;
    const uint32_t version = r.u32();
    if (version < 1 || version > kGalacticSaveVersion || !r.ok()) return false;
    const uint32_t rows = r.u32(), routes = r.u32(), types = r.u32(), np = r.u32();
    if (!r.ok() || rows != planets_.size() || routes != map_.routes.size() || types != rules_->types.size() ||
        np != players_.size())
        return false;
    const int32_t P = static_cast<int32_t>(np);
    const auto player_ok = [P](int32_t p) { return p >= kNoPlayer && p < P; };
    const auto planet_ok = [rows](PlanetIndex p) { return p == kInvalidPlanet || p < rows; };
    const auto types_ok = [types](const std::vector<uint32_t>& v) {
        for (uint32_t t : v)
            if (t >= types) return false;
        return true;
    };

    const int32_t frame = r.i32(), pause = r.i32(), last_cycle = r.i32(), next_cycle = r.i32(), cycle = r.i32();
    const bool linked = r.boolean();
    const int32_t neutral = r.i32(), local = r.i32();
    const bool auto_reveal = r.boolean(), reveal_sfx = r.boolean(), build_locks = r.boolean(), emit = r.boolean(),
               slot_choices = r.boolean();
    if (!r.ok() || pause < 0 || !player_ok(neutral) || !player_ok(local)) return false;

    std::vector<PlayerState> players(np);
    std::vector<float> last_income(np, 0.0f);
    for (uint32_t i = 0; i < np; ++i) {
        PlayerState& p = players[i];
        p.faction = r.u32();
        p.human = r.boolean();
        p.ai = r.boolean();
        p.difficulty = r.u32();
        p.account.credits = r.f32();
        p.account.cap = r.f32();
        p.tech_level = r.i32();
        p.tech_cost_mult = r.f32();
        p.cheat = r.boolean();
        if (!get_u32s(r, p.bm_purchased)) return false;
        p.bm_tutorial = r.boolean();
        p.sabotage_tutorial = r.boolean();
        for (int32_t& c : p.corruptions_completed) c = r.i32();
        if (!get_u32s(r, p.locked_types) || !get_u32s(r, p.unlocked_types) || !get_u32s(r, p.production_disabled)) return false;
        if (!types_ok(p.locked_types) || !types_ok(p.unlocked_types) || !types_ok(p.production_disabled)) return false;
        p.max_tech_level = r.i32();
        uint32_t nb = 0;
        if (!get_count(r, nb, 8)) return false;
        p.built.resize(nb);
        for (auto& [type, count] : p.built) {
            type = r.u32();
            count = r.i32();
            if (type >= types) return false;
        }
        last_income[i] = r.f32();
        if (!r.ok()) return false;
    }
    uint32_t nrel = 0;
    if (!get_count(r, nrel, 1) || (nrel != 0 && nrel != np * np)) return false;
    std::vector<uint8_t> relations(nrel);
    for (uint8_t& rel : relations) {
        rel = r.u8();
        if (rel > static_cast<uint8_t>(Relation::Enemy)) return false;
    }

    std::vector<PlanetState> planets = planets_;  // keeps the definition-derived position / links of start()
    for (uint32_t i = 0; i < rows; ++i) {
        PlanetState& p = planets[i];
        const bool is_planet = r.boolean();
        const uint32_t type = r.u32();
        if (!r.ok() || is_planet != p.is_planet || type != p.type) return false;  // not the session's galaxy
        if (!get_handle(r, st, p.object)) return false;
        p.initialised = r.boolean();
        p.owner = r.i32();
        p.control = r.f32();
        p.target_control = r.f32();
        p.leaning = r.i32();
        p.base_value = r.i32();
        p.black_market = r.f32();
        p.black_market_influences = r.boolean();
        p.destroyed = r.boolean();
        p.corruption = static_cast<int8_t>(r.u8());
        p.corrupting_player = r.i32();
        p.corruption_protected = r.boolean();
        p.corruption_removing = r.boolean();
        p.corruption_start = r.u32();
        p.corruption_end = r.u32();
        p.corruption_changed = r.u32();
        p.corruption_pending = static_cast<int8_t>(r.u8());
        p.corruption_pending_player = r.i32();
        p.corruption_particles = r.boolean();
        for (float& v : p.corruption_pairs) v = r.f32();
        p.revealed = r.boolean();
        p.reveal_delay = r.i32();
        uint32_t nlists = 0;
        if (!get_count(r, nlists, 4) || nlists > np) return false;
        p.visibility.assign(nlists, {});
        for (std::vector<VisibilityModifier>& l : p.visibility) {
            uint32_t nm = 0;
            if (!get_count(r, nm, 28)) return false;
            l.resize(nm);
            for (VisibilityModifier& m : l) {
                m.value = r.u32();
                m.expiry = r.u32();
                m.name_hash = r.u32();
                m.key.object = r.i32();
                m.key.serial = r.u32();
                m.key.sub = r.u32();
                m.key.extra = r.u32();
            }
        }
        p.piracy_pool = r.f32();
        p.siphon_player = r.i32();
        p.siphon_fraction = r.f32();
        p.siphon_until = r.i32();
        p.siphon_amount = r.f32();
        p.halt_until = r.i32();
        p.min_income_set = r.boolean();
        p.max_income_set = r.boolean();
        p.min_income = r.f32();
        p.max_income = r.f32();
        p.build_time_bonus = r.f32();
        p.build_cost_bonus = r.f32();
        p.hidden = r.boolean();
        p.in_battle = r.boolean();
        if (!r.ok()) return false;
        if (!player_ok(p.owner) || !player_ok(p.leaning) || !player_ok(p.corrupting_player) ||
            !player_ok(p.corruption_pending_player) || !player_ok(p.siphon_player))
            return false;
        uint32_t ni = 0;
        if (!get_count(r, ni, 29)) return false;
        p.income.resize(ni);
        for (IncomeEntry& e : p.income) {
            e.kind = r.u8();
            e.value = r.f32();
            e.expiry = r.i32();
            e.ability = r.u32();
            e.source = r.i32();
            e.tag = r.u32();
            e.next_eval = r.i32();
            e.def = r.u32();
            if (e.def != 0xFFFFFFFFu && e.def >= rules_->special.abilities.size()) return false;
        }
        for (std::vector<BuildEntry>& q : p.queue) {
            uint32_t nq = 0;
            if (!get_count(r, nq, 28)) return false;
            q.resize(nq);
            for (BuildEntry& e : q) {
                e.type = r.u32();
                e.build_time = r.f32();
                e.start = r.i32();
                e.duration = r.i32();
                e.remaining = r.f32();
                e.player = r.i32();
                e.cost = r.f32();
                if (!r.ok() || e.type >= types || e.player < 0 || e.player >= P) return false;
            }
        }
        p.starbase_level = r.i32();
        p.starbase_owner = r.i32();
        if (!r.ok() || !player_ok(p.starbase_owner)) return false;
        if (!get_handle(r, st, p.starbase) || !get_handles(r, st, p.structures) || !get_handles(r, st, p.surface)) return false;
        uint32_t ninf = 0;
        if (!get_count(r, ninf, 4)) return false;
        p.influence.resize(ninf);
        for (int32_t& id : p.influence) id = r.i32();
        if (!get_u32s(r, p.orbit)) return false;  // fleet indices: checked once the fleets are read
        p.sabotage_count = r.i32();
        uint32_t nr = 0;
        if (!get_count(r, nr, 13)) return false;
        p.respawns.resize(nr);
        for (RespawnEntry& e : p.respawns) {
            e.due = r.i32();
            e.type = r.u32();
            e.owner = r.i32();
            e.flag = r.u8();
            if (!r.ok() || e.type >= types || !player_ok(e.owner)) return false;
        }
        if (version >= 3) {  // v1 / v2 keep start()'s draws (the AI save holds the live last-run frames)
            for (int32_t& phase : p.queue_phase) phase = r.i32();
            for (const int32_t phase : p.queue_phase)
                if (p.is_planet ? phase < 1 || phase > econ::kFramesPerSecond : phase != 0) return false;
        }
        if (!r.ok()) return false;
    }

    uint32_t nf = 0;
    if (!get_count(r, nf, 1)) return false;
    std::vector<FleetState> fleets(nf);
    for (FleetState& f : fleets) {
        f.alive = r.boolean();
        if (!get_handle(r, st, f.object)) return false;
        f.owner = r.i32();
        uint32_t nc = 0;
        if (!r.ok() || !player_ok(f.owner) || !get_count(r, nc, 5)) return false;
        f.contents.assign(nc, ObjectHandle{});
        f.carried.assign(nc, 0);
        for (uint32_t i = 0; i < nc; ++i) {
            if (!get_handle(r, st, f.contents[i])) return false;
            f.carried[i] = r.u8();
        }
        f.planet = r.u32();
        f.from = r.u32();
        f.to = r.u32();
        if (!get_u32s(r, f.path)) return false;
        f.path_index = r.u32();
        f.base_speed = r.f32();
        f.hop_speed = r.f32();
        f.position = get_vec3(r);
        f.facing = get_vec3(r);
        f.moving = r.boolean();
        f.slot = static_cast<int8_t>(r.u8());
        f.want_planet = r.u32();
        f.want_slot = static_cast<int8_t>(r.u8());
        if (!r.ok() || !planet_ok(f.planet) || !planet_ok(f.from) || !planet_ok(f.to) || !planet_ok(f.want_planet) ||
            f.path_index > f.path.size() || f.slot < -1 || f.slot >= kOrbitSlots || f.want_slot < -1 || f.want_slot >= kOrbitSlots)
            return false;
        for (PlanetIndex hop : f.path)
            if (hop >= rows) return false;
        if (f.alive && f.owner == kNoPlayer) return false;
    }
    for (const PlanetState& p : planets)
        for (uint32_t fi : p.orbit)
            if (fi >= nf) return false;

    uint32_t ns = 0;
    if (!get_count(r, ns, 4)) return false;
    std::vector<int32_t> stealthed(ns);
    for (int32_t& id : stealthed) id = r.i32();
    uint32_t ncl = 0;
    if (!get_count(r, ncl, 13)) return false;
    std::vector<VictoryClaim> claims(ncl);
    for (VictoryClaim& c : claims) {
        c.player = r.i32();
        c.type = r.u8();
        c.frame = r.i32();
        c.countdown = r.i32();
        if (!r.ok() || !player_ok(c.player)) return false;
    }
    const int32_t hold = version >= 2 ? r.i32() : 0;
    if (hold < 0) return false;
    uint32_t ne = 0;
    if (!get_count(r, ne, 37)) return false;
    std::vector<GalacticEvent> events(ne);
    for (GalacticEvent& e : events) {
        e.frame = r.i32();
        const uint8_t kind = r.u8();
        if (kind >= static_cast<uint8_t>(GalacticEventKind::Count)) return false;
        e.kind = static_cast<GalacticEventKind>(kind);
        e.player = r.i32();
        e.other_player = r.i32();
        e.planet = r.u32();
        e.other_planet = r.u32();
        e.fleet = r.u32();
        e.type = r.u32();
        e.object = r.i32();
        e.value = r.i32();
    }
    if (!r.ok() || !r.done()) return false;

    // ---- commit ----
    frame_ = frame;
    pause_ = pause;
    last_cycle_frame_ = last_cycle;
    next_cycle_frame_ = next_cycle;
    cycle_counter_ = cycle;
    linked_battle_pending_ = linked;
    neutral_player_ = neutral;
    local_player_ = local;
    auto_reveal_ = auto_reveal;
    reveal_sfx_ = reveal_sfx;
    initial_build_locks_ = build_locks;
    emit_events_ = emit;
    slot_choices_ = slot_choices;
    players_ = std::move(players);
    last_income_ = std::move(last_income);
    relations_ = std::move(relations);
    planets_ = std::move(planets);
    fleets_ = std::move(fleets);
    stealthed_ = std::move(stealthed);
    claims_ = std::move(claims);
    victory_hold_ = hold;
    events_ = std::move(events);
    // Rebuilt caches: store slot -> fleet / planet (creation order, so a reused slot maps to the newest fleet as
    // create_fleet left it), the per-tick owner derivation scratch, the spawn payload latch.
    fleet_by_slot_.clear();
    for (uint32_t i = 0; i < fleets_.size(); ++i) {
        const ObjectHandle h = fleets_[i].object;
        if (!h) continue;
        if (fleet_by_slot_.size() <= h.slot()) fleet_by_slot_.resize(static_cast<size_t>(h.slot()) + 1, kNoFleet);
        fleet_by_slot_[h.slot()] = i;
    }
    planet_by_slot_.clear();
    for (PlanetIndex i = 0; i < planets_.size(); ++i) {
        const ObjectHandle h = planets_[i].object;
        if (!planets_[i].is_planet || !h) continue;
        if (planet_by_slot_.size() <= h.slot()) planet_by_slot_.resize(static_cast<size_t>(h.slot()) + 1, kInvalidPlanet);
        planet_by_slot_[h.slot()] = i;
    }
    derived_.assign(planets_.size(), Derived{});
    last_spawn_ = {};
    return true;
}

// ---- ConflictManager ----------------------------------------------------------------------------------------------------

namespace {

void put_info(ByteWriter& w, const objects::ObjectStore& st, const ConflictInfo& c) {
    w.u8(static_cast<uint8_t>(c.kind)).u32(c.planet).i32(c.attacker).i32(c.defender);
    put_handles(w, st, c.units);
    w.str(c.map);
}

bool get_info(ByteReader& r, const objects::ObjectStore& st, ConflictInfo& c) {
    const uint8_t kind = r.u8();
    if (kind != static_cast<uint8_t>(ConflictKind::Land) && kind != static_cast<uint8_t>(ConflictKind::Space)) return false;
    c.kind = static_cast<ConflictKind>(kind);
    c.planet = r.u32();
    c.attacker = r.i32();
    c.defender = r.i32();
    return r.ok() && get_handles(r, st, c.units) && get_str(r, c.map);
}

void put_f64s(ByteWriter& w, const std::vector<double>& v) {
    w.u32(static_cast<uint32_t>(v.size()));
    for (double x : v) w.f64(x);
}

bool get_f64s(ByteReader& r, std::vector<double>& v) {
    uint32_t n = 0;
    if (!get_count(r, n, 8)) return false;
    v.assign(n, 0.0);
    for (double& x : v) x = r.f64();
    return r.ok();
}

void put_u8s(ByteWriter& w, const std::vector<uint8_t>& v) {
    w.u32(static_cast<uint32_t>(v.size()));
    for (uint8_t x : v) w.u8(x);
}

bool get_u8s(ByteReader& r, std::vector<uint8_t>& v) {
    uint32_t n = 0;
    if (!get_count(r, n, 1)) return false;
    v.assign(n, 0);
    for (uint8_t& x : v) x = r.u8();
    return r.ok();
}

bool valid_state(int8_t s) {
    switch (static_cast<ConflictState>(s)) {
        case ConflictState::Idle:
        case ConflictState::Vote:
        case ConflictState::StartBattle:
        case ConflictState::Decide:
        case ConflictState::Dialog:
        case ConflictState::AutoSetup:
        case ConflictState::AutoRunning:
        case ConflictState::InBattle: return true;
    }
    return false;
}

}  // namespace

void ConflictManager::write(ByteWriter& w) const {
    const objects::ObjectStore& st = sim_->store();
    w.u32(kConflictSaveVersion);
    w.u32(static_cast<uint32_t>(sim_->planets().size()));
    for (const std::vector<ConflictInfo>* q : {&space_q_, &land_q_}) {
        w.u32(static_cast<uint32_t>(q->size()));
        for (const ConflictInfo& c : *q) put_info(w, st, c);
    }
    w.u8(static_cast<uint8_t>(state_)).u8(static_cast<uint8_t>(active_)).u64(tick_).u64(timer_start_);
    w.boolean(force_).i32(retreating_).i32(pause_held_).boolean(has_choice_).u8(static_cast<uint8_t>(choice_));

    // Auto-resolve in flight.
    w.boolean(ar_in_.space).i32(ar_in_.attacker).i32(ar_in_.retreating).boolean(ar_in_.from_tactical);
    w.u32(static_cast<uint32_t>(ar_in_.combatants.size()));
    for (const ArCombatant& c : ar_in_.combatants) {
        w.i32(c.owner);
        w.u32(static_cast<uint32_t>(c.pieces.size()));
        for (const ArPiece& p : c.pieces) w.u32(p.type).u64(p.categories).f32(p.power);
        w.f32(c.own_power).boolean(c.hero).boolean(c.always_dies).boolean(c.super).boolean(c.super_killer);
        w.u32(static_cast<uint32_t>(c.starbase.size()));
        for (const ArStarbaseLevel& l : c.starbase) w.u32(l.type).f32(l.power);
    }
    w.u32(static_cast<uint32_t>(ar_in_.players.size()));
    for (const ArSideInfo& s : ar_in_.players) w.i32(s.player).boolean(s.ai).boolean(s.playable);
    w.u32(static_cast<uint32_t>(ar_in_.cargo.size()));
    for (const AutoResolveInput::Cargo& c : ar_in_.cargo) w.i32(c.owner).boolean(c.hero);
    put_handles(w, st, ar_objects_);
    put_handles(w, st, ar_cargo_);

    // Last result.
    const AutoResolveResult& a = last_ar_;
    w.boolean(a.valid).boolean(a.degenerate).i32(a.winner).i32(a.loser).i32(a.retreating);
    put_u8s(w, a.survives);
    put_u32s(w, a.starbase_type);
    put_u8s(w, a.cargo_survives);
    for (int s = 0; s < 2; ++s) put_f64s(w, a.vec[s]);
    for (int s = 0; s < 2; ++s) put_f64s(w, a.rem[s]);
    for (int s = 0; s < 2; ++s) w.f64(a.strength[s]).f64(a.budget[s]).i32(a.side_player[s]).u32(a.losses[s]);
    w.u32(a.draws);

    // Active battle request.
    put_info(w, st, request_.info);
    w.str(request_.map).i32(request_.base_level);
    w.u32(static_cast<uint32_t>(request_.forces.size()));
    for (const BattleForces& f : request_.forces) {
        w.i32(f.player);
        put_handles(w, st, f.start);
        put_handles(w, st, f.reserve);
    }
    put_handle(w, st, request_.starbase);
    put_handles(w, st, request_.structures);
    w.boolean(request_.land_base_destruction_forces_retreat).boolean(request_.space_station_destruction_forces_retreat);
    w.boolean(request_.linked);

    w.u32(static_cast<uint32_t>(planet_map_.size()));
    for (const std::string& m : planet_map_) w.str(m);
    w.u32(static_cast<uint32_t>(log_.size()));
    for (const ConflictLogEntry& e : log_) {
        w.u64(e.tick).i32(e.galactic_frame).u8(static_cast<uint8_t>(e.kind)).u32(e.planet).i32(e.attacker).i32(e.defender);
        w.u8(e.how).i32(e.winner).i32(e.loser).i32(e.retreating).u32(e.attacker_losses).u32(e.defender_losses).u32(e.rng_draws);
        w.str(e.map);
    }
}

bool ConflictManager::read(ByteReader& r) {
    const objects::ObjectStore& st = sim_->store();
    const uint32_t version = r.u32();
    if (version < 1 || version > kConflictSaveVersion || !r.ok()) return false;
    const uint32_t rows = r.u32();
    if (!r.ok() || rows != sim_->planets().size()) return false;
    const int32_t P = static_cast<int32_t>(sim_->players().size());
    const auto player_ok = [P](int32_t p) { return p >= kNoPlayer && p < P; };

    std::vector<ConflictInfo> qs[2];
    for (std::vector<ConflictInfo>& q : qs) {
        uint32_t n = 0;
        if (!get_count(r, n, 21)) return false;
        q.resize(n);
        for (ConflictInfo& c : q)
            if (!get_info(r, st, c) || c.planet >= rows || !player_ok(c.attacker) || !player_ok(c.defender)) return false;
    }
    const int8_t state = static_cast<int8_t>(r.u8());
    const uint8_t active = r.u8();
    const uint64_t tick = r.u64(), timer_start = r.u64();
    const bool force = r.boolean();
    const int32_t retreating = r.i32(), pause_held = r.i32();
    const bool has_choice = r.boolean();
    const uint8_t choice = r.u8();
    if (!r.ok() || !valid_state(state) ||
        (active != static_cast<uint8_t>(ConflictKind::Land) && active != static_cast<uint8_t>(ConflictKind::Space)) ||
        !player_ok(retreating) || pause_held < 0 || choice > static_cast<uint8_t>(BattleChoice::Retreat))
        return false;

    AutoResolveInput in;
    in.space = r.boolean();
    in.attacker = r.i32();
    in.retreating = r.i32();
    in.from_tactical = r.boolean();
    uint32_t nc = 0;
    if (!r.ok() || !get_count(r, nc, 17)) return false;
    in.combatants.resize(nc);
    const auto& types = sim_->rules().types;
    for (ArCombatant& c : in.combatants) {
        c.owner = r.i32();
        uint32_t np = 0;
        if (!get_count(r, np, 16)) return false;
        c.pieces.resize(np);
        for (ArPiece& p : c.pieces) {
            p.type = r.u32();
            p.categories = r.u64();
            p.power = r.f32();
            p.name = p.type < types.size() ? std::string_view(types[p.type].name) : std::string_view();  // as combatant_of
        }
        c.own_power = r.f32();
        c.hero = r.boolean();
        c.always_dies = r.boolean();
        c.super = r.boolean();
        c.super_killer = r.boolean();
        uint32_t nsb = 0;
        if (!get_count(r, nsb, 8)) return false;
        c.starbase.resize(nsb);
        for (ArStarbaseLevel& l : c.starbase) {
            l.type = r.u32();
            l.power = r.f32();
        }
        if (!r.ok()) return false;
    }
    uint32_t nsides = 0;
    if (!get_count(r, nsides, 6)) return false;
    in.players.resize(nsides);
    for (ArSideInfo& s : in.players) {
        s.player = r.i32();
        s.ai = r.boolean();
        s.playable = r.boolean();
    }
    uint32_t ncargo = 0;
    if (!get_count(r, ncargo, 5)) return false;
    in.cargo.resize(ncargo);
    for (AutoResolveInput::Cargo& c : in.cargo) {
        c.owner = r.i32();
        c.hero = r.boolean();
    }
    std::vector<ObjectHandle> ar_objects, ar_cargo;
    if (!r.ok() || !get_handles(r, st, ar_objects) || !get_handles(r, st, ar_cargo)) return false;
    // auto_finish walks the object lists in parallel with the combatant / cargo lists.
    if (ar_objects.size() != in.combatants.size() || ar_cargo.size() != in.cargo.size()) return false;

    AutoResolveResult a;
    a.valid = r.boolean();
    a.degenerate = r.boolean();
    a.winner = r.i32();
    a.loser = r.i32();
    a.retreating = r.i32();
    if (!r.ok() || !get_u8s(r, a.survives) || !get_u32s(r, a.starbase_type) || !get_u8s(r, a.cargo_survives)) return false;
    for (int s = 0; s < 2; ++s)
        if (!get_f64s(r, a.vec[s])) return false;
    for (int s = 0; s < 2; ++s)
        if (!get_f64s(r, a.rem[s])) return false;
    for (int s = 0; s < 2; ++s) {
        a.strength[s] = r.f64();
        a.budget[s] = r.f64();
        a.side_player[s] = r.i32();
        a.losses[s] = r.u32();
    }
    a.draws = r.u32();
    if (!r.ok()) return false;

    BattleRequest req;
    if (!get_info(r, st, req.info) || !get_str(r, req.map)) return false;
    req.base_level = r.i32();
    uint32_t nforces = 0;
    if (!r.ok() || !get_count(r, nforces, 12)) return false;
    req.forces.resize(nforces);
    for (BattleForces& f : req.forces) {
        f.player = r.i32();
        if (!r.ok() || !get_handles(r, st, f.start) || !get_handles(r, st, f.reserve)) return false;
    }
    if (!get_handle(r, st, req.starbase) || !get_handles(r, st, req.structures)) return false;
    req.land_base_destruction_forces_retreat = r.boolean();
    req.space_station_destruction_forces_retreat = r.boolean();
    if (version >= 2) req.linked = r.boolean();

    uint32_t nmaps = 0;
    if (!r.ok() || !get_count(r, nmaps, 4)) return false;
    std::vector<std::string> maps(nmaps);
    for (std::string& m : maps)
        if (!get_str(r, m)) return false;
    uint32_t nlog = 0;
    if (!get_count(r, nlog, 54)) return false;
    std::vector<ConflictLogEntry> log(nlog);
    for (ConflictLogEntry& e : log) {
        e.tick = r.u64();
        e.galactic_frame = r.i32();
        const uint8_t kind = r.u8();
        if (kind != static_cast<uint8_t>(ConflictKind::Land) && kind != static_cast<uint8_t>(ConflictKind::Space)) return false;
        e.kind = static_cast<ConflictKind>(kind);
        e.planet = r.u32();
        e.attacker = r.i32();
        e.defender = r.i32();
        e.how = r.u8();
        e.winner = r.i32();
        e.loser = r.i32();
        e.retreating = r.i32();
        e.attacker_losses = r.u32();
        e.defender_losses = r.u32();
        e.rng_draws = r.u32();
        if (!get_str(r, e.map)) return false;
    }
    if (!r.ok() || !r.done()) return false;

    // ---- commit ----
    space_q_ = std::move(qs[0]);
    land_q_ = std::move(qs[1]);
    state_ = static_cast<ConflictState>(state);
    active_ = static_cast<ConflictKind>(active);
    tick_ = tick;
    timer_start_ = timer_start;
    force_ = force;
    retreating_ = retreating;
    pause_held_ = pause_held;
    has_choice_ = has_choice;
    choice_ = static_cast<BattleChoice>(choice);
    ar_in_ = std::move(in);
    ar_objects_ = std::move(ar_objects);
    ar_cargo_ = std::move(ar_cargo);
    last_ar_ = std::move(a);
    request_ = std::move(req);
    planet_map_ = std::move(maps);
    log_ = std::move(log);
    return true;
}

}  // namespace alamo::galactic
