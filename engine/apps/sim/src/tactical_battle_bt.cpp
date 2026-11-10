// --- battle host (BT pass): battle setup / transition rules of docs/re/TACTICAL_GAPS.md §4 (BATTLE_TRANSITIONS §7 items
// 2, 6, 8, 9) and the build-pad combat units / TacticalBuild / TacticalSell of L1_S4_REMAINING §3.6 (tactical_battle.hpp
// "battle host (BT pass)" blocks). Serial code; synced draws: retreat attrition (one inline draw per eligible unit) and the
// hard-point draws of AbilitySystem::add_unit for pad objects.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>

#include "alamo/core/dmath.hpp"
#include "alamo/core/ascii.hpp"
#include "alamo/core/hash.hpp"
#include "alamo/terrain/chunk.hpp"
#include "alamo/galactic/campaign.hpp"
#include "alamo/objects/behaviors.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/sim/map_spawn.hpp"
#include "alamo/sim/tactical_battle.hpp"
#include "alamo/tactical/movement/special_locomotors.hpp"
#include "alamo/tactical/movement/ted_grid.hpp"
#include "alamo/tactical/movement/unit_params.hpp"

namespace alamo::sim {

namespace tc = alamo::tactical::combat;
namespace mv = alamo::tactical::movement;
namespace cmd = alamo::commands;

namespace {

bool iequal_trim(std::string_view a, std::string_view b) { return ascii_iequals(ascii_trim(a), ascii_trim(b)); }

objects::BehaviorTypeId pad_behavior() {
    static const objects::BehaviorTypeId id = objects::behavior_type_id("TACTICAL_BUILD_OBJECTS");
    return id;
}
objects::BehaviorTypeId uc_behavior() {
    static const objects::BehaviorTypeId id = objects::behavior_type_id("TACTICAL_UNDER_CONSTRUCTION");
    return id;
}

}  // namespace

bool TacticalBattle::bt_is_super_weapon(uint32_t type) const {
    const objects::GameObjectTypes& types = catalog_->types();
    return type < types.size() && types.has_behavior(type, objects::BehaviorSet::Space, objects::beh::TacticalSuperWeapon);
}

// ---- force split (constructor) ----------------------------------------------------------------------------------

void TacticalBattle::bt_force_split() {
    if (space_) {
        // §4.6: bit 0x2c of the space bitset = TACTICAL_SUPER_WEAPON -> split rule 1: a super-weapon always starts.
        for (Participant& p : parts_)
            if (p.reserve && !p.starbase && !p.structure && bt_is_super_weapon(p.type)) p.reserve = false;
        return;
    }
    // §4.2 land split 0x2bc6a0: the defender's surface units all start (no cap); the attacker = lead unit or raid force.
    if (skirmish_map()) return;  // skai: a skirmish has no invasion: both slots' starting units start
    std::vector<uint32_t> att;  // attacker participants, request order (start list first, then reserve)
    for (uint32_t pass = 0; pass < 2; ++pass)
        for (uint32_t i = 0; i < parts_.size(); ++i) {
            const Participant& p = parts_[i];
            if (p.side == 1 && !p.starbase && !p.structure && p.reserve == (pass == 1)) att.push_back(i);
        }
    if (att.size() <= 1) return;
    const BattleCatalog::TransitionConstants& k = catalog_->transition_constants();
    // Raid test 0x432e20: required faction, every member free or limited, 1 <= limited <= max, heroes <= max.
    {
        bool raid = k.raid_limited_mask != 0 || k.raid_free_mask != 0;
        if (raid && !k.raid_required_faction.empty()) {
            const int32_t pl = req_.info.attacker;
            std::string faction;
            if (pl >= 0 && static_cast<size_t>(pl) < galaxy_->players().size()) {
                const uint32_t f = galaxy_->players()[static_cast<size_t>(pl)].faction;
                if (f < galaxy_->rules().factions.size()) faction = galaxy_->rules().factions[f].name;
            }
            raid = iequal_trim(faction, k.raid_required_faction);
        }
        int32_t limited = 0, heroes = 0;
        for (const uint32_t i : att) {
            if (!raid) break;
            const galactic::ConflictTypeInfo& ti = rules_->type(parts_[i].type);
            if (ti.hero) ++heroes;
            if ((ti.categories & k.raid_free_mask) != 0) continue;  // [M] free mask tested first
            if ((ti.categories & k.raid_limited_mask) != 0) ++limited;
            else raid = false;
        }
        raid = raid && limited >= 1 && limited <= k.raid_max_limited && heroes <= k.raid_max_heroes;
        if (raid) {
            bt_raid_ = true;
            for (const uint32_t i : att) parts_[i].reserve = false;
            return;
        }
    }
    // Lead unit 0x29c0c0: (b) Preferred_Pathfinder_Types in order, non-heroes first, then heroes; (c) first non-hero,
    // else first hero. [port] rule (a) (fleet-lead flag / forced type) has no ground-unit source.
    const auto& gtypes = galaxy_->rules().types;
    auto name_of = [&](uint32_t t) -> std::string_view { return t < gtypes.size() ? std::string_view(gtypes[t].name) : std::string_view(); };
    uint32_t lead = 0xFFFFFFFFu;
    for (const std::string& want : k.preferred_pathfinder_types) {
        for (int hero = 0; hero < 2 && lead == 0xFFFFFFFFu; ++hero)
            for (const uint32_t i : att)
                if (rules_->type(parts_[i].type).hero == (hero == 1) && iequal_trim(name_of(parts_[i].type), want)) {
                    lead = i;
                    break;
                }
        if (lead != 0xFFFFFFFFu) break;
    }
    for (int hero = 0; hero < 2 && lead == 0xFFFFFFFFu; ++hero)
        for (const uint32_t i : att)
            if (rules_->type(parts_[i].type).hero == (hero == 1)) {
                lead = i;
                break;
            }
    for (const uint32_t i : att) parts_[i].reserve = i != lead;
}

// ---- pad types (setup) -------------------------------------------------------------------------------------------

void TacticalBattle::prepare_models(const BattleCatalog& catalog, const galactic::GalacticSim& galaxy, const galactic::ConflictRules& rules,
                                    const galactic::BattleRequest& request, const terrain::MapData& map, const ParallelFor& pf) {
    const objects::GameObjectTypes& types = catalog.types();
    const bool space = request.info.kind == galactic::ConflictKind::Space;
    std::vector<uint32_t> list;
    auto use = [&](uint32_t t) {
        if (t < types.size() && std::find(list.begin(), list.end(), t) == list.end()) list.push_back(t);
    };
    const objects::ObjectStore& gs = galaxy.store();
    for (const galactic::BattleForces& f : request.forces)
        for (const std::vector<objects::ObjectHandle>* hs : {&f.start, &f.reserve})
            for (const objects::ObjectHandle h : *hs) {
                if (!gs.alive(h)) continue;
                const auto t = static_cast<uint32_t>(gs.type(h));
                const galactic::ConflictTypeInfo& ti = rules.type(t);
                use(t);
                for (const auto& [mt, n] : ti.members) use(mt);
            }
    const TypeCrcIndex index(types);
    for (const terrain::ObjectPlacement& pl : map.objects) {
        const objects::TypeIndex t = index.find(pl.type_crc);
        if (t == objects::kNone || t >= types.size()) continue;
        use(static_cast<uint32_t>(t));
        if (pad_behavior() == objects::kNoBehavior || !types.has_behavior(t, objects::BehaviorSet::Union, pad_behavior())) continue;
        for (const char* tag : {"Tactical_Buildable_Objects_Multiplayer", "Tactical_Buildable_Objects_Campaign"})
            for (const std::string_view tok : types.get_list(t, tag)) {
                const objects::TypeIndex uc = types.raw().find(tok);
                if (uc == objects::kNone || uc >= types.size()) continue;
                use(static_cast<uint32_t>(uc));
                const objects::TypeIndex ft = types.resolve_name(types.get_string(uc, "Tactical_Buildable_Constructed"));
                if (ft != objects::kNone) use(static_cast<uint32_t>(ft));
            }
    }
    catalog.preload(list, space,
                    BattleCatalog::kPreloadBones | BattleCatalog::kPreloadAvoid | (space ? 0u : BattleCatalog::kPreloadExtents), pf);
}

void TacticalBattle::bt_add_pad_types(const terrain::MapData& map) {
    if (pad_behavior() == objects::kNoBehavior) return;
    const objects::GameObjectTypes& types = catalog_->types();
    const TypeCrcIndex index(types);
    std::vector<uint32_t> pads;
    for (const terrain::ObjectPlacement& pl : map.objects) {
        const objects::TypeIndex t = index.find(pl.type_crc);
        if (t == objects::kNone || t >= types.size() || !types.has_behavior(t, objects::BehaviorSet::Union, pad_behavior())) continue;
        if (std::find(pads.begin(), pads.end(), t) == pads.end()) pads.push_back(t);
    }
    const size_t before = types_used_.size();
    auto use = [this](uint32_t t) {
        if (std::find(types_used_.begin(), types_used_.end(), t) == types_used_.end()) types_used_.push_back(t);
    };
    for (const uint32_t pt : pads)
        for (const std::string_view tok : types.get_list(pt, pad_list_tag())) {
            const objects::TypeIndex uc = types.raw().find(tok);
            if (uc == objects::kNone || uc >= types.size()) continue;  // a faction name (row header)
            use(uc);
            const objects::TypeIndex ft = types.resolve_name(types.get_string(uc, "Tactical_Buildable_Constructed"));
            if (ft != objects::kNone && ft < types.size()) use(ft);
        }
    if (pad_records_ && pad_restore_ && !cfg_.skirmish && !pads.empty())  // §9.4: the finished types bt_pad_restore may create
        for (const PadRecord& r : *pad_records_)
            if (r.type < types.size()) use(r.type);
    if (types_used_.size() != before) tables_extend();
}

// ---- pad objects as battle units -----------------------------------------------------------------------------------

uint32_t TacticalBattle::bt_adopt_built(TacticalMode& mode, objects::ObjectHandle h, int32_t owner, float health_fraction, SimRng* rng,
                                        bool map, uint32_t container) {
    objects::ObjectStore& st = mode.primary();
    if (!st.alive(h) || !abilities_ || !rng) return 0xFFFFFFFFu;
    const uint32_t t = static_cast<uint32_t>(st.type(h));
    if (std::find(types_used_.begin(), types_used_.end(), t) == types_used_.end()) return 0xFFFFFFFFu;  // not in the tables
    Participant p;
    p.galactic_id = objects::kNoObjectId;
    p.owner = owner;
    p.type = t;
    p.side = owner == req_.info.defender ? 0 : 1;
    p.deployed = true;
    p.built = true;
    p.map = map;
    p.third = map && owner != req_.info.defender && owner != req_.info.attacker;
    p.pop = 0.0f;
    const auto& gt = galaxy_->rules().types;
    p.power = t < gt.size() ? gt[t].ai_combat_power : 0.0f;
    p.spawned = p.alive = 1;
    const auto pi = static_cast<uint32_t>(parts_.size());
    parts_.push_back(p);
    Unit u;
    u.id = st.id(h);
    u.h = h;
    u.participant = pi;
    u.type = t;
    u.side = p.side;
    const bool member = container < containers_.size();  // OI host: a wingman joining a team
    u.movable = (map || member) && mode.movable(h);  // pad objects are structures [port: never given to MovementSystem]
    bool leader = false;  // s78f t07: the first member of an empty team leads it (bt_spawn_company's rule)
    if (member) {
        u.container = container;
        leader = containers_[container].alive == 0;
        ++containers_[container].alive;
    }
    u.power = p.power;
    if (u.movable) {  // §3.1: a map-placed unit with a locomotor
        mv::UnitDesc md = mv::unit_desc_from_type(catalog_->types(), t);
        md.locomotor = mv::locomotor_kind_from_type(catalog_->types(), t, space_ ? mv::MoveMode::Space : mv::MoveMode::Land);
        const Vec3 pos = st.position(h), f = st.facing(h);
        md.id = u.id;
        md.pos = {pos.x, pos.y};
        md.z = pos.z;
        md.yaw_deg = dmath::atan2(f.y, f.x) * (180.0f / dmath::kPiF);
        md.owner = owner;
        mv_type_extents(md, t);  // s78n t20
        movement_->add_unit(md);
    }
    tc::UnitSpawn cs;
    cs.id = u.id;
    cs.desc = desc_of(t);
    cs.owner = owner;
    cs.position = tactical::Vec3{st.position(h).x, st.position(h).y, st.position(h).z};
    cs.facing = tactical::Vec3{st.facing(h).x, st.facing(h).y, st.facing(h).z};
    cs.health_fraction = std::clamp(health_fraction, 0.0f, 1.0f);
    if (member && !space_) {  // s78f t07: land team members join combat with their team (see bt_spawn_company)
        cs.team = static_cast<uint32_t>(containers_[container].id);
        cs.team_leader = leader;
    }
    if (member && space_) cs.damage_team = static_cast<uint32_t>(containers_[container].id);  // s78g t06 (see deploy)
    abilities_->add_unit(cs, cs.desc, frame_, *rng, member ? containers_[container].id : tc::kNoObject);
    u.range = combat_->attack_range(u.id);
    const auto it = std::lower_bound(units_.begin(), units_.end(), u.id, [](const Unit& a, objects::ObjectId v) { return a.id < v; });
    const Unit& placed = *units_.insert(it, u);
    hg_adopt(placed, *rng);  // hangars: engine mode enter, SPAWN_SQUADRON add_carrier (attach order)
    ++stats_.spawned;
    s4_register(mode, placed);
    l1_register(mode, placed.h, static_cast<int32_t>(placed.type));
    bt_used_ = true;
    return pi;
}

void TacticalBattle::bt_pad_health() {
    size_t w = 0;
    for (size_t i = 0; i < bt_ramps_.size(); ++i) {
        const UcRamp r = bt_ramps_[i];
        const Unit* u = unit_by_id(r.id);
        if (!u || !u->alive || !combat_->has_unit(r.id) || !combat_->alive(r.id)) continue;
        const float mh = combat_->max_health(r.id);
        combat_->set_health(r.id, std::min(mh, combat_->health(r.id) + r.rate));  // vf6: health += rate
        bt_ramps_[w++] = r;
    }
    bt_ramps_.resize(w);
}

float TacticalBattle::build_cost(objects::TypeIndex uc_type) const {
    const objects::GameObjectTypes& types = catalog_->types();
    if (uc_type >= types.size()) return 0.0f;
    // Both are int fields (the float view of an int field is 0).
    // t50 0x3744e0: a skirmish pad is priced from Tactical_Build_Cost_Multiplayer alone (no campaign fallback).
    if (skirmish_land()) return static_cast<float>(std::max(0, types.get_int(uc_type, "Tactical_Build_Cost_Multiplayer")));
    int32_t c = types.get_int(uc_type, "Tactical_Build_Cost_Campaign");
    if (c <= 0) c = types.get_int(uc_type, "Tactical_Build_Cost_Multiplayer");
    return static_cast<float>(std::max(0, c));
}

cmd::RejectReason TacticalBattle::tactical_build(TacticalMode& mode, uint8_t player, objects::ObjectId pad, objects::TypeIndex type,
                                                 SimRng& rng, bool ai) {
    using R = cmd::RejectReason;
    if (finished_ || !combat_) return R::NotAllowed;
    bt_rng_ = &rng;
    objects::ObjectStore& st = mode.primary();
    const objects::GameObjectTypes& types = catalog_->types();
    const objects::ObjectHandle ph = st.find(pad);
    if (!st.alive(ph) || pad_behavior() == objects::kNoBehavior || !st.has_behavior(ph, pad_behavior()) || type >= types.size())
        return R::InvalidTarget;
    if (pad_contents(pad) != objects::kNoObjectId) return R::NotAllowed;  // UC present / built present
    if (fog() && fog_object_hidden(static_cast<int32_t>(player), pad, true)) return R::NotAllowed;  // pad fogged (0x35f470)
    const int32_t owner = st.owner(ph);
    // q6 t19 (AI_TACTICAL §7.5.1): 0x39a230(pad, player) = relation(player -> pad owner) == 0. A Neutral-owned (relation 2)
    // pad is not allied, for humans and AI alike; the structure belongs to the builder, the pad keeps its owner.
    if (owner < 0 || !bt_allied(static_cast<int32_t>(player), owner)) return R::NotAllowed;  // "not owned by an ally"
    if (const PadState* s = pad_state(pad); s && s->cooldown_end > frame_) return R::NotAllowed;
    if (uc_behavior() == objects::kNoBehavior || !types.has_behavior(type, objects::BehaviorSet::Union, uc_behavior())) return R::InvalidUnit;
    if (!ai) {
        const float cost = build_cost(type);
        const float have = credits_get_ ? credits_get_(static_cast<int32_t>(player)) : 0.0f;
        if (cost > have) return R::CannotAfford;
        if (cost > 0.0f && credits_add_) credits_add_(static_cast<int32_t>(player), -cost);  // execute 0x5d78a0: charge 1
    }
    return pad_build(mode, pad, type, false, static_cast<int32_t>(player)) == 1 ? R::None : R::NotAllowed;
}

cmd::RejectReason TacticalBattle::tactical_sell(TacticalMode& mode, uint8_t player, objects::ObjectId object) {
    using R = cmd::RejectReason;
    if (finished_) return R::NotAllowed;
    objects::ObjectStore& st = mode.primary();
    PadState* s = nullptr;
    for (PadState& p : pads_)
        if (p.built == object && object != objects::kNoObjectId) s = &p;
    const objects::ObjectHandle h = st.find(object);
    if (!s || !st.alive(h)) return R::InvalidUnit;
    if (st.owner(h) != static_cast<int32_t>(player)) return R::NotOwner;
    if (s->locked) return R::NotAllowed;  // silently refused while locked (the only reader of the lock)
    // Refund: the UC entry whose constructed type matches (0x374590) x Tactical_Sell_Percentage (+0xa08).
    const objects::GameObjectTypes& types = catalog_->types();
    const objects::TypeIndex bt = st.type(h);
    float cost = 0.0f;
    const objects::ObjectHandle ph = st.find(s->pad);
    if (st.alive(ph))
        for (const std::string_view tok : types.get_list(st.type(ph), pad_list_tag())) {
            const objects::TypeIndex uc = types.raw().find(tok);
            if (uc == objects::kNone || uc >= types.size()) continue;
            if (types.resolve_name(types.get_string(uc, "Tactical_Buildable_Constructed")) == bt) {
                cost = build_cost(uc);
                break;
            }
        }
    const float refund = std::floor(cost * (bt < types.size() ? types.get_float(bt, "Tactical_Sell_Percentage") : 0.0f));
    if (refund > 0.0f && credits_add_) credits_add_(static_cast<int32_t>(player), refund);
    // Release 0x42ecd0: no cooldown; then the object is killed [port: removed from the battle at once].
    s->built = objects::kNoObjectId;
    s->built_type = objects::kNone;
    s->locked = false;
    // q6 t19 (AI_TACTICAL §7.5.1 item 4): a sale clears the slot and the persistence record but resets no ownership and
    // starts no cooldown: the pad keeps its owner until someone captures it (the capture rules of an empty pad).
    if (st.alive(ph)) pad_persist(st, ph, bt, st.position(h), false);  // §9.4: the sale removes the persistent record (the object's x, y)
    if (Unit* u = unit_by_id(object); u && u->alive) {
        u->alive = false;
        --parts_[u->participant].alive;
        abilities_->remove(u->id);
        unit_gone(mode, *u);
    }
    mode.destroy(h);
    return R::None;
}

// ---- HIJACK reserve ------------------------------------------------------------------------------------------------

bool TacticalBattle::can_add_reserve(uint32_t galactic_type) const {
    Participant p;
    p.type = galactic_type;
    for (const uint32_t t : spawn_types(p))
        if (std::find(types_used_.begin(), types_used_.end(), t) == types_used_.end()) return false;
    if (!rules_->type(galactic_type).members.empty() &&
        std::find(types_used_.begin(), types_used_.end(), galactic_type) == types_used_.end())
        return false;
    return true;
}

bool TacticalBattle::add_reserve(objects::ObjectHandle galactic, int32_t owner) {
    if (finished_) return false;
    const objects::ObjectStore& gs = galaxy_->store();
    if (!gs.alive(galactic) || (owner != req_.info.attacker && owner != req_.info.defender)) return false;
    const objects::ObjectId gid = gs.id(galactic);
    for (const Participant& p : parts_)
        if (!p.built && p.galactic_id == gid) return false;
    if (!can_add_reserve(static_cast<uint32_t>(gs.type(galactic)))) return false;
    const size_t n = parts_.size();
    add_participant(galactic, owner, owner == req_.info.defender ? 0 : 1, true, false, false);
    if (parts_.size() == n) return false;
    bt_used_ = true;
    return true;
}

// q6 t19 (SKIRMISH_SETUP §3.1 item 1, 0x27fad0): a finished land tactical build joins the player's reinforcement pool.
bool TacticalBattle::add_built_reserve(int32_t owner, uint32_t type, objects::ObjectId& key) {
    key = objects::kNoObjectId;
    if (finished_ || !can_add_reserve(type)) return false;
    Participant p;
    p.galactic_id = kBuiltReserveKeyBase + static_cast<objects::ObjectId>(parts_.size());
    p.owner = owner;
    p.type = type;
    p.side = owner == req_.info.defender ? 0 : 1;
    p.reserve = true;
    p.built = true;  // no galactic counterpart: not in the battle result
    const galactic::ConflictTypeInfo& ti = rules_->type(type);
    p.pop = static_cast<float>(ti.space_population_override >= 0 ? ti.space_population_override : ti.population);
    const auto& types = galaxy_->rules().types;
    p.power = type < types.size() ? types[type].ai_combat_power : 0.0f;
    for (const auto& [mt, n] : ti.members)
        if (mt < types.size()) p.power += static_cast<float>(n) * types[mt].ai_combat_power;
    parts_.push_back(p);
    bt_used_ = true;
    key = p.galactic_id;
    return true;
}

// ---- retreat attrition (§4.9) ------------------------------------------------------------------------------------

void TacticalBattle::bt_retreat_attrition(uint8_t side, SimRng& rng) {
    const BattleCatalog::TransitionConstants& k = catalog_->transition_constants();
    const int32_t player = side_player(side);
    float p = space_ ? k.space_retreat_attrition : k.land_retreat_attrition;
    if (!space_) {
        // + Blockade_Run_Attrition_Factor when a fleet of another player orbits the planet with a non-transport member
        // [M: SpaceUnit / Squadron members], unless a retreating unit's type has Prevents_Blockade_Run_Attrition.
        bool blockade = false;
        for (int32_t o = 0; o < static_cast<int32_t>(galaxy_->players().size()) && !blockade; ++o) {
            if (o == player) continue;
            for (const uint32_t fi : galaxy_->fleets_at(req_.info.planet, o)) {
                const galactic::FleetState& f = galaxy_->fleets()[fi];
                for (const objects::ObjectHandle m : f.contents) {
                    const auto t = galaxy_->store().alive(m) ? static_cast<uint32_t>(galaxy_->store().type(m)) : galactic::kNoIndex;
                    if (t < galaxy_->rules().types.size() && (galaxy_->rules().types[t].kind == galactic::UnitKind::SpaceUnit ||
                                                              galaxy_->rules().types[t].kind == galactic::UnitKind::Squadron))
                        blockade = true;
                }
            }
        }
        if (blockade)
            for (const Unit& u : units_)
                if (u.side == side && u.alive && !u.retreated && catalog_->types().get_bool(u.type, "Prevents_Blockade_Run_Attrition"))
                    blockade = false;
        if (blockade) p = std::min(1.0f, p + k.blockade_run_attrition);
    }
    const float r = 0.0f;  // [M] the retreating player's tactical list +0x160 (attrition reduction): no port source -> 0
    bt_attrition_ids_.clear();
    // Retreating units walked last to first; not a team container (containers are not units here), not a named hero,
    // with a galactic counterpart: one inline synced draw each, also at p = 0.
    for (size_t i = units_.size(); i-- > 0;) {
        const Unit& u = units_[i];
        if (u.side != side || !u.alive || u.retreated || parts_[u.participant].built) continue;
        if (rules_->type(u.type).hero) continue;
        const float draw = rng.inline_unit_float();
        if (draw < p - r) bt_attrition_ids_.push_back(u.id);
    }
    if (!bt_attrition_ids_.empty()) bt_used_ = true;
}

void TacticalBattle::bt_apply_attrition(uint32_t frame) {
    // Killed through Take_Damage kind 1 (source CRC 0x5d5e0b6b) [port: CombatSystem::kill_unit, kind 1]: reported as deaths
    // of this tick's combat (consume_events), so the galactic write-back sees them like any death.
    for (const objects::ObjectId id : bt_attrition_ids_)
        if (combat_->has_unit(id) && combat_->alive(id)) {
            combat_->kill_unit(id, tc::kNoObject, 1, frame);
            ++bt_attrition_kills_;
        }
    bt_attrition_ids_.clear();
}

// ---- battle end: lead object (§4.8 step 4) -------------------------------------------------------------------------

void TacticalBattle::bt_battle_end() {
    if (bt_lead_ == objects::kNoObjectId || result_.winner < 0) return;
    const Unit* lead = nullptr;
    for (const Unit& u : units_)
        if (u.id == bt_lead_) lead = &u;
    if (!lead || !lead->alive || lead->retreated) return;
    const int32_t owner = parts_[lead->participant].owner;
    if (owner == result_.winner) return;
    bool killer = false;
    for (const Unit& u : units_)
        if (u.alive && !u.retreated && parts_[u.participant].owner == result_.winner && rules_->type(u.type).super_killer) killer = true;
    if (killer) {
        bt_sw_killed_ = true;
        const Participant& lp = parts_[lead->participant];
        if (!lp.built && std::find(result_.destroyed.begin(), result_.destroyed.end(), lp.galactic) == result_.destroyed.end())
            result_.destroyed.push_back(lp.galactic);
    } else {
        result_.retreated = result_.winner;  // the winner without a killer is the side that retreats
    }
}

// ---- battle host follow-up: TEAM containers (REMAINING_GAPS §3.4), landing reinforcement points (§3.3) --------------

objects::TypeIndex TacticalBattle::representative_type(objects::ObjectId id) const {
    if (id == objects::kNoObjectId) return objects::kNone;
    for (const Container& ct : containers_)  // container order; ids are unique
        if (ct.id == id) return ct.team && ct.type != 0xFFFFFFFFu ? static_cast<objects::TypeIndex>(ct.type) : objects::kNone;
    return objects::kNone;
}

bool TacticalBattle::bt_team(uint32_t company_type, uint32_t first_member, uint32_t& object_type, uint32_t& behavior_type,
                             uint32_t& squad) const {
    const objects::GameObjectTypes& types = catalog_->types();
    object_type = behavior_type = 0xFFFFFFFFu;
    squad = 0;
    if (company_type >= types.size()) return false;
    // 0x2a00b0: the helper runs when the first spawned member's type has Create_Team (+0x67), or always in space.
    if (!space_ && !(first_member < types.size() && types.get_bool(first_member, "Create_Team"))) return false;
    // Create_Team_Type (+0x1e50 / +0x1e58), else the type named "Team"; neither resolves -> nothing is created.
    const std::string_view name = types.get_string(company_type, "Create_Team_Type");
    objects::TypeIndex tt = objects::kNone;
    if (!ascii_trim(name).empty()) tt = types.resolve_name(ascii_trim(name));
    const bool named = tt != objects::kNone && tt < types.size();
    if (!named) tt = types.resolve_name("Team");
    if (tt == objects::kNone || tt >= types.size()) return false;
    const int32_t k = types.get_int(company_type, "Max_Squad_Size");  // company type +0x1e44, default 10
    squad = k > 0 ? static_cast<uint32_t>(k) : 10u;
    object_type = static_cast<uint32_t>(tt);
    behavior_type = named ? object_type : company_type;  // TEAM behaviour type: the company type without Create_Team_Type
    return true;
}

void TacticalBattle::bt_team_positions(TacticalMode& mode) {
    // A1_READINGS §29.4: each TEAM service (0x405b10) moves the container to the centre of its members' joint bounding box
    // [H]; [M] the members' boxes are taken as equal, so the centre of their positions' box is used. s78n t03 (TACTICAL_
    // COMBAT §5.4.4 item 2): only when the container has no locomotor or its vf +0x230 is false - space squadrons (the
    // FIGHTER_FLYING §4.10 B centre). A land squad container (LandTeamContainer) is never re-centred: its own mover places
    // it (MovementSystem, mv_squad_add). [port] A land squad whose members cannot steer (no mover) keeps this stand-in.
    objects::ObjectStore& st = mode.primary();
    for (const Container& c : containers_) {
        if (!c.team || c.removed || !st.alive(c.h)) continue;
        if (!space_ && movement_ && movement_->has_unit(c.id)) continue;
        bool any = false;
        Vec3 lo{0.0f, 0.0f, 0.0f}, hi{0.0f, 0.0f, 0.0f};
        for (const Unit& u : units_)
            if (u.alive && !u.retreated && u.container != 0xFFFFFFFFu && containers_[u.container].id == c.id) {
                const Vec3 p = st.position(u.h);
                if (!any) {
                    lo = hi = p;
                    any = true;
                    continue;
                }
                lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
                hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
            }
        if (any) st.set_position(c.h, Vec3{(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f});
    }
}

void TacticalBattle::combat_team_positions(const objects::ObjectStore& store) {
    // s78n t03 (TACTICAL_COMBAT §5.4.4 item 2.1): TeamTargeting 0x62eeb0 scans and range-tests from the land container's
    // position - the same store position the fog test, the AI threat entry and Bribe's T read.
    combat_->clear_team_positions();
    if (space_) return;
    for (const Container& c : containers_) {
        if (!c.team || c.removed || !store.alive(c.h)) continue;
        const Vec3 p = store.position(c.h);
        combat_->set_team_position(static_cast<uint32_t>(c.id), tactical::Vec3{p.x, p.y, p.z});
    }
}

void TacticalBattle::type_half_extents(uint32_t type, float& hx, float& hy) const {
    // s78n t09 (TACTICAL_COMBAT §5.4.6 item 1, 0x371be0): both Custom_Hard extents > 0 -> Scale_Factor x (X, Y), no model
    // read; else the mode's model (no fallback to another mode's): none -> (0, 0); else per axis the custom value when > 0,
    // else the collision-bound half x / y (ModelClass vf +0x58: avoid_model's bound_h) x Scale_Factor. [port] A named model
    // whose file does not load (no art data) takes per axis the custom value, else 0 (s78n t10: as drop_half_extents).
    hx = hy = 0.0f;
    const objects::GameObjectTypes& types = catalog_->types();
    if (type >= types.size()) return;
    const float sf = types.get_float(type, "Scale_Factor");
    const float sc = sf > 0.0f ? sf : 1.0f;
    const float cx = types.get_float(type, "Custom_Hard_XExtent"), cy = types.get_float(type, "Custom_Hard_YExtent");
    if (cx > 0.0f && cy > 0.0f) {
        hx = sc * cx;
        hy = sc * cy;
        return;
    }
    if (ascii_trim(types.get_string(type, space_ ? "Space_Model_Name" : "Land_Model_Name")).empty()) return;
    const tactical::movement::AvoidModel* am = catalog_->avoid_model(type, space_);
    if (!am) {
        hx = cx > 0.0f ? sc * cx : 0.0f;
        hy = cy > 0.0f ? sc * cy : 0.0f;
        return;
    }
    hx = cx > 0.0f ? sc * cx : sc * std::fabs(am->bound_h.x);
    hy = cy > 0.0f ? sc * cy : sc * std::fabs(am->bound_h.y);
}

void TacticalBattle::mv_type_extents(tactical::movement::UnitDesc& d, uint32_t type) const {
    float hx = 0.0f, hy = 0.0f;
    type_half_extents(type, hx, hy);
    d.type_hx = hx;
    d.type_hy = hy;
}

void TacticalBattle::unit_half_extents(const Unit& u, float& hx, float& hy) const {
    // s78n t09 (TACTICAL_COMBAT §5.4.6 items 2 / 4, 0x396850 on a non-container): a Deploys walker in Walk state 0x18 / 0x19
    // takes 0x371d20 - the deployed pair when both deployed extents > 0, else 0x371be0 with the x / y replaced only when
    // Custom_Hard_XExtent_Deployed > 0 (the y test reads the x field) - and every other object 0x371be0.
    type_half_extents(u.type, hx, hy);
    deployed_half_extents(u.type, u.id, hx, hy);
}

void TacticalBattle::deployed_half_extents(uint32_t type, objects::ObjectId id, float& hx, float& hy) const {
    const objects::GameObjectTypes& types = catalog_->types();
    if (type >= types.size() || !types.get_bool(type, "Deploys") || !movement_ || !movement_->has_unit(id)) return;
    const mv::LocoState ds = movement_->deploy_state(id);
    if (ds != mv::LocoState::WalkDeploying && ds != mv::LocoState::WalkDeployed) return;
    const float sf = types.get_float(type, "Scale_Factor");
    const float sc = sf > 0.0f ? sf : 1.0f;
    const float dx = types.get_float(type, "Custom_Hard_XExtent_Deployed");
    const float dy = types.get_float(type, "Custom_Hard_YExtent_Deployed");
    if (dx > 0.0f) {
        hx = sc * dx;
        hy = sc * dy;  // with dy <= 0: 0 (the deployed y is used only together with the deployed x)
    }
}

bool TacticalBattle::squad_joint_slots(const objects::ObjectStore& st, objects::ObjectId container,
                                       std::vector<tactical::abilities::SquadSlot>& slots, float& jiggle, Vec3& facing) const {
    // s78n t09 (TACTICAL_COMBAT §5.4.4 item 1, 0x404450): the container's member array (live members, team index order;
    // null / gone entries skipped), each member's slot (along +0x114, side +0x118) and its 0x396850 half-extents (hx with
    // the along offset, hy with the side offset), the container type's MaxJiggleDistance (+0x16ac) and its facing.
    // s78n review: index lookups only (this runs per fog query, in the parallel candidate filter too) - the movement
    // system's team container test and its (container, team index) member index, the store's id lookup and the units'
    // ascending-id binary search. Only mv_squad_add makes team container movers (removed with the container: unit_gone).
    slots.clear();
    jiggle = 0.0f;
    if (space_ || !movement_ || !movement_->is_land_team_container(container)) return false;
    const objects::ObjectHandle ch = st.find(container);
    if (!st.alive(ch)) return false;
    for (const mv::MovementSystem::LandTeamSlot& e : movement_->land_team(container)) {
        const auto it = std::lower_bound(units_.begin(), units_.end(), e.member, [](const Unit& u, objects::ObjectId v) { return u.id < v; });
        if (it == units_.end() || it->id != e.member || !it->alive || it->retreated) continue;
        tactical::abilities::SquadSlot s;
        s.along = e.slot.x;
        s.side = e.slot.y;
        unit_half_extents(*it, s.hx, s.hy);
        slots.push_back(s);
    }
    const objects::TypeIndex t = st.type(ch);
    if (!slots.empty() && t < catalog_->types().size()) jiggle = catalog_->types().get_float(t, "MaxJiggleDistance");
    facing = st.facing(ch);
    return true;
}

bool TacticalBattle::squad_joint_box(const objects::ObjectStore& st, objects::ObjectId id, float& hx, float& hy) const {
    hx = hy = 0.0f;
    std::vector<tactical::abilities::SquadSlot> slots;
    float jiggle = 0.0f;
    Vec3 facing{1.0f, 0.0f, 0.0f};
    if (!squad_joint_slots(st, id, slots, jiggle, facing)) return false;
    tactical::abilities::squad_joint_half_extents(slots, jiggle, hx, hy);
    return true;
}

void TacticalBattle::bt_landing_points(TacticalMode& mode) {
    // §3.3: reinforcement / capture points keep their map owner (mini 02); nothing reassigns them at battle start. The
    // attacker's landing points are the REINFORCEMENT_POINT objects it owns from the map (store order, sorted by id).
    if (space_) return;
    static const objects::BehaviorTypeId kPoint = objects::behavior_type_id("REINFORCEMENT_POINT");
    if (kPoint == objects::kNoBehavior) return;
    const int32_t attacker = req_.info.attacker;
    if (attacker < 0) return;
    const objects::ObjectStore& st = mode.primary();
    std::vector<objects::ObjectId> mine;
    for (const objects::ObjectHandle h : st.all_objects())
        if (st.active(h) && st.has_behavior(h, kPoint) && st.owner(h) == attacker) mine.push_back(st.id(h));
    if (mine.empty()) {
        // [probe 2026-10-29] the attacker lands on a Neutral (map owner 3) reinforcement point - 3 / 17 units from its
        // start centroid - that it owns at the first frame; every other point stays Neutral. [inferred] the point = the
        // Is_Neutral-owned REINFORCEMENT_POINT nearest the attacker's anchor (Attacker Entry marker, else the fallback),
        // 2-D squared distance, ties -> lower id. [port] also for the port's skirmish invasions.
        objects::ObjectId best = objects::kNoObjectId;
        double best_d = 0.0;
        for (const objects::ObjectHandle h : st.all_objects()) {
            if (!st.active(h) || !st.has_behavior(h, kPoint) || !bt_owner_neutral(st.owner(h))) continue;
            const Vec3 p = st.position(h);
            const double dx = static_cast<double>(p.x) - static_cast<double>(anchor_[1].x);
            const double dy = static_cast<double>(p.y) - static_cast<double>(anchor_[1].y);
            const double dd = dx * dx + dy * dy;
            const objects::ObjectId id = st.id(h);
            if (best == objects::kNoObjectId || dd < best_d || (dd == best_d && id < best)) {
                best = id;
                best_d = dd;
            }
        }
        if (best != objects::kNoObjectId) {
            const objects::ObjectHandle h = mode.primary().find(best);
            if (script_change_owner(mode, h, attacker)) {
                s4_capture_grant(mode, best, attacker);
                mine.push_back(best);
            }
        }
    }
    std::sort(mine.begin(), mine.end());
    bt_landing_ = mine;
    if (!mine.empty()) bt_used_ = true;
}

// ---- map load (REMAINING_GAPS §3.1) ---------------------------------------------------------------------------------

namespace {

enum MarkerKind : uint8_t { kMkOther = 0, kMkStation, kMkOrbital, kMkInBase, kMkOutBase, kMkDefend, kMkAttack, kMkDeathStar };

uint8_t marker_kind(uint32_t crc) {
    static const uint32_t station = name_crc("Space Station Position"), orbital = name_crc("Orbital Special Structure Position"),
                          in_base = name_crc("In Base Special Structure Position"),
                          out_base = name_crc("Out Base Special Structure Position"), defend = name_crc("Defending Forces Position"),
                          attack = name_crc("Attacker Entry Position"), death_star = name_crc("DEATH_STAR_POSITION");
    if (crc == station) return kMkStation;
    if (crc == orbital) return kMkOrbital;
    if (crc == in_base) return kMkInBase;
    if (crc == out_base) return kMkOutBase;
    if (crc == defend) return kMkDefend;
    if (crc == attack) return kMkAttack;
    if (crc == death_star) return kMkDeathStar;
    return kMkOther;
}

// Mini 0x0B of the placement (0x459 object chunk): the base level byte (obj +0x39c); absent = 0.
int32_t placement_base_level(const terrain::ObjectPlacement& pl) {
    const terrain::RawMini* m = terrain::find_mini(pl.placement_raw, 0x0B);
    return (m && !m->data.empty()) ? static_cast<int32_t>(m->data[0]) : 0;
}

// t50: the slot of a skirmish team marker "Team_NN_<what>" (MultiplayerStructureMarker / Marker types of the MP maps): NN as a
// number, -1 when the name has another shape. [M] team NN = skirmish slot NN.
int32_t team_marker_slot(std::string_view name) {
    if (name.size() < 9) return -1;
    static constexpr std::string_view kTeam = "TEAM_";
    if (!ascii_istarts_with(name, kTeam)) return -1;
    if (name[5] < '0' || name[5] > '9' || name[6] < '0' || name[6] > '9' || name[7] != '_') return -1;
    return (name[5] - '0') * 10 + (name[6] - '0');
}

// Facing of a map placement: its yaw rotates the model as authored, whose nose is model -Y (ALO convention), so the
// facing is Rz(yaw) x (0, -1, 0) (render::object_transform of it equals the placement transform).
Vec3 yaw_facing(float yaw_deg) {
    float s = 0.0f, c = 1.0f;
    dmath::sincos(yaw_deg * (dmath::kPiF / 180.0f), s, c);
    return {s, -c, 0.0f};
}

// Map_Load_Spawn_Table: {type, weight} pairs -> {type, cumulative weight} (24-byte entries, type +0x20b0). Unresolved
// names are skipped [M].
std::vector<std::pair<objects::TypeIndex, float>> spawn_table(const objects::GameObjectTypes& types, objects::TypeIndex t) {
    std::vector<std::pair<objects::TypeIndex, float>> out;
    const std::span<const std::string_view> list = types.get_list(t, "Map_Load_Spawn_Table");
    float cum = 0.0f;
    for (size_t i = 0; i + 1 < list.size(); i += 2) {
        const objects::TypeIndex x = types.resolve_name(ascii_trim(list[i]));
        const float w = std::strtof(std::string(ascii_trim(list[i + 1])).c_str(), nullptr);
        if (x == objects::kNone || x >= types.size()) continue;
        cum = cum + w;
        out.emplace_back(x, cum);
    }
    return out;
}

}  // namespace

std::vector<std::pair<bool, Vec3>> TacticalBattle::bt_start_markers() const {
    std::vector<std::pair<bool, Vec3>> out;
    for (const MapMarker& m : bt_markers_)
        if (m.kind == kMkDefend || m.kind == kMkAttack) out.emplace_back(m.kind == kMkDefend, m.pos);
    return out;
}

bool TacticalBattle::bt_owner_valid(int32_t owner) const {
    return owner >= 0 && static_cast<size_t>(owner) < galaxy_->players().size() && static_cast<uint32_t>(owner) < bt_players_;
}

bool TacticalBattle::bt_owner_neutral(int32_t owner) const {
    if (owner < 0 || static_cast<size_t>(owner) >= galaxy_->players().size()) return false;
    const uint32_t f = galaxy_->players()[static_cast<size_t>(owner)].faction;
    return f < galaxy_->rules().factions.size() && galaxy_->rules().factions[f].is_neutral;
}

int32_t TacticalBattle::bt_relation(int32_t p, int32_t q) const {
    // S7 t30 (TACTICAL_COMBAT §2.10) [H]. Players without a galactic player: enemies of everyone else (the old default).
    const auto& ps = galaxy_->players();
    if (p < 0 || q < 0 || static_cast<size_t>(p) >= ps.size() || static_cast<size_t>(q) >= ps.size()) return p == q ? 0 : 1;
    const auto& fs = galaxy_->rules().factions;
    const uint32_t fp = ps[static_cast<size_t>(p)].faction, fq = ps[static_cast<size_t>(q)].faction;
    const bool np = bt_owner_neutral(p), nq = bt_owner_neutral(q);
    if (cfg_.skirmish) {  // 0x297300: every pair overwritten, the XML lists ignored
        if (np) return 2;  // its own entry too
        if (p == q) return 0;
        if (nq) return 2;
        return fp == fq ? 0 : 1;
    }
    // Campaign (Create_Default_Players 0x294360). [port] once a script wrote the galactic table (Make_Ally / Make_Enemy),
    // it is the source: enemy there = enemy here, anything else 0 / 2.
    if (p == q) return 0;
    if (!galaxy_->diplomacy_state_default()) {
        const galactic::Relation r = galaxy_->relation(p, q);
        if (r == galactic::Relation::Enemy) return 1;
        return r == galactic::Relation::Neutral ? 2 : 0;
    }
    if (np || nq) return 2;
    if (fp == fq) return 0;
    if (fp >= fs.size() || fq >= fs.size()) return 1;
    // [port] a faction with neither list stays an enemy of every other non-neutral faction (the old default): only the
    // synthetic test data has such factions; every shipped non-neutral faction lists its Enemies.
    if (fs[fp].allies.empty() && fs[fp].enemies.empty()) return 1;
    const std::string& gname = fs[fq].name;
    int32_t v = 2;  // the constructor's value ("undefined feelings")
    for (const std::string& n : fs[fp].allies)
        if (ascii_iequals(n, gname)) v = 0;
    for (const std::string& n : fs[fp].enemies)  // Enemies wins over Allies
        if (ascii_iequals(n, gname)) v = 1;
    return v;
}

int32_t TacticalBattle::bt_map_owner(uint32_t raw) const {
    // [probe 2026-10-29] (tools/probe_mod/s10_map_owners): a .ted owner (mini 02) is a faction index (FactionFiles order)
    // resolved to that faction's player; a native faction without a player belongs to the Hostile player (Hutts props
    // -> Hostile in skirmish). [inferred] "native" = a faction neither Is_Playable nor Is_Neutral; the Hostile player =
    // the first player of GalacticRules::native_owner_faction (modc t03: by data; FoC: Hostile). Anything else without a
    // player: -1 (dropped as an unknown id).
    const auto& ps = galaxy_->players();
    const auto& fs = galaxy_->rules().factions;
    for (size_t p = 0; p < ps.size(); ++p)
        if (ps[p].faction == raw) return static_cast<int32_t>(p);
    if (raw >= fs.size() || fs[raw].is_playable || fs[raw].is_neutral) return -1;
    const uint32_t native = galaxy_->rules().native_owner_faction();
    for (size_t p = 0; native != galactic::kNoIndex && p < ps.size(); ++p)
        if (ps[p].faction == native) return static_cast<int32_t>(p);
    return -1;
}

void TacticalBattle::bt_map_prepass() {
    // §3.1 items 2-4, in map object order (no draws): an object whose base level (mini 0x0B) exceeds the battle's base
    // level (D +0x130 land / +0x148 space) is destroyed; an unknown owner id is destroyed ("An object was saved with a
    // Player ID that no longer exists!"); the branched-map discard (single player, defender given, type
    // Is_Branched_Map_Discardable) keeps only owner = defender, Loads_When_Faction_Present and owner = attacker, an owner
    // faction without Can_Control_Planets, or an Is_Marker type [M: the behaviour test 0x370320 (arguments lost) is not
    // modelled]; story t10: a linked battle (the [M] global 0x140b155d8 != -1, [port] = BattleRequest::linked) also keeps
    // owner = attacker (Kessel UM01's jailed UMP_TYBER_ZANN). The dropped placements leave map_ (no grid stamp, no spawn). The
    // kept placements' selectable / mobile types and every Map_Load_Spawn_Table entry join the battle tables.
    const objects::GameObjectTypes& types = catalog_->types();
    const TypeCrcIndex index(types);
    const int32_t base = req_.base_level, def = req_.info.defender, att = req_.info.attacker;
    auto can_control = [this](int32_t o) {
        const uint32_t f = galaxy_->players()[static_cast<size_t>(o)].faction;
        return f < galaxy_->rules().factions.size() && galaxy_->rules().factions[f].can_control_planets;
    };
    std::vector<terrain::ObjectPlacement> kept;
    kept.reserve(map_.objects.size());
    const size_t before = types_used_.size();
    auto use = [this](uint32_t t) {
        if (std::find(types_used_.begin(), types_used_.end(), t) == types_used_.end()) types_used_.push_back(t);
    };
    auto combat_candidate = [&types, this](objects::TypeIndex t) {
        return (types.has_behavior(t, objects::BehaviorSet::Union, objects::beh::Locomotor) ||
                types.has_behavior(t, objects::BehaviorSet::Union, objects::beh::Selectable)) &&
               rules_->type(static_cast<uint32_t>(t)).members.empty();
    };
    // t50: land skirmish team markers ("Team_NN_Command Center", "_Barracks", "_BuildPads_Position", "_Reinforcement_Point_Plus2_Cap",
    // ...): the marker stands for the pad (or point) of slot NN's faction, i.e. the Marker_For_Specific_Object_Type entry whose
    // Affiliation names that faction. [M] the retail skirmish setup creates these at game start; the rule is read from the
    // marker data (MULTIPLAYER_STRUCTURE_MARKERS.XML), the slot -> player map is skirmish_slot_player. The pad takes the marker's place.
    const auto& type_crcs = types.raw().types().name_crc;
    auto team_pad = [&](const terrain::ObjectPlacement& marker, objects::TypeIndex mt, terrain::ObjectPlacement& out) -> bool {
        const int32_t slot = team_marker_slot(types.raw().types().name[mt]);
        const int32_t player = slot >= 0 ? skirmish_slot_player(slot) : -1;
        if (player < 0 || static_cast<size_t>(player) >= galaxy_->players().size()) return false;
        const uint32_t f = galaxy_->players()[static_cast<size_t>(player)].faction;
        if (f >= galaxy_->rules().factions.size()) return false;
        const std::string& faction = galaxy_->rules().factions[f].name;
        for (const std::string_view tok : types.get_list(mt, "Marker_For_Specific_Object_Type")) {
            const objects::TypeIndex x = types.resolve_name(ascii_trim(tok));
            if (x == objects::kNone || x >= types.size() || x >= type_crcs.size()) continue;
            for (const std::string_view a : types.get_list(x, "Affiliation"))
                if (iequal_trim(a, faction)) {
                    out = marker;
                    out.type_crc = type_crcs[x];
                    out.owner = static_cast<uint32_t>(player);
                    return true;
                }
        }
        return false;
    };
    for (terrain::ObjectPlacement& pl : map_.objects) {
        if (placement_base_level(pl) > base) continue;
        if (skirmish_land() || skirmish_map()) {  // t50: the team marker is replaced by its pad (the marker is not kept: it would block the pad)
            const objects::TypeIndex mt = index.find(pl.type_crc);
            terrain::ObjectPlacement pad;
            if (mt != objects::kNone && mt < types.size() && team_pad(pl, mt, pad)) {
                const objects::TypeIndex et = index.find(pad.type_crc);
                if (et != objects::kNone && et < types.size()) {
                    if (combat_candidate(et)) use(static_cast<uint32_t>(et));
                    for (const auto& [x, w] : spawn_table(types, et))
                        if (combat_candidate(x)) use(static_cast<uint32_t>(x));
                }
                kept.push_back(std::move(pad));
                continue;
            }
        }
        const int32_t o = bt_map_owner(pl.owner);  // [probe 2026-10-29] faction index -> player
        if (!bt_owner_valid(o)) continue;
        pl.owner = static_cast<uint32_t>(o);  // map_ owners are player ids from here on (bt_map_load, markers)
        const objects::TypeIndex t = index.find(pl.type_crc);
        if (t != objects::kNone && t < types.size()) {
            if (def >= 0 && !cfg_.map_battle && types.get_bool(t, "Is_Branched_Map_Discardable")) {
                const bool keep = o == def || ((req_.linked || types.get_bool(t, "Loads_When_Faction_Present")) && o == att) ||
                                  !can_control(o) || types.get_bool(t, "Is_Marker");
                if (!keep) continue;
            }
            if (combat_candidate(t)) use(static_cast<uint32_t>(t));
            for (const auto& [x, w] : spawn_table(types, t))
                if (combat_candidate(x)) use(static_cast<uint32_t>(x));
        }
        kept.push_back(std::move(pl));
    }
    map_.objects = std::move(kept);
    if (types_used_.size() != before) tables_extend();
}

void TacticalBattle::bt_map_load(TacticalMode& mode, SimRng& rng) {
    // §3.1 in map object order. Map_Load_Spawn_Table (skipped when 0x140b15490 is set [port: never]): one synced float(0, 1)
    // r1; a spawn only when r1 >= 1 - W (W = the last cumulative weight); then r2 = synced float(0, W) [M upper bound], the
    // first cumulative >= r2; owner = the first player whose faction is in the chosen type's Affiliation, else the
    // marker's owner; the placeholder is removed. Spawned objects:
    //   * CAPTURE_POINT / garrison structures without a locomotor: S4 placement with the map owner (§3.3); the selectable
    //     ones are also third-party battle combat units of that owner (any owner, Is_Neutral included);
    //   * selectable / mobile objects (map units, turrets, base layouts) of a non-Is_Neutral owner: battle combat units
    //     (map participants; a third party when owned by neither side) [port: Is_Neutral-owned ones stay inert store
    //     objects (every player is an enemy of every other in CombatSystem); company types placed on the map are not
    //     expanded and are skipped];
    //   * everything else (props, markers): inert store objects.
    // MARKER objects with an owner are remembered for §3.2 (bt_markers_); the start markers are also recognised by name
    // when the type is missing from the data [port: synthetic maps].
    const objects::GameObjectTypes& types = catalog_->types();
    const TypeCrcIndex index(types);
    static const objects::BehaviorTypeId kMarker = objects::behavior_type_id("MARKER");
    const auto& names = types.raw().types().name_crc;
    // S8 t01 / S7 t13: a placement's LAND_OBSTACLE stamp / SPACE_OBSTACLE static entry (id mv::map_stamp_id) moves to the
    // object spawned for it, so combat's per-object cell query (PassabilityGrid::occupies, the slot-43 aim walk) finds it and
    // the entry leaves with the object (land_obstacles_sync / space_obstacles_sync). A Map_Load_Spawn_Table pick drops the
    // placeholder's footprint and gets the spawned type's (MOVEMENT §1.3 / §2.2: the behaviours stamp on attach, the
    // placeholder is removed); a placement that spawns nothing (no pick, a skipped map company, a failed spawn) loses it.
    auto own_obstacles = [&](const terrain::ObjectPlacement& p, objects::TypeIndex spawned, objects::ObjectHandle h) {
        if (!movement_) return;
        const mv::ObjectId from = mv::map_stamp_id(static_cast<size_t>(&p - map_.objects.data()));
        const bool live = mode.primary().alive(h);
        const bool same = live && spawned == index.find(p.type_crc);
        if (space_) {
            mv::SpaceOccupancy& occ = movement_->space();
            if (same && occ.rekey_static(from, mode.primary().id(h))) return;
            occ.remove_static(from);
            if (live) space_obstacle_attach(mode, h);
        } else {
            mv::PassabilityGrid& grid = movement_->grid();
            if (same && grid.rekey(from, mode.primary().id(h))) return;
            grid.unstamp(from);
            if (live && mode.primary().has_behavior(h, objects::beh::LandObstacle)) land_obstacle_attach(mode, h);
        }
    };
    // AI_GALACTIC_TOKENS §9.4: a pad's base level byte +0x39C (mini 0x0B) for the persistence add (non-zero ones only).
    auto note_pad = [&](const terrain::ObjectPlacement& p, objects::ObjectHandle h) {
        const int32_t level = placement_base_level(p);
        if (level == 0 || !mode.primary().alive(h) || pad_behavior() == objects::kNoBehavior || !mode.primary().has_behavior(h, pad_behavior()))
            return;
        const objects::ObjectId id = mode.primary().id(h);
        const auto it = std::lower_bound(pad_levels_.begin(), pad_levels_.end(), id,
                                         [](const std::pair<objects::ObjectId, int32_t>& e, objects::ObjectId v) { return e.first < v; });
        if (it == pad_levels_.end() || it->first != id) pad_levels_.insert(it, {id, level});
    };
    for (const terrain::ObjectPlacement& pl : map_.objects) {
        objects::TypeIndex t = index.find(pl.type_crc);
        int32_t owner = static_cast<int32_t>(pl.owner);
        const Vec3 pos{pl.position[0], pl.position[1], pl.position[2]};
        const Vec3 face = yaw_facing(pl.rotation_deg[2]);
        if (t == objects::kNone || t >= types.size()) {
            const uint8_t k = marker_kind(pl.type_crc);
            if (k == kMkDeathStar && !bt_have_ds_) {
                bt_ds_pos_ = {pos.x, pos.y, space_ ? 0.0f : pos.z};
                bt_have_ds_ = true;
            } else if (k != kMkOther) {
                MapMarker m;
                m.kind = k;
                m.owner = owner;
                m.pos = pos;
                m.facing = face;
                bt_markers_.push_back(m);
            }
            continue;
        }
        if (const auto table = spawn_table(types, t); !table.empty()) {
            const float w = table.back().second;
            const float r1 = rng.range_float(0.0f, 1.0f);
            if (!(r1 >= 1.0f - w)) {  // no spawn: the placeholder goes
                own_obstacles(pl, objects::kNone, objects::ObjectHandle{});
                continue;
            }
            const float r2 = rng.range_float(0.0f, w);
            objects::TypeIndex pick = table.back().first;
            for (const auto& [x, cum] : table)
                if (cum >= r2) {
                    pick = x;
                    break;
                }
            t = pick;
            bt_used_ = true;
            int32_t aff = -1;
            const std::span<const std::string_view> af = types.get_list(t, "Affiliation");
            for (size_t p = 0; p < galaxy_->players().size() && aff < 0; ++p) {
                const uint32_t f = galaxy_->players()[p].faction;
                if (f >= galaxy_->rules().factions.size()) continue;
                for (const std::string_view a : af)
                    if (iequal_trim(a, galaxy_->rules().factions[f].name)) {
                        aff = static_cast<int32_t>(p);
                        break;
                    }
            }
            if (aff >= 0 && bt_owner_valid(aff)) owner = aff;
        }
        objects::SpawnParams sp;
        sp.type = t;
        sp.owner = owner;
        sp.position = pos;
        sp.facing = face;
        const uint8_t kind = marker_kind(names[t]);
        if (kind == kMkDeathStar && !bt_have_ds_) {  // battle host: super-weapon spawner 0x2a00b0 (TACTICAL_GAPS §4.8)
            bt_ds_pos_ = {pos.x, pos.y, space_ ? 0.0f : pos.z};
            bt_have_ds_ = true;
        }
        const bool mobile = types.has_behavior(t, objects::BehaviorSet::Union, objects::beh::Locomotor);
        const bool selectable = types.has_behavior(t, objects::BehaviorSet::Union, objects::beh::Selectable);
        objects::ObjectHandle placed{};
        if (!mobile && s4_placement(mode, t, sp, pl.health_fraction, &placed)) {  // S4: capture points / garrison structures
            own_obstacles(pl, t, placed);
            note_pad(pl, placed);
            // a1f t07: a SELECTABLE one (Secondary_Abandoned_Sensor: Tactical_Health 700, Structure, Neutral at start) is a
            // combat object too, so Destroy_Structure / attack orders can kill it; whatever its map owner (the Is_Neutral
            // player's objects are skipped by the target scan, TACTICAL_COMBAT §2.2). [port] a third party: never decides
            // victory nor counts in a side's power (the shipped ones are Victory_Relevant No).
            if (selectable && placed && rules_->type(static_cast<uint32_t>(t)).members.empty()) {
                const uint32_t pi = bt_adopt_built(mode, placed, owner, pl.health_fraction, &rng, true);
                if (pi < parts_.size()) parts_[pi].third = true;
            }
            continue;
        }
        if (mobile || selectable) {
            if (!rules_->type(static_cast<uint32_t>(t)).members.empty()) {  // [port] map-placed companies
                own_obstacles(pl, objects::kNone, objects::ObjectHandle{});
                continue;
            }
            const objects::ObjectHandle h = mode.spawn(sp, pl.health_fraction);
            own_obstacles(pl, t, h);
            note_pad(pl, h);
            if (!bt_owner_neutral(owner)) bt_adopt_built(mode, h, owner, pl.health_fraction, &rng, true);
            continue;
        }
        const objects::ObjectHandle h = mode.spawn(sp, pl.health_fraction);
        own_obstacles(pl, t, h);
        note_pad(pl, h);
        if (kMarker != objects::kNoBehavior && types.has_behavior(t, objects::BehaviorSet::Union, kMarker) && owner >= 0) {
            MapMarker m;
            m.id = mode.primary().exists(h) ? mode.primary().id(h) : objects::kNoObjectId;
            m.type = t;
            m.kind = kind;
            m.owner = owner;
            m.pos = pos;
            m.facing = face;
            for (const std::string_view s : types.get_list(t, "Marker_For_Specific_Object_Type")) {
                const objects::TypeIndex x = types.resolve_name(ascii_trim(s));
                if (x != objects::kNone && x < types.size()) m.specific.push_back(x);
            }
            bt_markers_.push_back(std::move(m));
        } else if (kind != kMkOther && kind != kMkDeathStar) {  // a start marker type without MARKER behaviour [port]
            MapMarker m;
            m.type = t;
            m.kind = kind;
            m.owner = owner;
            m.pos = pos;
            m.facing = face;
            bt_markers_.push_back(std::move(m));
        }
    }
}

// ---- post-load placement of the galactic forces (REMAINING_GAPS §3.2, 0x2a1eb0) -------------------------------------

void TacticalBattle::bt_place_forces(TacticalMode& mode, SimRng& rng, const ParallelFor& pf) {
    const objects::GameObjectTypes& types = catalog_->types();
    const int32_t def = req_.info.defender, att = req_.info.attacker;
    auto flat = [this](Vec3 p) { return space_ ? Vec3{p.x, p.y, 0.0f} : p; };
    auto of_kind = [this](uint8_t k) {
        std::vector<size_t> out;
        for (size_t i = 0; i < bt_markers_.size(); ++i)
            if (bt_markers_[i].kind == k) out.push_back(i);
        return out;
    };
    auto take = [&](size_t i, Participant& p) {
        bt_markers_[i].used = true;
        p.spots.emplace_back(flat(bt_markers_[i].pos), bt_markers_[i].facing);
    };
    // 1. Starbase (space): one of the Space Station markers, synced int(0, n - 1) when n >= 2 [port: the galactic starbase
    //    participant is placed there; DUMMY_STAR_BASE / Base_Level and the super-weapon registration are not modelled].
    for (Participant& p : parts_) {
        if (!p.starbase || p.reserve) continue;
        const std::vector<size_t> st = of_kind(kMkStation);
        if (!st.empty()) {
            const size_t k = st.size() >= 2 ? static_cast<size_t>(rng.range_int(0, static_cast<int32_t>(st.size()) - 1)) : 0u;
            take(st[k], p);
            bt_used_ = true;
        }
        break;
    }
    // Structure markers present at all? [port] maps without any keep the formation fallback for the structures.
    bool structure_markers = false;
    for (const MapMarker& m : bt_markers_)
        structure_markers = structure_markers || m.kind == kMkOrbital || m.kind == kMkInBase || m.kind == kMkOutBase || !m.specific.empty();
    // 2. Tactical_Additional_Structure_Type (+0x8d0): a temporary structure of that type for the defender when the planet has
    //    none yet (placed by step 4; the galactic temporary is killed after the placement: no galactic counterpart).
    const size_t n0 = parts_.size();
    for (size_t i = 0; i < n0; ++i) {
        if (!parts_[i].structure || parts_[i].built) continue;
        const objects::TypeIndex x = types.resolve_name(ascii_trim(types.get_string(parts_[i].type, "Tactical_Additional_Structure_Type")));
        if (x == objects::kNone || x >= types.size()) continue;
        bool have = false;
        for (const Participant& q : parts_) have = have || (q.structure && q.type == static_cast<uint32_t>(x));
        if (have) continue;
        Participant p;
        p.owner = def;
        p.type = static_cast<uint32_t>(x);
        p.side = 0;
        p.structure = true;
        p.built = true;
        parts_.push_back(p);
        bt_used_ = true;
    }
    // 3. Place_Other_Type_At_Every_Specific_Marker_Position (+0x22f0): the other type at every unused specific marker that
    //    lists it, for the structure's owner (the spawned objects carry the structure's galactic id) [M: the land / space
    //    structure behaviour slot filter is not modelled].
    for (Participant& p : parts_) {
        if (!p.structure) continue;
        const objects::TypeIndex o = types.resolve_name(ascii_trim(types.get_string(p.type, "Place_Other_Type_At_Every_Specific_Marker_Position")));
        if (o == objects::kNone || o >= types.size()) continue;
        p.other_type = static_cast<int32_t>(o);
        for (size_t i = 0; i < bt_markers_.size(); ++i) {
            const MapMarker& m = bt_markers_[i];
            if (!m.used && std::find(m.specific.begin(), m.specific.end(), o) != m.specific.end()) take(i, p);
        }
        p.unplaced = p.spots.empty();
        bt_used_ = true;
    }
    // 4. Other structures, pass 0 [port: pass 1 (the remembered marker +0x3b0 of the planet's structures) has no galactic
    //    storage; the marker / slot written back is not kept]: a specific marker listing the type, else the Base_Position
    //    group (Orbital / In_Base / Out_Base; In_Base falls over to out-base and vice versa), else any free in-base then
    //    out-base marker; none left -> not spawned. Spawned for the record defender. [M] the space special-weapon rule.
    if (structure_markers)
        for (Participant& p : parts_) {
            if (!p.structure || p.starbase || p.other_type >= 0) continue;
            auto first_free = [&](uint8_t k) -> int64_t {
                for (size_t i = 0; i < bt_markers_.size(); ++i)
                    if (!bt_markers_[i].used && bt_markers_[i].kind == k) return static_cast<int64_t>(i);
                return -1;
            };
            int64_t pick = -1;
            for (size_t i = 0; i < bt_markers_.size() && pick < 0; ++i) {
                const MapMarker& m = bt_markers_[i];
                if (!m.used && std::find(m.specific.begin(), m.specific.end(), static_cast<objects::TypeIndex>(p.type)) != m.specific.end())
                    pick = static_cast<int64_t>(i);
            }
            if (pick < 0) {
                const std::string_view bp = ascii_trim(types.get_string(p.type, "Base_Position"));
                if (iequal_trim(bp, "Orbital")) pick = first_free(kMkOrbital);
                else if (iequal_trim(bp, "In_Base")) pick = first_free(kMkInBase) >= 0 ? first_free(kMkInBase) : first_free(kMkOutBase);
                else if (iequal_trim(bp, "Out_Base")) pick = first_free(kMkOutBase) >= 0 ? first_free(kMkOutBase) : first_free(kMkInBase);
            }
            if (pick < 0) pick = first_free(kMkInBase) >= 0 ? first_free(kMkInBase) : first_free(kMkOutBase);
            if (pick < 0) {
                p.unplaced = true;
                continue;
            }
            take(static_cast<size_t>(pick), p);
            p.owner = def >= 0 ? def : p.owner;
            bt_used_ = true;
        }
    // Deploy: the starbase, then the structures (participant order), then the start forces.
    for (uint32_t i = 0; i < parts_.size(); ++i)
        if (parts_[i].starbase && !parts_[i].reserve) deploy(mode, i, rng);
    for (uint32_t i = 0; i < parts_.size(); ++i)
        if (parts_[i].structure && !parts_[i].reserve) {
            if (parts_[i].unplaced) parts_[i].deployed = true;  // not spawned: never deployed later either
            else deploy(mode, i, rng);
        }
    land_obstacles_sync(mode, pf);  // S8 t02: the starbase / structures stamp on attach, before the start forces are placed
    // 6 / 7. Facing slots and anchors: the first Defending Forces marker (position, facing); the attacker's entry marker =
    //    the farthest from the first non-attacker DUMMY_GROUND_STRUCTURE with a non-zero Reinforcement_Prevention_Radius
    //    [M: the 0x2a9ff0 filter], else the nearest to the first Defending Forces marker. Space: the attacker looks at the
    //    last Defending Forces marker. [port] Formation placement 0x2a00b0 = rows of 6 behind the anchor along the facing;
    //    Space_Escort_Unit_Types and the planet-specific upgrades (step 5) are not spawned.
    const bool skirmish_records = skirmish_land() || skirmish_map();
    // s78n t20 (SKIRMISH_SETUP §2.2 [H]): a land skirmish places its start forces at the spawn-point markers only. Before, a map
    // that also carries campaign markers (_MP_LAND_UTAPAU has a Defending Forces Position at (1671, 4620), next to the
    // Rebel spawn) moved side 0's anchor onto it: the Empire's start infantry stood beside the Rebel base, partly snapped
    // into a pocket between a structure footprint and a cliff that no path leaves.
    const uint8_t spawn_marked = skirmish_records ? bt_skirmish_anchors() : uint8_t{0};  // t50; skai: space skirmish map battles too
    const std::vector<size_t> dm = of_kind(kMkDefend), am = of_kind(kMkAttack);
    if (!dm.empty() && !(spawn_marked & 1u)) {
        anchor_[0] = flat(bt_markers_[dm.front()].pos);
        facing_[0] = bt_markers_[dm.front()].facing;
        // a1h t06: mode +0x288 record of the defender (TACTICAL_GAPS §4.2.2 item 1). s78i t02: a skirmish writes its
        // records from the spawn-point markers instead (bt_skirmish_anchors; TACTICAL_COMBAT §3.1.1 "other spawns").
        if (!skirmish_records) drop_facing_[0] = facing_[0];
    }
    if (!am.empty() && !(spawn_marked & 2u)) {
        static const objects::BehaviorTypeId kDummy = objects::behavior_type_id("DUMMY_GROUND_STRUCTURE");
        const objects::ObjectStore& st = mode.primary();
        bool have_block = false;
        Vec3 block{};
        if (kDummy != objects::kNoBehavior)
            for (const objects::ObjectHandle h : st.all_objects()) {
                if (!st.active(h) || !st.has_behavior(h, kDummy) || st.owner(h) == att) continue;
                if (types.get_float(st.type(h), "Reinforcement_Prevention_Radius") == 0.0f) continue;
                block = st.position(h);
                have_block = true;
                break;
            }
        size_t best = am.front();
        float best_d = 0.0f;
        for (size_t k = 0; k < am.size(); ++k) {
            const Vec3 p = bt_markers_[am[k]].pos;
            const Vec3 ref = have_block ? block : (dm.empty() ? p : bt_markers_[dm.front()].pos);
            const float dx = p.x - ref.x, dy = p.y - ref.y, d = dx * dx + dy * dy;
            if (k == 0 || (have_block ? d > best_d : d < best_d)) {
                best = am[k];
                best_d = d;
            }
        }
        anchor_[1] = flat(bt_markers_[best].pos);
        facing_[1] = bt_markers_[best].facing;
        // a1h t06: the attacker's +0x288 record is written only when its start list is non-empty (record +0x28).
        if (!skirmish_records &&
            std::any_of(parts_.begin(), parts_.end(), [](const Participant& q) { return q.side == 1 && !q.reserve && !q.third; }))
            drop_facing_[1] = facing_[1];
        if (space_ && !dm.empty()) {
            const Vec3 to{bt_markers_[dm.back()].pos.x - anchor_[1].x, bt_markers_[dm.back()].pos.y - anchor_[1].y, 0.0f};
            const float len = dmath::sqrt(to.x * to.x + to.y * to.y);
            if (len > 0.0f) facing_[1] = Vec3{to.x / len, to.y / len, 0.0f};
        }
    }
    // [port] a side without its marker faces the other side's anchor.
    for (int s = 0; s < 2; ++s) {
        if (s == 0 ? !dm.empty() : !am.empty()) continue;
        if (spawn_marked & (1u << s)) continue;  // s78n t20: the marker's own facing
        const Vec3 d{anchor_[1 - s].x - anchor_[s].x, anchor_[1 - s].y - anchor_[s].y, 0.0f};
        const float len = dmath::sqrt(d.x * d.x + d.y * d.y);
        facing_[s] = len > 0.0f ? Vec3{d.x / len, d.y / len, 0.0f} : Vec3{s == 0 ? -1.0f : 1.0f, 0.0f, 0.0f};
    }
    for (uint8_t side = 0; side < 2; ++side) {
        // cu8l t03 (CU8K_T03_T08_T09_RE §1.1, §4 item 1; replaces cu8j's count-only second adopt): the campaign spawn
        // 0x2a00b0 (one call per side: the defender, then the attacker) registers, per galactic unit that spawned
        // something (group k, k := k + 1 wrapping at the side's marker count), each unit it creates whose own Ship_Class
        // is 0..6 (the unit made from the galactic object itself: not in the Super category) as itself (before the team
        // helper: placement skips it as a team member), then the first chunk's TEAM container when its first member's
        // Ship_Class is 0..6 (later chunks are neither registered nor placed). In space the fleet placement 0x4fbb10
        // runs at the end: the defender on the DEFENDING FORCES markers, the attacker on the ATTACKER ENTRY markers
        // (map order [M]).
        const bool placement = space_ && !cfg_.skirmish;
        const std::vector<size_t>& marks = side == 0 ? dm : am;
        std::vector<FleetEntry> reg;
        uint32_t group = 0;
        for (uint32_t i = 0; i < parts_.size(); ++i)
            if (!parts_[i].reserve && !parts_[i].starbase && !parts_[i].structure && !parts_[i].built && parts_[i].side == side) {
                const size_t u0 = units_.size(), c0 = containers_.size();
                deploy(mode, i, rng);
                if (!placement || units_.size() <= u0) continue;
                const uint32_t g = group;
                group = marks.empty() || group + 1 < marks.size() ? group + 1 : 0u;
                const bool team = !rules_->type(parts_[i].type).members.empty();
                for (size_t k = u0; k < units_.size(); ++k) {
                    const uint32_t t = units_[k].type;
                    if (t >= types.size()) continue;
                    const int32_t sc = types.get_int(t, "Ship_Class");
                    if (sc < 0 || sc > 6) continue;
                    if (!team && (types.get_mask(t, "CategoryMask") & 0x40u) != 0) continue;  // CategoryMask Super [M]
                    FleetEntry e;
                    e.unit = static_cast<uint32_t>(k);
                    e.group = g;
                    reg.push_back(e);
                }
                if (containers_.size() > c0 && units_[u0].container == c0 && units_[u0].type < types.size()) {
                    const int32_t sc = types.get_int(units_[u0].type, "Ship_Class");  // the head of the member list
                    if (sc >= 0 && sc <= 6) {
                        FleetEntry e;
                        e.container = static_cast<uint32_t>(c0);
                        e.group = g;
                        reg.push_back(e);
                    }
                }
            }
        if (!placement) continue;
        bt_fleet_placement(mode, rng, reg, marks);
        // §1.9: the attacker's call (arrival flag 1 in space), with In_Game_Cinematics and 0x140b27d5e: the vf82 pass.
        // [port] 0x140b27d5e (the story link's arrival option, P9 != 0) is taken as on in every battle: the Kashyyyk run
        // (no linked battle) held it, so its start value is 1 [M], and the one link that clears it (P9 = 0: the
        // _EXEC_DEMO campaign) cannot be told from that start value by the port's LinkedPresentation.
        if (side == 1 && l1_k_.in_game_cinematics) bt_fleet_arrival(mode, rng, reg);
    }
}

// cu8l t03 (CU8K_T03_T08_T09_RE §1.3): the cull-box half extents. [M] The type's model bounds (Space_Model_Name, else the
// other models) x Scale_Factor in the object's frame - hx along the facing (the model's -Y nose), hy across it; [port]
// without model data the movement radius both ways.
mv::fleet::HalfBox TacticalBattle::fleet_box(uint32_t type) const {
    mv::fleet::HalfBox b;
    const objects::GameObjectTypes& types = catalog_->types();
    if (type >= types.size()) return b;
    float sf = types.columns().scale_factor[type];
    if (!(sf > 0.0f)) sf = 1.0f;
    if (const BattleCatalog::ModelInfo* mi = catalog_->model_info(type, true, true); mi && mi->has_bounds) {
        b.hx = 0.5f * (mi->max[1] - mi->min[1]) * sf;
        b.hy = 0.5f * (mi->max[0] - mi->min[0]) * sf;
        return b;
    }
    const mv::UnitDesc d = mv::unit_desc_from_type(types, type);
    b.hx = b.hy = d.radius;
    return b;
}

// Squadron_Offsets (type +0x998: one "x, y, z" per line, list order), as (x, y).
std::vector<mv::V2> TacticalBattle::fleet_offsets(uint32_t type) const {
    std::vector<mv::V2> out;
    const objects::GameObjectTypes& types = catalog_->types();
    if (type >= types.size()) return out;
    const std::span<const std::string_view> toks = types.get_list(type, "Squadron_Offsets");
    for (size_t i = 0; i + 1 < toks.size(); i += 3) {
        const std::string xs(ascii_trim(toks[i])), ys(ascii_trim(toks[i + 1]));
        out.push_back(mv::V2{static_cast<float>(std::strtod(xs.c_str(), nullptr)), static_cast<float>(std::strtod(ys.c_str(), nullptr))});
    }
    return out;
}

// cu8l t03 (CU8K_T03_T08_T09_RE §1.2 - §1.8, §4 items 2 / 3): per anchor (the side's markers in order) its group's
// registered objects in registration order - team members skipped, a container classed by its first member - laid out by
// mv::fleet::place_group; then per placed object in placement order: orientation := the marker's, teleport to the new
// position (own z), leave its coordinator and adopt (one int(1, FPS) when the predicate admits it: mv_adopt with leave),
// and a TEAM container's members to container + Rot(yaw) . offset with the marker's orientation (no draw).
void TacticalBattle::bt_fleet_placement(TacticalMode& mode, SimRng& rng, const std::vector<FleetEntry>& reg,
                                        const std::vector<size_t>& markers) {
    objects::ObjectStore& st = mode.primary();
    const objects::GameObjectTypes& types = catalog_->types();
    for (size_t a = 0; a < markers.size(); ++a) {
        const MapMarker& mk = bt_markers_[markers[a]];
        std::vector<mv::fleet::FleetObject> objs;
        std::vector<FleetEntry> refs;
        std::vector<std::vector<uint32_t>> members;  // per object: the live members (units_ indices), member order
        for (const FleetEntry& e : reg) {
            if (e.group != a) continue;
            mv::fleet::FleetObject o;
            std::vector<uint32_t> mem;
            if (e.unit != 0xFFFFFFFFu) {
                const Unit& u = units_[e.unit];
                if (u.container != 0xFFFFFFFFu || !u.alive || !st.alive(u.h)) continue;  // 0x39b1a0: team members skipped
                o.ship_class = types.get_int(u.type, "Ship_Class");
                o.box = fleet_box(u.type);
            } else {
                const Container& ct = containers_[e.container];
                if (ct.removed || !st.alive(ct.h)) continue;
                for (uint32_t k = 0; k < units_.size(); ++k)
                    if (units_[k].container == e.container && units_[k].alive && st.alive(units_[k].h)) mem.push_back(k);
                if (mem.empty()) continue;
                o.team = true;
                o.ship_class = types.get_int(units_[mem.front()].type, "Ship_Class");  // 0x4ec820: the first member's
                for (const uint32_t k : mem) o.members.push_back(fleet_box(units_[k].type));
                o.offsets_team = fleet_offsets(ct.type);          // team +0x50: Create_Team_Type, else the company type
                o.offsets_company = fleet_offsets(ct.squad_type); // team +0x58: the company type
                o.units_team = ct.type < types.size() ? static_cast<uint32_t>(types.get_list(ct.type, "Squadron_Units").size()) : 0u;
                o.units_company =
                    ct.squad_type < types.size() ? static_cast<uint32_t>(types.get_list(ct.squad_type, "Squadron_Units").size()) : 0u;
            }
            objs.push_back(std::move(o));
            refs.push_back(e);
            members.push_back(std::move(mem));
        }
        if (objs.empty()) continue;
        Vec3 face = mk.facing;
        if (face.x == 0.0f && face.y == 0.0f) face = Vec3{1.0f, 0.0f, 0.0f};
        const float yaw = mv::norm_deg(mv::heading_deg(mv::V2{face.x, face.y}));  // the anchor's yaw (+0x8c) [M: 0..360]
        const std::vector<mv::fleet::Placed> placed = mv::fleet::place_group(objs, mv::V2{mk.pos.x, mk.pos.y}, yaw);
        for (const mv::fleet::Placed& p : placed) {
            const FleetEntry& e = refs[p.index];
            if (e.unit != 0xFFFFFFFFu) {
                const Unit& u = units_[e.unit];
                const Vec3 at{p.pos.x, p.pos.y, st.position(u.h).z};
                (void)teleport(mode, u.id, at, face);
                ++stats_.fleet_placed;
                if (mv_adopt(mode, u.h, rng, true)) ++stats_.fleet_adopts;
                continue;
            }
            const Container& ct = containers_[e.container];
            st.set_position(ct.h, Vec3{p.pos.x, p.pos.y, st.position(ct.h).z});
            st.set_facing(ct.h, face);
            ++stats_.fleet_placed;
            if (mv_adopt(mode, ct.h, rng, true)) ++stats_.fleet_adopts;
            const std::vector<uint32_t>& mem = members[p.index];
            for (size_t j = 0; j < mem.size() && j < p.members.size(); ++j) {
                const Unit& m = units_[mem[j]];
                (void)teleport(mode, m.id, Vec3{p.members[j].x, p.members[j].y, st.position(m.h).z}, face);
            }
        }
        bt_used_ = true;
    }
}

// cu8l t03 (CU8K_T03_T08_T09_RE §1.9, §4 items 4 / 5): after every group is placed, per registration entry in registration
// order (placed or not), the object's LOCOMOTOR vf82(0): TeamLocomotor 0x62f800 (a TEAM container) leaves its coordinator
// and takes the movement lock - the first +1 splits and adopts: one int(1, FPS) - and calls every member's vf82 (Fighter
// 0x5cb020 / SimpleSpace: the hyperspace arrival, no draw); SimpleSpace 0x6251d0 (a ship) leaves its coordinator and
// starts the arrival (state 5, lock +1 [port: from here]), no draw; Fighter 0x5cb020: the arrival, no lock, no draw;
// Starship / Fleet / plain: nothing. The arrivals then run in arrival_service.
void TacticalBattle::bt_fleet_arrival(TacticalMode& mode, SimRng& rng, const std::vector<FleetEntry>& reg) {
    const objects::ObjectStore& st = mode.primary();
    auto token_is = [this](uint32_t type, std::string_view want) { return ascii_iequals(mv_locomotor_token(type), want); };
    auto has_rec = [this](objects::ObjectId id, uint32_t container) {
        return std::any_of(arrivals_.begin(), arrivals_.end(),
                           [&](const ArrivalRec& r) { return r.id == id && r.container == container; });
    };
    // A unit's vf82(0): SimpleSpace / Fighter start the arrival; a ship outside a team composes at its finish.
    auto arrive = [&](const Unit& u) {
        if (!u.alive || u.retreated || !st.alive(u.h)) return;
        const bool ship = token_is(u.type, "SIMPLE_SPACE_LOCOMOTOR");
        if (!ship && !token_is(u.type, "FIGHTER_LOCOMOTOR")) return;
        if (ship && movement_ && u.movable) (void)movement_->stop_and_leave(u.id);  // suspend, leave the coordinator
        if (has_rec(u.id, 0xFFFFFFFFu)) return;
        ArrivalRec r;
        r.id = u.id;
        r.timer = kArrivalState5;
        r.composes = ship && u.container == 0xFFFFFFFFu;
        arrivals_.push_back(r);
        ++stats_.fleet_arrivals;
    };
    for (const FleetEntry& e : reg) {
        if (e.unit != 0xFFFFFFFFu) {
            arrive(units_[e.unit]);
            continue;
        }
        const Container& ct = containers_[e.container];
        if (ct.removed || !st.alive(ct.h) || !token_is(static_cast<uint32_t>(st.type(ct.h)), "TEAM_LOCOMOTOR")) continue;
        // 0x62f800: leave; lock 0 -> 1 (0x3a8e50): split from any formation and adopt (the container is outside the
        // movement system: one draw when the predicate admits it).
        if (mv_adopt(mode, ct.h, rng, true)) ++stats_.fleet_lock_adopts;
        if (!has_rec(ct.id, e.container)) {
            ArrivalRec r;
            r.id = ct.id;
            r.container = e.container;
            r.composes = true;
            arrivals_.push_back(r);
        }
        for (const Unit& m : units_)
            if (m.container == e.container) arrive(m);
    }
    // Serviced in object order (ascending id): units first, then the containers.
    std::stable_sort(arrivals_.begin(), arrivals_.end(), [](const ArrivalRec& x, const ArrivalRec& y) {
        const bool cx = x.container != 0xFFFFFFFFu, cy = y.container != 0xFFFFFFFFu;
        return cx != cy ? !cx : x.id < y.id;
    });
}

bool TacticalBattle::arrival_locked(objects::ObjectId key) const {
    for (const ArrivalRec& r : arrivals_)
        if (r.id == key && (r.container != 0xFFFFFFFFu || r.composes)) return true;
    return false;
}

// cu8l t03: the arrival timers (L1_S4_REMAINING §3.4 "Hyperspace in", CU8K_T03_T08_T09_RE §1.9 "What follows"). [port] f
// (the arrival distance's place between the min / max of all arrivals) is 0.5, the value for equal distances: the
// arrival start points (vf +0x2a0) are not modelled. The hyperspace hold (P9 = 2) freezes the timers after the first
// frame's draw [M]. A ship outside a team finishes with a new coordinator holding where it is (hold_from_birth: one space
// composition at this tick's step 10; [port] an order the unit took meanwhile keeps running); a squadron container once
// none of its members' records is left (finished or gone): the same for its live members (one composition).
void TacticalBattle::arrival_service(TacticalMode& mode, SimRng& rng) {
    if (arrivals_.empty()) return;
    const objects::ObjectStore& st = mode.primary();
    const bool held = arrival_held();
    constexpr float kF = 0.5f;
    const int32_t steps = 5 * static_cast<int32_t>(4.0f * kF);
    std::vector<ArrivalRec> keep;
    keep.reserve(arrivals_.size());
    std::vector<objects::ObjectId> one;
    for (ArrivalRec& r : arrivals_) {
        if (r.container != 0xFFFFFFFFu) {
            keep.push_back(r);
            continue;
        }
        const Unit* u = unit_by_id(r.id);
        if (!u || !u->alive || u->retreated || !st.alive(u->h)) continue;
        if (r.timer == kArrivalState5) {
            r.timer = kArrivalState4;
        } else if (r.timer == kArrivalState4) {
            r.timer = std::clamp(rng.range_int(1, 4) + steps, 0, 24);
        } else if (!held && ++r.timer > 149) {
            if (r.composes && movement_ && u->movable) {
                one.assign(1, r.id);
                if (movement_->hold_from_birth(one)) ++stats_.fleet_arrival_ends;
            }
            continue;
        }
        keep.push_back(r);
    }
    arrivals_.swap(keep);
    keep.clear();
    for (const ArrivalRec& r : arrivals_) {
        if (r.container == 0xFFFFFFFFu) {
            keep.push_back(r);
            continue;
        }
        bool waiting = false, any = false;
        std::vector<objects::ObjectId> live;
        for (const Unit& m : units_) {
            if (m.container != r.container || !m.alive || m.retreated) continue;
            any = true;
            if (m.movable) live.push_back(m.id);
            for (const ArrivalRec& x : arrivals_)
                waiting = waiting || (x.container == 0xFFFFFFFFu && x.id == m.id);
        }
        if (waiting) {
            keep.push_back(r);
            continue;
        }
        if (any && movement_ && !live.empty() && movement_->hold_from_birth(live)) ++stats_.fleet_arrival_ends;  // 0x630390: lock -1, new coordinator
    }
    arrivals_.swap(keep);
}

// ---- build pads: attachment bone, kill list, persistence (REMAINING_GAPS §6.5) --------------------------------------

Vec3 TacticalBattle::bt_pad_attach_point(const TacticalMode& mode, objects::ObjectHandle pad) const {
    // 0x42e280: no pad model -> the pad position; else the Tactical_Build_Attachment_Bone_Name bone (type +0xa38, skipped
    // when empty) of the pad model, bind pose [M: static pads], Scale_Factor and the pad's yaw; a missing bone -> the pad.
    const objects::ObjectStore& st = mode.primary();
    const Vec3 p = st.position(pad);
    const objects::GameObjectTypes& types = catalog_->types();
    const objects::TypeIndex t = st.type(pad);
    if (t >= types.size()) return p;
    const std::string_view bone = ascii_trim(types.get_string(t, "Tactical_Build_Attachment_Bone_Name"));
    if (bone.empty()) return p;
    Vec3 out = p;
    return model_bone_point(st, pad, bone, out) > 0 ? out : p;
}

int TacticalBattle::model_bone_point(const objects::ObjectStore& st, objects::ObjectHandle h, std::string_view bone,
                                     Vec3& out) const {
    const objects::GameObjectTypes& types = catalog_->types();
    if (!st.exists(h)) return -1;
    const objects::TypeIndex t = st.type(h);
    if (t >= types.size()) return -1;
    const BattleCatalog::ModelInfo* mi = catalog_->model_info(static_cast<uint32_t>(t), space_, true);
    if (!mi) return -1;
    const int32_t b = mi->find(ascii_upper(ascii_trim(bone)));
    if (b < 0 || static_cast<size_t>(b) >= mi->bone_pos.size()) return 0;
    const Vec3 p = st.position(h);
    float s = types.columns().scale_factor[t];
    if (!(s > 0.0f)) s = 1.0f;
    const Vec3 f = st.facing(h);
    float fx = f.x, fy = f.y;
    const float len2 = fx * fx + fy * fy;
    if (!(len2 > 0.0f)) {
        fx = 1.0f;
        fy = 0.0f;
    } else {
        const float inv = 1.0f / dmath::sqrt(len2);
        fx = fx * inv;
        fy = fy * inv;
    }
    const auto& lp = mi->bone_pos[static_cast<size_t>(b)];
    const float lx = lp[0] * s, ly = lp[1] * s, lz = lp[2] * s;
    // Model space (nose -Y, left +X) -> world: X along left (-fy, fx), Y along -facing.
    out = {p.x + (-fy * lx - fx * ly), p.y + (fx * lx - fy * ly), p.z + lz};
    return 1;
}

void TacticalBattle::bt_kill_silently(TacticalMode& mode, objects::ObjectHandle h) {
    objects::ObjectStore& st = mode.primary();
    if (!st.alive(h)) return;
    const objects::ObjectId id = st.id(h);
    if (Unit* u = unit_by_id(id); u && u->alive && !u->retreated) {
        u->alive = false;
        --parts_[u->participant].alive;
        hg_unit_removed(id);  // hangars / engine loops
        abilities_->remove(id);
        if (u->movable) movement_->remove_unit(id);
        else movement_->object_removed(id);  // s78f review: fighter formation blocks naming it
        unit_gone(mode, *u);
    }
    mode.destroy(h);
}

void TacticalBattle::bt_pad_restore(TacticalMode& mode) {
    // AI_GALACTIC_TOKENS §9.4 recreate 0x42d8b0 (from the pad's setup vf13 0x42df80): campaign only (not a skirmish), not
    // while a save is loaded (pad_restore_ false), with the battle planet's list; the sandbox gate (state 9) does NOT apply
    // and Tactically_Built_Child_Object_Persists is not tested. Per pad (store order): the LAST record whose x and y equal
    // the pad's own x and y exactly (0x4b7c00) gives the finished type and the owner; that type is created at once for the
    // record's owner at the pad's build point (0x42e280) with the pad's facing, at full health (no UC phase, no cost), and
    // takes the pad's child slot. No match, an unknown type or owner -1: nothing (the record stays).
    // [port] an owner that is not a player of this battle's galaxy, or a type the battle tables cannot hold: nothing. The
    // pad's +0x3CB byte is not copied; the pad's owner is never changed (§9.11, below).
    if (!pad_records_ || pad_records_->empty() || !pad_restore_ || cfg_.skirmish || !combat_) return;
    static const objects::BehaviorTypeId kPad = objects::behavior_type_id("TACTICAL_BUILD_OBJECTS");
    if (kPad == objects::kNoBehavior) return;
    objects::ObjectStore& st = mode.primary();
    std::vector<objects::ObjectHandle> pads;
    for (const objects::ObjectHandle h : st.all_objects())
        if (st.active(h) && st.has_behavior(h, kPad)) pads.push_back(h);
    for (const objects::ObjectHandle ph : pads) {
        if (pad_contents(st.id(ph)) != objects::kNoObjectId) continue;  // [port] already holds a child
        const Vec3 pp = st.position(ph);
        const PadRecord* rec = nullptr;
        for (const PadRecord& r : *pad_records_)
            if (r.x == pp.x && r.y == pp.y) rec = &r;
        if (!rec || rec->type >= catalog_->types().size() || rec->owner < 0 || !bt_owner_valid(rec->owner)) continue;
        if (std::find(types_used_.begin(), types_used_.end(), rec->type) == types_used_.end()) continue;  // not in the tables
        objects::SpawnParams sp;
        sp.type = static_cast<objects::TypeIndex>(rec->type);
        sp.owner = rec->owner;
        sp.position = bt_pad_attach_point(mode, ph);
        sp.facing = st.facing(ph);
        const objects::ObjectHandle bh = mode.spawn(sp);
        if (!st.alive(bh)) continue;
        bt_adopt_built(mode, bh, rec->owner, 1.0f, bt_rng_);
        PadState* s = pad_state(st.id(ph), true);
        pads_mode_ = &mode;
        s->built = st.id(bh);
        s->built_type = sp.type;
        // q6 t19 (AI_GALACTIC_TOKENS §9.11): the child slot is filled first, so 0x501050(capture, -1, -1) re-derives the
        // controller by rule (a) = the pad's CURRENT owner: the pad keeps its owner (a defender turret may stand on a
        // Neutral pad) and only the capture record (controller +0x08, progress) is refreshed. The recreate does not register
        // the pad as a listener of the object's signal 0x28 (the port's pad state polls the child's death either way).
        s4_capture_grant(mode, st.id(ph), st.owner(ph));
        bt_used_ = true;
        ++stats_.pad_restored;
    }
}

// ---- land skirmish base (t50) ---------------------------------------------------------------------------------------

int32_t TacticalBattle::skirmish_slot_player(int32_t slot) const {
    // [M] a skirmish's slots are its two battle sides' players in ascending player order (the host builds the galaxy with
    // the human first; the headless runs with the first AI first), so slot 0 / 1 = "Player_0" / "Team_00" and "Player_1" / "Team_01".
    int32_t ids[2] = {req_.info.defender, req_.info.attacker};
    if (ids[0] > ids[1]) std::swap(ids[0], ids[1]);
    int32_t out[2] = {-1, -1};
    int32_t n = 0;
    for (const int32_t v : ids)
        if (v >= 0 && bt_owner_valid(v) && (n == 0 || out[n - 1] != v)) out[n++] = v;
    return slot >= 0 && slot < n ? out[slot] : -1;
}

float TacticalBattle::skirmish_land_unit_cap(int32_t player) const {
    if (!galaxy_ || player < 0 || static_cast<size_t>(player) >= galaxy_->players().size()) return -1.0f;
    int32_t count = 0;
    for (int32_t slot = 0; slot < 2; ++slot) count += skirmish_slot_player(slot) >= 0 ? 1 : 0;
    return rules_->faction(galaxy_->players()[static_cast<size_t>(player)].faction).unit_cap_for_players(static_cast<float>(count));
}

void TacticalBattle::bt_skirmish_base(TacticalMode& mode) {
    // t50 [M]: MP_Default_Pre_Built_Base (GameConstants, shipped yes) with a pad type that respects the option
    // (Respects_Skirmish_Pre_Build_Bases_Option, L1_S4_REMAINING §3.6 "pre-placed types spawn at start"): the pads of the
    // MultiplayerStructureMarkers "Team_00_Command Center" and "Team_00_Barracks" start built, for the two slots' factions, at
    // no charge: the pad's first buildable under-construction type of the owner's faction row, as its Tactical_Buildable_Constructed
    // type. The other pads (factories, research, power, communications array, generic and resource pads) start empty. The retail
    // pre-placed type itself is not decoded (.ted mini 0x2B is 3 on exactly the base pads); which pads it builds is the open item.
    if (!skirmish_land() || !combat_ || pad_behavior() == objects::kNoBehavior || uc_behavior() == objects::kNoBehavior) return;
    const objects::GameObjectTypes& types = catalog_->types();
    std::vector<objects::TypeIndex> pre;
    for (const char* name : {"Team_00_Command Center", "Team_00_Barracks"}) {
        const objects::TypeIndex mt = types.resolve_name(name);
        if (mt == objects::kNone || mt >= types.size()) continue;
        for (const std::string_view tok : types.get_list(mt, "Marker_For_Specific_Object_Type")) {
            const objects::TypeIndex x = types.resolve_name(ascii_trim(tok));
            if (x != objects::kNone && x < types.size()) pre.push_back(x);
        }
    }
    if (pre.empty()) return;
    objects::ObjectStore& st = mode.primary();
    std::vector<objects::ObjectHandle> pads;
    for (const objects::ObjectHandle h : st.all_objects())
        if (st.active(h) && st.has_behavior(h, pad_behavior()) && std::find(pre.begin(), pre.end(), st.type(h)) != pre.end()) pads.push_back(h);
    for (const objects::ObjectHandle ph : pads) {
        const int32_t owner = st.owner(ph);
        if (owner < 0 || (owner != skirmish_slot_player(0) && owner != skirmish_slot_player(1))) continue;
        if (pad_contents(st.id(ph)) != objects::kNoObjectId) continue;
        const uint32_t f = galaxy_->players()[static_cast<size_t>(owner)].faction;
        if (f >= galaxy_->rules().factions.size()) continue;
        const std::string& faction = galaxy_->rules().factions[f].name;
        objects::TypeIndex uc = objects::kNone;
        bool in_row = false;
        for (const std::string_view tok : types.get_list(st.type(ph), pad_list_tag())) {
            const objects::TypeIndex x = types.raw().find(tok);
            if (x == objects::kNone || x >= types.size()) {
                in_row = iequal_trim(tok, faction);
                continue;
            }
            if (in_row && uc == objects::kNone && types.has_behavior(x, objects::BehaviorSet::Union, uc_behavior())) uc = x;
        }
        if (uc == objects::kNone) continue;
        const objects::TypeIndex ft = types.resolve_name(types.get_string(uc, "Tactical_Buildable_Constructed"));
        if (ft == objects::kNone || ft >= types.size() ||
            std::find(types_used_.begin(), types_used_.end(), static_cast<uint32_t>(ft)) == types_used_.end())
            continue;
        objects::SpawnParams sp;
        sp.type = ft;
        sp.owner = owner;
        sp.position = bt_pad_attach_point(mode, ph);
        sp.position.z += 0.1f;
        sp.facing = st.facing(ph);
        const objects::ObjectHandle bh = mode.spawn(sp);
        if (!st.alive(bh)) continue;
        bt_adopt_built(mode, bh, owner, 1.0f, bt_rng_);
        PadState* s = pad_state(st.id(ph), true);
        pads_mode_ = &mode;
        s->built = st.id(bh);
        s->built_type = ft;
        bt_used_ = true;
        ++stats_.skirmish_prebuilt;
    }
    for (const objects::ObjectHandle h : st.all_objects())
        if (st.active(h) && st.has_behavior(h, pad_behavior()) &&
            (st.owner(h) == skirmish_slot_player(0) || st.owner(h) == skirmish_slot_player(1)))
            ++stats_.skirmish_pads;
}

uint8_t TacticalBattle::bt_skirmish_anchors() {
    // t50 [M]: a land skirmish side starts at its slot's map marker: "Player_<slot>_Spawn_Point_Marker" owned by the slot's player
    // (the explicit-faction MP maps carry one per faction and slot), else "Team_0<slot>_Spawn_Point_Marker" /
    // "Team_0<slot>_Base_Position_Marker" (the team maps; the first in map order). No marker: the fallback anchor stays.
    if (!skirmish_land() && !skirmish_map()) return 0;
    const objects::GameObjectTypes& types = catalog_->types();
    uint8_t found = 0;
    for (uint8_t side = 0; side < 2; ++side) {
        const int32_t player = side_player(side);
        int32_t slot = -1;
        for (int32_t k = 0; k < 2; ++k)
            if (skirmish_slot_player(k) == player) slot = k;
        if (slot < 0) continue;
        const std::string player_marker = "Player_" + std::to_string(slot) + "_Spawn_Point_Marker";
        const std::string team_spawn = "Team_0" + std::to_string(slot) + "_Spawn_Point_Marker";
        const std::string team_base = "Team_0" + std::to_string(slot) + "_Base_Position_Marker";
        const MapMarker* pick = nullptr;
        for (const std::string* want : {&player_marker, &team_spawn, &team_base}) {
            for (const MapMarker& m : bt_markers_) {
                if (m.type == objects::kNone || m.type >= types.size()) continue;
                if (!iequal_trim(types.raw().types().name[m.type], *want)) continue;
                if (want == &player_marker && m.owner != player) continue;
                pick = &m;
                break;
            }
            if (pick) break;
        }
        if (pick) {
            anchor_[side] = Vec3{pick->pos.x, pick->pos.y, 0.0f};
            // s78i t02 (TACTICAL_COMBAT §3.1.1 "other spawns", Creating_Starting_Forces_For_Skirmish): the side's mode
            // +0x288 record = that spawn marker's facing; no marker -> the record stays zero (yaw 0).
            drop_facing_[side] = pick->facing;
            // s78n t20 (SKIRMISH_SETUP §2.2 [H]: each start company is placed at the spawn marker "with the marker
            // orientation"): on land the start rows (slot_position) face the marker's way.
            // [port] land only: a space skirmish keeps the older anchors (its station / fleet placement reads them, and the
            // retail-checked space parity scenario depends on it).
            if (!space_) {
                facing_[side] = pick->facing;
                found = static_cast<uint8_t>(found | (1u << side));
            }
            stats_.skirmish_marked[side] = true;
            stats_.skirmish_marker[side] = anchor_[side];
        }
    }
    return found;
}

void TacticalBattle::bt_shield_stamps(const TacticalMode& mode) {
    if (space_ || !movement_) return;
    static const objects::BehaviorTypeId kBaseShield = objects::behavior_type_id("BASE_SHIELD");
    if (kBaseShield == objects::kNoBehavior) return;
    const objects::ObjectStore& st = mode.primary();
    const objects::GameObjectTypes& types = catalog_->types();
    mv::PassabilityGrid& grid = movement_->grid();
    // Gone: unstamp (ascending id).
    std::erase_if(bt_shields_, [&](objects::ObjectId id) {
        if (st.alive(st.find(id))) return false;
        grid.unstamp_secondary(id);
        return true;
    });
    // New: the store's object list order, inserted sorted.
    for (const objects::ObjectHandle h : st.all_objects()) {
        if (!st.active(h) || !st.has_behavior(h, kBaseShield)) continue;
        const float r = types.get_float(st.type(h), "Base_Shield_Radius");
        // MOVEMENT §1.1 step 3: the occupant's SecondaryOccupationPassability (+0x1694; shipped "Shield" on the 740-radius
        // generators) over the shield radius [inferred footprint]. Types without the field are no secondary occupants.
        const auto bits = static_cast<uint32_t>(types.get_mask(st.type(h), "SecondaryOccupationPassability"));
        if (!(r > 0.0f) || bits == 0) continue;
        const objects::ObjectId id = st.id(h);
        const auto it = std::lower_bound(bt_shields_.begin(), bt_shields_.end(), id);
        if (it != bt_shields_.end() && *it == id) continue;
        const Vec3 c = st.position(h);
        grid.stamp_secondary_circle(id, mv::V2{c.x, c.y}, r, bits);
        bt_shields_.insert(it, id);
    }
}

void TacticalBattle::bt_hash(StateHasher& h) const {
    h.boolean(bt_raid_);
    h.i32(bt_lead_);
    h.boolean(bt_sw_killed_);
    h.u32(bt_attrition_kills_);
    h.u32(static_cast<uint32_t>(bt_ramps_.size()));
    for (const UcRamp& r : bt_ramps_) {
        h.i32(r.id);
        h.f32(r.rate);
    }
    h.u32(static_cast<uint32_t>(parts_.size()));
    for (const Participant& p : parts_) {
        h.boolean(p.built);
        h.boolean(p.reserve);
        h.boolean(p.map);       // REMAINING_GAPS §3
        h.boolean(p.unplaced);
    }
}

}  // namespace alamo::sim
