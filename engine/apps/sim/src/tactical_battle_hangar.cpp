// Battle host side of the hangars, the skirmish station upgrade and the engine-loop requests
// (docs/re/STATION_HANGARS_SOUND.md; engine/tactical/hangar: HangarSystem, EngineLoops, complete_station_upgrade).
//
//  * Setup: every battle type with SPAWN_SQUADRON gets a HangarDesc (Starting / Reserve_Spawned_Units_Tech_0..5,
//    Spawned_Squadron_Delay_Seconds, Is_Community_Property, station = DUMMY_STAR_BASE [port], DEPLOY_SQUAD through the
//    AbilitySystem); the spawn-list types, their members / team types and Next_Level_Base join the battle tables.
//  * Spawner creation: add_carrier(id, desc, frame, &rng) right after the unit's add_unit (attach order).
//  * Each tick after the combat tick and its events: HangarSystem::update, then the launch requests in order
//    (squadron: members at the bay origin with its facing, then the team container, spawned flag +0x3a2 |= 2; single
//    ship: rings r = 0 / 200 / 400 / 600 / 800 at 45 degree steps around the flyout point), bind_launch, the initial
//    order (S7 t24, escort: a guard of the carrier - a move to the carrier's position with the guard flag 0x02 and the
//    squadron's formation record {flags 0x02, goal object = the carrier}, so its auto targets are chased within
//    Guard_Chase_Range of the carrier, followed by the movement layer (S7 t37: object goal block); else a move to the flyout point), then
//    clear_requests().
//  * Removal: squadron container / single ship gone -> squadron_removed; carrier gone -> remove_carrier.
//  * Station upgrade (SpawnPurpose::StationUpgrade): spawn Next_Level_Base for S's owner at S, complete_station_upgrade,
//    tech +1 for the owner then each ally (battle-local [port]), S removed silently, the upgrade recorded for the host
//    (selection / control groups, announcement).
//  * Engine loops: mode_enter at creation, locomotor_service every SimpleSpace service (not fighters / squadrons),
//    engines offline / online from the ability events, remove on death / removal; the tick's requests are published
//    for the audio host (engine_loop_requests()).
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <vector>

#include "alamo/core/dmath.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/objects/behaviors.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/sim/tactical_battle.hpp"
#include "alamo/tactical/movement/special_locomotors.hpp"
#include "alamo/tactical/movement/unit_params.hpp"

namespace alamo::sim {

namespace tc = alamo::tactical::combat;
namespace ta = alamo::tactical::abilities;
namespace mv = alamo::tactical::movement;
namespace th = alamo::tactical::hangar;

namespace {
Vec3 hg_sv(tactical::Vec3 v) { return Vec3{v.x, v.y, v.z}; }
std::string_view hg_trim(std::string_view s) {
    auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!s.empty() && ws(s.front())) s.remove_prefix(1);
    while (!s.empty() && ws(s.back())) s.remove_suffix(1);
    return s;
}
objects::BehaviorTypeId spawn_squadron_behavior() {
    static const objects::BehaviorTypeId id = objects::behavior_type_id("SPAWN_SQUADRON");
    return id;
}
objects::BehaviorTypeId star_base_behavior() {
    static const objects::BehaviorTypeId id = objects::behavior_type_id("DUMMY_STAR_BASE");
    return id;
}
objects::BehaviorTypeId dummy_upgrade_behavior() {
    static const objects::BehaviorTypeId id = objects::behavior_type_id("DUMMY_UPGRADE");
    return id;
}
bool hg_same(std::string_view a, std::string_view b) {
    a = hg_trim(a);
    b = hg_trim(b);
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const char x = (a[i] >= 'a' && a[i] <= 'z') ? static_cast<char>(a[i] - 32) : a[i];
        const char y = (b[i] >= 'a' && b[i] <= 'z') ? static_cast<char>(b[i] - 32) : b[i];
        if (x != y) return false;
    }
    return true;
}
constexpr const char* kBuildList = "Tactical_Buildable_Objects_Multiplayer";
}  // namespace

bool TacticalBattle::hg_is_spawner(uint32_t type) const {
    const objects::GameObjectTypes& types = catalog_->types();
    return spawn_squadron_behavior() != objects::kNoBehavior && type < types.size() &&
           types.has_behavior(static_cast<objects::TypeIndex>(type), objects::BehaviorSet::Union, spawn_squadron_behavior());
}

void TacticalBattle::hg_add_types() {
    // The types a spawner can launch (every tech slot), their Squadron_Units / Company_Units members and team types, and
    // the station chain (Next_Level_Base) join the battle tables before combat exists. Transitive, at most three rebuilds.
    if (!tables_) return;
    const objects::GameObjectTypes& types = catalog_->types();
    for (int pass = 0; pass < 3; ++pass) {
        const size_t before = types_used_.size();
        auto use = [&](objects::TypeIndex t) {
            if (t == objects::kNone || t >= types.size()) return;
            const auto ut = static_cast<uint32_t>(t);
            if (std::find(types_used_.begin(), types_used_.end(), ut) == types_used_.end()) types_used_.push_back(ut);
        };
        // A launchable / buildable type with its Squadron_Units / Company_Units members and team types.
        auto use_spawnable = [&](int32_t x) {
            if (x < 0) return;
            use(static_cast<objects::TypeIndex>(x));
            std::vector<int32_t> members;
            if (oi_company_members(x, members) && !members.empty()) {
                for (const int32_t m : members) use(static_cast<objects::TypeIndex>(m));
                uint32_t ot = 0xFFFFFFFFu, bt = 0xFFFFFFFFu, squad = 0;
                if (bt_team(static_cast<uint32_t>(x), static_cast<uint32_t>(members.front()), ot, bt, squad)) {
                    use(static_cast<objects::TypeIndex>(ot));
                    use(static_cast<objects::TypeIndex>(bt));
                }
            }
        };
        const std::vector<uint32_t> now = types_used_;
        for (const uint32_t t : now) {
            const auto ti = static_cast<objects::TypeIndex>(t);
            // Skirmish (space; skai t03: land too): the tactical production lists (every faction row) join the tables.
            if (cfg_.skirmish)
                for (const std::string_view tok : types.get_list(ti, kBuildList)) {
                    const objects::TypeIndex x = types.resolve_name(hg_trim(tok));
                    if (x != objects::kNone) use_spawnable(static_cast<int32_t>(x));
                }
            if (hg_is_spawner(t))
                for (const char* kind : {"Starting_Spawned_Units_Tech_", "Reserve_Spawned_Units_Tech_"})
                    for (uint32_t k = 0; k < th::kTechSlots; ++k) {
                        const std::string tag = std::string(kind) + std::to_string(k);
                        const th::SpawnList list = th::parse_spawn_list(types.get_list(ti, tag), [&types](std::string_view n) {
                            const objects::TypeIndex x = types.resolve_name(n);
                            return x == objects::kNone ? -1 : static_cast<int32_t>(x);
                        });
                        for (const th::SpawnEntry& e : list) use_spawnable(e.type);
                    }
            const std::string_view next = hg_trim(types.get_string(ti, "Next_Level_Base"));
            if (!next.empty()) use(types.resolve_name(next));
        }
        if (types_used_.size() == before) break;
        tables_extend();
    }
}

void TacticalBattle::hg_setup(TacticalMode& mode) {
    (void)mode;
    const objects::GameObjectTypes& types = catalog_->types();
    engine_loops_.set_space_idle_movement_speed(catalog_->space_idle_movement_speed());
    hg_tech_bonus_.assign(bt_players_, 0);
    std::vector<th::HangarDesc> descs;
    hangar_desc_.assign(types.size(), -1);
    auto resolve = [&types](std::string_view n) {
        const objects::TypeIndex x = types.resolve_name(n);
        return x == objects::kNone ? -1 : static_cast<int32_t>(x);
    };
    for (const uint32_t t : types_used_) {
        if (!hg_is_spawner(t) || hangar_desc_[t] >= 0) continue;
        const auto ti = static_cast<objects::TypeIndex>(t);
        th::HangarDesc d;
        if (t < galaxy_->rules().types.size()) d.name = galaxy_->rules().types[t].name;
        for (uint32_t k = 0; k < th::kTechSlots; ++k) {
            d.starting[k] = th::parse_spawn_list(types.get_list(ti, "Starting_Spawned_Units_Tech_" + std::to_string(k)), resolve);
            d.reserve[k] = th::parse_spawn_list(types.get_list(ti, "Reserve_Spawned_Units_Tech_" + std::to_string(k)), resolve);
        }
        // The schema types it as an int; the value as written (default "5.0") is read as a float.
        const std::string delay(hg_trim(types.get_string(ti, "Spawned_Squadron_Delay_Seconds")));
        if (!delay.empty()) d.delay_seconds = std::strtof(delay.c_str(), nullptr);
        d.community_property = types.get_bool(ti, "Is_Community_Property");
        d.station = star_base_behavior() != objects::kNoBehavior &&
                    types.has_behavior(ti, objects::BehaviorSet::Union, star_base_behavior());
        hangar_desc_[t] = static_cast<int32_t>(descs.size());
        descs.push_back(std::move(d));
    }
    th::HangarConfig hc;
    hc.skirmish = cfg_.skirmish;
    hangars_ = std::make_unique<th::HangarSystem>(*combat_, std::move(descs), hc);
    hangars_->set_abilities(abilities_.get());
    th::HangarHost h;
    h.tech_level = [this](int32_t p) { return hg_tech(p); };
    h.in_hyperspace = [this](tc::ObjectId id) { return l1_jumps_.departing(static_cast<objects::ObjectId>(id)); };
    // [port] unset: cinematic camera (no tactical cinematics gate yet), corruption / planet restrictions, the skirmish
    // pending-delivery list, bay bones (the hangar's fallback: the hardpoint attachment offset with the carrier facing).
    hangars_->set_host(std::move(h));
    contents_ = std::make_unique<th::ContentsSystem>();
    production_ = std::make_unique<th::TacticalProduction>();
}

int32_t TacticalBattle::hg_tech(int32_t player) const {
    int32_t t = 0;
    if (player >= 0 && static_cast<size_t>(player) < galaxy_->players().size())
        t = static_cast<int32_t>(galaxy_->players()[static_cast<size_t>(player)].tech_level);
    if (player >= 0 && static_cast<size_t>(player) < hg_tech_bonus_.size()) t += hg_tech_bonus_[static_cast<size_t>(player)];
    return t;
}

void TacticalBattle::hg_ability_host(tactical::abilities::AbilityHost& h) {
    h.squadron_deploy_ready = [this](tc::ObjectId carrier) { return hangars_ && hangars_->deploy_ready(carrier); };
    h.squadron_deploy = [this](tc::ObjectId carrier, uint32_t frame, float recharge) {
        if (hangars_) hangars_->deploy_squad(carrier, frame, recharge);
    };
    h.upgrade_container = [this](tc::ObjectId upgrade) { return contents_ ? contents_->container_of(upgrade) : tc::kNoObject; };
}

bool TacticalBattle::hg_engine_unit(const Unit& u) const {
    // SimpleSpace locomotor service (§3.3): space, movable, not a fighter / squadron member / hangar launch.
    if (!space_ || !u.movable || !movement_ || !movement_->has_unit(u.id)) return false;
    if (movement_->view(u.id).locomotor == mv::LocomotorKind::Fighter) return false;
    if ((u.flags_3a2 & 2u) != 0) return false;
    return rules_->type(parts_[u.participant].type).members.empty();
}

void TacticalBattle::hg_adopt(const Unit& u, SimRng& rng) {
    if (hg_engine_unit(u)) engine_loops_.mode_enter(u.id, frame_);
    if (!hangars_ || u.type >= hangar_desc_.size() || hangar_desc_[u.type] < 0) return;
    if (hangars_->add_carrier(u.id, static_cast<uint32_t>(hangar_desc_[u.type]), frame_, &rng)) hg_used_ = true;
}

void TacticalBattle::hg_unit_removed(objects::ObjectId id) {
    engine_loops_.remove(id);
    if (contents_) {
        // Any removal leaves its container; a container's contents go with it (removed next frame [port: the original's
        // eject / kill policy for station contents is not specified]) and its build orders are refunded.
        if (contents_->remove(id)) prod_used_ = true;
        for (const objects::ObjectId o : contents_->remove_container(id)) {
            hg_orphans_.push_back(o);
            prod_used_ = true;
        }
    }
    if (production_)
        for (const th::BuildOrder& o : production_->builder_removed(id)) {
            hg_refund(o.id, true);
            prod_used_ = true;
        }
    if (!hangars_) return;
    if (hangars_->has_carrier(id)) hangars_->remove_carrier(id);
    if (hangars_->carrier_of(id) != tc::kNoObject) hangars_->squadron_removed(id, frame_);
}

void TacticalBattle::hg_engine_service() {
    for (const Unit& u : units_) {
        if (!u.alive || u.retreated || !hg_engine_unit(u)) continue;
        engine_loops_.locomotor_service(u.id, movement_->view(u.id).speed, l1_jumps_.departing(u.id), frame_);
    }
}

objects::ObjectId TacticalBattle::hg_spawn(TacticalMode& mode, int32_t type, int32_t owner, Vec3 origin, Vec3 facing, Vec3 ring_center,
                                          bool launched, SimRng& rng, std::vector<objects::ObjectId>& ordered) {
    // A launched / produced unit: a squadron (company members at `origin` with `facing`, ids ascending, then its team
    // container) or a single ship on the first free ring spot around `ring_center`. Returns the object the hangar binds
    // (team container, else the first member / the ship); `ordered` gets the movable units for the initial order.
    objects::ObjectStore& store = mode.primary();
    objects::ObjectId bound = objects::kNoObjectId;
    std::vector<int32_t> members;
    if (oi_company_members(type, members) && !members.empty()) {
        // Squadron: the members at the bay origin with its facing (ids ascending), then the team container.
        std::vector<objects::ObjectHandle> hs;
        for (const int32_t m : members) {
            objects::SpawnParams sp;
            sp.type = static_cast<objects::TypeIndex>(m);
            sp.owner = owner;
            sp.position = origin;
            sp.facing = facing;
            const objects::ObjectHandle h = mode.spawn(sp, 1.0f);
            if (h) hs.push_back(h);
        }
        if (hs.empty()) return objects::kNoObjectId;
        uint32_t team_obj = 0xFFFFFFFFu, team_beh = 0xFFFFFFFFu, squad = 0;
        uint32_t ci = 0xFFFFFFFFu;
        if (bt_team(static_cast<uint32_t>(type), static_cast<uint32_t>(store.type(hs.front())), team_obj, team_beh, squad)) {
            objects::SpawnParams sp;
            sp.type = static_cast<objects::TypeIndex>(team_obj);
            sp.owner = owner;
            sp.position = origin;
            sp.facing = facing;
            const objects::ObjectHandle ch = store.create(sp);
            if (store.alive(ch)) {
                Container ct;
                ct.id = store.id(ch);
                ct.h = ch;
                ct.team = true;
                ct.type = team_beh;
                ct.owner = owner;
                ct.squad_type = static_cast<uint32_t>(type);  // S7 fighters: Squadron_Formation_Error_Tolerance
                ci = static_cast<uint32_t>(containers_.size());
                containers_.push_back(ct);
                ++stats_.containers;
            }
        }
        std::vector<tc::ObjectId> group;
        for (const objects::ObjectHandle h : hs) {
            const objects::ObjectId id = store.id(h);
            if (bt_adopt_built(mode, h, owner, 1.0f, &rng, false, ci) == 0xFFFFFFFFu) {
                store.destroy(h);
                continue;
            }
            if (Unit* u = unit_by_id(id)) {
                // q6 land end chain: a company without a team type (e.g. Boba_Fett_Team_Land_MP, one hero) has no
                // container, so bt_adopt_built left its members immobile (only map / team members move there). They move
                // like oi_spawn_unit's single units. q6 t19: land production no longer comes here (SKIRMISH_SETUP §3.1: it
                // goes to the pool and lands through deploy, which moves such members); [port] kept for hangar launches.
                const objects::TypeIndex mt = store.type(h);
                if (ci == 0xFFFFFFFFu && !u->movable && movement_ && mode.movable(h) &&
                    catalog_->types().has_behavior(mt, objects::BehaviorSet::Union, objects::beh::Locomotor)) {
                    const auto ut = static_cast<uint32_t>(mt);
                    mv::UnitDesc md = mv::unit_desc_from_type(catalog_->types(), ut);
                    md.locomotor = mv::locomotor_kind_from_type(catalog_->types(), ut, space_ ? mv::MoveMode::Space : mv::MoveMode::Land);
                    const Vec3 p = store.position(h), f = store.facing(h);
                    md.id = id;
                    md.pos = {p.x, p.y};
                    md.z = p.z;
                    md.yaw_deg = dmath::atan2(f.y, f.x) * (180.0f / dmath::kPiF);
                    md.owner = owner;
                    movement_->add_unit(md);
                    u->movable = true;
                }
                if (launched) u->flags_3a2 = static_cast<uint8_t>(u->flags_3a2 | 2u);  // hangar spawned (+0x3a2 |= 2)
                if (u->movable) ordered.push_back(id);
            }
            group.push_back(id);
        }
        if (ci != 0xFFFFFFFFu) {
            if (group.empty()) {
                containers_[ci].removed = true;
                store.destroy(containers_[ci].h);
            } else {
                abilities_->add_container(containers_[ci].id, desc_of(team_beh), owner, group, frame_, &rng);
                bound = containers_[ci].id;
            }
        } else if (!group.empty()) {
            bound = group.front();  // [port] no team type: the first member stands for the squadron
        }
    } else {
        // Single ship: the first free spot on rings r = 0, 200, ..., 800 around the flyout point, 45 degree steps
        // [port: "free" = no live battle unit within 100 units].
        Vec3 at = ring_center;
        bool found = false;
        for (int ring = 0; ring <= 4 && !found; ++ring) {
            const int steps = ring == 0 ? 1 : 8;
            for (int k = 0; k < steps && !found; ++k) {
                const float a = static_cast<float>(k) * (dmath::kPiF / 4.0f);
                const float rr = 200.0f * static_cast<float>(ring);
                const Vec3 p{ring_center.x + rr * dmath::cos(a), ring_center.y + rr * dmath::sin(a), ring_center.z};
                bool free = true;
                for (const Unit& u : units_) {
                    if (!u.alive || u.retreated || !store.alive(u.h)) continue;
                    const Vec3 q = store.position(u.h);
                    const float dx = q.x - p.x, dy = q.y - p.y;
                    if (dx * dx + dy * dy < 100.0f * 100.0f) {
                        free = false;
                        break;
                    }
                }
                if (free) {
                    at = p;
                    found = true;
                }
            }
        }
        const objects::ObjectId id = oi_spawn_unit(mode, type, at, facing, owner);
        if (id == objects::kNoObjectId) return objects::kNoObjectId;
        bound = id;
        if (const Unit* u = unit_by_id(id); u && u->movable) ordered.push_back(id);
    }
    return bound;
}

void TacticalBattle::hg_tick(TacticalMode& mode, SimRng& rng) {
    if (!hangars_) return;
    hangars_->update(frame_, rng);
    objects::ObjectStore& store = mode.primary();
    const std::vector<th::LaunchRequest> launches(hangars_->launches().begin(), hangars_->launches().end());
    for (const th::LaunchRequest& r : launches) {
        if (r.type < 0) continue;
        std::vector<objects::ObjectId> ordered;
        const objects::ObjectId bound = hg_spawn(mode, r.type, r.owner, hg_sv(r.origin), hg_sv(r.facing), hg_sv(r.flyout), true, rng, ordered);
        if (bound == objects::kNoObjectId) continue;
        hangars_->bind_launch(r.serial, bound);
        ++hg_launches_;
        // Initial order (STATION_HANGARS_SOUND §2.5): escort -> guard the carrier (order kind 3 with the carrier as target;
        // S7 t24: block flags 0x02, slot goal = the carrier, Guard_Chase_Range, followed by the movement layer); else (a
        // station, or the carrier already gone) a formation move to the flyout point. S7 t25 (FIGHTER_FLYING §4.10 E): a
        // single launched ship (no Squadron_Units) gets no move order.
        std::vector<int32_t> squad_members;
        if (!ordered.empty() && oi_company_members(r.type, squad_members) && !squad_members.empty()) {
            const Unit* c = r.escort ? unit_by_id(r.carrier) : nullptr;
            if (c && c->alive && !c->retreated && store.alive(c->h)) {
                const Vec3 to = store.position(c->h);
                // S7 t37: the move carries the carrier as its object goal (Coordinator::Move_To kind 3): the movement
                // layer follows it (A1_READINGS §33.4.1).
                exec_move(mode, ordered, to.x, to.y, rng, mv::kNoFacing, false, mv::kMoveFlagGuardMove, false, c->id);
                fighter_order(ordered, mv::kMoveFlagGuardMove, Vec3{to.x, to.y, 0.0f}, c->id);
            } else {
                const Vec3 to = hg_sv(r.flyout);
                exec_move(mode, ordered, to.x, to.y, rng, mv::kNoFacing, false);
            }
        }
    }
    hangars_->clear_requests();
}

void TacticalBattle::hg_station_upgrade(TacticalMode& mode, const tactical::abilities::SpawnRequest& q) {
    // STATION_HANGARS_SOUND.md §1.2 steps 2-11 (host order, ability SpawnPurpose::StationUpgrade).
    const objects::GameObjectTypes& types = catalog_->types();
    objects::ObjectStore& store = mode.primary();
    Unit* s = unit_by_id(q.source);
    if (!s || !s->alive || s->retreated || !store.alive(s->h)) return;
    const objects::TypeIndex next = types.resolve_name(hg_trim(types.get_string(static_cast<objects::TypeIndex>(s->type), "Next_Level_Base")));
    if (next == objects::kNone || next >= types.size()) return;  // top level: no upgrade
    const objects::ObjectHandle sh = s->h;
    const int32_t owner = store.owner(sh);
    const Vec3 pos = store.position(sh), facing = store.facing(sh);
    // 3. S' for S's owner at S's position / facing: its own creation draws (hardpoints, SAP init, hangar first-due).
    const objects::ObjectId sid = q.source;
    const objects::ObjectId nid = oi_spawn_unit(mode, static_cast<int32_t>(next), pos, facing, owner);
    if (nid == objects::kNoObjectId) return;
    // 4, 9, 10 (hangar part): hardpoint copy by index, spawn flag, S dropped from the hangar.
    th::complete_station_upgrade(*combat_, hangars_.get(), sid, nid, frame_);
    // 5. contents S -> S' (list order; the upgrade object itself moves with them).
    if (contents_ && contents_->move_all(sid, nid) > 0) prod_used_ = true;
    // 7-8. tech +1 for the upgrade object's owner, then every ally in player order [port: battle-local bonus]; each
    // bumped player's build orders at S continue at S' (progress kept).
    const objects::ObjectHandle uh = store.find(q.target);
    const int32_t o = store.alive(uh) ? store.owner(uh) : owner;
    auto bump = [&](int32_t p) {
        ++hg_tech_bonus_[static_cast<size_t>(p)];
        if (production_ && production_->retarget(p, sid, nid) > 0) prod_used_ = true;
    };
    if (o >= 0 && static_cast<size_t>(o) < hg_tech_bonus_.size()) {
        bump(o);
        for (size_t p = 0; p < hg_tech_bonus_.size(); ++p)
            if (static_cast<int32_t>(p) != o && !combat_->is_enemy(o, static_cast<int32_t>(p))) bump(static_cast<int32_t>(p));
    }
    // 6 / 11: the host copies selection and control groups and plays the announcement (station_upgrades()).
    hg_upgrades_.push_back(StationUpgrade{sid, nid, o});
    // 10. S removed silently (no death clone, explosion or survivors); its contents list is empty after the move.
    if (contents_)
        for (const objects::ObjectId left : contents_->remove_container(sid)) hg_orphans_.push_back(left);
    if (Unit* again = unit_by_id(sid)) bt_kill_silently(mode, again->h);
    hg_used_ = true;
}

// ---- station contents / skirmish tactical production (contents.hpp HOST WIRING) ----

uint8_t TacticalBattle::hg_build_check(const objects::ObjectStore& st, int32_t player, objects::ObjectId builder,
                                       objects::TypeIndex type) const {
    const objects::GameObjectTypes& types = catalog_->types();
    if (!production_enabled() || player < 0 || static_cast<size_t>(player) >= galaxy_->players().size()) return 1;
    if (type == objects::kNone || type >= types.size()) return 1;
    if (std::find(types_used_.begin(), types_used_.end(), static_cast<uint32_t>(type)) == types_used_.end()) return 1;
    const objects::ObjectHandle bh = st.find(builder);
    if (!st.alive(bh) || st.owner(bh) != player) return 1;
    bool unit = false;
    for (const Unit& u : units_)
        if (u.id == builder) unit = u.alive && !u.retreated;
    if (!unit) return 1;
    // The builder type's Tactical_Buildable_Objects_Multiplayer row of the player's faction.
    std::string faction;
    const uint32_t f = galaxy_->players()[static_cast<size_t>(player)].faction;
    if (f < galaxy_->rules().factions.size()) faction = galaxy_->rules().factions[f].name;
    const std::string name = type < galaxy_->rules().types.size() ? galaxy_->rules().types[type].name : std::string();
    bool listed = false, in_row = false;
    for (const std::string_view tok : types.get_list(st.type(bh), kBuildList)) {
        if (types.resolve_name(hg_trim(tok)) == objects::kNone) {
            in_row = hg_same(tok, faction);
            continue;
        }
        listed = listed || (in_row && hg_same(tok, name));
    }
    if (!listed) return 1;
    // Tactical_Build_Prerequisites: every listed type owned alive by the player [port: all-of].
    for (const std::string_view tok : types.get_list(type, "Tactical_Build_Prerequisites")) {
        const objects::TypeIndex pre = types.resolve_name(hg_trim(tok));
        if (pre == objects::kNone) continue;
        bool have = false;
        for (const objects::ObjectHandle h : st.all_objects())
            if (st.active(h) && st.type(h) == pre && st.owner(h) == player) {
                have = true;
                break;
            }
        if (!have) return 4;
    }
    // Build_Limit_Current_For_All_Allies: live objects of the player and allies plus their queued orders.
    const int32_t limit = types.get_int(type, "Build_Limit_Current_For_All_Allies");  // default -1: none
    if (limit > 0) {
        auto allied = [&](int32_t o) { return o == player || (o >= 0 && combat_ && !combat_->is_enemy(player, o)); };
        uint32_t n = 0;
        for (const objects::ObjectHandle h : st.all_objects())
            if (st.active(h) && st.type(h) == type && allied(st.owner(h))) ++n;
        for (uint32_t p = 0; p < bt_players_; ++p)
            if (allied(static_cast<int32_t>(p))) n += production_->count_type(static_cast<int32_t>(p), static_cast<int32_t>(type));
        if (n >= static_cast<uint32_t>(limit)) return 5;
    }
    return 0;
}

std::vector<TacticalBattle::BuildOptionInfo> TacticalBattle::station_build_options(const objects::ObjectStore& st, int32_t player,
                                                                                   objects::ObjectId builder) const {
    std::vector<BuildOptionInfo> out;
    if (!production_enabled() || player < 0 || static_cast<size_t>(player) >= galaxy_->players().size()) return out;
    const objects::ObjectHandle bh = st.find(builder);
    if (!st.alive(bh) || st.owner(bh) != player) return out;
    const objects::GameObjectTypes& types = catalog_->types();
    std::string faction;
    const uint32_t f = galaxy_->players()[static_cast<size_t>(player)].faction;
    if (f < galaxy_->rules().factions.size()) faction = galaxy_->rules().factions[f].name;
    const float credits = credits_get_ ? credits_get_(player) : 0.0f;
    bool in_row = false;
    for (const std::string_view tok : types.get_list(st.type(bh), kBuildList)) {
        const objects::TypeIndex t = types.resolve_name(hg_trim(tok));
        if (t == objects::kNone) {
            in_row = hg_same(tok, faction);
            continue;
        }
        if (!in_row) continue;
        BuildOptionInfo o;
        o.type = static_cast<int32_t>(t);
        o.cost = std::max(0, types.get_int(t, "Tactical_Build_Cost_Multiplayer"));
        for (const th::BuildOrder& q : production_->queue(player))
            if (q.builder == builder && q.type == o.type) ++o.queued;
        const uint8_t c = hg_build_check(st, player, builder, t);
        if (c == 1) continue;  // not buildable here (e.g. outside the battle tables)
        o.block = c != 0 ? c : (static_cast<float>(o.cost) > credits ? uint8_t{3} : uint8_t{0});
        out.push_back(o);
    }
    return out;
}

void TacticalBattle::hg_refund(uint32_t order, bool pay) {
    for (size_t i = 0; i < hg_order_cost_.size(); ++i)
        if (hg_order_cost_[i].order == order) {
            if (pay && hg_order_cost_[i].cost > 0.0f && credits_add_) credits_add_(hg_order_cost_[i].player, hg_order_cost_[i].cost);
            hg_order_cost_.erase(hg_order_cost_.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
}

commands::RejectReason TacticalBattle::hg_production_command(TacticalMode& mode, uint8_t player, const commands::EventData& ev) {
    using R = commands::RejectReason;
    if (!production_enabled()) return R::Unhandled;  // campaign / land battles: no tactical production
    const objects::GameObjectTypes& types = catalog_->types();
    const objects::ObjectStore& st = mode.primary();
    const uint32_t loc = ev.u("location");
    if (loc == commands::kNoObject || loc > 0x7FFFFFFFu) return R::InvalidTarget;
    const auto builder = static_cast<objects::ObjectId>(loc);
    const auto p = static_cast<int32_t>(player);
    if (ev.u("cancel") != 0) {
        // The index-th order of the player at this builder (queue order).
        uint32_t k = 0;
        const uint32_t want = ev.u("index");
        for (const th::BuildOrder& o : production_->queue(p)) {
            if (o.builder != builder) continue;
            if (k++ != want) continue;
            const uint32_t id = o.id;
            production_->cancel(id);
            hg_refund(id, true);
            prod_used_ = hg_used_ = true;
            return R::None;
        }
        return R::InvalidTarget;
    }
    const auto type = static_cast<objects::TypeIndex>(ev.u("type_index"));
    if (!hg_buildable(st, p, builder, type)) return R::NotAllowed;
    const float cost = static_cast<float>(std::max(0, types.get_int(type, "Tactical_Build_Cost_Multiplayer")));  // int field
    if (cost > 0.0f) {
        const float have = credits_get_ ? credits_get_(p) : 0.0f;
        if (have < cost) return R::CannotAfford;
        if (credits_add_) credits_add_(p, -cost);
    }
    // [port] Tactical_Build_Time_Seconds without the AI Space_Build_Time_Multiplier / build-time modifiers.
    const float seconds = static_cast<float>(types.get_int(type, "Tactical_Build_Time_Seconds"));  // int field
    const uint32_t id = production_->enqueue(p, builder, static_cast<int32_t>(type), seconds, frame_);
    hg_order_cost_.push_back(HgOrderCost{id, p, cost});
    prod_used_ = hg_used_ = true;
    return R::None;
}

void TacticalBattle::hg_remove_silently(objects::ObjectId id) {
    if (contents_ && contents_->remove(id)) prod_used_ = true;
    if (unit_by_id(id)) return;  // battle units: bt_kill_silently does the rest
    if (abilities_ && abilities_->has_object(id)) abilities_->remove(id);
    for (size_t i = 0; i < hg_upgrade_objs_.size(); ++i)
        if (hg_upgrade_objs_[i].id == id) {
            hg_upgrade_objs_.erase(hg_upgrade_objs_.begin() + static_cast<std::ptrdiff_t>(i));
            prod_used_ = true;
            break;
        }
}

void TacticalBattle::hg_production(TacticalMode& mode, SimRng& rng) {
    objects::ObjectStore& store = mode.primary();
    // Contents of containers removed last frame: removed silently [port].
    if (!hg_orphans_.empty()) {
        const std::vector<objects::ObjectId> orphans = std::move(hg_orphans_);
        hg_orphans_.clear();
        for (const objects::ObjectId id : orphans) {
            hg_remove_silently(id);
            bt_kill_silently(mode, store.find(id));
        }
    }
    if (!production_enabled()) return;
    const objects::GameObjectTypes& types = catalog_->types();
    production_->update(frame_);
    const std::vector<th::BuildOrder> done(production_->completed().begin(), production_->completed().end());
    production_->clear_completed();
    for (const th::BuildOrder& o : done) {
        prod_used_ = hg_used_ = true;
        hg_refund(o.id, false);
        const Unit* b = unit_by_id(o.builder);
        if (!b || !b->alive || b->retreated || !store.alive(b->h) || o.type < 0) continue;  // builder gone: dropped
        const auto t = static_cast<objects::TypeIndex>(o.type);
        const Vec3 pos = store.position(b->h), facing = store.facing(b->h);
        if (dummy_upgrade_behavior() != objects::kNoBehavior && types.has_behavior(t, objects::BehaviorSet::Union, dummy_upgrade_behavior())) {
            // DUMMY_UPGRADE: an object of the player in the builder's contents (no combat unit); its Skirmish_Automatic
            // ability (e.g. Starbase_Upgrade_Ability) is scheduled by add_container.
            objects::SpawnParams sp;
            sp.type = t;
            sp.owner = o.player;
            sp.position = pos;
            sp.facing = facing;
            const objects::ObjectHandle h = store.create(sp);
            if (!store.alive(h)) continue;
            const objects::ObjectId id = store.id(h);
            contents_->add(o.builder, id);
            const std::vector<tc::ObjectId> none;
            abilities_->add_container(id, desc_of(static_cast<uint32_t>(t)), o.player, none, frame_, &rng);
            hg_upgrade_objs_.push_back(HgUpgradeObject{id, h});
        } else if (!space_) {
            // q6 t19 (SKIRMISH_SETUP §3 / §3.1 item 1): a finished land build never spawns. 0x27fad0 appends the type to
            // the player's reinforcement pool; the human / AI Reinforce drops it later through its landing object
            // (Company_Transport_Unit, A1_READINGS §35). Signal 0x22 (production_done) tells the AI build task.
            ProductionDone d;
            d.player = o.player;
            d.type = static_cast<uint32_t>(t);
            (void)add_built_reserve(o.player, d.type, d.key);
            production_done_.push_back(d);
        } else {
            // Space: the normal tactical spawn at the builder [port: SKIRMISH_SETUP §3 puts space builds in the pool too;
            // no initial order].
            std::vector<objects::ObjectId> ordered;
            (void)hg_spawn(mode, o.type, o.player, pos, facing, pos, false, rng, ordered);
        }
    }
}

void TacticalBattle::set_garrison_spawn(objects::ObjectId id, bool on) {
    if (!hangars_ || !hangars_->has_carrier(id)) return;
    hangars_->set_spawn_enabled(id, on);
    hg_used_ = true;
}

void TacticalBattle::hg_publish_engine_requests() {
    engine_loop_requests_.assign(engine_loops_.requests().begin(), engine_loops_.requests().end());
    engine_loops_.clear_requests();
}

void TacticalBattle::hg_hash(StateHasher& h) const {
    if (hangars_) hangars_->hash(h);
    h.u32(hg_launches_);
    h.u32(static_cast<uint32_t>(hg_tech_bonus_.size()));
    for (const int32_t t : hg_tech_bonus_) h.i32(t);
    if (!prod_used_) return;  // station contents / tactical production (absent until used)
    if (contents_) contents_->hash(h);
    if (production_) production_->hash(h);
    h.u32(static_cast<uint32_t>(hg_order_cost_.size()));
    for (const HgOrderCost& c : hg_order_cost_) {
        h.u32(c.order);
        h.i32(c.player);
        h.f32(c.cost);
    }
    h.u32(static_cast<uint32_t>(hg_upgrade_objs_.size()));
    for (const HgUpgradeObject& u : hg_upgrade_objs_) h.i32(u.id);
}

}  // namespace alamo::sim
