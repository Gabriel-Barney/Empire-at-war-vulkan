// A1 stage 2 hookup: BattleAi (battle_ai.hpp) - the tactical AI host of one S10 TacticalBattle.
#include "alamo/sim/battle_ai.hpp"

#ifdef ALAMO_SIM_HAVE_TACTICAL_AI
#include <algorithm>
#include <bit>
#include <cmath>
#include <map>

#include "alamo/ai/ai_types.hpp"
#include "alamo/ai/tactical/tactical_perception.hpp"
#include "alamo/core/dmath.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/galactic/campaign.hpp"
#include "alamo/lua/object_bridge.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/sim/sim_host.hpp"
#include "alamo/sim/tactical_battle.hpp"
#include "alamo/tactical/movement/unit_params.hpp"  // refixf t06: space drop footprint
#include "save_io.hpp"

namespace alamo::sim {

namespace at = alamo::ai::tactical;
namespace cmd = alamo::commands;
namespace mv = alamo::tactical::movement;
namespace tc = alamo::tactical::combat;

namespace {

constexpr objects::BehaviorTypeId kUnitAi = 0x12, kTeam = 0x16, kHideWhenFogged = 0x19, kIndigenous = 0x20,
                                  kBuildObjects = 0x29, kLobbing = 0x37, kCashPoint = 0x3C, kCapturePoint = 0x3D,
                                  kDamageTracking = 0x3E;
// Free-store admission behaviours (A1_READINGS §6; behaviours.cpp numbering = the original's)
constexpr objects::BehaviorTypeId kTacticalSuperWeapon = 0x2C, kSpecialWeapon = 0x2F, kDummyGroundBase = 0x5B,
                                  kDummyStarBaseBeh = 0x5C, kDummyGroundStructureBeh = 0x5F, kDummyOrbitalStructure = 0x60;
constexpr uint32_t kStandinInterval = 60;  // the S10 stand-in cadence (BattleConfig::order_interval default)

uint32_t le32(std::span<const uint8_t> b, size_t o) {
    return static_cast<uint32_t>(b[o]) | (static_cast<uint32_t>(b[o + 1]) << 8) | (static_cast<uint32_t>(b[o + 2]) << 16) |
           (static_cast<uint32_t>(b[o + 3]) << 24);
}

}  // namespace

std::string default_tactical_ai_type(int32_t player, const galactic::GalacticSim& g) {
    if (player < 0 || static_cast<size_t>(player) >= g.players().size()) return {};
    const galactic::PlayerState& ps = g.players()[static_cast<size_t>(player)];
    if (ps.human || ps.faction >= g.rules().factions.size()) return {};
    const galactic::FactionRules& f = g.rules().factions[ps.faction];
    if (f.is_neutral || !f.is_playable) return {};
    if (ai::ai_iequal(f.name, "Empire")) return "BasicEmpire";
    if (ai::ai_iequal(f.name, "Rebel")) return "BasicRebel";
    if (ai::ai_iequal(f.name, "Underworld")) return "AI_Player_Underworld";
    return {};
}

BattleAi::BattleAi(TacticalBattle& battle, TacticalMode& mode, SimHost& host, const TacticalAiSetup& setup)
    : b_(&battle), mode_(&mode), host_(&host) {
    const galactic::GalacticSim& g = *b_->galaxy_;
    at::TacticalAiConfig cfg;
    cfg.mode = b_->space_ ? at::TacticalMode::Space : at::TacticalMode::Land;
    cfg.difficulties = setup.difficulties;
    cfg.attacker = b_->req_.info.attacker;
    cfg.defender = b_->req_.info.defender;
    // t48: the port's skirmish battles also run under a galaxy, but are not campaign battles. [port] land skirmish stays on the
    // campaign rules: it has no skirmish base, and without barracks, command center and refinery the shipped equations only scout.
    cfg.campaign = !b_->cfg_.skirmish || !b_->space_;
    cfg.constants = setup.constants;
    cfg.log_decisions = setup.log_decisions;
    cfg.game_seed = host.session_seed();  // A2 tactical: perception '#' reseed (AI_GALACTIC_3 §7.3)
    allow_reinforcement_fraction_ = setup.constants.allow_reinforcement_percentage;  // A2 tactical (§3.2)
    const tactical::Aabb& bb = mode.bounds();
    cfg.map = {bb.min.x, bb.min.y, bb.max.x, bb.max.y};
#ifdef ALAMO_AI_TACTICAL_HAVE_TERRAIN
    if (!b_->space_) zones_ = at::hint_zones_from_map(b_->map_);
    if (b_->space_) at::space_region_bounds(b_->map_, cfg.region_map);  // A1_READINGS §8: the camera box
#endif
    zone_locator_.build(zones_);
    cfg.zones = zones_;
    cfg.plan_files = b_->space_ ? setup.space_plans : setup.land_plans;
    const uint32_t players = host.players();
    ai_side_.assign(players, 0);
    for (uint32_t p = 0; p < players; ++p) {
        lua::LuaPlayerSetup ls;
        if (p < g.players().size()) {
            const galactic::PlayerState& ps = g.players()[p];
            if (ps.faction < g.rules().factions.size()) {
                ls.faction = g.rules().factions[ps.faction].name;
                ls.neutral = g.rules().factions[ps.faction].is_neutral;
            }
            ls.human = ps.human;
            ls.ai = ps.ai;
            ls.credits = g.credits(static_cast<int32_t>(p));
            const auto pi = static_cast<int32_t>(p);
            const bool participant = pi == cfg.attacker || pi == cfg.defender;
            const std::string type = !participant ? std::string() : setup.ai_type ? setup.ai_type(pi, g) : default_tactical_ai_type(pi, g);
            if (!type.empty()) {
                cfg.players.push_back({pi, type, setup.difficulty});
                ai_side_[p] = 1;
            }
        }
        cfg.lua_players.push_back(ls);
    }
    world_ = std::make_unique<at::TacticalAiWorld>(mode.primary(), &mode.types(), setup.files, host.random(), *setup.defs,
                                                   *setup.equations, *this, std::move(cfg));
    lua::ObjectBridge& br = world_->object_bridge();
    br.abilities = &b_->abilities();
    TacticalBattle* bt = b_;
    br.ability_frame = [bt] { return bt->frame(); };
    br.is_enemy = [bt](int32_t x, int32_t y) { return bt->combat().is_enemy(x, y); };
    // refixf t03: Find_All_Objects_Of_Type / Find_Nearest match TEAM containers on their company / team type.
    const objects::ObjectStore* rep_store = &mode.primary();
    br.representative_type = [bt, rep_store](objects::ObjectHandle o) {
        return rep_store->exists(o) ? bt->representative_type(rep_store->id(o)) : objects::kNone;
    };
    br.order_finished = [bt](objects::ObjectId id, uint32_t) {
        return !bt->movement_->has_unit(id) || !bt->movement_->view(id).has_goal;
    };
    // OI follow-up t03 (A1_READINGS §28.9): this bridge's shares of the movement lock - Lock_Current_Orders, and the
    // locomotor's forced-move term while a Divert is the unit's current move (goal unchanged, not yet arrived).
    bt->set_script_movement_lock(this, [bt, &br](objects::ObjectId id) { return lua::script_movement_locked(br, id, [bt](objects::ObjectId u, Vec3& g) {
        if (!bt->movement_->has_unit(u)) return false;
        const auto v = bt->movement_->view(u);
        if (!v.has_goal || v.arrived) return false;
        g = Vec3{v.goal.x, v.goal.y, 0.0f};
        return true;
    }); });
    // OI host (L1/S4 wave 1 integrator ask 2): Change_Owner of the AI VMs goes through the battle (store + TreeSet::set_owner
    // + the battle systems), as the story VMs' bridge (CampaignBattles::change_owner); before, the binding set the store only.
    TacticalMode* owner_mode = &mode;
    br.change_owner = [bt, owner_mode](objects::ObjectHandle o, int32_t owner) { return bt->script_change_owner(*owner_mode, o, owner); };
    br.garrison_spawn_changed = [bt, owner_mode](objects::ObjectHandle o, bool on) {  // Lua Set_Garrison_Spawn
        if (owner_mode->primary().alive(o)) bt->set_garrison_spawn(owner_mode->primary().id(o), on);
    };
    // REMAINING_GAPS §4.2: movement threat mode reads the AI threat grid (point danger 0x653ff0) for the order's owner.
    const at::TacticalAiWorld* threat_world = world_.get();
    bt->movement_->set_threat_fn([threat_world](int32_t owner, mv::V2 p) { return threat_world->point_danger(owner, p.x, p.y); });
    // --- L1 bindings (script_remaining.hpp): Divert / Is_On_Diversion of the AI scripts (L1_S4_REMAINING §2.3) ---
    objects::ObjectStore* ast = &mode.primary();
    remaining_.move_to = [this, ast](objects::ObjectHandle u, Vec3 p) {
        if (!ast->alive(u)) return false;
        const objects::ObjectId id = ast->id(u);
        if (!move_unit(id, p.x, p.y)) return false;
        if (!b_->movement_locked(id)) b_->fighter_order(std::span<const objects::ObjectId>(&id, 1), 0, Vec3{p.x, p.y, 0.0f});  // S7 t22
        return true;
    };
    remaining_.move_goal = [bt, ast](objects::ObjectHandle u, Vec3& goal) {
        if (!ast->alive(u) || !bt->movement_->has_unit(ast->id(u))) return false;
        const mv::UnitView v = bt->movement_->view(ast->id(u));
        if (!v.has_goal) return false;
        goal = Vec3{v.goal.x, v.goal.y, 0.0f};
        return true;
    };
    // --- Int-final: the battle's L1 systems answer the AI VMs too (as the story VMs' l1_hooks in campaign_battles.cpp):
    //     land strikes (Bombing_Run / Fire_Orbital_Bombardment, BombingBlockStatus, availability, PlayerWrapper switches),
    //     damage statistics, projectile queries, space fields, weather, base shields. Ids: the battle mode's store. ---
    {
        TacticalMode* md = &mode;
        SimHost* hs = &host;
        auto bid = [ast](objects::ObjectHandle o) { return ast->exists(o) ? ast->id(o) : objects::kNoObjectId; };
        lua::RemainingScriptHooks& r = remaining_;
        r.rate_of_damage = [bt, bid](objects::ObjectHandle u) { return bt->rate_of_damage(bid(u)); };
        r.time_till_dead = [bt, bid](objects::ObjectHandle u) { return bt->time_till_dead(bid(u)); };
        r.final_blow_player = [bt, bid](objects::ObjectHandle o) { return bt->final_blow_player(bid(o)); };
        r.projectile_types = [bt, bid](objects::ObjectHandle u) { return bt->projectile_types(bid(u)); };
        r.current_projectile = [bt, bid](objects::ObjectHandle u) { return bt->current_projectile(bid(u)); };
        r.energy = [bt, bid](objects::ObjectHandle u) {
            const objects::ObjectId id = bid(u);
            return bt->combat().has_unit(id) ? bt->combat().energy(id) : -1.0f;
        };
        r.orbital_bombardment = [bt, md, hs](int32_t player, Vec3 p) { return bt->start_bombardment(*md, player, p, hs->random().synced()); };
        r.bombing_run = [bt, md](int32_t player, Vec3 p) { return bt->start_bombing_run(*md, player, p); };
        r.bombing_run_available = [bt](int32_t p) { return bt->bombing_run_available(p); };
        r.bombardment_available = [bt](int32_t p) { return bt->bombardment_available(p); };
        r.bombing_run_finished = [bt](int32_t p) { return bt->bombing_run_finished(p); };
        r.bombing_run_result = [bt](int32_t p) { return bt->bombing_run_result(p); };
        r.set_bombing_run_disabled = [bt](int32_t p, bool on) { bt->set_bombing_run_disabled(p, on); };
        r.set_bombardment_disabled = [bt](int32_t p, bool on) { bt->set_bombardment_disabled(p, on); };
        r.remove_bombardment = [bt](int32_t p, bool on) { bt->remove_bombardment(p, on); };
        r.force_weather = [bt, hs]() { return bt->force_weather(hs->random().synced()); };
        r.opposite_sides_of_shield = [bt](Vec3 a, Vec3 b, int32_t) { return bt->opposite_sides_of_shield(a, b, -1, true); };
        r.opposite_sides_of_shield4 = [bt](Vec3 a, Vec3 b, int32_t player, bool friendly) {
            return bt->opposite_sides_of_shield(a, b, player, friendly);
        };
        r.point_in_field = [bt](Vec3 p, uint32_t mask) { return (bt->field_kinds_at(p.x, p.y) & mask) != 0; };
        r.nearest_field = [bt, ast](Vec3 p, uint32_t mask) {
            const objects::ObjectId id = bt->nearest_field(p.x, p.y, mask);
            return id == objects::kNoObjectId ? objects::ObjectHandle{} : ast->find(id);
        };
        r.object_in_field = [bt, bid](objects::ObjectHandle o, uint32_t kind) { return bt->object_in_field(bid(o), kind); };
        r.build = [bt, md, bid](objects::ObjectHandle pad, objects::TypeIndex t, bool check) { return bt->pad_build(*md, bid(pad), t, check); };
        r.build_pad_contents = [bt, ast, bid](objects::ObjectHandle pad) {
            const objects::ObjectId id = bt->pad_contents(bid(pad));
            return id == objects::kNoObjectId ? objects::ObjectHandle{} : ast->find(id);
        };
        r.lock_build_pad = [bt, bid](objects::ObjectHandle pad, bool lock) { bt->pad_lock(bid(pad), lock); };
    }
    world_->set_global_values(host.global_values());  // the session-wide GlobalValue table (L1_S4_REMAINING §1.5)
    // --- end Int-final ---
    // --- battle host (L1 final list): the battle's L1 systems behind lua::TacticalScriptHooks for the AI VMs too (the
    //     story VMs' l1_hooks in campaign_battles.cpp): special weapons, TSW, engines, ground height, teleport, hyperspace
    //     out (Hyperspace_Away / Start_Cinematic_Space_Retreat), retreat increments, script spawns adopted by the battle. ---
    {
        TacticalMode* md = &mode;
        auto bid = [ast](objects::ObjectHandle o) { return ast->exists(o) ? ast->id(o) : objects::kNoObjectId; };
        lua::TacticalScriptHooks& k = tactical_;
        k.weapon_readiness = [bt, bid](objects::ObjectHandle w) { return bt->sw_readiness(bid(w)); };
        k.begin_targeting = [bt, bid](objects::ObjectHandle w) { return bt->sw_begin_targeting(bid(w)); };
        k.fire_at_object = [bt, md, bid](objects::ObjectHandle w, objects::ObjectHandle t) { return bt->sw_fire_at_object(*md, bid(w), bid(t)); };
        k.fire_lobbing_at = [bt, md, bid](objects::ObjectHandle w, Vec3 p) { return bt->sw_fire_lobbing_at(*md, bid(w), p); };
        k.special_weapons_allowed = [bt](int32_t player) { return bt->special_weapons_allowed(player); };
        k.tsw_state = [bt, bid](objects::ObjectHandle t) { return bt->tsw_state(bid(t)); };
        k.tsw_fire = [bt, bid](objects::ObjectHandle t) { return bt->tsw_fire(bid(t)); };
        k.engines_online = [bt, bid](objects::ObjectHandle u) { return bt->engines_online(bid(u)); };
        k.ground_height = [bt](float x, float y) { return bt->ground_height(x, y); };
        k.teleport = [bt, md, bid](objects::ObjectHandle u, Vec3 p, Vec3 f) { bt->teleport(*md, bid(u), p, f); };
        k.hyperspace_out = [bt, bid](objects::ObjectHandle u, int32_t delay) { bt->hyperspace_out(bid(u), delay, false); };
        k.hyperspace_out_link = [bt, bid](objects::ObjectHandle u, int32_t delay, bool keep) { return bt->hyperspace_out(bid(u), delay, keep); };
        k.retreat_increment_frames = [bt](int32_t player) { return bt->retreat_increment_frames(player); };
        k.spawn = [bt, md](const objects::SpawnParams& p) {
            const objects::ObjectHandle o = md->spawn(p);
            bt->adopt_object(*md, o);
            return o;
        };
#if defined(ALAMO_LUA_HAVE_FOG) && defined(ALAMO_SIM_HAVE_FOG)
        // refixf t03: Find_Nearest(obj, filter, player, bool) skips objects fogged for the player (0x35f470, force = 1).
        fog_hooks_.object_hidden = [bt, bid](objects::ObjectHandle o, int32_t player, bool force) {
            return bt->fog_object_hidden(player, bid(o), force);
        };
        k.fog = &fog_hooks_;
#endif
        br.tactical = &tactical_;
    }
    // --- end battle host ---
    br.remaining = &remaining_;
    // --- end L1 bindings ---
#ifdef ALAMO_SIM_HAVE_FOG
    // --- fog of war (owner: fog agent): FOW refog (a cell of a player's grid regrew to 0) -> threat-grid refog at
    // the FOW cell centre (FOG_OF_WAR §4.3, AI_TACTICAL_PERCEPTION §4.7). Cleared in the destructor. ---
    at::TacticalAiWorld* aw = world_.get();
    b_->set_fog_refog([aw](int32_t player, uint32_t, float x, float y) { aw->refog(player, x, y); });
#endif
}

BattleAi::~BattleAi() {
    if (b_->movement_) b_->movement_->set_threat_fn({});  // the battle outlives its AI
    b_->set_script_movement_lock(this, {});
#ifdef ALAMO_SIM_HAVE_FOG
    b_->set_fog_refog({});  // fog: the battle outlives its AI (CampaignBattles resets ai_ first)
#endif
}

bool BattleAi::start(std::vector<std::string>* diag) {
    if (!world_->load_plans(diag) && diag) diag->push_back("tactical AI: no plan loaded");
    return world_->start(diag);
}

bool BattleAi::controls(int32_t player) const {
    return player >= 0 && static_cast<size_t>(player) < ai_side_.size() && ai_side_[static_cast<size_t>(player)] && world_->is_ai_player(player);
}

void BattleAi::update() {
    if (b_->finished_) return;
    world_->update(static_cast<int64_t>(host_->frame()), b_->frame_);
    standin_orders();
}

void BattleAi::end() { world_->end_battle(); }

std::string BattleAi::summary() const {
    return "tactical AI: " + std::to_string(world_->orders_issued()) + " orders, " + std::to_string(world_->orders_rejected()) +
           " rejected, " + std::to_string(world_->decision_log().size()) + " decisions, " + std::to_string(world_->lua_errors().size()) +
           " Lua errors; executed" + [this] {
               std::string s;
               for (const auto& [k, v] : executed_) s += " " + k + " " + std::to_string(v.first + v.second);
               if (stale_) s += " (" + std::to_string(stale_) + " no-ops on dead targets)";
               return s;
           }();
}

void BattleAi::hash(StateHasher& h) const { world_->hash(h); }

// ---- N1 stage 2 save / load (owner: n2-host): chunk "battle.ai" ---------------------------------------------------
namespace {
constexpr uint32_t kBattleAiSaveVersion = 2;  // 2: UnitAI re-projection stamps
}

void BattleAi::write(ByteWriter& w) const {
    w.u32(kBattleAiSaveVersion);
    write_section(w, [this](ByteWriter& sub) { write_if_saveable(*world_, sub); });
    save_io::Out o(w);
    o(ai_side_, stale_, executed_, opp_prev_, unit_ai_stamps_);
}

bool BattleAi::read(ByteReader& r) {
    if (r.u32() != kBattleAiSaveVersion || !r.ok()) return false;
    const std::span<const uint8_t> world = r.raw(r.u32());
    std::vector<uint8_t> side;
    uint32_t stale = 0;
    std::map<std::string, std::pair<uint32_t, uint32_t>> executed;
    std::vector<std::pair<uint64_t, int32_t>> opp;
    std::map<objects::ObjectId, int32_t> stamps;
    save_io::In in(r);
    in(side, stale, executed, opp, stamps);
    if (!r.ok() || !r.done() || side != ai_side_) return false;  // the AI players are rebuilt by start()
    const std::map<objects::ObjectId, int32_t> stamps_before = unit_ai_stamps_;
    unit_ai_stamps_ = std::move(stamps);  // before relink(): its fill() must find every listed object stamped (no draws)
    ByteReader wr(world);
    if (!read_if_saveable(*world_, wr) || !wr.ok() || !wr.done()) {
        unit_ai_stamps_ = stamps_before;
        return false;
    }
    // battle.ai is the last battle chunk (CampaignBattles registers it after the battle parts): every other battle
    // system is restored, so the world takes its types / host callbacks now (tactical_ai.hpp relink).
    if (!world_->relink()) {
        unit_ai_stamps_ = stamps_before;
        return false;
    }
    stale_ = stale;
    executed_ = std::move(executed);
    opp_prev_ = std::move(opp);
    return true;
}

// ---- perception snapshot ------------------------------------------------------------------------------------------

void BattleAi::fill(at::PerceptionWorld& w, std::vector<at::TacticalAiObject>& out) {
    const objects::ObjectStore& st = mode_->primary();
    const objects::GameObjectTypes& types = mode_->types();
    const galactic::GalacticSim& g = *b_->galaxy_;
    const tc::CombatSystem& c = *b_->combat_;
    const uint32_t players = host_->players();
    if (w.types.empty()) {
        // Static parts (types indexed by objects::TypeIndex, players, relations, map, hard points).
        w.mode = b_->space_ ? at::TacticalMode::Space : at::TacticalMode::Land;
        w.campaign = !b_->cfg_.skirmish || !b_->space_;  // as TacticalAiConfig::campaign above
        const tactical::Aabb& bb = mode_->bounds();
        w.map = {bb.min.x, bb.min.y, bb.max.x, bb.max.y};
        // S10 land outcome: the campaign start markers behind IsFriendly / IsEnemyStartLocation (AI_TACTICAL_PERCEPTION
        // §3: Defending Forces / Attacker Entry Position by the "is defender" side). Before, the list stayed empty, every
        // start-location leaf failed and with it Land_Area_Needs_Scouting: the AI never scouted for fogged enemies.
        w.markers.clear();
        if (w.campaign) {
            for (const auto& [defend, pos] : b_->bt_start_markers()) {
                at::StartMarker m;
                m.kind = defend ? at::StartMarker::Kind::DefendingForces : at::StartMarker::Kind::AttackerEntry;
                m.position = {pos.x, pos.y, pos.z};
                w.markers.push_back(m);
            }
        } else {
            // t48: skirmish reads Player_%d_Position_Marker of Self. [port] the port's skirmish deploys both sides at the battle
            // anchors, not at those markers, so each side's start position is its anchor (defender 0, attacker 1).
            for (const int side : {0, 1}) {
                at::StartMarker m;
                m.kind = at::StartMarker::Kind::PlayerPosition;
                m.player_id = side == 0 ? b_->req_.info.defender : b_->req_.info.attacker;
                m.position = {b_->anchor_[side].x, b_->anchor_[side].y, b_->anchor_[side].z};
                w.markers.push_back(m);
            }
        }
        const at::TacticalPerception defaults;
        const auto& mask = defaults.config().category_mask;
        // battle host: type flag +0x29 = listed in some type's Squadron_Units (0x377500), sorted for lookup.
        std::vector<uint32_t> squadron_members;
        for (size_t t = 0; t < types.size(); ++t)
            for (const std::string_view n : types.get_list(static_cast<objects::TypeIndex>(t), "Squadron_Units"))
                if (const objects::TypeIndex m = types.resolve_name(n); m != objects::kNone && m < types.size())
                    squadron_members.push_back(static_cast<uint32_t>(m));
        std::sort(squadron_members.begin(), squadron_members.end());
        squadron_members.erase(std::unique(squadron_members.begin(), squadron_members.end()), squadron_members.end());
        w.types.resize(types.size());
        for (size_t t = 0; t < types.size(); ++t) {
            const auto ti = static_cast<objects::TypeIndex>(t);
            at::PerceptionType& pt = w.types[t];
            pt.name = t < types.raw().types().name.size() ? std::string(types.raw().types().name[t]) : std::string();
            pt.category = types.get_mask(ti, "CategoryMask");
            if (pt.category == 0)
                for (const std::string_view n : types.get_list(ti, "CategoryMask")) pt.category |= mask(n);
            pt.combat_power = types.get_float(ti, "AI_Combat_Power");
            // A1_READINGS §29 [H]: in a tactical mode 0x370e60 scales a company / squadron type whose first member has
            // Create_Team by clamp(Max_Squad_Size / member count, 0, 1) (the load-time power is the members' sum).
            {
                auto trimmed = [](std::string_view v) {
                    while (!v.empty() && (v.front() == ' ' || v.front() == '	')) v.remove_prefix(1);
                    while (!v.empty() && (v.back() == ' ' || v.back() == '	')) v.remove_suffix(1);
                    return v;
                };
                std::span<const std::string_view> list = types.get_list(ti, "Company_Units");
                if (list.empty()) list = types.get_list(ti, "Squadron_Units");
                const objects::TypeIndex first = list.empty() ? objects::kNone : types.resolve_name(trimmed(list.front()));
                if (first != objects::kNone && first < types.size() && types.get_bool(first, "Create_Team")) {
                    float r = static_cast<float>(types.get_int(ti, "Max_Squad_Size")) / static_cast<float>(list.size());
                    r = r < 0.0f ? 0.0f : (r > 1.0f ? 1.0f : r);
                    pt.combat_power = pt.combat_power * r;
                }
            }
            pt.is_hero = types.get_bool(ti, "Is_Named_Hero");
            pt.bombing_run_prevention_radius = static_cast<float>(types.get_int(ti, "Bombing_Run_Prevention_Radius"));
            pt.community_property = types.get_bool(ti, "Is_Community_Property");
            // --- A2 tactical: PotentialPlan inputs (tactical_pp.hpp) ---
            pt.tech_level = types.get_int(ti, "Tech_Level");
            pt.population = type_population(static_cast<int32_t>(t));
            pt.build_cost = static_cast<float>(types.get_int(ti, "Build_Cost_Credits"));  // int field
            for (const std::string_view f : types.get_list(ti, "Affiliation")) pt.affiliation.emplace_back(f);
            // flags +0x67 Create_Team / +0x29 squadron member / +0x21 Cinematic_Object_Only, behaviours 0x5C DUMMY_STAR_BASE /
            // 0x5B DUMMY_GROUND_BASE (AI_GALACTIC_3 §6 table) [battle host: replaces the Is_Dummy stand-in]
            static const objects::BehaviorTypeId kStarBase = objects::behavior_type_id("DUMMY_STAR_BASE");
            static const objects::BehaviorTypeId kGroundBase = objects::behavior_type_id("DUMMY_GROUND_BASE");
            pt.pp_excluded = types.get_bool(ti, "Create_Team") || types.get_bool(ti, "Cinematic_Object_Only") ||
                             std::binary_search(squadron_members.begin(), squadron_members.end(), static_cast<uint32_t>(t)) ||
                             (kStarBase != objects::kNoBehavior && types.has_behavior(ti, objects::BehaviorSet::Union, kStarBase)) ||
                             (kGroundBase != objects::kNoBehavior && types.has_behavior(ti, objects::BehaviorSet::Union, kGroundBase));
            static const objects::BehaviorTypeId kUc = objects::behavior_type_id("TACTICAL_UNDER_CONSTRUCTION");
            pt.under_construction = kUc != objects::kNoBehavior && types.has_behavior(ti, objects::BehaviorSet::Union, kUc);
            // Pad price 0x3744e0 (_Campaign, else _Multiplayer; [port] skirmish too, no planet
            // factor); other types: the same tags as their tactical price [M] (only TacticalBuild of build plans reads it).
            // Skirmish production of non-UC types (station buy bar): Tactical_Build_Cost_Multiplayer.
            pt.tactical_cost = (!pt.under_construction && b_->cfg_.skirmish)
                                   ? static_cast<float>(std::max(0, types.get_int(ti, "Tactical_Build_Cost_Multiplayer")))
                                   : b_->build_cost(ti);  // int fields (TacticalBattle::build_cost: _Campaign, else _Multiplayer)
            pt.generic_hero = types.get_bool(ti, "Is_Generic_Hero");
            pt.special_weapon = types.get_bool(ti, "Is_Special_Weapon_In_Space") ||
                                types.has_behavior(ti, objects::BehaviorSet::Union, kLobbing);
            pt.has_ai_script = !types.get_string(ti, "Lua_Script").empty();
            // --- end A2 tactical ---
        }
        w.players.resize(players);
        w.relation.assign(static_cast<size_t>(players) * players, 0);
        for (uint32_t a = 0; a < players; ++a) {
            at::PerceptionPlayerInfo& pi = w.players[a];
            pi.id = static_cast<int32_t>(a);
            if (a < g.players().size() && g.players()[a].faction < g.rules().factions.size()) {
                pi.faction = g.rules().factions[g.players()[a].faction].name;
                pi.neutral_faction = g.rules().factions[g.players()[a].faction].is_neutral;
                pi.is_human = g.players()[a].human;
            }
            pi.attacker = static_cast<int32_t>(a) == b_->req_.info.attacker;
            for (uint32_t bq = 0; bq < players; ++bq) {
                if (a == bq) continue;
                const bool hostile = c.is_enemy(static_cast<int32_t>(a), static_cast<int32_t>(bq));
                w.relation[static_cast<size_t>(a) * players + bq] = hostile ? 1 : 2;
            }
        }
        // S9 hard points (AI_TACTICAL_PERCEPTION �sec. 4): kind, weapon range, health; power = the unit's AI_Combat_Power
        // shared by its weapon hard points [port M].
        const tc::CombatSystem* cs = &c;
        TacticalBattle* bt = b_;
        w.hard_points = [cs, bt](int32_t id, std::vector<at::PerceptionHardPoint>& hps) {
            hps.clear();
            if (!cs->has_unit(id)) return;
            const uint32_t n = cs->hardpoint_count(id);
            const tc::UnitDesc& ud = cs->data().units[cs->desc_index(id)];
            uint32_t weapons = 0;
            for (uint32_t k = 0; k < n; ++k) weapons += tc::is_weapon(cs->hardpoint_kind(id, k)) ? 1u : 0u;
            const TacticalBattle::Unit* u = bt->unit_by_id(id);
            const float power = u && weapons > 0 ? u->power / static_cast<float>(weapons) : 0.0f;
            for (uint32_t k = 0; k < n; ++k) {
                at::PerceptionHardPoint hp;
                hp.type = static_cast<int32_t>(cs->hardpoint_kind(id, k));
                const uint32_t hd = k < ud.hardpoints.size() ? ud.hardpoints[k] : 0xFFFFFFFFu;
                hp.weapon_range = hd < cs->data().hardpoints.size() ? cs->data().hardpoints[hd].fire_range : 0.0f;
                hp.power = power;
                const float mx = cs->hardpoint_max(id, k);
                hp.health = mx > 0.0f ? cs->hardpoint_health(id, k) / mx : 1.0f;
                hp.destroyable = true;
                hp.destroyed = cs->hardpoint_destroyed(id, k);
                hps.push_back(hp);
            }
        };
    }
    for (uint32_t a = 0; a < players && a < w.players.size(); ++a) {
        w.players[a].credits = a < g.players().size() ? g.credits(static_cast<int32_t>(a)) : 0.0f;
        w.players[a].pop_used = static_cast<int32_t>(population_used(static_cast<int32_t>(a)));
        const float cap = population_cap(static_cast<int32_t>(a));
        w.players[a].pop_cap = cap < 0.0f ? 0 : static_cast<int32_t>(cap);
        w.players[a].can_retreat = b_->retreat_side_ < 0 && b_->frame_ >= b_->retreat_allowed_frame_;
        // Int-final: IsBombingRunAvailable / IsOrbitalBombardmentAvailable from the battle's land strike managers.
        w.players[a].bombing_run_available = b_->bombing_run_available(static_cast<int32_t>(a));
        w.players[a].orbital_bombardment_available = b_->bombardment_available(static_cast<int32_t>(a));
        w.players[a].reinforcements.clear();
        for (const at::TacticalReinforcement& r : reinforcement_pool(static_cast<int32_t>(a))) w.players[a].reinforcements.push_back(r.type);
    }
    // A1_READINGS §26: TimeLastSeen's cell test - the base passability word (.ted 0x10A, not the effective word with
    // stamped objects) of cell ((int)((x + 10) / 20), (int)((y + 10) / 20)); off the grid counts as blocked. A map
    // without a passability chunk keeps base Clear (MOVEMENT §1.1).
    if (!b_->space_ && !w.point_blocked) {
        const terrain::MapData* map = &b_->map_;
        w.point_blocked = [map](float x, float y) {
            const float fx = (x + 10.0f) / 20.0f, fy = (y + 10.0f) / 20.0f;
            if (!(fx > -1.0f) || !(fy > -1.0f) || !(fx < 65536.0f) || !(fy < 65536.0f)) return true;
            const auto cx = static_cast<uint32_t>(static_cast<int32_t>(fx));
            const auto cy = static_cast<uint32_t>(static_cast<int32_t>(fy));
            if (cx >= map->terrain.width || cy >= map->terrain.height) return true;
            if (!map->passability_present) return false;
            return (map->passability_at(cx, cy) & (terrain::kPassSteep | terrain::kPassWater)) != 0u;
        };
    }
#ifdef ALAMO_SIM_HAVE_FOG
    // --- fog of war (owner: fog agent): the real fog with force = 1 (the AI target-selection form); the AI gate
    // (non-human players unfogged while AIUsesFogOfWar is False) is applied by the perception's own force-0
    // tests (threat-grid fog memory). fow_active = the battle's FOW-active flag. ---
    if (const tactical::fog::FogOfWar* fog = b_->fog()) {
        w.fow_active = fog->fow_active();
        if (!w.object_fogged) {
            const TacticalBattle* fb = b_;
            w.object_fogged = [fb](int32_t player, int32_t id) { return fb->fog_object_hidden(player, id, true); };
            w.point_fogged = [fb](int32_t player, float x, float y) { return fb->fog_point_fogged(player, x, y, true); };
        }
    }
    // --- end fog ---
#endif
    // Team containers -> members (battle units, ascending id).
    std::map<objects::ObjectId, std::vector<int32_t>> members;
    for (const TacticalBattle::Unit& u : b_->units_)
        if (u.alive && !u.retreated && u.container != 0xFFFFFFFFu) members[b_->containers_[u.container].id].push_back(u.id);
    // t48: Variable_Self.BaseLevel in a land campaign battle: the conflict planet's ground structures, whoever asks. [inferred] the
    // spec's "without flag 4" = still alive in the galactic store (the flag's meaning is not recorded).
    w.conflict_ground_structures = 0;
    if (const auto pl = b_->req_.info.planet; !b_->space_ && static_cast<size_t>(pl) < g.planets().size())
        for (const objects::ObjectHandle s : g.planets()[pl].structures)
            if (g.store().alive(s)) ++w.conflict_ground_structures;
    // Objects of a live bombing run (the land mode's +0x38 bombing-run manager set, A1_READINGS §6.2 item 9), ascending.
    std::vector<objects::ObjectId> bombing_run_objects;
    for (const TacticalBattle::L1BombingRun& run : b_->l1_runs_)
        for (const TacticalBattle::L1Bomber& bm : run.bombers) bombing_run_objects.push_back(bm.id);
    std::sort(bombing_run_objects.begin(), bombing_run_objects.end());
    w.objects.clear();
    out.clear();
    std::map<objects::ObjectId, int32_t> stamps;
    for (const objects::ObjectHandle h : st.all_objects()) {
        if (!st.active(h) || st.has_behavior(h, objects::beh::Particle)) continue;
        const objects::ObjectId id = st.id(h);
        const auto type = static_cast<int32_t>(st.type(h));
        const auto ti = st.type(h);
        const TacticalBattle::Unit* u = b_->unit_by_id(id);
        if (u && (!u->alive || u->retreated)) continue;
        at::PerceptionObject o;
        o.id = id;
        o.owner = st.owner(h);
        o.type = type;
        if (u && u->container != 0xFFFFFFFFu) o.rep_type = static_cast<int32_t>(st.type(b_->containers_[u->container].h));
        o.has_unit_ai = st.has_behavior(h, kUnitAi) && c.has_unit(id);
        o.team_or_hide_when_fogged = st.has_behavior(h, kTeam) || st.has_behavior(h, kHideWhenFogged);
        o.mobile = u && u->movable;
        if (!u && st.has_behavior(h, kTeam))  // battle host: a TEAM container moves with its live members
            if (const auto tm = members.find(id); tm != members.end())
                for (const int32_t m : tm->second)
                    if (const TacticalBattle::Unit* mu = b_->unit_by_id(m); mu && mu->movable) o.mobile = true;
        const Vec3 p = st.position(h), f = st.facing(h);
        o.position = {p.x, p.y, p.z};
        o.transform[0] = f.x;
        o.transform[4] = f.y;
        o.transform[3] = p.x;
        o.transform[7] = p.y;
        o.transform[11] = p.z;
        if (c.has_unit(id)) {
            const float mx = c.max_health(id);
            o.health = mx > 0.0f ? c.health(id) / mx : 1.0f;
            o.shield = c.shield_fraction(id);
            o.max_attack_range = u ? u->range : 0.0f;
        }
        if (o.mobile && b_->movement_->has_unit(id)) o.speed = b_->movement_->view(id).speed * 30.0f;  // [M] units per second
        o.combat_power = u ? u->power : (static_cast<size_t>(type) < w.types.size() ? w.types[static_cast<size_t>(type)].combat_power : 0.0f);
        // A1_READINGS §29 [H]: a TEAM container is in the threat grid (UNIT_AI from its own type) as one whole-object entry
        // of its company type (0x396380, team data +0x58), with that type's tactical power, the first member's attack range
        // and the mean of the members' hull fractions; the members (no UNIT_AI) have no entries.
        if (!u && st.has_behavior(h, kTeam))
            for (const TacticalBattle::Container& ct : b_->containers_) {
                if (ct.id != id || !ct.team) continue;
                if (ct.squad_type < w.types.size()) {
                    o.type = static_cast<int32_t>(ct.squad_type);
                    o.rep_type = type;  // 0x3973b0: Create_Team_Type, else the company type
                    o.combat_power = w.types[ct.squad_type].combat_power;
                }
                o.has_unit_ai = st.has_behavior(h, kUnitAi);
                if (const auto tm = members.find(id); tm != members.end() && !tm->second.empty()) {
                    float hull = 0.0f;
                    for (const int32_t m : tm->second)
                        hull += c.has_unit(m) && c.max_health(m) > 0.0f ? c.health(m) / c.max_health(m) : 1.0f;
                    o.health = hull / static_cast<float>(tm->second.size());
                    if (const TacticalBattle::Unit* mu = b_->unit_by_id(tm->second.front())) o.max_attack_range = mu->range;
                }
                break;
            }
        o.has_damage_tracking = b_->damage_tracking_.tracks(id);
        o.is_build_pad = st.has_behavior(h, kBuildObjects);
        o.is_cash_point = st.has_behavior(h, kCashPoint);
        // Variable_Target.IsContestable (AI_TACTICAL_PERCEPTION §3, 0x731400 [M]): behaviour 0x3D (CAPTURE_POINT,
        // OBJECT_RUNTIME §4.3) and the type is affiliated (0x3751a0) with the faction of the special player 0x331cc0 (the
        // first player of an Is_Neutral faction) [port: = the first Is_Neutral faction, GameObjectTypes::default_faction()].
        if (st.has_behavior(h, kCapturePoint)) {
            const std::span<const uint32_t> fs = types.factions(ti);
            o.contestable = std::find(fs.begin(), fs.end(), types.default_faction()) != fs.end();
        }
        // t41: Variable_Self.BaseLevel in space (AI_TACTICAL_PERCEPTION, Player tokens). Unset, every station read as level 0, so a
        // defender holding one passed the "BaseLevel == 0" retreat test of the shipped Want_To_Retreat_From_Space.
        // [inferred] the level (0x397540) = the type's Base_Level: the battle does not lower it while the station lives.
        static const objects::BehaviorTypeId kDummyStarBase = objects::behavior_type_id("DUMMY_STAR_BASE");
        if (kDummyStarBase != objects::kNoBehavior && st.has_behavior(h, kDummyStarBase)) {
            o.dummy_star_base = true;
            o.starbase_level = types.get_int(ti, "Base_Level");
        }
        // t48: Variable_Self.BaseLevel in land skirmish: the count of P's alive objects with this behaviour.
        static const objects::BehaviorTypeId kDummyGroundStructure = objects::behavior_type_id("DUMMY_GROUND_STRUCTURE");
        o.dummy_ground_structure = kDummyGroundStructure != objects::kNoBehavior && st.has_behavior(h, kDummyGroundStructure);
        o.transport_landing = st.has_behavior(h, objects::beh::TransportLanding);
        o.visible_to = ~0ull;
        // land: the first zone (list order) whose cell set holds the position's cell (A1_READINGS §4, ZoneLocator)
        if (!zones_.empty()) o.zone = zone_locator_.zone_at(p.x, p.y);
        const auto mt = members.find(id);
        at::TacticalAiObject a;
        a.handle = h;
        const objects::TypeIndex rep = o.rep_type >= 0 ? static_cast<objects::TypeIndex>(o.rep_type) : ti;
        a.target.id = id;
        a.target.rep_has_land_evaluator = types.get_bool(rep, "Has_Land_Evaluator");
        a.target.rep_has_space_evaluator = types.get_bool(rep, "Has_Space_Evaluator");
        a.target.own_has_space_evaluator = types.get_bool(ti, "Has_Space_Evaluator");
        a.applies.has_locomotor = st.has_behavior(h, objects::beh::Locomotor);
        a.applies.has_build_objects = o.is_build_pad;
        a.applies.has_cash_point = o.is_cash_point;
        a.applies.reinforcement_enabling_radius = types.get_float(ti, "Reinforcement_Enabling_Radius");
        a.applies.last_state_visible_under_fow = types.get_bool(ti, "Last_State_Visible_Under_FOW");
        a.applies.initial_state_visible_under_fow = types.get_bool(ti, "Initial_State_Visible_Under_FOW");
        at::FreeStoreObjectInfo& fs = a.free_store;
        fs.is_transport = st.has_behavior(h, objects::beh::Transport);
        fs.type_create_team = types.get_bool(ti, "Create_Team");
        fs.type_is_dummy = types.get_bool(ti, "Is_Dummy");
        fs.type_stay_in_transport = types.get_bool(ti, "Stay_In_Transport_During_Ground_Battle");
        fs.has_indigenous_or_explosion = st.has_behavior(h, kIndigenous);
        fs.has_lobbing_superweapon = st.has_behavior(h, kLobbing);
        fs.has_locomotor = a.applies.has_locomotor && o.mobile;
        fs.is_starship_combatant = st.has_behavior(h, objects::beh::StarshipCombatant) || (u != nullptr && o.mobile);  // [port] battle units
        fs.is_team = st.has_behavior(h, kTeam);
        // Behaviour-in-type tests (0x370320) read the type's behaviour union [M: which set]; the rest are object tests.
        const auto type_has = [&](objects::BehaviorTypeId b) { return types.has_behavior(ti, objects::BehaviorSet::Union, b); };
        fs.type_category_bit_5f = type_has(kDummyGroundStructureBeh);
        fs.type_bit_5b = type_has(kDummyGroundBase);
        // TEAM: the representative (container) type or any live member type has INDIGENOUS_UNIT
        if (fs.is_team) {
            fs.team_has_bit_20 = type_has(kIndigenous);
            if (mt != members.end())
                for (const int32_t m : mt->second)
                    if (const objects::ObjectHandle mh = st.find(static_cast<objects::ObjectId>(m));
                        mh && types.has_behavior(st.type(mh), objects::BehaviorSet::Union, kIndigenous))
                        fs.team_has_bit_20 = true;
        }
        // obj +0x2b0 / +0xf8 (contained or attached): inside a garrison, a hard point / attached child (store attach
        // parent), or a DEPLOY_TROOPERS member pinned to its carrier. [port] a team member's link to its TEAM container
        // (TacticalBattle::Unit::container, store container) is not read as "contained" here.
        fs.subobject_f8_nonempty = b_->garrison_holder_of(id) != objects::kNoObjectId || static_cast<bool>(st.attach_parent(h)) ||
                                   (u != nullptr && u->attached_to != objects::kNoObjectId);
        fs.in_mode_manager_set = !b_->space_ && std::binary_search(bombing_run_objects.begin(), bombing_run_objects.end(), id);
        fs.dead_or_removing = (u != nullptr && !u->alive) || !st.alive(h);  // dead and dying (refix t01 clone) objects are skipped above: always false here
        fs.in_world = true;  // [M] obj +0x3c8: every listed battle object is in the world (retreated units are skipped above)
        fs.type_squadron_member = types.squadron_member(ti);
        fs.type_company_member = types.company_member(ti);
        fs.has_special_weapon = st.has_behavior(h, kSpecialWeapon);
        fs.type_dummy_orbital_structure = type_has(kDummyOrbitalStructure);
        fs.type_dummy_star_base = type_has(kDummyStarBaseBeh);
        fs.has_tactical_super_weapon = st.has_behavior(h, kTacticalSuperWeapon);
        if (mt != members.end()) a.team_members = mt->second;
        a.moving = o.mobile && b_->movement_->has_unit(id) && b_->movement_->view(id).has_goal;
        a.attack_target = c.has_unit(id) && c.current_target(id) != tc::kNoObject ? c.current_target(id) : -1;
        // A1 t06 [port]: a TEAM container has no movement / combat entry of its own (its orders go to the live members,
        // team_units), so its move and attack state is its members': moving while any member has a movement goal, the
        // attack target of the first member (ascending id) that has one. The original's TEAM object moves as one unit, so
        // its blocking move statuses run until the team arrives. Before, the container always read idle: every TF move of
        // an infantry squad (Land_Scout_Area's Attack_Move / Explore_Area, Destroy_Structure's Attack_Move) completed one
        // Planning service after the order, and the order-lock clearing handed the squad back to the free store at once.
        if (!u && mt != members.end()) {
            for (const int32_t m : mt->second) {
                const auto mid = static_cast<objects::ObjectId>(m);
                const TacticalBattle::Unit* mu = b_->unit_by_id(mid);
                if (mu && mu->movable && b_->movement_->has_unit(mid) && b_->movement_->view(mid).has_goal) a.moving = true;
                if (a.attack_target < 0 && c.has_unit(mid) && c.current_target(mid) != tc::kNoObject) a.attack_target = c.current_target(mid);
            }
        }
        a.damage_tracking = o.has_damage_tracking || st.has_behavior(h, kDamageTracking);
        // UnitAI data pack (A1_READINGS §14): two synced draws per UNIT_AI object, F + Get_Int(0, 300) - 300 then
        // F + Get_Int(0, 150) - 150. [port] drawn when fill() first lists the object (store order, after the battle tick
        // of its creation frame), not inside the object's construction.
        if (st.has_behavior(h, kUnitAi)) {
            const auto it = unit_ai_stamps_.find(id);
            const int32_t stamp = it != unit_ai_stamps_.end()
                                      ? it->second
                                      : at::draw_unit_ai_stamps(host_->random().synced(), b_->frame_).reprojection_check;
            stamps.emplace(id, stamp);
            a.reprojection_stamp = stamp;
        }
        w.objects.push_back(std::move(o));
        out.push_back(std::move(a));
    }
    unit_ai_stamps_ = std::move(stamps);  // objects no longer listed drop out
}

// ---- order execution ----------------------------------------------------------------------------------------------

bool BattleAi::move_unit(objects::ObjectId id, float x, float y, float threat_threshold) {
    const TacticalBattle::Unit* u = b_->unit_by_id(id);
    if (!u || !u->alive || u->retreated || !u->movable) return false;
    if (b_->movement_locked(id)) return true;  // OI follow-up t03: the locked unit keeps its order (A1_READINGS §28.9)
    const tactical::Aabb& bb = mode_->bounds();
    mv::MoveOrder o;
    o.units = {id};
    o.destination = {std::clamp(x, bb.min.x, bb.max.x), std::clamp(y, bb.min.y, bb.max.y)};
    o.threat_threshold = threat_threshold;
    b_->movement_->move(o, host_->random().synced(), b_->frame_);
    return true;
}

cmd::RejectReason BattleAi::execute(uint8_t player, const cmd::EventData& ev) {
    const cmd::RejectReason r = execute_order(player, ev);
    std::string key(ev.def().name);
    if (ev.type() == cmd::EventType::PortExtension) key += "." + std::to_string(ev.u("subtype") & 0xFFu);
    auto& e = executed_[key + (r == cmd::RejectReason::None ? "" : std::string(" ") + std::string(cmd::reject_reason_name(r)))];
    ++(r == cmd::RejectReason::None ? e.first : e.second);
    return r;
}

cmd::RejectReason BattleAi::execute_order(uint8_t player, const cmd::EventData& ev) {
    using R = cmd::RejectReason;
    using T = cmd::EventType;
    if (b_->finished_) return R::NotAllowed;
    objects::ObjectStore& st = mode_->primary();
    // battle host: unit orders resolve through team_units (a unit, or a TEAM container's live members).
    switch (ev.type()) {
        case T::MoveToPositionFacing: {
            const tactical::Aabb& bb = mode_->bounds();
            mv::MoveOrder o;
            for (const uint32_t id : ev.ids("units"))  // battle host: TEAM containers -> their members
                for (const objects::ObjectId mid : team_units(player, id))
                    if (b_->unit_by_id(mid)->movable && std::find(o.units.begin(), o.units.end(), mid) == o.units.end()) o.units.push_back(mid);
            if (o.units.empty()) return R::InvalidUnit;
            const float x = ev.f("x"), y = ev.f("y");
            if (!std::isfinite(x) || !std::isfinite(y)) return R::InvalidPosition;
            b_->drop_locked(o.units);  // OI follow-up t03: the group move leaves locked units out
            if (o.units.empty()) return R::None;
            o.destination = {std::clamp(x, bb.min.x, bb.max.x), std::clamp(y, bb.min.y, bb.max.y)};
            b_->movement_->move(o, host_->random().synced(), b_->frame_);
            for (const objects::ObjectId id : o.units) {
                b_->combat_->clear_order(id);
                b_->combat_->set_attack_goal(id, tc::kNoObject);  // refixf t06: a position block
            }
            b_->fighter_order(o.units, 0, Vec3{o.destination.x, o.destination.y, 0.0f});  // S7 t22: the squadrons' formation
            return R::None;
        }
        case T::MoveToObject: {
            // Lua Move_To(object) without a threat threshold (ev_move_to_object): a move to the target's current position,
            // like the LuaOrder MoveTo path [port: the original's follow-object behaviour is not modelled].
            const uint32_t tw = ev.u("target");
            const objects::ObjectHandle th = tw <= 0x7FFFFFFFu ? st.find(static_cast<objects::ObjectId>(tw)) : objects::ObjectHandle{};
            if (!st.alive(th)) {
                ++stale_;
                return R::None;  // COMMANDS.md sec. 6.2: a missing / dying target is a silent no-op
            }
            mv::MoveOrder o;
            for (const uint32_t id : ev.ids("movers"))  // battle host: TEAM containers -> their members
                for (const objects::ObjectId mid : team_units(player, id))
                    if (mid != static_cast<objects::ObjectId>(tw) && b_->unit_by_id(mid)->movable &&
                        std::find(o.units.begin(), o.units.end(), mid) == o.units.end())
                        o.units.push_back(mid);
            if (o.units.empty()) return R::InvalidUnit;
            const Vec3 tp = st.position(th);
            b_->drop_locked(o.units);  // OI follow-up t03
            if (o.units.empty()) return R::None;
            for (const objects::ObjectId id : o.units) b_->combat_->set_attack_goal(id, tc::kNoObject);  // refixf t06: kind 1
            b_->fighter_order(o.units, 0, tp);  // S7 t22: a position snapshot (kind 1)
            if (o.units.size() == 1) {
                b_->combat_->clear_order(o.units.front());
                return move_unit(o.units.front(), tp.x, tp.y) ? R::None : R::InvalidUnit;
            }
            const tactical::Aabb& gb = mode_->bounds();
            o.destination = {std::clamp(tp.x, gb.min.x, gb.max.x), std::clamp(tp.y, gb.min.y, gb.max.y)};
            b_->movement_->move(o, host_->random().synced(), b_->frame_);
            for (const objects::ObjectId id : o.units) b_->combat_->clear_order(id);
            return R::None;
        }
        case T::Attack: {
            const uint32_t tw = ev.u("target");
            const TacticalBattle::Unit* t = tw <= 0x7FFFFFFFu ? b_->unit_by_id(static_cast<objects::ObjectId>(tw)) : nullptr;
            if (!t || !t->alive || t->retreated || !b_->combat_->is_enemy(player, st.owner(t->h))) return R::InvalidTarget;
            int32_t hp = -1;
            if (const uint32_t ref2 = ev.u("ref2"); ref2 > 0)
                for (uint32_t k = 0; k < b_->combat_->hardpoint_count(t->id) && hp < 0; ++k)
                    if (static_cast<uint32_t>(b_->combat_->hardpoint_kind(t->id, k)) == ref2 - 1 && b_->combat_->hardpoint_targetable(t->id, k))
                        hp = static_cast<int32_t>(k);
            uint32_t n = 0;
            const Vec3 tp = st.position(t->h);
            std::vector<objects::ObjectId> attackers;  // battle host: TEAM containers -> their members
            for (const uint32_t id : ev.ids("attackers"))
                for (const objects::ObjectId mid : team_units(player, id))
                    if (std::find(attackers.begin(), attackers.end(), mid) == attackers.end()) attackers.push_back(mid);
            for (const objects::ObjectId aid : attackers) {
                const TacticalBattle::Unit* u = b_->unit_by_id(aid);
                if (!u) continue;
                b_->combat_->order_attack(u->id, t->id, hp);
                b_->combat_->set_attack_goal(u->id, t->id, hp);  // refixf t06: Attack_Target block (kind 3, hard point, §2.8)
                if (!b_->movement_locked(u->id))  // S7 t22: an object block; a locked unit keeps its formation (as Move_To)
                    b_->fighter_order(std::span<const objects::ObjectId>(&u->id, 1), 0, tp, t->id);
                ++n;
                if (!u->movable) continue;
                const Vec3 up = st.position(u->h);
                const float dx = tp.x - up.x, dy = tp.y - up.y, d = dmath::sqrt(dx * dx + dy * dy);
                const float range = std::max(u->range, 100.0f);
                if (d > 0.8f * range) {
                    const float k = (d - 0.7f * range) / d;
                    move_unit(u->id, up.x + dx * k, up.y + dy * k);
                }
            }
            return n ? R::None : R::InvalidUnit;
        }
        case T::PortExtension: {
            const uint32_t sub = ev.u("subtype");
            const auto b = ev.bytes_view("data");
            if (sub < lua::kLuaOrderSubtype || sub > lua::kLuaOrderSubtype + 0xFFu || b.size() < lua::kLuaOrderPayloadBytes) return R::Unhandled;
            const auto order = static_cast<lua::LuaOrder>(sub - lua::kLuaOrderSubtype);
            // battle host: a TEAM container's order goes to its live members (the container itself has no movement).
            // refixf t06: a task-force group order (tactical_ai.hpp HOST CONTRACT) carries its further units after the 24-byte
            // block, one u32 each: all of them form one order (MOVEMENT §6.3 [H]: one coordinator for the task force).
            std::vector<objects::ObjectId> ids = team_units(player, le32(b, 0));
            for (size_t off = lua::kLuaOrderPayloadBytes; off + 4 <= b.size(); off += 4)
                for (const objects::ObjectId mid : team_units(player, le32(b, off)))
                    if (std::find(ids.begin(), ids.end(), mid) == ids.end()) ids.push_back(mid);
            if (ids.empty()) return R::InvalidUnit;
            if (order == lua::LuaOrder::Stop) {
                for (const objects::ObjectId id : ids) {
                    const objects::ObjectId one[] = {id};
                    if (b_->unit_by_id(id)->movable) b_->movement_->stop(one);
                    b_->combat_->clear_order(id);
                    b_->combat_->set_attack_goal(id, tc::kNoObject);  // refixf t06
                }
                b_->fighter_order_clear(ids);  // S7 t22: Stop leaves the formation [M]
                return R::None;
            }
            if (order != lua::LuaOrder::MoveTo && order != lua::LuaOrder::AttackMove && order != lua::LuaOrder::Guard) return R::Unhandled;
            mv::MoveOrder group;
            for (const objects::ObjectId id : ids)
                if (b_->unit_by_id(id)->movable) group.units.push_back(id);
            if (group.units.empty()) return R::InvalidUnit;
            const TacticalBattle::Unit* u = b_->unit_by_id(group.units.front());
            float x = std::bit_cast<float>(le32(b, 8)), y = std::bit_cast<float>(le32(b, 12));
            const uint32_t tw = le32(b, 4);
            if (tw != cmd::kNoObject) {
                const objects::ObjectHandle th = tw <= 0x7FFFFFFFu ? st.find(static_cast<objects::ObjectId>(tw)) : objects::ObjectHandle{};
                if (!st.alive(th)) {
                    ++stale_;
                    return R::None;  // COMMANDS.md sec. 6.2: a missing / dying target is a silent no-op (stale script reference)
                }
                const Vec3 tp = st.position(th);
                x = tp.x;
                y = tp.y;
                const TacticalBattle::Unit* tu = b_->unit_by_id(static_cast<objects::ObjectId>(tw));
                if (order == lua::LuaOrder::AttackMove && tu && tu->alive && !tu->retreated && b_->combat_->has_unit(tu->id)) {
                    // refixf t06 (TACTICAL_COMBAT §2.8): an Attack_Move on an object. A: a task-force order (arg bit, header)
                    // issues Order_Attack (ordered 1) to every unit now; a refusing unit is dropped from the move, none left
                    // -> the order fails. B: the object is each moving unit's formation attack goal (the combat system locks
                    // it in weapon range). This replaces the a1f t07 neutral-target stand-in (§2.8: an enemy Is_Neutral
                    // player's object qualifies for B). An object's own Lua Attack_Move (arg 0) gets B only.
                    if (le32(b, 20) & at::kTfAttackMoveIssueAttack) {
                        std::vector<objects::ObjectId> kept;
                        for (const objects::ObjectId id : group.units)
                            if (!b_->combat_->has_unit(id) || b_->combat_->order_attack_issue(id, tu->id, -1)) kept.push_back(id);
                        for (const objects::ObjectId id : ids)  // members that do not move still get the order
                            if (std::find(group.units.begin(), group.units.end(), id) == group.units.end() && b_->combat_->has_unit(id))
                                b_->combat_->order_attack_issue(id, tu->id, -1);
                        if (kept.empty()) return R::InvalidTarget;
                        group.units = std::move(kept);
                        u = b_->unit_by_id(group.units.front());
                    }
                    for (const objects::ObjectId id : group.units) b_->combat_->set_attack_goal(id, tu->id, -1);
                    // Staging (0x40e260 [M], §2.8): land formations (type 1) only - a single space Attack_Move is not staged.
                    // The member M with the largest Targeting_Min_Attack_Distance, when inside that min range of the target,
                    // moves the goal to d = min + 0.2 x (Get_Base_Attack_Range(M) - min) from the target [port: on the line
                    // from the target to M].
                    if (!b_->space_) {
                        objects::ObjectId m = objects::kNoObjectId;
                        float mn = 0.0f;
                        for (const objects::ObjectId id : group.units)
                            if (b_->combat_->has_unit(id) && b_->combat_->min_attack_distance(id) > mn) {
                                mn = b_->combat_->min_attack_distance(id);
                                m = id;
                            }
                        if (m != objects::kNoObjectId && b_->combat_->inside_min_range(m, tu->id)) {
                            const float d = mn + 0.2f * (b_->combat_->base_attack_range_of(m) - mn);
                            const Vec3 mp = st.position(b_->unit_by_id(m)->h);
                            const float dx = mp.x - tp.x, dy = mp.y - tp.y, len = dmath::sqrt(dx * dx + dy * dy);
                            x = len > 0.0f ? tp.x + dx / len * d : tp.x + d;
                            y = len > 0.0f ? tp.y + dy / len * d : tp.y;
                        }
                    }
                } else if (order == lua::LuaOrder::AttackMove) {
                    for (const objects::ObjectId id : group.units) b_->combat_->set_attack_goal(id, tc::kNoObject);
                }
            }
            if (order != lua::LuaOrder::AttackMove || tw == cmd::kNoObject)  // refixf t06: position / guard / move blocks
                for (const objects::ObjectId id : group.units) b_->combat_->set_attack_goal(id, tc::kNoObject);
            if (!std::isfinite(x) || !std::isfinite(y)) return R::InvalidPosition;
            // S7 t22: the squadrons' formation - block flags 0x01 attack-move / 0x02 guard; an Attack_Move / Guard of an
            // object keeps it as the slot goal (Move_To(object) is a position snapshot, kind 1).
            const uint8_t fflags = order == lua::LuaOrder::AttackMove ? mv::kMoveFlagAttackMove
                                   : order == lua::LuaOrder::Guard    ? mv::kMoveFlagGuardMove
                                                                      : uint8_t{0};
            const objects::ObjectId fobj =
                tw != cmd::kNoObject && order != lua::LuaOrder::MoveTo ? static_cast<objects::ObjectId>(tw) : objects::kNoObjectId;
            const uint32_t targ = le32(b, 20);  // REMAINING_GAPS §4.2: Move_To threat threshold (float bits, 0 = off)
            const float tt = order == lua::LuaOrder::MoveTo && targ != 0u ? std::bit_cast<float>(targ) : mv::kThreatOff;
            // refixf t02 / t06 (MOVEMENT §6.3 [H]): Lua Attack_Move / Move_To on a task force build one parameter block and ONE
            // coordinator for all its units, so the formation slots and the destination reservations (§2.4 step 4) give each
            // unit its own spot. The tactical AI now sends that as one group order (the ids above); the t02 stand-in that
            // merged one frame's per-unit orders here is gone. An object's own Lua order stays a one-unit move.
            if (group.units.size() == 1) {  // a single unit: unchanged path
                if (order != lua::LuaOrder::AttackMove) b_->combat_->clear_order(u->id);
                if (!b_->movement_locked(u->id)) b_->fighter_order(group.units, fflags, Vec3{x, y, 0.0f}, fobj);
                return move_unit(u->id, x, y, tt) ? R::None : R::InvalidUnit;
            }
            group.threat_threshold = tt;
            const tactical::Aabb& gb = mode_->bounds();
            group.destination = {std::clamp(x, gb.min.x, gb.max.x), std::clamp(y, gb.min.y, gb.max.y)};
            b_->drop_locked(group.units);  // OI follow-up t03: locked members keep their order [M: group-move reading]
            if (group.units.empty()) return R::None;
            b_->movement_->move(group, host_->random().synced(), b_->frame_);  // one formation move of the team
            b_->fighter_order(group.units, fflags, Vec3{group.destination.x, group.destination.y, 0.0f}, fobj);  // S7 t22
            if (order != lua::LuaOrder::AttackMove)
                for (const objects::ObjectId id : group.units) b_->combat_->clear_order(id);
            return R::None;
        }
        case T::Reinforce: {
            // --- A2 tactical: the human reinforcement path (TacticalBattle::order_reinforce) + the tactical
            // population cap (population_cap: the cap the battle itself does not test on land). ---
            const auto key = static_cast<objects::ObjectId>(ev.u("unit"));
            if (!b_->space_) {
                const float cap = population_cap(player);
                for (const TacticalBattle::Participant& p : b_->parts_)
                    if (p.galactic_id == key && p.owner == static_cast<int32_t>(player) && p.reserve && !p.deployed && cap >= 0.0f &&
                        population_used(player) + p.pop > cap)
                        return R::QueueFull;
            }
            return b_->order_reinforce(*mode_, player, key, ev.f("x"), ev.f("y"), host_->random().synced());
            // --- end A2 tactical ---
        }
        case T::Retreat: return b_->request_retreat(player) ? R::None : R::NotAllowed;
        case T::Production:  // skirmish station production (Produce_Force of non-UC types): the battle's Production handler
            return b_->hg_production_command(*mode_, player, ev);
        case T::TacticalBuild: {
            // --- A2 tactical: TacticalBuildEvent 0x42edf0 for an AI player (L1_S4_REMAINING §3.6): refusals in order,
            // then the build starts at once without a charge (pad_build, check_rules false). ---
            // battle host: the shared TacticalBuild hook (TacticalBattle::tactical_build, ai = true: same refusal order, no
            // charge; the UC / constructed objects become battle combat units).
            const uint32_t pw = ev.u("pad");
            if (pw == commands::kNoObject || pw > 0x7FFFFFFFu) return R::InvalidTarget;
            return b_->tactical_build(*mode_, player, static_cast<objects::ObjectId>(pw), static_cast<objects::TypeIndex>(ev.u("type_index")),
                                      host_->random().synced(), true);
            // --- end A2 tactical ---
        }
        case T::SetUnitAbilityMode:
        case T::TacticalSpecialAbility:
        case T::TacticalSpecialAbilityWithDummyTarget:
        case T::SetAbilityAutofire: return b_->handle_command(*mode_, player, ev, host_->random().synced());
        default: return R::Unhandled;
    }
}

std::vector<at::TacticalReinforcement> BattleAi::reinforcement_pool(int32_t player) {
    std::vector<at::TacticalReinforcement> out;
    for (const TacticalBattle::Participant& p : b_->parts_) {
        if (p.owner != player || !p.reserve || p.deployed) continue;
        at::TacticalReinforcement r;
        r.key = p.galactic_id;
        r.type = static_cast<int32_t>(p.type);
        r.population = p.pop;
        r.units = static_cast<uint32_t>(b_->spawn_types(p).size());
        // --- A2 tactical: count(E) and the member match facts (AI_GALACTIC_3 §1.3) ---
        const objects::GameObjectTypes& types = mode_->types();
        const auto ti = static_cast<objects::TypeIndex>(p.type);
        if (ti < types.size()) {
            const auto members = types.get_list(ti, "Company_Units");
            const auto n = static_cast<int32_t>(members.size());
            if (n > 0) {
                const objects::TypeIndex m = types.resolve_name(members[0]);
                if (m != objects::kNone && m < types.size()) {
                    r.member_type = static_cast<int32_t>(m);
                    r.member_create_team = types.get_bool(m, "Create_Team");
                }
                const int32_t squad = std::max(1, types.get_int(ti, "Max_Squad_Size"));
                r.count = r.member_create_team ? (n + squad - 1) / squad : n;
            }
        }
        // --- end A2 tactical ---
        out.push_back(r);
    }
    return out;
}

bool BattleAi::can_reinforce(int32_t player) {
    if (b_->finished_) return false;
    const int32_t side = player == b_->req_.info.defender ? 0 : player == b_->req_.info.attacker ? 1 : -1;
    return side >= 0 && b_->retreat_side_ != side;
}

// --- A2 tactical: tactical population caps (AI_GALACTIC_3 §3.3) [H]: space = the faction's Space_Tactical_Unit_Cap;
// land campaign battle = Max_Ground_Forces_On_Planet for the defender, min(that, capacity) for any other player, where
// capacity = Base_Population_Capacity + Σ Additional_Population_Capacity of the player's live objects in the battle
// (the reinforcement points it holds). [port] land skirmish takes the same rule: no Land_Skirmish_Unit_Cap curve yet. ---
float BattleAi::population_cap(int32_t player) {
    const auto& ps = b_->galaxy_->players();
    const uint32_t f = player >= 0 && static_cast<size_t>(player) < ps.size() ? ps[static_cast<size_t>(player)].faction : galactic::kNoIndex;
    if (b_->space_) return b_->rules_->faction(f).space_unit_cap;
    const float planet_cap = static_cast<float>(b_->galaxy_->rules().constants.max_ground_forces_on_planet);
    if (player == b_->req_.info.defender) return planet_cap;
    const objects::ObjectStore& st = mode_->primary();
    const objects::GameObjectTypes& types = mode_->types();
    int32_t capacity = f < b_->galaxy_->rules().factions.size() ? b_->galaxy_->rules().factions[f].base_population_capacity : 0;
    for (const objects::ObjectHandle h : st.owned_by(player))
        if (st.active(h) && st.type(h) < types.size()) capacity += types.get_int(st.type(h), "Additional_Population_Capacity");
    return std::min(planet_cap, static_cast<float>(capacity));
}

// §3.2: Σ population shares of the player's live battle objects [port: per galactic participant, charged once], then
// (int)sum + 1 when the fraction exceeds Allow_Reinforcement_Percentage_Normalized (shipped 0.0: any fraction rounds up).
float BattleAi::population_used(int32_t player) {
    float pop = 0.0f;
    std::vector<uint8_t> counted(b_->parts_.size(), 0);
    for (const TacticalBattle::Unit& u : b_->units_)
        if (u.alive && !u.retreated && b_->parts_[u.participant].owner == player && !counted[u.participant]) {
            counted[u.participant] = 1;
            pop += b_->parts_[u.participant].pop;
        }
    const float whole = std::floor(pop);
    return pop - whole > allow_reinforcement_fraction_ ? whole + 1.0f : whole;
}

float BattleAi::type_population(int32_t type) const {  // pop(T, mode) 0x373500
    const objects::GameObjectTypes& types = mode_->types();
    if (type < 0 || static_cast<size_t>(type) >= types.size()) return 0.0f;
    const auto ti = static_cast<objects::TypeIndex>(type);
    const char* tag = b_->space_ ? "Space_Override_Population_Value" : "Land_Override_Population_Value";
    if (!types.get_string(ti, tag).empty() && types.get_int(ti, tag) >= 0) return static_cast<float>(types.get_int(ti, tag));
    return static_cast<float>(types.get_int(ti, "Population_Value"));
}

// Skirmish station production: the player's live objects whose builder list (their faction row of
// Tactical_Buildable_Objects_Multiplayer) holds `type`, ascending id; {} in campaign / land battles (§1.11).
std::vector<int32_t> BattleAi::tactical_builders(int32_t player, int32_t type) {
    std::vector<int32_t> out;
    if (!b_->production_enabled() || type < 0) return out;
    const objects::ObjectStore& st = mode_->primary();
    for (const objects::ObjectHandle h : st.owned_by(player)) {
        if (!st.active(h)) continue;
        const objects::ObjectId id = st.id(h);
        if (b_->hg_build_check(st, player, id, static_cast<objects::TypeIndex>(type)) != 1) out.push_back(static_cast<int32_t>(id));
    }
    std::sort(out.begin(), out.end());
    return out;
}

// True only in skirmish space battles with production: not Cinematic_Object_Only, a price, and some builder passes the
// Production checks now (list / faction row, prerequisites, Build_Limit_Current_For_All_Allies). Credits are the
// PotentialPlan's own check.
bool BattleAi::can_build_now(int32_t player, int32_t type) {
    if (!b_->production_enabled() || type < 0) return false;
    const objects::GameObjectTypes& types = b_->catalog_->types();
    const auto t = static_cast<objects::TypeIndex>(type);
    if (t >= types.size() || types.get_bool(t, "Cinematic_Object_Only")) return false;
    if (types.get_int(t, "Tactical_Build_Cost_Multiplayer") <= 0) return false;
    const objects::ObjectStore& st = mode_->primary();
    for (const int32_t id : tactical_builders(player, type))
        if (b_->hg_build_check(st, player, static_cast<objects::ObjectId>(id), t) == 0) return true;
    return false;
}
// --- end A2 tactical ---

const at::DamageTrackingStore* BattleAi::damage_tracking() { return &b_->damage_tracking_; }
const galactic::ContrastTable* BattleAi::contrast() { return &b_->rules_->contrast; }

// --- A2 tactical: build pads (L1_S4_REMAINING §3.6; TacticalBattle pads, tactical_battle_pads.cpp) ---------------

namespace {
bool pad_name_equal(std::string_view a, std::string_view b) {
    while (!a.empty() && (a.front() == ' ' || a.front() == '\t')) a.remove_prefix(1);
    while (!a.empty() && (a.back() == ' ' || a.back() == '\t')) a.remove_suffix(1);
    return ai::ai_iequal(a, b);
}
}  // namespace

// The pads `player` can build on: live TACTICAL_BUILD_OBJECTS objects owned by the player or an ally (0x39a230), empty,
// not fogged for the player, unblocked, cooldown over, ascending id [M: the free store's pad table +0xE8 order]; accepts = the UC types of the pad type's Tactical_Buildable_Objects_Campaign
// row for the player's faction (S10 battles are campaign battles, as pad_build).
std::vector<at::TacticalPad> BattleAi::build_pads(int32_t player) {
    std::vector<at::TacticalPad> out;
    const objects::ObjectStore& st = mode_->primary();
    const objects::GameObjectTypes& types = mode_->types();
    std::string faction;
    const auto& ps = b_->galaxy_->players();
    if (player >= 0 && static_cast<size_t>(player) < ps.size() && ps[static_cast<size_t>(player)].faction < b_->galaxy_->rules().factions.size())
        faction = b_->galaxy_->rules().factions[ps[static_cast<size_t>(player)].faction].name;
    for (const objects::ObjectHandle h : st.all_objects()) {
        if (!st.alive(h) || !st.has_behavior(h, kBuildObjects)) continue;
        const objects::ObjectId id = st.id(h);
        // TacticalBuild Setup A (AI_GALACTIC_3 §1.7): friendly / visible, empty and unblocked (0x42e400), cooldown over.
        if (build_pad_check(player, static_cast<int32_t>(id), -1) != at::PadCheck::Ok) continue;
        at::TacticalPad p;
        p.id = id;
        const Vec3 q = st.position(h);
        p.position = {q.x, q.y, q.z};
        bool in_row = false;
        for (const std::string_view tok : types.get_list(st.type(h), "Tactical_Buildable_Objects_Campaign")) {
            const objects::TypeIndex t = types.resolve_name(tok);
            if (t == objects::kNone || t >= types.size()) {
                in_row = pad_name_equal(tok, faction);
                continue;
            }
            if (in_row && std::find(p.accepts.begin(), p.accepts.end(), static_cast<int32_t>(t)) == p.accepts.end())
                p.accepts.push_back(static_cast<int32_t>(t));
        }
        if (!p.accepts.empty()) out.push_back(std::move(p));
    }
    std::sort(out.begin(), out.end(), [](const at::TacticalPad& a, const at::TacticalPad& b) { return a.id < b.id; });
    return out;
}

// Build / Build_All's three refusals (AI_TACTICAL §7.5): not friendly / visible (0x39a2c0: allied-owned and not fogged
// for the player), game rules (0x42e400: empty and no live non-allied Influences_Capture_Point object within the pad
// type's Capture_Point_Radius, 3-D), rebuild cooldown (0x42daf0).
at::PadCheck BattleAi::build_pad_check(int32_t player, int32_t pad, int32_t) {
    const objects::ObjectStore& st = mode_->primary();
    const objects::GameObjectTypes& types = mode_->types();
    const objects::ObjectHandle ph = pad >= 0 ? st.find(static_cast<objects::ObjectId>(pad)) : objects::ObjectHandle{};
    if (!st.alive(ph) || !st.has_behavior(ph, kBuildObjects)) return at::PadCheck::NotFriendlyOrVisible;
    const int32_t owner = st.owner(ph);
    const tc::CombatSystem& c = *b_->combat_;
    if (owner < 0 || (owner != player && c.is_enemy(player, owner))) return at::PadCheck::NotFriendlyOrVisible;
#ifdef ALAMO_SIM_HAVE_FOG
    if (b_->fog() && b_->fog_object_hidden(player, static_cast<objects::ObjectId>(pad), true)) return at::PadCheck::NotFriendlyOrVisible;
#endif
    if (b_->pad_contents(static_cast<objects::ObjectId>(pad)) != objects::kNoObjectId) return at::PadCheck::Blocked;
    const objects::TypeIndex pt = st.type(ph);
    const double r = pt < types.size() ? static_cast<double>(types.get_float(pt, "Capture_Point_Radius")) : 0.0;
    const Vec3 pp = st.position(ph);
    for (const objects::ObjectHandle h : st.all_objects()) {
        if (!st.alive(h) || h == ph) continue;
        const objects::TypeIndex t = st.type(h);
        if (t >= types.size() || !types.get_bool(t, "Influences_Capture_Point")) continue;
        const int32_t o = st.owner(h);
        if (o == owner || !c.is_enemy(owner, o)) continue;
        const Vec3 q = st.position(h);
        const double dx = static_cast<double>(q.x) - pp.x, dy = static_cast<double>(q.y) - pp.y, dz = static_cast<double>(q.z) - pp.z;
        if (dx * dx + dy * dy + dz * dz <= r * r) return at::PadCheck::Blocked;
    }
    if (const TacticalBattle::PadState* s = b_->pad_state(static_cast<objects::ObjectId>(pad)); s && s->cooldown_end > b_->frame_)
        return at::PadCheck::RebuildCooldown;
    return at::PadCheck::Ok;
}
// --- end A2 tactical ---

// --- battle host follow-up: TEAM order expansion, opportunity targets, HIJACK -----------------------------------------

// ---- refixf t06: space TF.Reinforce ring search (header) ----
bool BattleAi::space_drop_frame(int32_t player, at::SpaceDropFrame& out) {
    uint8_t side = 0;
    for (const TacticalBattle::Participant& p : b_->parts_)
        if (p.owner == player) {
            side = p.side;
            break;
        }
    const Vec3 f = b_->facing_[side];
    out.entry_facing_deg = static_cast<float>(dmath::atan2(static_cast<double>(f.y), static_cast<double>(f.x)) * (180.0 / dmath::kPi));
    const tactical::Aabb& bb = mode_->bounds();
    out.has_box = true;
    out.min_x = bb.min.x;
    out.min_y = bb.min.y;
    out.max_x = bb.max.x;
    out.max_y = bb.max.y;
    return true;
}

bool BattleAi::space_prevention_hit(int32_t player, const at::Vec3f& p) {
    const objects::ObjectStore& st = mode_->primary();
    const objects::GameObjectTypes& types = mode_->types();
    for (const objects::ObjectHandle h : st.all_objects()) {  // GOM list +0x2f0 stand-in: GOM order
        if (!st.active(h)) continue;
        const objects::TypeIndex t = st.type(h);
        if (t >= types.size()) continue;
        const float r = types.get_float(t, "Reinforcement_Prevention_Radius");
        if (!(r > 0.0f)) continue;
        const int32_t o = st.owner(h);
        if (o >= 0 && (o == player || !b_->combat_->is_enemy(player, o))) continue;  // allied (0x39a230)
        const Vec3 q = st.position(h);
        const float dx = p.x - q.x, dy = p.y - q.y;
        if (dx * dx + dy * dy < r * r) return true;
    }
    return false;
}

bool BattleAi::space_drop_ok(int32_t player, int32_t key, const at::Vec3f& p, float facing_deg, float sweep) {
    const tactical::Aabb& bb = mode_->bounds();
    if (!(p.x >= bb.min.x && p.x <= bb.max.x && p.y >= bb.min.y && p.y <= bb.max.y)) return false;
    if (space_prevention_hit(player, p)) return false;
#ifdef ALAMO_SIM_HAVE_FOG
    if (!b_->fog_reinforce_point_ok(player, p.x, p.y)) return false;  // fogged for the player (force 1)
#endif
    const TacticalBattle::Participant* part = nullptr;
    for (const TacticalBattle::Participant& q : b_->parts_)
        if (q.galactic_id == static_cast<objects::ObjectId>(key) && q.owner == player && q.reserve && !q.deployed) {
            part = &q;
            break;
        }
    if (!part || !b_->movement_) return false;
    const objects::GameObjectTypes& types = mode_->types();
    const auto ti = static_cast<objects::TypeIndex>(part->type);
    if (ti >= types.size()) return false;
    const mv::UnitDesc ud = mv::unit_desc_from_type(types, ti);
    auto layer = static_cast<uint32_t>(types.get_mask(ti, "Space_Layer"));
    if (layer == 0) layer = 4u;
    double sn = 0.0, cs = 1.0;
    dmath::sincos(static_cast<double>(facing_deg) * (dmath::kPi / 180.0), sn, cs);
    const mv::V2 to{p.x, p.y};
    const mv::V2 from{p.x - sweep * static_cast<float>(cs), p.y - sweep * static_cast<float>(sn)};
    const float now = static_cast<float>(b_->frame_);
    return b_->movement_->space().query_segment(from, to, now, now + 115.0f, ud.radius, layer | 8u, tc::kNoObject, true, b_->frame_) == 0;
}

std::vector<objects::ObjectId> BattleAi::team_units(uint8_t player, uint32_t wire) const {
    std::vector<objects::ObjectId> out;
    if (wire == cmd::kNoObject || wire > 0x7FFFFFFFu) return out;
    const auto id = static_cast<objects::ObjectId>(wire);
    const objects::ObjectStore& st = mode_->primary();
    if (const TacticalBattle::Unit* u = b_->unit_by_id(id)) {
        if (u->alive && !u->retreated && st.owner(u->h) == static_cast<int32_t>(player)) out.push_back(u->id);
        return out;
    }
    for (size_t c = 0; c < b_->containers_.size(); ++c) {  // a team container: its live members, ascending id
        if (b_->containers_[c].id != id || b_->containers_[c].removed) continue;
        for (const TacticalBattle::Unit& m : b_->units_)
            if (m.container == c && m.alive && !m.retreated && st.owner(m.h) == static_cast<int32_t>(player)) out.push_back(m.id);
        break;
    }
    return out;
}

std::vector<at::OpportunityTargetEvent> BattleAi::opportunity_targets() {
    std::vector<at::OpportunityTargetEvent> out;
    std::vector<std::pair<uint64_t, int32_t>> now;
    const tc::CombatSystem& c = *b_->combat_;
    for (const TacticalBattle::Unit& u : b_->units_) {
        if (!u.alive || u.retreated || !c.has_unit(u.id)) continue;
        for (uint32_t k = 0; k < c.hardpoint_count(u.id) && k < 256; ++k) {
            const tc::ObjectId t = c.hardpoint_opportunity_target(u.id, k);
            if (t == tc::kNoObject) continue;
            const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(u.id)) << 8) | k;
            now.emplace_back(key, t);
            const auto it = std::lower_bound(opp_prev_.begin(), opp_prev_.end(), key,
                                             [](const std::pair<uint64_t, int32_t>& e, uint64_t v) { return e.first < v; });
            if (it != opp_prev_.end() && it->first == key && it->second == t) continue;  // same target as before
            if (!c.alive(t)) continue;
            at::OpportunityTargetEvent e;
            e.owner = u.container != 0xFFFFFFFFu ? b_->containers_[u.container].id : u.id;  // signal to the team object
            e.target = t;
            e.hardpoint_type = static_cast<int32_t>(c.hardpoint_kind(u.id, k));
            out.push_back(e);
        }
    }
    opp_prev_ = std::move(now);  // ascending (units ascending id, hard points ascending)
    return out;
}

at::TacticalReinforcement BattleAi::hijack_unit(int32_t player, int32_t type, bool steal) {
    at::TacticalReinforcement fail;
    GalacticMode* gm = host_->galactic_mode();
    if (b_->finished_ || !gm || !gm->sim() || type < 0 || !b_->can_add_reserve(static_cast<uint32_t>(type))) return fail;
    galactic::GalacticSim& gs = *gm->sim();
    const PlanetIndex planet = b_->req_.info.planet;  // step 1: the destination is the battle planet
    const bool hero = b_->rules_->type(static_cast<uint32_t>(type)).hero;
    const objects::ObjectHandle h = gs.magic_spawn(player, planet, static_cast<uint32_t>(type), steal, hero);  // steps 2-4
    if (!gs.store().alive(h)) return fail;
    // Step 5: a unit created on the surface launches into the orbit fleet (op 4 + form; FAILED when blockaded).
    const auto& surface = gs.planets()[planet].surface;
    if (std::find(surface.begin(), surface.end(), h) != surface.end()) {
        const objects::ObjectHandle one[] = {h};
        if (gs.ai_blockaded(planet) || gs.launch_units(player, planet, one) != cmd::RejectReason::None) return fail;
    }
    if (!b_->add_reserve(h, player)) return fail;
    const auto key = static_cast<int32_t>(gs.store().id(h));
    for (const at::TacticalReinforcement& r : reinforcement_pool(player))
        if (r.key == key) return r;
    return fail;
}
// --- end battle host ---

void BattleAi::unit_released(int32_t player, int32_t object_id, ai::tactical::TacticalAiHost::UnitRelease path) {
    if (b_->finished_ || !b_->abilities_ || object_id < 0) return;
    const objects::ObjectStore& st = mode_->primary();
    const objects::ObjectHandle h = st.find(static_cast<objects::ObjectId>(object_id));
    if (!st.alive(h) || st.owner(h) != player) return;
    alamo::tactical::abilities::AbilitySystem& ab = *b_->abilities_;
    if (!ab.has_object(object_id)) return;
    // 0x5ff610: no Active_By_Default while movement is locked (0x39ad40: the counter's shares and the forced-move term,
    // A1_READINGS §28.9; HUNT is not part of it).
    if (path == ai::tactical::TacticalAiHost::UnitRelease::TaskForce && b_->movement_locked(object_id)) return;
    ab.apply_active_by_default(object_id, b_->frame_);
}

// S7 t15 A1: the tick's signal 0x29 list (TacticalBattle::ability_finished_events).
std::vector<int32_t> BattleAi::ability_finished() {
    const std::span<const objects::ObjectId> ids = b_->ability_finished_events();
    return std::vector<int32_t>(ids.begin(), ids.end());
}

bool BattleAi::movement_locked(int32_t object_id) { return object_id >= 0 && b_->movement_locked(object_id); }

// [port] The S10 stand-in (attack-move to 70 % of the attack range of the nearest enemy) for the sides that have
// no tactical AI, on its own cadence.
void BattleAi::standin_orders() {
    if (b_->frame_ == 0 || (b_->frame_ - 1) % kStandinInterval != 0) return;
    const objects::ObjectStore& st = mode_->primary();
    for (const TacticalBattle::Unit& u : b_->units_) {
        if (!u.alive || u.retreated || !u.movable || controls(st.owner(u.h))) continue;
        if (b_->commanded_by_player(st.owner(u.h))) continue;  // integration fixes: human units take commands only
        const Vec3 up = st.position(u.h);
        const TacticalBattle::Unit* best = nullptr;
        float best_d2 = 0.0f;
        for (const TacticalBattle::Unit& e : b_->units_) {
            if (!e.alive || e.retreated || e.side == u.side) continue;
            const Vec3 ep = st.position(e.h);
            const float dx = ep.x - up.x, dy = ep.y - up.y, d2 = dx * dx + dy * dy;
            if (!best || d2 < best_d2) {
                best = &e;
                best_d2 = d2;
            }
        }
        if (!best) continue;
        const float range = std::max(u.range, 100.0f), dist = dmath::sqrt(best_d2);
        if (dist <= 0.8f * range) continue;
        const Vec3 ep = st.position(best->h);
        const float k = (dist - 0.7f * range) / dist;
        const Vec3 dest{up.x + (ep.x - up.x) * k, up.y + (ep.y - up.y) * k, 0.0f};
        move_unit(u.id, dest.x, dest.y);
        if (!b_->movement_locked(u.id)) b_->fighter_order(std::span<const objects::ObjectId>(&u.id, 1), 0, dest);  // S7 t22
        ++b_->stats_.orders;
    }
}

}  // namespace alamo::sim
#endif
