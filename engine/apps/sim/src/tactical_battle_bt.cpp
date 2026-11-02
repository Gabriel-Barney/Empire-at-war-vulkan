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

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.remove_suffix(1);
    return s;
}

bool iequal_trim(std::string_view a, std::string_view b) {
    a = trim(a);
    b = trim(b);
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const char x = (a[i] >= 'a' && a[i] <= 'z') ? static_cast<char>(a[i] - 'a' + 'A') : a[i];
        const char y = (b[i] >= 'a' && b[i] <= 'z') ? static_cast<char>(b[i] - 'a' + 'A') : b[i];
        if (x != y) return false;
    }
    return true;
}

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
        for (const std::string_view tok : types.get_list(pt, "Tactical_Buildable_Objects_Campaign")) {
            const objects::TypeIndex uc = types.raw().find(tok);
            if (uc == objects::kNone || uc >= types.size()) continue;  // a faction name (row header)
            use(uc);
            const objects::TypeIndex ft = types.resolve_name(types.get_string(uc, "Tactical_Buildable_Constructed"));
            if (ft != objects::kNone && ft < types.size()) use(ft);
        }
    if (types_used_.size() != before) tables_ = std::make_unique<BattleTables>(catalog_->build_battle(types_used_, descs_, space_));
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
    if (member) {
        u.container = container;
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
        movement_->add_unit(md);
    }
    tc::UnitSpawn cs;
    cs.id = u.id;
    cs.desc = desc_of(t);
    cs.owner = owner;
    cs.position = tactical::Vec3{st.position(h).x, st.position(h).y, st.position(h).z};
    cs.facing = tactical::Vec3{st.facing(h).x, st.facing(h).y, st.facing(h).z};
    cs.health_fraction = std::clamp(health_fraction, 0.0f, 1.0f);
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
#ifdef ALAMO_SIM_HAVE_FOG
    if (fog() && fog_object_hidden(static_cast<int32_t>(player), pad, true)) return R::NotAllowed;  // pad fogged (0x35f470)
#endif
    const int32_t owner = st.owner(ph);
    if (owner < 0 || (owner != static_cast<int32_t>(player) && combat_->is_enemy(static_cast<int32_t>(player), owner)))
        return R::NotAllowed;  // not allied-owned (0x39a230)
    if (const PadState* s = pad_state(pad); s && s->cooldown_end > frame_) return R::NotAllowed;
    if (uc_behavior() == objects::kNoBehavior || !types.has_behavior(type, objects::BehaviorSet::Union, uc_behavior())) return R::InvalidUnit;
    if (!ai) {
        const float cost = build_cost(type);
        const float have = credits_get_ ? credits_get_(static_cast<int32_t>(player)) : 0.0f;
        if (cost > have) return R::CannotAfford;
        if (cost > 0.0f && credits_add_) credits_add_(static_cast<int32_t>(player), -cost);  // execute 0x5d78a0: charge 1
    }
    return pad_build(mode, pad, type, false) == 1 ? R::None : R::NotAllowed;
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
        for (const std::string_view tok : types.get_list(st.type(ph), "Tactical_Buildable_Objects_Campaign")) {
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
    if (st.alive(ph)) {
        pad_persist(st, ph, bt, -1, false);  // §6.5: the sale removes the persistent record
        s4_capture_reset(mode, s->pad);      // §6.5: the child went away -> the pad's capture point back to neutral
    }
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
    if (!trim(name).empty()) tt = types.resolve_name(trim(name));
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
    // [H]; [M] the members' boxes are taken as equal, so the centre of their positions' box is used.
    objects::ObjectStore& st = mode.primary();
    for (const Container& c : containers_) {
        if (!c.team || c.removed || !st.alive(c.h)) continue;
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
        const objects::TypeIndex x = types.resolve_name(trim(list[i]));
        const float w = std::strtof(std::string(trim(list[i + 1])).c_str(), nullptr);
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

int32_t TacticalBattle::bt_map_owner(uint32_t raw) const {
    // [probe 2026-10-29] (tools/probe_mod/s10_map_owners): a .ted owner (mini 02) is a faction index (FactionFiles order)
    // resolved to that faction's player; a native faction without a player belongs to the Hostile player (Hutts props
    // -> Hostile in skirmish). [inferred] "native" = a faction neither Is_Playable nor Is_Neutral; the Hostile player =
    // the first player whose faction is named Hostile. Anything else without a player: -1 (dropped as an unknown id).
    const auto& ps = galaxy_->players();
    const auto& fs = galaxy_->rules().factions;
    for (size_t p = 0; p < ps.size(); ++p)
        if (ps[p].faction == raw) return static_cast<int32_t>(p);
    if (raw >= fs.size() || fs[raw].is_playable || fs[raw].is_neutral) return -1;
    for (size_t p = 0; p < ps.size(); ++p)
        if (ps[p].faction < fs.size() && iequal_trim(fs[ps[p].faction].name, "Hostile")) return static_cast<int32_t>(p);
    return -1;
}

void TacticalBattle::bt_map_prepass() {
    // §3.1 items 2-4, in map object order (no draws): an object whose base level (mini 0x0B) exceeds the battle's base
    // level (D +0x130 land / +0x148 space) is destroyed; an unknown owner id is destroyed ("An object was saved with a
    // Player ID that no longer exists!"); the branched-map discard (single player, defender given, type
    // Is_Branched_Map_Discardable) keeps only owner = defender, Loads_When_Faction_Present and owner = attacker, an owner
    // faction without Can_Control_Planets, or an Is_Marker type [M: the behaviour test 0x370320 (arguments lost) and the
    // global 0x140b155d8 exception are not modelled]. The dropped placements leave map_ (no grid stamp, no spawn). The
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
    for (terrain::ObjectPlacement& pl : map_.objects) {
        if (placement_base_level(pl) > base) continue;
        const int32_t o = bt_map_owner(pl.owner);  // [probe 2026-10-29] faction index -> player
        if (!bt_owner_valid(o)) continue;
        pl.owner = static_cast<uint32_t>(o);  // map_ owners are player ids from here on (bt_map_load, markers)
        const objects::TypeIndex t = index.find(pl.type_crc);
        if (t != objects::kNone && t < types.size()) {
            if (def >= 0 && !cfg_.map_battle && types.get_bool(t, "Is_Branched_Map_Discardable")) {
                const bool keep = o == def || (types.get_bool(t, "Loads_When_Faction_Present") && o == att) || !can_control(o) ||
                                  types.get_bool(t, "Is_Marker");
                if (!keep) continue;
            }
            if (combat_candidate(t)) use(static_cast<uint32_t>(t));
            for (const auto& [x, w] : spawn_table(types, t))
                if (combat_candidate(x)) use(static_cast<uint32_t>(x));
        }
        kept.push_back(std::move(pl));
    }
    map_.objects = std::move(kept);
    if (types_used_.size() != before) tables_ = std::make_unique<BattleTables>(catalog_->build_battle(types_used_, descs_, space_));
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
            if (!bt_owner_neutral(owner)) bt_adopt_built(mode, h, owner, pl.health_fraction, &rng, true);
            continue;
        }
        const objects::ObjectHandle h = mode.spawn(sp, pl.health_fraction);
        own_obstacles(pl, t, h);
        if (kMarker != objects::kNoBehavior && types.has_behavior(t, objects::BehaviorSet::Union, kMarker) && owner >= 0) {
            MapMarker m;
            m.id = mode.primary().exists(h) ? mode.primary().id(h) : objects::kNoObjectId;
            m.type = t;
            m.kind = kind;
            m.owner = owner;
            m.pos = pos;
            m.facing = face;
            for (const std::string_view s : types.get_list(t, "Marker_For_Specific_Object_Type")) {
                const objects::TypeIndex x = types.resolve_name(trim(s));
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

void TacticalBattle::bt_place_forces(TacticalMode& mode, SimRng& rng) {
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
        const objects::TypeIndex x = types.resolve_name(trim(types.get_string(parts_[i].type, "Tactical_Additional_Structure_Type")));
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
        const objects::TypeIndex o = types.resolve_name(trim(types.get_string(p.type, "Place_Other_Type_At_Every_Specific_Marker_Position")));
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
                const std::string_view bp = trim(types.get_string(p.type, "Base_Position"));
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
    land_obstacles_sync(mode);  // S8 t02: the starbase / structures stamp on attach, before the start forces are placed
    // 6 / 7. Facing slots and anchors: the first Defending Forces marker (position, facing); the attacker's entry marker =
    //    the farthest from the first non-attacker DUMMY_GROUND_STRUCTURE with a non-zero Reinforcement_Prevention_Radius
    //    [M: the 0x2a9ff0 filter], else the nearest to the first Defending Forces marker. Space: the attacker looks at the
    //    last Defending Forces marker. [port] Formation placement 0x2a00b0 = rows of 6 behind the anchor along the facing;
    //    Space_Escort_Unit_Types and the planet-specific upgrades (step 5) are not spawned.
    const std::vector<size_t> dm = of_kind(kMkDefend), am = of_kind(kMkAttack);
    if (!dm.empty()) {
        anchor_[0] = flat(bt_markers_[dm.front()].pos);
        facing_[0] = bt_markers_[dm.front()].facing;
        drop_facing_[0] = facing_[0];  // a1h t06: mode +0x288 record of the defender (TACTICAL_GAPS §4.2.2 item 1)
    }
    if (!am.empty()) {
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
        if (std::any_of(parts_.begin(), parts_.end(), [](const Participant& q) { return q.side == 1 && !q.reserve && !q.third; }))
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
        const Vec3 d{anchor_[1 - s].x - anchor_[s].x, anchor_[1 - s].y - anchor_[s].y, 0.0f};
        const float len = dmath::sqrt(d.x * d.x + d.y * d.y);
        facing_[s] = len > 0.0f ? Vec3{d.x / len, d.y / len, 0.0f} : Vec3{s == 0 ? -1.0f : 1.0f, 0.0f, 0.0f};
    }
    for (uint8_t side = 0; side < 2; ++side)
        for (uint32_t i = 0; i < parts_.size(); ++i)
            if (!parts_[i].reserve && !parts_[i].starbase && !parts_[i].structure && !parts_[i].built && parts_[i].side == side)
                deploy(mode, i, rng);
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
    const std::string_view bone = trim(types.get_string(t, "Tactical_Build_Attachment_Bone_Name"));
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
    std::string upper(trim(bone));
    for (char& c : upper) c = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
    const int32_t b = mi->find(upper);
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
        unit_gone(mode, *u);
    }
    mode.destroy(h);
}

void TacticalBattle::bt_pad_restore(TacticalMode& mode) {
    // §6.5 restore 0x4b7c00: every pad whose x and y equal a record's exactly gets its persistent child back (the last
    // matching record wins), at the attachment point + 0.1 z, for the record's builder when it is a battle player [port],
    // else the pad owner.
    if (!pad_records_ || pad_records_->empty() || !combat_) return;
    static const objects::BehaviorTypeId kPad = objects::behavior_type_id("TACTICAL_BUILD_OBJECTS");
    if (kPad == objects::kNoBehavior) return;
    objects::ObjectStore& st = mode.primary();
    std::vector<objects::ObjectHandle> pads;
    for (const objects::ObjectHandle h : st.all_objects())
        if (st.active(h) && st.has_behavior(h, kPad)) pads.push_back(h);
    for (const objects::ObjectHandle ph : pads) {
        const Vec3 pp = st.position(ph);
        const PadRecord* rec = nullptr;
        for (const PadRecord& r : *pad_records_)
            if (r.x == pp.x && r.y == pp.y) rec = &r;
        if (!rec || rec->type >= catalog_->types().size()) continue;
        if (std::find(types_used_.begin(), types_used_.end(), rec->type) == types_used_.end()) continue;  // not in the tables
        const int32_t owner = (rec->value == req_.info.defender || rec->value == req_.info.attacker) && bt_owner_valid(rec->value) ? rec->value
                                                                                                                          : st.owner(ph);
        if (owner < 0) continue;
        objects::SpawnParams sp;
        sp.type = static_cast<objects::TypeIndex>(rec->type);
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
        s->built_type = sp.type;
        bt_used_ = true;
    }
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
