#include "alamo/sim/tactical_battle.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include "alamo/core/dmath.hpp"
#include "alamo/core/hash.hpp"
#include "alamo/core/profile.hpp"
#include "alamo/galactic/special_data.hpp"  // P4 tactical: special_name_crc (Black Market gate)
#include "alamo/objects/game_object_types.hpp"
#include "alamo/sim/map_spawn.hpp"
#include "alamo/tactical/movement/ted_grid.hpp"
#include "alamo/tactical/movement/special_locomotors.hpp"
#include "alamo/tactical/movement/unit_params.hpp"
#ifdef ALAMO_SIM_HAVE_ANIM
#include <map>
#include <utility>

#include "alamo/sim/sim_anim.hpp"
#endif

namespace alamo::sim {

namespace tc = alamo::tactical::combat;
namespace ta = alamo::tactical::abilities;
namespace mv = alamo::tactical::movement;
namespace cmd = alamo::commands;
using galactic::kNoPlayer;

namespace {

#ifdef ALAMO_SIM_HAVE_AI_TACTICAL
constexpr objects::BehaviorTypeId kDamageTrackingBehavior = 0x3E;  // DAMAGE_TRACKING (serviced, interval 30)
constexpr uint32_t kDamageTrackingInterval = 30;
#endif
constexpr uint32_t kNoContainer = 0xFFFFFFFFu;

Vec3 facing_of_yaw(float yaw_deg) {
    float s = 0.0f, c = 1.0f;
    dmath::sincos(yaw_deg * (dmath::kPiF / 180.0f), s, c);
    return {c, s, 0.0f};
}

float yaw_of(Vec3 f) { return dmath::atan2(f.y, f.x) * (180.0f / dmath::kPiF); }

tactical::Vec3 tv(Vec3 v) { return tactical::Vec3{v.x, v.y, v.z}; }
Vec3 sv(tactical::Vec3 v) { return Vec3{v.x, v.y, v.z}; }

std::string victory_name(uint8_t v) {
    switch (v) {
        case galactic::kVictoryAllDestroyed: return "all-destroyed";
        case galactic::kVictoryAllRetreated: return "all-retreated";
        case galactic::kVictoryStarbaseDestroyed: return "starbase-destroyed";
        default: return std::to_string(v);
    }
}

}  // namespace

// ---- CombatHooks forwarder: the ability system first, then the animation hookups (R3) -------------------------

class BattleHooks final : public tc::CombatHooks {
public:
    BattleHooks(TacticalBattle& b, ta::AbilitySystem& a) : b_(&b), a_(&a) {}
    void set_tick(SimRng* rng, uint64_t global_frame) {
        rng_ = rng;
        gframe_ = global_frame;
    }
#ifdef ALAMO_SIM_HAVE_ANIM
    void set_anim(SimAnimation* anim, anim::AnimWorld* world, objects::ModeKind kind) {
        anim_ = anim;
        world_ = world;
        kind_ = kind;
    }
    anim::AnimWorld* world() const { return world_; }
    // --- R3 anim: behaviour hand-offs (SimAnimation: flinch on hits, death clip / corpse, death clones) ---
    void set_anim_mode(const GameMode* mode) { mode_ = mode; }
    void anim_hit(const tc::HitEvent& e) {
        // Flinch (0x62dde0) of a live victim; travel direction = impact point -> victim centre (fallback:
        // shooter -> victim). Victims dying this tick are skipped (their death clip follows).
        if (!anim_ || !mode_ || !world_ || !rng_ || e.victim == tc::kNoObject || !world_->contains(e.victim)) return;
        const tc::CombatSystem& c = *b_->combat_;
        if (!c.has_unit(e.victim)) return;
        for (const tc::DeathEvent& d : c.deaths())
            if (d.id == e.victim) return;
        const tactical::Vec3 vp = c.position(e.victim), f = c.facing(e.victim);
        Vec3 dir{vp.x - e.position.x, vp.y - e.position.y, 0.0f};
        if (dir.x == 0.0f && dir.y == 0.0f && e.shooter != tc::kNoObject && c.has_unit(e.shooter)) {
            const tactical::Vec3 sp = c.position(e.shooter);
            dir = Vec3{vp.x - sp.x, vp.y - sp.y, 0.0f};
        }
        anim_->hit(*mode_, e.victim, Vec3{f.x, f.y, f.z}, dir, *rng_, gframe_);
    }
    void anim_death(objects::ObjectId id, bool has_clone, uint8_t kind) {
        // Die + DeathBehavior: the death clip (by damage kind when the type sets none), spin-away, then the
        // corpse rows (SimAnimation). [port] With a Death_Clone the clone carries the death animation (anim_clone)
        // and the original is removed at once.
        if (!anim_ || !mode_ || !rng_ || has_clone) return;
        anim_->begin_death(*mode_, id, *rng_, gframe_, kind);
    }
    const GameMode* anim_mode() const { return mode_; }
    bool anim_clone(objects::ObjectHandle clone, uint8_t kind, const anim::DeathPlan* plan) {  // false: no animation service
        if (!anim_ || !mode_) return false;
        anim_->death_clone_spawned(*mode_, mode_->primary().id(clone), kind, plan);
        return true;
    }
    // refix t03: the infantry corpse path - the dying unit's row becomes the clone type's corpse (no clone object).
    bool anim_corpse_clone(objects::ObjectId dying, int32_t clone_type, uint8_t kind) {
        if (!anim_ || !mode_ || !rng_ || clone_type < 0) return false;
        return anim_->corpse_clone(*mode_, dying, static_cast<objects::TypeIndex>(clone_type), *rng_, gframe_, kind) !=
               anim::DeathResult::NoRow;
    }
    // --- end R3 anim ---
#endif

    void begin_tick(uint32_t frame) override { a_->begin_tick(frame); }
    void before_units(uint32_t frame, SimRng& rng) override {
        b_->l1_fields(frame, rng);  // L1 systems: nebula / ion storm / asteroid field behaviours (phase D start)
        b_->oi_apply_crush(frame);  // OI tactical: crush damage (movement CrushEvents of this tick)
        b_->bt_apply_attrition(frame);  // battle host: retreat attrition victims (TACTICAL_GAPS §4.9)
        a_->before_units(frame, rng);
    }
    void unit_begin(tc::ObjectId id, uint32_t frame, SimRng& rng) override { a_->unit_begin(id, frame, rng); }
    void unit_after_targeting(tc::ObjectId id, uint32_t frame, SimRng& rng) override { a_->unit_after_targeting(id, frame, rng); }
    bool shot_allowed(tc::ObjectId shooter, tc::ObjectId target, int32_t target_hp, int32_t& projectile) override {
#ifdef ALAMO_SIM_HAVE_ANIM
        // ANIM_PARTICLES A.8: the weapon refuses to fire while the object plays a flinch clip.
        if (world_ && world_->contains(shooter) && anim::is_flinch(world_->current_type(shooter))) return false;
#endif
        return a_->shot_allowed(shooter, target, target_hp, projectile);
    }
    bool fire_gate(tc::ObjectId shooter, const tc::HardpointDesc* hardpoint, int32_t& projectile, float& damage_scale) override {
        return a_->fire_gate(shooter, hardpoint, projectile, damage_scale);
    }
    void fire_done(tc::ObjectId shooter, const tc::HardpointDesc* hardpoint, uint32_t frame) override {
        a_->fire_done(shooter, hardpoint, frame);
    }
    void shot_fired(tc::ObjectId shooter, tc::ObjectId target, int32_t target_hp, int32_t projectile, uint32_t frame) override {
        a_->shot_fired(shooter, target, target_hp, projectile, frame);
#ifdef ALAMO_SIM_HAVE_ANIM
        play_attack(shooter);
        // R3 anim: muzzle flash - the hardpoint's fire bones from the fire-bone query of this shot, else the unit's
        // own weapon (bone -1).
        if (anim_ && mode_ && world_ && world_->contains(shooter)) {
            if (muzzle_shooter_ == shooter) {  // hardpoint shot: its index, plus its fire bones when they resolved
                anim_->muzzle_flash(*mode_, shooter, muzzle_hp_, muzzle_bones_.first, gframe_);
                if (muzzle_bones_.second >= 0) anim_->muzzle_flash(*mode_, shooter, muzzle_hp_, muzzle_bones_.second, gframe_);
            } else {
                anim_->muzzle_flash(*mode_, shooter, -1, -1, gframe_);
            }
        }
        muzzle_shooter_ = tc::kNoObject;
#endif
    }
    float lucky_shot(tc::ObjectId source, tc::ObjectId victim, int32_t& hardpoint, tactical::Vec3 impact, bool has_impact) override {
        return a_->lucky_shot(source, victim, hardpoint, impact, has_impact);
    }
    bool damage_listener(tc::ObjectId victim, tc::ObjectId source, int32_t projectile_desc, uint32_t frame, SimRng* rng) override {
        return a_->damage_listener(victim, source, projectile_desc, frame, rng);
    }
    void hardpoint_destroyed(tc::ObjectId owner, uint32_t index, tc::HardpointKind kind, bool last_of_kind, uint32_t frame) override {
        a_->hardpoint_destroyed(owner, index, kind, last_of_kind, frame);
    }
    // HARDPOINT_DISABLED.md §9: Set_System_Online offline (destroyed or disabled) / online (repaired to max).
    void hardpoint_offline(tc::ObjectId owner, uint32_t index, tc::HardpointKind kind, bool last_of_kind, bool destroyed,
                           uint32_t frame) override {
        a_->hardpoint_offline(owner, index, kind, last_of_kind, destroyed, frame);
    }
    void hardpoint_online(tc::ObjectId owner, uint32_t index, tc::HardpointKind kind, uint32_t frame) override {
        a_->hardpoint_online(owner, index, kind, frame);
    }
    void unit_dying(tc::ObjectId id, uint32_t frame) override { a_->unit_dying(id, frame); }
    void ion_stunned(tc::ObjectId id, uint32_t frame) override { a_->ion_stunned(id, frame); }
    bool ion_storm_shields_down(tc::ObjectId id, uint32_t frame) const override { return a_->ion_storm_shields_down(id, frame); }  // L1 systems
    void units_removed() override { a_->units_removed(); }
    // OI abilities: detonation effects / land stuns (TACTICAL_GAPS §1.10)
    void unit_effect(tc::ObjectId id, tc::UnitEffect e, int32_t player, tactical::Vec3 p, uint32_t frame) override {
        a_->unit_effect(id, e, player, p, frame);
    }
    // --- fog follow-up: S8 targeting filter step 4 / special-weapon force 1 (read-only: parallel gather phase) ---
    bool target_hidden(int32_t player, tc::ObjectId target, bool force) const override {
#ifdef ALAMO_SIM_HAVE_FOG
        return b_->fog_object_hidden(player, target, force);
#else
        (void)player, (void)target, (void)force;
        return false;
#endif
    }
    // --- end fog follow-up ---
    bool fire_bones(tc::ObjectId shooter, uint32_t hardpoint, tactical::Vec3& a, tactical::Vec3& b) override {
#ifdef ALAMO_SIM_HAVE_ANIM
        return anim_fire_bones(shooter, hardpoint, a, b);
#else
        (void)shooter, (void)hardpoint, (void)a, (void)b;
        return false;
#endif
    }

private:
#ifdef ALAMO_SIM_HAVE_ANIM
    uint32_t object_type(tc::ObjectId id) const {
        const tc::CombatSystem& c = *b_->combat_;
        const int32_t t = c.data().units[c.desc_index(id)].object_type;
        return t < 0 ? 0xFFFFFFFFu : static_cast<uint32_t>(t);
    }
    anim::Mat34 object_to_world(tc::ObjectId id, uint32_t type) const {
        // Position, yaw facing (the model's nose -Y along it, z up) and Scale_Factor [port: no pitch / roll].
        const tc::CombatSystem& c = *b_->combat_;
        const tactical::Vec3 p = c.position(id), f = c.facing(id);
        float s = type < b_->catalog_->types().size() ? b_->catalog_->types().columns().scale_factor[type] : 1.0f;
        if (!(s > 0.0f)) s = 1.0f;
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
        // Model columns: X = left (-fy, fx), Y = -facing (Alamo models face -Y), Z = up (render::object_transform).
        anim::Mat34 m;
        m.m[0][0] = -fy * s;
        m.m[0][1] = -fx * s;
        m.m[0][2] = 0.0f;
        m.m[0][3] = p.x;
        m.m[1][0] = fx * s;
        m.m[1][1] = -fy * s;
        m.m[1][2] = 0.0f;
        m.m[1][3] = p.y;
        m.m[2][0] = 0.0f;
        m.m[2][1] = 0.0f;
        m.m[2][2] = s;
        m.m[2][3] = p.z;
        return m;
    }
    void play_attack(tc::ObjectId id) {
        // WeaponBehavior (0x54d090, A.7 attack row): after the projectile, Attack with a random variant
        // (synced), blend 0.4 or 0.1 from a finished attack / Attack Idle; Attack_Animation_Is_Overlay ->
        // overlay slot, timed 3.0 [M].
        if (!world_ || !world_->contains(id)) return;
        const anim::AnimModel* m = world_->model(id);
        if (!m || m->variant_count(anim::kAnimAttack) == 0) return;
        const uint32_t t = object_type(id);
        if (t == 0xFFFFFFFFu) return;
        const anim::TypeAnimParams& p = anim_->params_for(t, kind_);
        if (p.attack_overlay) {
            world_->play_overlay(id, anim::kAnimAttack, anim::kRandomVariant, 1.0f, 3.0f, rng_, gframe_);
        } else {
            const anim::SlotView cur = world_->slot(id);
            anim::PlayRequest r;
            r.type = anim::kAnimAttack;
            r.index = anim::kRandomVariant;
            r.loop = false;
            r.blend_seconds =
                ((anim::is_attack_like(cur.type) || cur.type == anim::kAnimAttackIdle) && cur.mode == anim::SlotMode::Finished) ? 0.1f : 0.4f;
            world_->play(id, r, rng_, gframe_);
        }
        ++b_->stats_.anim_plays;
    }
    bool anim_fire_bones(tc::ObjectId shooter, uint32_t hp, tactical::Vec3& a, tactical::Vec3& b) {
        // R3 anim: the hardpoint of this fire query (the shot that follows flashes it, bones filled below).
        muzzle_shooter_ = shooter;
        muzzle_hp_ = static_cast<int32_t>(hp);
        muzzle_bones_ = {-1, -1};
        if (!world_ || !world_->contains(shooter)) return false;
        const anim::AnimModel* m = world_->model(shooter);
        if (!m || !m->has_skeleton) return false;
        const tc::CombatSystem& c = *b_->combat_;
        const tc::UnitDesc& ud = c.data().units[c.desc_index(shooter)];
        if (hp >= ud.hardpoints.size()) return false;
        const uint32_t hd = ud.hardpoints[hp];
        if (hd >= b_->tables_->fire_bones.size()) return false;
        const auto key = std::make_pair(m, hd);
        auto it = bones_.find(key);
        if (it == bones_.end()) {
            const auto& names = b_->tables_->fire_bones[hd];
            const int32_t ba = names[0].empty() ? -1 : m->skeleton.find(names[0]);
            const int32_t bb = names[1].empty() ? -1 : m->skeleton.find(names[1]);
            it = bones_.emplace(key, std::make_pair(ba, bb)).first;
        }
        if (it->second.first < 0) return false;
        muzzle_bones_ = it->second;  // R3 anim: the shot that follows flashes these bones
        const anim::Mat34 w = object_to_world(shooter, object_type(shooter));
        anim::Float3 pa, pb;
        if (!world_->bone_world_position(shooter, it->second.first, w, pa)) return false;
        pb = pa;
        if (it->second.second >= 0 && !world_->bone_world_position(shooter, it->second.second, w, pb)) pb = pa;
        a = tactical::Vec3{pa.x, pa.y, pa.z};
        b = tactical::Vec3{pb.x, pb.y, pb.z};
        return true;
    }
    SimAnimation* anim_ = nullptr;
    anim::AnimWorld* world_ = nullptr;
    objects::ModeKind kind_ = objects::ModeKind::Space;
    const GameMode* mode_ = nullptr;  // R3 anim
    tc::ObjectId muzzle_shooter_ = tc::kNoObject;              // R3 anim: last fire-bone query
    std::pair<int32_t, int32_t> muzzle_bones_{-1, -1};        // R3 anim
    int32_t muzzle_hp_ = -1;                                  // R3 anim: HardPoints index of that query
    std::map<std::pair<const anim::AnimModel*, uint32_t>, std::pair<int32_t, int32_t>> bones_;  // lookup only
#endif
    TacticalBattle* b_;
    ta::AbilitySystem* a_;
    SimRng* rng_ = nullptr;
    uint64_t gframe_ = 0;
};

// ---- construction ---------------------------------------------------------------------------------------------

TacticalBattle::TacticalBattle(const BattleCatalog& catalog, const galactic::GalacticSim& galaxy,
                               const galactic::ConflictRules& rules, galactic::BattleRequest request, BattleConfig cfg)
    : catalog_(&catalog), galaxy_(&galaxy), rules_(&rules), req_(std::move(request)), cfg_(cfg) {
    space_ = req_.info.kind == galactic::ConflictKind::Space;
    for (const galactic::BattleForces& f : req_.forces) {
        const uint8_t side = f.player == req_.info.defender ? 0 : 1;
        for (objects::ObjectHandle h : f.start) add_participant(h, f.player, side, false, false, false);
        for (objects::ObjectHandle h : f.reserve) add_participant(h, f.player, side, true, false, false);
    }
    const objects::ObjectStore& gs = galaxy.store();
    if (gs.alive(req_.starbase)) {
        const int32_t o = gs.owner(req_.starbase);
        add_participant(req_.starbase, o, o == req_.info.defender ? 0 : 1, false, true, false);
    }
    for (objects::ObjectHandle s : req_.structures)
        if (gs.alive(s)) add_participant(s, gs.owner(s), gs.owner(s) == req_.info.defender ? 0 : 1, false, false, true);
    bt_force_split();  // battle host: land lead / raid split, space super-weapon start rule (TACTICAL_GAPS §4.2 / §4.6)
    // Types spawned in this battle, first-use order -> the battle tables (team containers after their members,
    // the dummy target type last).
    auto use = [this](uint32_t t) {
        if (std::find(types_used_.begin(), types_used_.end(), t) == types_used_.end()) types_used_.push_back(t);
    };
    for (const Participant& p : parts_) {
        const std::vector<uint32_t> st = spawn_types(p);
        for (uint32_t t : st) use(t);
        if (!rules_->type(p.type).members.empty()) use(p.type);
        // §3.4: the company's TEAM object type and its TEAM behaviour type (the company type when Create_Team_Type is empty).
        uint32_t ot = 0xFFFFFFFFu, bt = 0xFFFFFFFFu, squad = 0;
        if (!rules_->type(p.type).members.empty() && !st.empty() && bt_team(p.type, st.front(), ot, bt, squad)) {
            use(ot);
            use(bt);
        }
        // §3.2 steps 2-3: Tactical_Additional_Structure_Type temporaries, Place_Other_Type_At_Every_Specific_Marker_Position.
        if (p.structure) {
            const objects::GameObjectTypes& types = catalog.types();
            for (const char* tag : {"Tactical_Additional_Structure_Type", "Place_Other_Type_At_Every_Specific_Marker_Position"}) {
                const objects::TypeIndex x = types.resolve_name(types.get_string(p.type, tag));
                if (x != objects::kNone && x < types.size()) use(static_cast<uint32_t>(x));
            }
        }
    }
    const objects::TypeIndex dummy = catalog.types().resolve_name("Dummy_Barrage_Target");
    if (dummy != objects::kNone) {
        dummy_type_ = static_cast<int32_t>(dummy);
        use(static_cast<uint32_t>(dummy));
    }
    l1_add_types(types_used_);  // L1 systems: the mode's special-weapon types (projectiles in the tables)
    tables_ = std::make_unique<BattleTables>(catalog.build_battle(types_used_, descs_, space_));  // battle host: model bones
}

TacticalBattle::~TacticalBattle() = default;

// N1 stage 2 (tactical_battle_save.cpp): after a load, the combat hooks get the tick context of the saving run's last
// battle tick (command-phase hooks before the next battle tick use it).
void TacticalBattle::resume_tick_context() {
    if (frame_ > 0 && hooks_ && bt_rng_) hooks_->set_tick(bt_rng_, oi_global_frame_);
}

void TacticalBattle::add_participant(objects::ObjectHandle g, int32_t owner, uint8_t side, bool reserve, bool starbase,
                                     bool structure) {
    const objects::ObjectStore& gs = galaxy_->store();
    if (!gs.alive(g)) return;
    Participant p;
    p.galactic = g;
    p.galactic_id = gs.id(g);
    p.owner = owner;
    p.type = static_cast<uint32_t>(gs.type(g));
    p.side = side;
    p.reserve = reserve;
    p.starbase = starbase;
    p.structure = structure;
    const galactic::ConflictTypeInfo& ti = rules_->type(p.type);
    p.pop = static_cast<float>(ti.space_population_override >= 0 ? ti.space_population_override : ti.population);
    const auto& types = galaxy_->rules().types;
    p.power = p.type < types.size() ? types[p.type].ai_combat_power : 0.0f;
    for (const auto& [mt, n] : ti.members)
        if (mt < types.size()) p.power += static_cast<float>(n) * types[mt].ai_combat_power;
    parts_.push_back(p);
}

std::vector<uint32_t> TacticalBattle::spawn_types(const Participant& p) const {
    std::vector<uint32_t> out;
    const galactic::ConflictTypeInfo& ti = rules_->type(p.type);
    if (ti.members.empty()) {
        out.push_back(p.type);
    } else {
        for (const auto& [mt, n] : ti.members)
            for (int32_t i = 0; i < n; ++i) out.push_back(mt);
    }
    return out;
}

uint32_t TacticalBattle::desc_of(uint32_t type) const {
    for (size_t i = 0; i < types_used_.size(); ++i)
        if (types_used_[i] == type) return descs_[i];
    return 0;
}

const BattleTables::UnitVisuals* TacticalBattle::visuals_of_type(uint32_t type) const {
    for (size_t i = 0; i < types_used_.size(); ++i)
        if (types_used_[i] == type) return descs_[i] < tables_->units.size() ? &tables_->units[descs_[i]] : nullptr;
    return nullptr;
}

TacticalBattle::Unit* TacticalBattle::unit_by_id(objects::ObjectId id) {
    const auto it = std::lower_bound(units_.begin(), units_.end(), id, [](const Unit& u, objects::ObjectId v) { return u.id < v; });
    return (it == units_.end() || it->id != id) ? nullptr : &*it;
}

// FIGHTER_FLYING §6.3.1 type data: Uses_Multiple_Locomotors (+0x74), Stationary_Space_Layer (+0x16e0), the query
// half-extents 0x396850 (Custom_Hard_X/YExtent else |land model bbox max x / y|, x Scale_Factor) and the land model's
// (type +0x2a0, no fallback to other modes' models) bounding box z for the hit value. Pure function of the type and the
// model files: cached per type, not saved.
bool TacticalBattle::clearance_info(objects::ObjectId id, mv::ClearanceInfo& out) {
    const Unit* u = unit_by_id(id);
    const objects::GameObjectTypes& types = catalog_->types();
    if (!u || u->type >= types.size()) return false;
    const uint32_t t = u->type;
    if (clr_known_.size() < types.size()) {
        clr_known_.resize(types.size(), 0u);
        clr_types_.resize(types.size());
    }
    if (!clr_known_[t]) {
        mv::ClearanceInfo ci;
        ci.uses_multiple_locomotors = types.get_bool(t, "Uses_Multiple_Locomotors");
        ci.stationary_space_layer = static_cast<uint32_t>(types.get_int(t, "Stationary_Space_Layer"));
        ci.scale_factor = types.columns().scale_factor[t];
        const BattleCatalog::ModelInfo* m = catalog_->model_info(t, false, false);
        const bool box = m && m->has_bounds;
        ci.has_box = box;
        if (box) {
            ci.box_lo_z = m->min[2];
            ci.box_hi_z = m->max[2];
        }
        ci.half = mv::clearance_half_extents(types.get_float(t, "Custom_Hard_XExtent"), types.get_float(t, "Custom_Hard_YExtent"),
                                             ci.scale_factor, box, box ? m->max[0] : 0.0f, box ? m->max[1] : 0.0f);
        clr_types_[t] = ci;
        clr_known_[t] = 1u;
    }
    out = clr_types_[t];
    return true;
}

// SPACE_AVOIDANCE §2 / §3 / §7: the avoidance tree members of a space battle. Membership (§2.1, decided by the type:
// not Is_Asteroid_Field / Is_Ion_Storm / Is_Nebula, Space_Layer with a bit other than Land) is a pure function of the
// type, so evaluating it over the live objects equals the retail insert-at-spawn / remove-at-destroy set [port: the
// Uses_Multiple_Locomotors branch of 0x397900 is land-only]. Positions / facings are the committed store values; the
// model is the type's Space_Model_Name collision data (BattleCatalog::avoid_model). [port] the model-level collide
// switch (off while contained) is not modelled: space members are never garrisoned.
void TacticalBattle::avoid_snapshot(const objects::ObjectStore& store) {
    avoid_members_.clear();
    const objects::GameObjectTypes& types = catalog_->types();
    if (avoid_type_.size() < types.size()) avoid_type_.resize(types.size(), int8_t{-1});
    for (const ObjectHandle h : store.objects_newest_first()) {
        if (!store.alive(h)) continue;
        const uint32_t t = store.type(h);
        if (t >= types.size()) continue;
        if (avoid_type_[t] < 0) {
            const bool member = !types.get_bool(t, "Is_Asteroid_Field") && !types.get_bool(t, "Is_Ion_Storm") &&
                                !types.get_bool(t, "Is_Nebula") && (types.get_mask(t, "Space_Layer") & ~uint64_t{0x10}) != 0;
            const bool shielded = types.has_behavior(t, objects::BehaviorSet::Union, objects::beh::Shielded);
            avoid_type_[t] = static_cast<int8_t>(member ? (shielded ? 2 : 1) : 0);
        }
        if (avoid_type_[t] == 0) continue;
        mv::AvoidObstacle o;
        o.id = store.id(h);
        const Vec3 p = store.position(h), f = store.facing(h);
        o.pos = {p.x, p.y, p.z};
        o.facing = {f.x, f.y};
        o.scale = types.columns().scale_factor[t];
        o.model = catalog_->avoid_model(t, true);
        o.skip_shield = avoid_type_[t] == 2;
        mv::avoid_place(o);
        avoid_members_.push_back(o);
    }
    std::sort(avoid_members_.begin(), avoid_members_.end(),
              [](const mv::AvoidObstacle& a, const mv::AvoidObstacle& b) { return a.id < b.id; });
}

// ---- setup ---------------------------------------------------------------------------------------------------------

void TacticalBattle::setup(TacticalMode& mode, terrain::MapData map, SimRng& rng, uint32_t players, const ParallelFor& pf) {
    map_ = std::move(map);
    // --- OI objects: computed behaviour intervals (SHIELDED / POWERED) from GameConstants, before any object exists ---
    {
        const tc::CombatConfig& gc = catalog_->combat_config();
        objects::BehaviorIntervalConfig ic;
        ic.fps = gc.fps;
        ic.shield_recharge_interval_secs = gc.shield_recharge_interval_secs;
        ic.energy_recharge_interval_secs = gc.energy_recharge_interval_secs;
        mode.primary().set_interval_config(ic);
    }
    // --- end OI objects ---
    bt_rng_ = &rng;            // battle host: pad objects join the battle with their add_unit draws
    bt_players_ = players;
    bt_map_prepass();          // REMAINING_GAPS §3.1: base-level filter + branched-map discard; map types -> tables
    bt_add_pad_types(map_);    // battle host: the map's pad UC / constructed types join the tables (before combat exists)
    oi_add_ability_types();    // OI abilities: ability-spawned / survivor types join the tables (before combat exists)
    hg_add_types();            // hangars: launched squadrons / ships and the station chain join the tables
    const objects::GameObjectTypes& types = catalog_->types();
    float mn[2] = {-5000.0f, -5000.0f}, mx[2] = {5000.0f, 5000.0f};
    if (!map_.playable_box(mn, mx)) {
        mn[0] = mn[1] = -5000.0f;
        mx[0] = mx[1] = 5000.0f;
    }
    float zmin = -1.0e6f, zmax = 1.0e6f;
    if (map_.is_space()) {
        zmin = map_.space.playable_min[2];
        zmax = map_.space.playable_max[2];
    }
    mode.set_bounds({{mn[0], mn[1], zmin}, {mx[0], mx[1], zmax}});
    // [port] fallback anchors / facings (maps without Defending Forces / Attacker Entry markers): +-45% of the box on X,
    // facing each other. bt_place_forces replaces them from the markers (§3.2 steps 6-7).
    {
        const float cx = 0.5f * (mn[0] + mx[0]), cy = 0.5f * (mn[1] + mx[1]), hx = 0.5f * (mx[0] - mn[0]);
        anchor_[0] = {cx + 0.45f * hx, cy, 0.0f};
        anchor_[1] = {cx - 0.45f * hx, cy, 0.0f};
        facing_[0] = {-1.0f, 0.0f, 0.0f};
        facing_[1] = {1.0f, 0.0f, 0.0f};
    }
    // Movement world.
    mv::MovementConfig mc;
    mc.mode = space_ ? mv::MoveMode::Space : mv::MoveMode::Land;
    movement_ = std::make_unique<mv::MovementSystem>(mc);
    // OI tactical: crush hostility = the combat relation (player +0x370 table, MOVEMENT.md 5.5).
    movement_->set_hostility([this](int32_t a, int32_t b) { return combat_ && combat_->is_enemy(a, b); });
#ifdef ALAMO_SIM_HAVE_ANIM
    // REMAINING_GAPS §4.3-4.7: special-locomotor anim lengths = the model's clip (variant 0) in sim frames (30 / s).
    movement_->set_anim_length_fn([this](objects::ObjectId id, int32_t type) -> uint32_t {
        const anim::AnimWorld* w = hooks_ ? hooks_->world() : nullptr;
        const anim::AnimModel* m = w ? w->model(id) : nullptr;
        const anim::ClipRef* c = m ? m->clip(type, 0) : nullptr;
        if (!c || !c->clip || c->clip->fps <= 0.0f) return 0u;
        return static_cast<uint32_t>(std::ceil(static_cast<double>(c->clip->frame_count) * 30.0 / static_cast<double>(c->clip->fps)));
    });
#endif
    if (space_) {
        mv::space_from_map(movement_->space(), map_, &types);
        // FIGHTER_FLYING §5: the fighter cell grids build from the playable box at the first tick with a fighter; a
        // map without one gets the fallback box above.
        if (!movement_->space().has_box()) movement_->space().set_box({mn[0], mn[1]}, {mx[0], mx[1]});
        // SPACE_AVOIDANCE: the fighter avoidance hook over this tick's tree members (avoid_snapshot).
        movement_->set_fighter_avoid_fn([this](mv::ObjectId, mv::F3 c, mv::F3 h, mv::F3 a, mv::F3 b, mv::F3& hit) {
            return mv::space_avoid(avoid_members_, c, h, a, b, hit);
        });
    } else {
        mv::PassabilityGrid grid = mv::grid_from_map(map_);
        // battle host: model bounds for the LAND_OBSTACLE footprints without Obstacle_Width / _Height (MOVEMENT 1.3).
        mv::MapStampHooks stamp_hooks;
        const BattleCatalog* cat = catalog_;
        stamp_hooks.model_half_extents = [cat](uint32_t t, float& hx, float& hy, float& hz) { return cat->model_half_extents(t, hx, hy, hz); };
        mv::stamp_map_obstacles(grid, map_, types, &stamp_hooks);
        movement_->set_grid(std::move(grid));
        movement_->air().set_box({mn[0], mn[1]}, {mx[0], mx[1]});  // REMAINING_GAPS §4.3-4.6: air units plan in the playable box
        const terrain::MapData* m = &map_;
        movement_->set_height_fn([m](float x, float y) { return m->terrain.sample_height(x, y); });
        // FIGHTER_FLYING §6: flying hooks. object_position = a circling target that is not a mover (structures: the
        // combat position). clearance (0x63b640, §6.3.1) = the movers' Corvette-layer query with the type data of
        // clearance_info (MovementSystem::clearance_query).
        mv::FlyingHooks fh;
        fh.clearance = [this](mv::ObjectId id, mv::FlyVec3 from, mv::FlyVec3 to, float m) {
            return movement_->clearance_query(id, from, to, m,
                                              [this](mv::ObjectId o, mv::ClearanceInfo& ci) { return clearance_info(o, ci); });
        };
        fh.object_position = [this](mv::ObjectId id, mv::FlyVec3& pos) {
            if (!combat_ || !combat_->has_unit(id) || !combat_->alive(id)) return false;
            const tactical::Vec3 p = combat_->position(id);
            pos = mv::FlyVec3{p.x, p.y, p.z};
            return true;
        };
        movement_->set_flying_hooks(std::move(fh));
    }
    // Combat wrapped by the ability system (S9: it installs itself as the combat hooks; the forwarder adds the
    // animation hookups and replaces it as the installed hooks).
    tc::CombatConfig cc = catalog_->combat_config();  // OI tactical: GameConstants-driven defaults
    cc.mode = space_ ? tc::GameMode::Space : tc::GameMode::Land;
    cc.object_max_health_multiplier = space_ ? catalog_->space_health_multiplier() : catalog_->land_health_multiplier();
    cc.hardpoint_health_multiplier = space_ ? 1.5f : 1.0f;
    std::vector<tc::PlayerInfo> infos(players);
    for (uint32_t p = 0; p < players && p < galaxy_->players().size(); ++p) {
        infos[p].ai = !galaxy_->players()[p].human;
        // TACTICAL_COMBAT §2.2: the target scan skips players whose faction is Is_Neutral (player +0x68 -> faction +0x108).
        infos[p].active = !bt_owner_neutral(static_cast<int32_t>(p));
    }
    combat_ = std::make_unique<tc::CombatSystem>(tables_->combat, cc, std::move(infos));
    // [M] an Is_Neutral player attacks nobody (BATTLE_TRANSITIONS: "a Neutral player is simply not an enemy"): its own row of
    // the relation matrix is cleared, so its map structures (abandoned turrets) pick no targets until captured. The other
    // players' rows keep it an enemy, so ordered attacks (Attack, an AI Attack_Move to the object) still reach its objects.
    for (uint32_t p = 0; p < players; ++p)
        if (bt_owner_neutral(static_cast<int32_t>(p)))
            for (uint32_t q = 0; q < players; ++q)
                if (q != p) combat_->set_enemy(static_cast<int32_t>(p), static_cast<int32_t>(q), false);
    combat_->reserve(256, 2048, 32, 256);
    abilities_ = std::make_unique<ta::AbilitySystem>(*combat_, tables_->abilities, catalog_->ability_config(space_, cfg_.skirmish));
    abilities_->reserve(256, 512);
    // --- P4 tactical: campaign Black Market gate, before any unit is added (Active_By_Default / automatic specials) ---
    for (uint32_t p = 0; p < players && p < galaxy_->players().size(); ++p) {
        std::vector<uint32_t> blocked = black_market_blocked_abilities(tables_->abilities, *galaxy_, static_cast<int32_t>(p));
        if (!blocked.empty()) abilities_->set_black_market_blocked(static_cast<int32_t>(p), std::move(blocked));
    }
    // --- end P4 tactical ---
    hooks_ =std::make_unique<BattleHooks>(*this, *abilities_);
    combat_->set_hooks(hooks_.get());
    combat_->bind_rng(&rng);  // Take_Damage listeners called outside the tick (host damage) draw here
    hg_setup(mode);           // hangars: SPAWN_SQUADRON descriptors and the HangarSystem (before any unit exists)
    oi_install(mode, rng);    // OI abilities: AbilityHost hooks, the abilities' RNG, repair credit hooks
#ifdef ALAMO_SIM_HAVE_ANIM
    if (anim_) hooks_->set_anim(anim_, &anim_->world(mode), mode.kind());
    if (anim_) hooks_->set_anim_mode(&mode);  // R3 anim: behaviour hand-offs
#endif
    retreat_allowed_frame_ = static_cast<uint32_t>((space_ ? rules_->space_retreat_allowed : rules_->land_retreat_allowed) * 30.0f);
    fog_begin(mode, players);  // fog of war: grids (owner: fog agent)
    bt_map_load(mode, rng);      // REMAINING_GAPS §3.1: map objects in map order (Map_Load_Spawn_Table draws)
    bt_pad_restore(mode);        // §6.5: persistent pad children
    l1_setup(mode);            // L1 systems: special weapons / hyperspace / engines (before the deploys)
    bt_place_forces(mode, rng);  // §3.2: starbase, structures, defender / attacker start forces
    bt_landing_points(mode);     // §3.3: the attacker's map-owned reinforcement points
    mode.finish_load(pf);
    space_obstacles_sync(mode);  // refixf t02: the starbase / structures as space static entries (MOVEMENT §2.2)
    bt_shield_stamps(mode);  // battle host: base-shield secondary occupants (land)
    fog_after_deploy(mode);  // fog of war: dense mask, battle scale (owner: fog agent)
}

Vec3 TacticalBattle::slot_position(uint8_t side, uint32_t mask) {
    const uint32_t k = placed_[side]++;
    const float spacing = space_ ? cfg_.space_spacing : cfg_.land_spacing;
    const float row = static_cast<float>(k / 6u), col = static_cast<float>(k % 6u) - 2.5f;
    const Vec3 f = facing_[side];
    const Vec3 right{-f.y, f.x, 0.0f};
    Vec3 p{anchor_[side].x - f.x * row * spacing + right.x * col * spacing,
           anchor_[side].y - f.y * row * spacing + right.y * col * spacing, 0.0f};
    float mn[2], mx[2];
    if (map_.playable_box(mn, mx)) {
        p.x = std::clamp(p.x, mn[0], mx[0]);
        p.y = std::clamp(p.y, mn[1], mx[1]);
    }
    if (!space_ && movement_) {
        const mv::PassabilityGrid& g = movement_->grid();
        const int32_t cx = mv::world_to_cell(p.x), cy = mv::world_to_cell(p.y);
        bool found = false;
        for (int32_t r = 0; r <= 60 && !found; ++r)
            for (int32_t dy = -r; dy <= r && !found; ++dy)
                for (int32_t dx = -r; dx <= r && !found; ++dx) {
                    if (std::max(std::abs(dx), std::abs(dy)) != r) continue;
                    if (!g.passable_cell(cx + dx, cy + dy, mask)) continue;
                    const mv::V2 c = mv::cell_center(cx + dx, cy + dy);
                    p.x = c.x;
                    p.y = c.y;
                    found = true;
                }
        p.z = map_.terrain.sample_height(p.x, p.y);
    }
    return p;
}

void TacticalBattle::deploy(TacticalMode& mode, uint32_t pi, SimRng& rng) {
    Participant& P = parts_[pi];
    if (P.deployed) return;
    P.deployed = true;
    const objects::GameObjectTypes& types = catalog_->types();
    const uint8_t side = P.side;
    const bool team = !rules_->type(P.type).members.empty();
    if (P.unplaced) return;  // §3.2 step 4: no free marker -> not spawned (the galactic structure is untouched)
    // 1. member objects (each consumes an id), movement.
    const size_t first = units_.size();
    std::vector<uint32_t> members_types = spawn_types(P);
    if (P.other_type >= 0) members_types.assign(P.spots.size(), static_cast<uint32_t>(P.other_type));  // §3.2 step 3
    size_t spot = 0;
    for (uint32_t t : members_types) {
        mv::UnitDesc md = mv::unit_desc_from_type(types, t);
        md.locomotor = mv::locomotor_kind_from_type(types, t, space_ ? mv::MoveMode::Space : mv::MoveMode::Land);  // REMAINING_GAPS §4.3-4.7
        Vec3 pos;
        Vec3 face = facing_[side];
        const bool lead_sw = space_ && bt_have_ds_ && !P.reserve && bt_lead_ == objects::kNoObjectId && bt_is_super_weapon(t);
        if (spot < P.spots.size()) {  // §3.2: the marker chosen for the starbase / structure / step-3 type
            pos = P.spots[spot].first;
            face = P.spots[spot].second;
            ++spot;
        } else if (P.starbase) {
            pos = {anchor_[side].x - facing_[side].x * 600.0f, anchor_[side].y - facing_[side].y * 600.0f, 0.0f};
        } else if (lead_sw) {
            pos = bt_ds_pos_;  // battle host: the spawned super-weapon at DEATH_STAR_POSITION (TACTICAL_GAPS §4.8) [M]
        } else {
            pos = slot_position(side, space_ ? 0u : movement_->classes().mask(md.movement_class));
        }
        objects::SpawnParams sp;
        sp.type = t;
        sp.owner = P.owner;
        sp.position = pos;
        sp.facing = face;
        sp.parent_mode_id = P.galactic_id;
        const objects::ObjectHandle h = mode.spawn(sp, 1.0f);
        if (lead_sw) {  // battle host: lead object +0x28 = the spawned super-weapon's id
            bt_lead_ = mode.primary().id(h);
            bt_lead_participant_ = pi;
            bt_used_ = true;
        }
        Unit u;
        u.id = mode.primary().id(h);
        u.h = h;
        u.participant = pi;
        u.type = t;
        u.side = side;
        u.movable = mode.movable(h) && !P.starbase && !P.structure;
        const auto& ti = galaxy_->rules().types;
        u.power = t < ti.size() ? ti[t].ai_combat_power : 0.0f;
        if (u.movable) {
            md.id = u.id;
            md.pos = {pos.x, pos.y};
            md.z = pos.z;
            md.yaw_deg = yaw_of(face);
            md.owner = P.owner;  // OI tactical: crush hostility
            movement_->add_unit(md);
        }
        units_.push_back(u);
        ++P.spawned;
        ++P.alive;
        ++stats_.spawned;
        if (P.reserve) story_reinforced_.push_back(u.id);  // L1/S4 wave 2b story hook (STORY_REINFORCE)
    }
    // 2. REMAINING_GAPS §3.4 team helper 0x2a5a60 (start placement 0x2a00b0): it runs when the first spawned member's type
    //    has Create_Team, or always in space. Members in list order in chunks of min(k, remaining), k = the company's
    //    Max_Squad_Size (default 10); per chunk one object of the Create_Team_Type (fallback "Team"; neither resolves ->
    //    nothing), owner / facing of the chunk's first member, position = the chunk's float centroid [M: the land
    //    passability snap of the centroid is not applied], TEAM behaviour type = the team type, or the company type when
    //    Create_Team_Type is empty. Land companies without Create_Team keep separate members (no container). Containers
    //    are created after every member (ids ascending: members, then the teams in chunk order); then the members join
    //    combat / abilities in id order, then the containers (S10-int: not in the trees, not combat units; the ability
    //    system services them). [M] Space formation offsets of the members are not applied (members keep their slots).
    uint32_t team_obj = 0xFFFFFFFFu, team_beh = 0xFFFFFFFFu, squad = 0;
    const bool run_team = team && units_.size() > first && bt_team(P.type, units_[first].type, team_obj, team_beh, squad);
    std::vector<std::vector<tc::ObjectId>> groups;
    const size_t c0 = containers_.size();
    if (run_team) {
        const size_t n = units_.size() - first, g = static_cast<size_t>(squad);
        for (size_t s = 0; s < n; s += g) {
            const size_t end = std::min(n, s + g);
            Vec3 c{0.0f, 0.0f, 0.0f};
            for (size_t i = first + s; i < first + end; ++i) {
                const Vec3 p = mode.primary().position(units_[i].h);
                c.x = c.x + p.x;
                c.y = c.y + p.y;
                c.z = c.z + p.z;
            }
            const float inv = static_cast<float>(end - s);
            objects::SpawnParams sp;
            sp.type = static_cast<objects::TypeIndex>(team_obj);
            sp.owner = mode.primary().owner(units_[first + s].h);
            sp.position = Vec3{c.x / inv, c.y / inv, c.z / inv};
            sp.facing = mode.primary().facing(units_[first + s].h);
            const objects::ObjectHandle ch = mode.primary().create(sp);
            Container ct;
            ct.id = mode.primary().id(ch);
            ct.h = ch;
            ct.team = true;
            ct.type = team_beh;
            ct.owner = sp.owner;
            ct.squad_type = P.type;  // S7 fighters: Squadron_Formation_Error_Tolerance
            ct.alive = static_cast<uint32_t>(end - s);
            groups.emplace_back();
            for (size_t i = first + s; i < first + end; ++i) {
                units_[i].container = static_cast<uint32_t>(containers_.size());
                groups.back().push_back(units_[i].id);
            }
            containers_.push_back(ct);
            ++stats_.containers;
            bt_used_ = true;
        }
    }
    // 3. combat / ability units (hardpoint draws, HardPoints order), then the containers.
    for (size_t i = first; i < units_.size(); ++i) {
        Unit& u = units_[i];
        tc::UnitSpawn cs;
        cs.id = u.id;
        cs.desc = desc_of(u.type);
        cs.owner = P.owner;
        cs.position = tv(mode.primary().position(u.h));
        cs.facing = tv(mode.primary().facing(u.h));
        abilities_->add_unit(cs, cs.desc, frame_, rng, u.container != kNoContainer ? containers_[u.container].id : tc::kNoObject);
        hg_adopt(u, rng);  // hangars: engine mode enter, SPAWN_SQUADRON add_carrier (attach order)
        u.range = combat_->attack_range(u.id);
        s4_register(mode, u);  // S4: garrison holder / occupant
        l1_register(mode, u.h, static_cast<int32_t>(u.type));  // L1 systems: special-weapon / TSW pack
#ifdef ALAMO_SIM_HAVE_AI_TACTICAL
        if (mode.primary().has_behavior(u.h, kDamageTrackingBehavior)) {
            damage_tracking_.enable(u.id, combat_->max_health(u.id), combat_->max_shield_of(u.id));
            u.tracked = true;
            u.dt_next = frame_;  // a new behaviour instance is due at once
        }
#endif
    }
    for (size_t k = 0; k < groups.size(); ++k)  // OI abilities: + the container's SAP-init draws
        abilities_->add_container(containers_[c0 + k].id, desc_of(team_beh), P.owner, groups[k], frame_, &rng);
}

// refixf t02 (MOVEMENT §2.2 [H]): SpaceObstacleBehaviorClass inserts its object into the space occupancy as a static entry on
// attach and removes it on detach. Map placements enter through mv::space_from_map (ids -(placement index + 1)); battle units
// whose type has SPACE_OBSTACLE (the starbase, space structures, upgraded / built stations) enter here under their object id,
// so the destination search (§2.4) and the planner keep ships off them. Before, the starbase was no obstacle: an Attack_Move /
// Move_To on it (one order per task-force unit) sent each ship to its exact centre, the ships stacked on one point and
// Project_By_Unit_Range gave nil (foc-station seeds 1 and 4). [port] attach = the first sync (end of setup, end of each tick)
// with the unit live, detach = the first sync after it died or retreated; footprint as space_from_map (radius) at the attach
// position + Space_Obstacle_Offset rotated by the facing [M: rotation sense]; units adopted from the map keep space_from_map's
// entry (not added twice).
void TacticalBattle::space_obstacles_sync(const TacticalMode& mode) {
    if (!space_ || !movement_) return;
    mv::SpaceOccupancy& occ = movement_->space();
    const objects::ObjectStore& st = mode.primary();
    const objects::GameObjectTypes& types = catalog_->types();
    for (const Unit& u : units_) {
        if (!u.alive || u.retreated || !st.alive(u.h)) {
            occ.remove_static(u.id);
            continue;
        }
        if (parts_[u.participant].map || occ.has_static(u.id)) continue;
        float r = 0.0f;
        uint32_t bits = 0;
        if (!mv::space_obstacle_footprint(types, u.type, r, bits)) continue;
        const Vec3 p = st.position(u.h), f = st.facing(u.h);
        const std::array<float, 4> off = types.get_vec(static_cast<objects::TypeIndex>(u.type), "Space_Obstacle_Offset");
        const float len = dmath::sqrt(f.x * f.x + f.y * f.y);
        const float fx = len > 0.0f ? f.x / len : 1.0f, fy = len > 0.0f ? f.y / len : 0.0f;
        occ.add_static(u.id, {p.x + off[0] * fx - off[1] * fy, p.y + off[0] * fy + off[1] * fx}, r, bits);
    }
}

// ---- tick ----------------------------------------------------------------------------------------------------------

float TacticalBattle::side_power(uint8_t side) const {
    float p = 0.0f;
    for (const Unit& u : units_)
        if (u.side == side && u.alive && !u.retreated && !parts_[u.participant].third) p += u.power;
    for (const Participant& q : parts_)
        if (q.side == side && !q.deployed && !q.third) p += q.power;
    return p;
}

void TacticalBattle::reinforce(TacticalMode& mode, SimRng& rng, bool force_empty_sides) {
    for (uint8_t side = 0; side < 2; ++side) {
        bool empty = true;
        float live_pop = 0.0f;
        std::vector<uint8_t> counted(parts_.size(), 0);
        for (const Unit& u : units_)
            if (u.side == side && u.alive && !u.retreated && !parts_[u.participant].third) {
                empty = false;
                if (!counted[u.participant]) {
                    counted[u.participant] = 1;
                    live_pop += parts_[u.participant].pop;
                }
            }
        const bool due = cfg_.reinforce_interval > 0 && frame_ % cfg_.reinforce_interval == 0;
        if (!due && !(force_empty_sides && empty)) continue;
        const int32_t player = side_player(side);
        const uint32_t f = player >= 0 && static_cast<size_t>(player) < galaxy_->players().size()
                               ? galaxy_->players()[static_cast<size_t>(player)].faction
                               : galactic::kNoIndex;
        const float cap = rules_->faction(f).space_unit_cap;
        bool first = true;
        for (uint32_t i = 0; i < parts_.size(); ++i) {
            Participant& p = parts_[i];
            if (p.side != side || !p.reserve || p.deployed) continue;
            if (!empty && commanded_by_player(p.owner)) continue;  // P host: a human player's reserves deploy by Reinforce commands
            const bool fits = !space_ || live_pop + p.pop <= cap;
            if (!fits && !(empty && first)) continue;  // [port] an empty side always gets its next pool unit
            deploy(mode, i, rng);
            live_pop += p.pop;
            ++stats_.reinforcements;
            first = false;
        }
    }
}

void TacticalBattle::issue_orders(TacticalMode& mode, SimRng& rng) {
    const objects::ObjectStore& store = mode.primary();
    for (const Unit& u : units_) {
        if (!u.alive || u.retreated || !u.movable) continue;
        if (commanded_by_player(store.owner(u.h))) continue;  // integration fixes: human units take commands only
        const Vec3 up = store.position(u.h);
        const Unit* best = nullptr;
        float best_d2 = 0.0f;
        for (const Unit& e : units_) {
            if (!e.alive || e.retreated || e.side == u.side) continue;
            const Vec3 ep = store.position(e.h);
            const float dx = ep.x - up.x, dy = ep.y - up.y;
            const float d2 = dx * dx + dy * dy;
            if (!best || d2 < best_d2) {
                best = &e;
                best_d2 = d2;
            }
        }
        if (!best) continue;
        const float range = std::max(u.range, 100.0f);
        const float dist = dmath::sqrt(best_d2);
        if (dist <= 0.8f * range) continue;  // in range: hold, combat fires
        const Vec3 ep = store.position(best->h);
        const float k = (dist - 0.7f * range) / dist;
        mv::MoveOrder o;
        o.units = {u.id};
        o.destination = {up.x + (ep.x - up.x) * k, up.y + (ep.y - up.y) * k};
        const tactical::Aabb& b = mode.bounds();
        o.destination.x = std::clamp(o.destination.x, b.min.x, b.max.x);
        o.destination.y = std::clamp(o.destination.y, b.min.y, b.max.y);
        movement_->move(o, rng, frame_);
        ++stats_.orders;
    }
}

// ---- integration fixes: human order API (rules of BattleAi::execute_order) -----------------------------------------

bool TacticalBattle::commanded_by_player(int32_t player) const {
    return cfg_.skip_human_units && player >= 0 && static_cast<size_t>(player) < galaxy_->players().size() &&
           galaxy_->players()[static_cast<size_t>(player)].human;
}

namespace {
// Live, non-retreated battle units of `player` from `ids` (selection order, first occurrence).
template <class UnitT, class Lookup>
std::vector<const UnitT*> owned_units(const objects::ObjectStore& store, uint8_t player, std::span<const objects::ObjectId> ids,
                                      Lookup&& lookup, bool need_movable) {
    std::vector<const UnitT*> out;
    for (const objects::ObjectId id : ids) {
        const UnitT* u = lookup(id);
        if (!u || !u->alive || u->retreated || store.owner(u->h) != static_cast<int32_t>(player)) continue;
        if (need_movable && !u->movable) continue;
        if (std::find(out.begin(), out.end(), u) == out.end()) out.push_back(u);
    }
    return out;
}
}  // namespace

int32_t TacticalBattle::first_hardpoint_of_type(objects::ObjectId target, int32_t type) const {
    if (!combat_ || type < 0 || !combat_->has_unit(target)) return -1;
    for (uint32_t k = 0; k < combat_->hardpoint_count(target); ++k)
        if (static_cast<int32_t>(combat_->hardpoint_kind(target, k)) == type && combat_->hardpoint_targetable(target, k))
            return static_cast<int32_t>(k);
    return -1;
}

void TacticalBattle::exec_move(TacticalMode& mode, std::span<const objects::ObjectId> ids, float x, float y, SimRng& rng,
                               float facing_deg, bool attack_move, uint8_t flags) {
    s4_cancel(ids);  // S4: a move replaces a pending garrison enter
    const tactical::Aabb& bb = mode.bounds();
    mv::MoveOrder o;
    o.units.assign(ids.begin(), ids.end());
    o.flags = flags;  // P1_DOUBLE_CLICK_MOVE.md §3.2: the event flags, unchanged
    o.destination = {std::clamp(x, bb.min.x, bb.max.x), std::clamp(y, bb.min.y, bb.max.y)};
    if (std::isfinite(facing_deg)) o.facing_deg = facing_deg;
    movement_->move(o, rng, frame_);
    if (!attack_move)
        for (const objects::ObjectId id : o.units) combat_->clear_order(id);
}

// Attack rule of BattleAi::execute_order: order_attack, then a move into 70 % of max(range, 100) beyond 80 %.
uint32_t TacticalBattle::exec_attack(TacticalMode& mode, std::span<const objects::ObjectId> ids, objects::ObjectId target, int32_t hp,
                                     SimRng& rng) {
    const objects::ObjectStore& st = mode.primary();
    const Unit* t = unit_by_id(target);
    if (!t) return 0;
    const tactical::Aabb& bb = mode.bounds();
    const Vec3 tp = st.position(t->h);
    uint32_t n = 0;
    for (const objects::ObjectId id : ids) {
        const Unit* u = unit_by_id(id);
        if (!u || u->id == t->id || !combat_->has_unit(u->id)) continue;
        combat_->order_attack(u->id, t->id, hp);
        ++n;
        if (!u->movable) continue;
        const Vec3 up = st.position(u->h);
        const float dx = tp.x - up.x, dy = tp.y - up.y, d = dmath::sqrt(dx * dx + dy * dy);
        const float range = std::max(u->range, 100.0f);
        if (d > 0.8f * range) {
            const float k = (d - 0.7f * range) / d;
            mv::MoveOrder o;
            o.units = {u->id};
            o.destination = {std::clamp(up.x + dx * k, bb.min.x, bb.max.x), std::clamp(up.y + dy * k, bb.min.y, bb.max.y)};
            movement_->move(o, rng, frame_);
        }
    }
    return n;
}

cmd::RejectReason TacticalBattle::order_move(TacticalMode& mode, uint8_t player, std::span<const objects::ObjectId> units, float x,
                                             float y, SimRng& rng, float facing_deg, bool attack_move, bool queued,
                                             uint8_t move_flags) {
    using R = cmd::RejectReason;
    if (finished_ || !movement_) return R::NotAllowed;
    const auto own = owned_units<Unit>(mode.primary(), player, units, [this](objects::ObjectId id) { return unit_by_id(id); }, true);
    if (own.empty()) return R::InvalidUnit;
    if (!std::isfinite(x) || !std::isfinite(y)) return R::InvalidPosition;
    std::vector<objects::ObjectId> ids;
    for (const Unit* u : own) ids.push_back(u->id);
    if (abilities_)  // OI abilities: order cancel 0x38f5d0 (BARRAGE / LUCKY / ION / HUNT off on a new order)
        for (const objects::ObjectId id : ids) abilities_->order_cancel(id, frame_);
    if (!queued) {
        clear_queues(ids);
        exec_move(mode, ids, x, y, rng, facing_deg, attack_move, move_flags);
        return R::None;
    }
    const tactical::Aabb& bb = mode.bounds();
    QueuedOrder q;
    q.kind = QueuedOrder::Kind::Move;
    q.player = player;
    q.x = std::clamp(x, bb.min.x, bb.max.x);
    q.y = std::clamp(y, bb.min.y, bb.max.y);
    if (std::isfinite(facing_deg)) q.facing = facing_deg;
    q.attack_move = attack_move;
    q.flags = move_flags;
    const std::vector<objects::ObjectId> now = enqueue(ids, q);
    if (!now.empty()) exec_move(mode, now, q.x, q.y, rng, q.facing, attack_move, move_flags);
    return R::None;
}

cmd::RejectReason TacticalBattle::order_facing(TacticalMode& mode, uint8_t player, std::span<const objects::ObjectId> units,
                                               float facing_deg, SimRng& rng) {
    using R = cmd::RejectReason;
    if (finished_ || !movement_) return R::NotAllowed;
    if (!std::isfinite(facing_deg)) return R::InvalidPosition;
    const objects::ObjectStore& st = mode.primary();
    const auto own = owned_units<Unit>(st, player, units, [this](objects::ObjectId id) { return unit_by_id(id); }, true);
    if (own.empty()) return R::InvalidUnit;
    for (const Unit* u : own) {
        if (abilities_) abilities_->order_cancel(u->id, frame_);
        const Vec3 p = st.position(u->h);
        mv::MoveOrder o;
        o.units = {u->id};
        o.destination = {p.x, p.y};
        o.facing_deg = facing_deg;
        movement_->move(o, rng, frame_);
    }
    return R::None;
}

cmd::RejectReason TacticalBattle::order_attack(TacticalMode& mode, uint8_t player, std::span<const objects::ObjectId> units,
                                               objects::ObjectId target, SimRng& rng, int32_t hardpoint, bool queued) {
    using R = cmd::RejectReason;
    if (finished_ || !combat_) return R::NotAllowed;
    const objects::ObjectStore& st = mode.primary();
    const Unit* t = unit_by_id(target);
    if (!t || !t->alive || t->retreated || !combat_->is_enemy(player, st.owner(t->h))) return R::InvalidTarget;
    const int32_t hp = (hardpoint >= 0 && static_cast<uint32_t>(hardpoint) < combat_->hardpoint_count(t->id)) ? hardpoint : -1;
    std::vector<objects::ObjectId> ids;
    for (const Unit* u : owned_units<Unit>(st, player, units, [this](objects::ObjectId id) { return unit_by_id(id); }, false))
        if (u->id != t->id && combat_->has_unit(u->id)) ids.push_back(u->id);
    if (ids.empty()) return R::InvalidUnit;
    if (abilities_)  // OI abilities: order cancel 0x38f5d0
        for (const objects::ObjectId id : ids) abilities_->order_cancel(id, frame_);
    if (!queued) {
        clear_queues(ids);
        exec_attack(mode, ids, t->id, hp, rng);
        return R::None;
    }
    QueuedOrder q;
    q.kind = QueuedOrder::Kind::Attack;
    q.player = player;
    q.target = t->id;
    q.hardpoint = hp;
    const std::vector<objects::ObjectId> now = enqueue(ids, q);
    if (!now.empty()) exec_attack(mode, now, t->id, hp, rng);
    return R::None;
}

cmd::RejectReason TacticalBattle::order_move_to_object(TacticalMode& mode, uint8_t player, std::span<const objects::ObjectId> units,
                                                       objects::ObjectId target, SimRng& rng, bool attack_move, bool queued) {
    using R = cmd::RejectReason;
    if (finished_ || !movement_) return R::NotAllowed;
    const objects::ObjectStore& st = mode.primary();
    const objects::ObjectHandle th = st.find(target);
    if (!st.alive(th)) return R::InvalidTarget;
    std::vector<objects::ObjectId> movers;
    movers.reserve(units.size());
    for (const objects::ObjectId id : units)
        if (id != target) movers.push_back(id);
    const Vec3 p = st.position(th);
    if (!queued) return order_move(mode, player, movers, p.x, p.y, rng, mv::kNoFacing, attack_move, false);
    const auto own = owned_units<Unit>(st, player, movers, [this](objects::ObjectId id) { return unit_by_id(id); }, true);
    if (own.empty()) return R::InvalidUnit;
    std::vector<objects::ObjectId> ids;
    for (const Unit* u : own) ids.push_back(u->id);
    QueuedOrder q;
    q.kind = QueuedOrder::Kind::MoveToObject;
    q.player = player;
    q.target = target;
    q.x = p.x;  // the target's position at issue (marker); the move goes to where it is at start
    q.y = p.y;
    q.attack_move = attack_move;
    const std::vector<objects::ObjectId> now = enqueue(ids, q);
    if (!now.empty()) exec_move(mode, now, p.x, p.y, rng, mv::kNoFacing, attack_move);
    return R::None;
}

cmd::RejectReason TacticalBattle::order_stop(TacticalMode& mode, uint8_t player, std::span<const objects::ObjectId> units) {
    using R = cmd::RejectReason;
    if (finished_ || !movement_) return R::NotAllowed;
    const auto own = owned_units<Unit>(mode.primary(), player, units, [this](objects::ObjectId id) { return unit_by_id(id); }, false);
    if (own.empty()) return R::InvalidUnit;
    std::vector<objects::ObjectId> movers, all;
    for (const Unit* u : own) {
        all.push_back(u->id);
        if (u->movable) movers.push_back(u->id);
        if (combat_->has_unit(u->id)) combat_->clear_order(u->id);
    }
    clear_queues(all);  // [port: Gabriel] Stop clears the queue
    if (!movers.empty()) movement_->stop(movers);
    return R::None;
}

// ---- P host (owner: game host agent): human reinforcements (Reinforce, COMMANDS.md id 12) ------------------------------

float TacticalBattle::population_used(int32_t player) const {
    float used = 0.0f;
    std::vector<uint8_t> counted(parts_.size(), 0);
    for (const Unit& u : units_)
        if (u.alive && !u.retreated && parts_[u.participant].owner == player && !counted[u.participant]) {
            counted[u.participant] = 1;
            used += parts_[u.participant].pop;
        }
    return used;
}

std::vector<TacticalBattle::IconLink> TacticalBattle::icon_links() const {
    std::vector<IconLink> out;
    std::vector<objects::ObjectId> by_part(parts_.size(), objects::kNoObjectId), by_cont(containers_.size(), objects::kNoObjectId);
    for (const Unit& u : units_) {  // ascending id
        if (!u.alive || u.retreated || !u.movable) continue;
        objects::ObjectId* lead = u.container < by_cont.size()     ? &by_cont[u.container]
                                  : u.participant < by_part.size() ? &by_part[u.participant]
                                                                   : nullptr;
        if (!lead) continue;
        if (*lead == objects::kNoObjectId) *lead = u.id;
        out.push_back({u.id, *lead});
    }
    return out;
}

float TacticalBattle::population_cap(int32_t player) const {
    if (!space_ || !galaxy_ || player < 0 || static_cast<size_t>(player) >= galaxy_->players().size()) return -1.0f;
    return rules_->faction(galaxy_->players()[static_cast<size_t>(player)].faction).space_unit_cap;
}

std::vector<TacticalBattle::Reserve> TacticalBattle::reserves(int32_t player) const {
    std::vector<Reserve> out;
    for (const Participant& p : parts_) {
        if (p.owner != player || !p.reserve || p.deployed) continue;
        Reserve r;
        r.galactic_id = p.galactic_id;
        r.type = p.type;
        r.population = p.pop;
        r.units = static_cast<uint32_t>(spawn_types(p).size());
        out.push_back(r);
    }
    return out;
}

cmd::RejectReason TacticalBattle::order_reinforce(TacticalMode& mode, uint8_t player, objects::ObjectId galactic_id, float x, float y,
                                                  SimRng& rng) {
    using R = cmd::RejectReason;
    if (finished_ || !movement_) return R::NotAllowed;
    if (!std::isfinite(x) || !std::isfinite(y)) return R::InvalidPosition;
    for (uint32_t i = 0; i < parts_.size(); ++i) {
        Participant& p = parts_[i];
        if (p.galactic_id != galactic_id || p.owner != static_cast<int32_t>(player) || !p.reserve || p.deployed) continue;
        if (retreat_side_ == static_cast<int32_t>(p.side)) return R::NotAllowed;
        if (space_) {  // the unit cap rule (BattleAi::execute_order Reinforce); land: no tactical cap in the S10 battle
            const uint32_t f = galaxy_->players()[static_cast<size_t>(player)].faction;
            float used = 0.0f;
            std::vector<uint8_t> counted(parts_.size(), 0);
            for (const Unit& u : units_)
                if (u.alive && !u.retreated && parts_[u.participant].owner == static_cast<int32_t>(player) && !counted[u.participant]) {
                    counted[u.participant] = 1;
                    used += parts_[u.participant].pop;
                }
            if (used + p.pop > rules_->faction(f).space_unit_cap) return R::QueueFull;
#ifdef ALAMO_SIM_HAVE_FOG
            // battle host (fog agent ask): space drop validity 0x4d6fd0 - fogged (force 1) / inside a non-allied prevention
            // radius -> refused (TACTICAL_GAPS §4.2).
            if (!fog_reinforce_point_ok(static_cast<int32_t>(player), x, y)) return R::InvalidPosition;
#endif
        }
        // [port] landing position: the battle's row-of-6 slots around the requested point (clamped to the mode bounds).
        const tactical::Aabb& b = mode.bounds();
        const Vec3 save_anchor = anchor_[p.side];
        const uint32_t save_placed = placed_[p.side];
        anchor_[p.side] = {std::clamp(x, b.min.x, b.max.x), std::clamp(y, b.min.y, b.max.y), 0.0f};
        placed_[p.side] = 0;
        deploy(mode, i, rng);
        anchor_[p.side] = save_anchor;
        placed_[p.side] = save_placed;
        ++stats_.reinforcements;
        return R::None;
    }
    return R::InvalidUnit;
}
// ---- end P host ----

// ---- [port: Gabriel] queueable unit actions: order queue ----------------------------------------------------------

std::span<const TacticalBattle::QueuedOrder> TacticalBattle::queued_orders(objects::ObjectId unit) const {
    const auto it = std::lower_bound(queues_.begin(), queues_.end(), unit, [](const OrderQueue& q, objects::ObjectId v) { return q.id < v; });
    if (it == queues_.end() || it->id != unit) return {};
    return it->orders;
}

void TacticalBattle::clear_queues(std::span<const objects::ObjectId> ids) {
    if (queues_.empty()) return;  // AI-only battles: never queued, nothing to do
    for (const objects::ObjectId id : ids) {
        const auto it = std::lower_bound(queues_.begin(), queues_.end(), id, [](const OrderQueue& q, objects::ObjectId v) { return q.id < v; });
        if (it != queues_.end() && it->id == id) queues_.erase(it);
    }
}

// Live state "busy": with a current queue order of a known kind, its completion rule; else any of moving, an
// ordered attack on a live target (whose ordered hard point still stands), a pending ability.
bool TacticalBattle::order_busy(objects::ObjectId id, const OrderQueue* q) const {
    const bool moving = movement_->has_unit(id) && movement_->view(id).has_goal;
    bool attacking = false;
    if (combat_->has_unit(id)) {
        const tc::ObjectId t = combat_->ordered_target(id);
        if (t != tc::kNoObject && combat_->has_unit(t) && combat_->alive(t)) {
            const int32_t hp = combat_->ordered_hardpoint(id);
            attacking = hp < 0 || static_cast<uint32_t>(hp) >= combat_->hardpoint_count(t) ||
                        !combat_->hardpoint_destroyed(t, static_cast<uint32_t>(hp));
        }
    }
    const bool pending = abilities_->has_object(id) && abilities_->pending_type(id) >= 0;
    if (q && q->has_current) {
        switch (q->current) {
            case QueuedOrder::Kind::Move:
            case QueuedOrder::Kind::MoveToObject: return moving;
            case QueuedOrder::Kind::Attack: return attacking;
            case QueuedOrder::Kind::Ability: return pending;
        }
    }
    return moving || attacking || pending;
}

// Appends `o` (a new group serial) to the queues of the busy / already-queued ids; returns the idle ids with
// an empty queue, which the caller runs at once (their queue then tracks that order as the current one only
// when more entries follow later: an empty queue is not kept).
std::vector<objects::ObjectId> TacticalBattle::enqueue(std::span<const objects::ObjectId> ids, const QueuedOrder& o) {
    queue_used_ = true;
    QueuedOrder e = o;
    e.group = ++queue_group_;
    std::vector<objects::ObjectId> now;
    for (const objects::ObjectId id : ids) {
        auto it = std::lower_bound(queues_.begin(), queues_.end(), id, [](const OrderQueue& q, objects::ObjectId v) { return q.id < v; });
        const bool has = it != queues_.end() && it->id == id;
        if (!has && !order_busy(id, nullptr)) {
            now.push_back(id);
            continue;
        }
        if (!has) {
            OrderQueue q;
            q.id = id;
            it = queues_.insert(it, std::move(q));
        }
        it->orders.push_back(e);
    }
    return now;
}

bool TacticalBattle::queue_object_live(const TacticalMode& mode, objects::ObjectId id) const {
    const auto it = std::lower_bound(units_.begin(), units_.end(), id, [](const Unit& u, objects::ObjectId v) { return u.id < v; });
    if (it != units_.end() && it->id == id) return it->alive && !it->retreated;
    const objects::ObjectStore& st = mode.primary();  // ability team containers
    return abilities_->has_object(id) && st.alive(st.find(id));
}

void TacticalBattle::advance_queues(TacticalMode& mode, SimRng& rng) {
    if (queues_.empty()) return;
    const objects::ObjectStore& st = mode.primary();
    std::erase_if(queues_, [&](const OrderQueue& q) { return !queue_object_live(mode, q.id); });
    struct Start {
        QueuedOrder o;
        std::vector<objects::ObjectId> ids;  // ascending
    };
    // An entry that can no longer run (owner changed, target gone / no longer an enemy, hard point destroyed) is
    // dropped at pop time and the next one is tried; the current order stays what it was.
    auto runnable = [&](objects::ObjectId id, const QueuedOrder& o) {
        if (st.owner(st.find(id)) != static_cast<int32_t>(o.player)) return false;
        switch (o.kind) {
            case QueuedOrder::Kind::Move: return true;
            case QueuedOrder::Kind::MoveToObject: return st.alive(st.find(o.target));
            case QueuedOrder::Kind::Attack: {
                const Unit* t = unit_by_id(o.target);
                if (!t || !t->alive || t->retreated || !combat_->is_enemy(o.player, st.owner(t->h))) return false;
                return o.hardpoint < 0 || !combat_->hardpoint_destroyed(t->id, static_cast<uint32_t>(o.hardpoint));
            }
            case QueuedOrder::Kind::Ability: return true;  // handle_ability re-validates
        }
        return false;
    };
    for (int pass = 0; pass < 8 && !queues_.empty(); ++pass) {
        std::vector<Start> starts;  // first-appearance order over ascending ids
        for (OrderQueue& q : queues_) {
            if (order_busy(q.id, &q)) continue;
            while (!q.orders.empty() && !runnable(q.id, q.orders.front())) q.orders.erase(q.orders.begin());
            if (q.orders.empty()) continue;
            QueuedOrder o = std::move(q.orders.front());
            q.orders.erase(q.orders.begin());
            q.has_current = true;
            q.current = o.kind;
            Start* s = nullptr;
            for (Start& c : starts)
                if (c.o.group == o.group && c.o.player == o.player) s = &c;
            if (!s) {
                starts.push_back({std::move(o), {}});
                s = &starts.back();
            }
            s->ids.push_back(q.id);
        }
        std::erase_if(queues_, [](const OrderQueue& q) { return q.orders.empty(); });
        if (starts.empty()) break;
        for (Start& s : starts) {
            const QueuedOrder& o = s.o;
            switch (o.kind) {
                case QueuedOrder::Kind::Move: exec_move(mode, s.ids, o.x, o.y, rng, o.facing, o.attack_move, o.flags); break;
                case QueuedOrder::Kind::MoveToObject: {
                    const objects::ObjectHandle th = st.find(o.target);
                    if (!st.alive(th)) break;  // target gone: dropped, the next entry runs next pass
                    const Vec3 p = st.position(th);
                    exec_move(mode, s.ids, p.x, p.y, rng, mv::kNoFacing, o.attack_move);
                    break;
                }
                case QueuedOrder::Kind::Attack: {
                    const Unit* t = unit_by_id(o.target);
                    if (!t || !t->alive || t->retreated || !combat_->is_enemy(o.player, st.owner(t->h))) break;
                    if (o.hardpoint >= 0 && combat_->hardpoint_destroyed(t->id, static_cast<uint32_t>(o.hardpoint))) break;
                    exec_attack(mode, s.ids, t->id, o.hardpoint, rng);
                    break;
                }
                case QueuedOrder::Kind::Ability: {
                    cmd::EventData ev = o.ability;
                    std::vector<uint32_t> w;
                    for (const objects::ObjectId id : s.ids) w.push_back(static_cast<uint32_t>(id));
                    const cmd::EventType t = ev.type();
                    ev.set_ids((t == cmd::EventType::TacticalSpecialAbility || t == cmd::EventType::TacticalSpecialAbilityWithDummyTarget)
                                   ? "users"
                                   : "units",
                               std::move(w));
                    handle_ability(mode, o.player, ev, rng);
                    break;
                }
            }
        }
    }
}

void TacticalBattle::hash_queues(StateHasher& h) const {
    h.u32(queue_group_);
    h.u32(static_cast<uint32_t>(queues_.size()));
    for (const OrderQueue& q : queues_) {
        h.i32(q.id);
        h.boolean(q.has_current);
        h.u8(static_cast<uint8_t>(q.current));
        h.u32(static_cast<uint32_t>(q.orders.size()));
        for (const QueuedOrder& o : q.orders) {
            h.u8(static_cast<uint8_t>(o.kind));
            h.u8(o.player);
            h.u32(o.group);
            h.i32(o.target);
            h.i32(o.hardpoint);
            h.f32(o.x);
            h.f32(o.y);
            h.f32(o.facing);
            h.boolean(o.attack_move);
            if (o.flags != 0) h.u8(o.flags);  // P1 move flags (absent = 0: older streams unchanged)
            if (o.kind == QueuedOrder::Kind::Ability) {
                std::vector<uint8_t> bytes;
                cmd::encode_payload(o.ability, bytes);
                h.u8(o.ability.type_id());
                h.u32(static_cast<uint32_t>(bytes.size()));
                for (const uint8_t b : bytes) h.u8(b);
            }
        }
    }
}
// ---- end human order API ---------------------------------------------------------------------------------------------

bool TacticalBattle::request_retreat(int32_t player) {
    const int32_t side = player == req_.info.defender ? 0 : player == req_.info.attacker ? 1 : -1;
    if (side < 0 || finished_ || end_hold_until_ != 0 || frame_ < retreat_allowed_frame_ || retreat_side_ >= 0) return false;
    const uint32_t f = static_cast<size_t>(player) < galaxy_->players().size() ? galaxy_->players()[static_cast<size_t>(player)].faction
                                                                                 : galactic::kNoIndex;
    const float secs = space_ ? rules_->faction(f).space_retreat_countdown : rules_->faction(f).land_retreat_countdown;
    retreat_side_ = side;
    retreat_done_frame_ = frame_ + static_cast<uint32_t>(secs * 30.0f);
    for (const Unit& u : units_)
        if (u.side == side && u.alive) combat_->set_flag(u.id, tc::kUnitRetreating, true);
    return true;
}

void TacticalBattle::retreat_policy() {
    if (!(cfg_.retreat_ratio > 0.0f) || retreat_side_ >= 0 || frame_ < retreat_allowed_frame_) return;
    for (uint8_t side = 0; side < 2; ++side)
        if (side_power(side) < cfg_.retreat_ratio * side_power(static_cast<uint8_t>(1 - side))) {
            request_retreat(side_player(side));
            return;
        }
}

void TacticalBattle::unit_gone(TacticalMode& mode, Unit& u) {
    if (u.container == kNoContainer) return;
    Container& c = containers_[u.container];
    if (c.alive > 0) --c.alive;
    if (c.alive == 0 && !c.removed) {
        c.removed = true;
        bt_removals_.push_back({c.owner, c.type, c.type, u.retreated});  // battle host: the container leaves with its last member
        hg_unit_removed(c.id);  // hangars: a launched squadron container is gone
        abilities_->remove(c.id);
        mode.primary().destroy(c.h);
    }
}

void TacticalBattle::complete_retreat(TacticalMode& mode, uint8_t side) {
    // Post-retreat (vf11): the retreating units leave the map and are preserved in the galaxy. Playtest QA D1: every other
    // object of the player dies (BT §3.4): in space that is what cannot jump out, the station and other units without a
    // locomotor. They go through this tick's combat phase D like the attrition victims, so they break up and count as losses.
    // [port] land keeps its old rule (structures stay); units with destroyed engines still count as retreatable.
    if (space_) {
        for (const Unit& u : units_) {
            if (u.side != side || !u.alive || u.retreated || u.movable) continue;
            if (std::find(bt_attrition_ids_.begin(), bt_attrition_ids_.end(), u.id) == bt_attrition_ids_.end()) {
                bt_attrition_ids_.push_back(u.id);
                bt_used_ = true;
            }
        }
    }
    for (Unit& u : units_) {
        if (u.side != side || !u.alive || u.retreated) continue;
        // battle host: retreat attrition victims stay to be killed at this tick's combat phase D (§4.9)
        if (std::find(bt_attrition_ids_.begin(), bt_attrition_ids_.end(), u.id) != bt_attrition_ids_.end()) continue;
        u.retreated = true;
        hg_unit_removed(u.id);     // hangars / engine loops: the object leaves the battle
        abilities_->remove(u.id);  // combat unit removed too
        if (u.movable) movement_->remove_unit(u.id);
#ifdef ALAMO_SIM_HAVE_AI_TACTICAL
        if (u.tracked) damage_tracking_.disable(u.id);
#endif
        unit_gone(mode, u);
        // [port] with an end hold the map stays on screen: the ship is gone (no jump-out flight yet), not left frozen there.
        if (cfg_.end_hold_frames > 0) mode.destroy(u.h);
    }
    retreat_complete_ = true;
}

void TacticalBattle::service_damage_tracking() {
#ifdef ALAMO_SIM_HAVE_AI_TACTICAL
    // Behaviour service step of DAMAGE_TRACKING (object service order is irrelevant: per-object state only).
    for (Unit& u : units_) {
        if (!u.tracked || !u.alive || u.retreated || frame_ < u.dt_next) continue;
        damage_tracking_.service(u.id, catalog_->threat_decay_step());
        u.dt_next = frame_ + kDamageTrackingInterval;
    }
#endif
}

void TacticalBattle::finish(int32_t winner, uint8_t victory, int32_t retreated) {
    finished_ = true;
    result_.winner = winner;
    result_.victory = victory;
    result_.retreated = retreated;
    result_.destroyed.clear();
    result_.starbase_destroyed = false;
    for (const Participant& p : parts_) {
        if (p.built || !p.deployed || p.spawned == 0 || p.alive > 0) continue;  // battle host: pad objects have no galactic object
        if (p.starbase) result_.starbase_destroyed = true;
        else result_.destroyed.push_back(p.galactic);
    }
    bt_battle_end();  // battle host: lead object / super-weapon step 4 (TACTICAL_GAPS §4.8)
    // battle host: every unit / container still on the map (or retreated) leaves the mode alive, ascending id.
    for (const Unit& u : units_)
        if (u.alive) bt_removals_.push_back({parts_[u.participant].owner, u.type, u.container != kNoContainer ? containers_[u.container].type : u.type, true});
    for (const Container& c : containers_)
        if (!c.removed) bt_removals_.push_back({c.owner, c.type, c.type, true});
}

// --- P4 sim (owner: galactic agent): tactical_battle.hpp stop_for_auto_resolve ---
bool TacticalBattle::stop_for_auto_resolve(int32_t player) {
    // Gate (P4_SIM_SYSTEMS §9.3 step 1): the retreat coordinator's "can retreat" check for the player = 0, i.e. the
    // checks of request_retreat: a side, not finished, retreat allowed (countdown), no retreat running.
    const bool side = player == req_.info.defender || player == req_.info.attacker;
    if (!side || finished_ || end_hold_until_ != 0 || frame_ < retreat_allowed_frame_ || retreat_side_ >= 0) return false;
    finish(kNoPlayer, galactic::kVictoryAllDestroyed, kNoPlayer);
    return true;
}
// --- end P4 sim ---

// --- P4 tactical: tactical_battle.hpp black_market_blocked_abilities ---
std::vector<uint32_t> black_market_blocked_abilities(const ta::AbilityData& data, const galactic::GalacticSim& galaxy,
                                                     int32_t player) {
    // The galactic item lists hold special_name_crc (the name as written); the tactical side matches name_crc.
    std::vector<uint32_t> out;
    for (const ta::AbilityObjectDesc& o : data.objects)
        for (const ta::UnitAbilityRecord& r : o.unit_abilities)
            if (r.must_be_bought_on_black_market && !r.gui_activated_ability_name.empty() &&
                !galaxy.bm_allows_unit_ability(player, galactic::special_name_crc(r.gui_activated_ability_name), true))
                out.push_back(r.gui_name_crc);
    for (const ta::SpecialAbilityDesc& s : data.specials)
        if (s.must_be_bought_on_black_market && !s.name.empty() &&
            !galaxy.bm_allows_special_ability(player, galactic::special_name_crc(s.name), true))
            out.push_back(s.name_crc);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}
// --- end P4 tactical ---

void TacticalBattle::conclude(int32_t winner, uint8_t victory, int32_t retreated) {
    if (cfg_.end_hold_frames == 0) {
        finish(winner, victory, retreated);
        return;
    }
    end_hold_until_ = frame_ + cfg_.end_hold_frames;
    end_winner_ = winner;
    end_victory_ = victory;
    end_retreated_ = retreated;
}

std::vector<TacticalBattle::RemovalRecord> TacticalBattle::unit_losses() const {
    std::vector<RemovalRecord> out;
    for (const RemovalRecord& r : bt_removals_) {
        if (r.survived) continue;
        const bool team = std::any_of(containers_.begin(), containers_.end(), [&r](const Container& c) { return c.type == r.object_type; });
        if (!team) out.push_back(r);
    }
    return out;
}

void TacticalBattle::check_victory() {
    if (finished_) return;
    if (end_hold_until_ != 0) {  // Playtest QA D1: decided; the world keeps running until the hold is over
        if (frame_ >= end_hold_until_) finish(end_winner_, end_victory_, end_retreated_);
        return;
    }
    // L1 systems: the TSW victory monitor hold (TSW_PLANET_DESTROY §2): nothing is declared from the TSW's fire until
    // its state-13 service; this check is the replay on the release frame, the TSW's story victory (0x10) first.
    if (l1_sw_.victory_holds() > 0) return;
    if (l1_tsw_story_winner_ >= 0) {
        const int32_t w = l1_tsw_story_winner_;
        l1_tsw_story_winner_ = -1;
        conclude(w, galactic::kVictoryStory, w == req_.info.attacker ? req_.info.defender : req_.info.attacker);
        return;
    }
    if (retreat_complete_) {
        conclude(side_player(static_cast<uint8_t>(1 - retreat_side_)), galactic::kVictoryAllRetreated, side_player(static_cast<uint8_t>(retreat_side_)));
        return;
    }
    bool has[2] = {false, false};
    bool station_lost = false;
    // BT §3.1a item 1 (0x372230): an object keeps its side alive for 0xb only when its own type (not its container's) is
    // victory-relevant in this mode: Victory_Relevant, else Space_ / Land_Victory_Relevant (all default false). Data only:
    // no behaviour / category test, so the Death Stars (Victory_Relevant No / unset) never count (TSW_PLANET_DESTROY §8;
    // probe 2026-10-31 (l1sys t06): the defender won 0xb with the attacker's Death Star alive). [port] polled every tick,
    // not the retail event trigger (a relevant object leaving a player), so a side with no relevant object at all loses at
    // once instead of never by 0xb.
    for (const Unit& u : units_)
        if (u.alive && !u.retreated && !parts_[u.participant].third && victory_relevant(u.type))  // §3.1: third-party map objects
            has[u.side] = true;
    // battle host: BT §2.5 land victory 0xe (ALL_ENEMY_BASE_COMPONENTS_DESTROYED, Land_Base_Destruction_Forces_Retreat) for
    // the attacker once every defender ground structure died. [port] 0xd (rogue-faction base components) not modelled.
    uint32_t base_parts = 0, base_alive = 0;
    for (const Participant& p : parts_) {
        // [port] a never-deployed participant (reserve / pool) still counts when one of its spawn types is victory-relevant;
        // BT §3.1a says pool units are not GOM objects and never count [M], not adopted until probed.
        if (!p.deployed && victory_relevant(p)) has[p.side] = true;
        if (p.starbase && p.deployed && p.alive == 0) station_lost = true;
        if (p.structure && p.side == 0) {
            ++base_parts;
            if (!p.deployed || p.alive > 0) ++base_alive;
        }
    }
    if (space_ && station_lost && req_.space_station_destruction_forces_retreat) {
        conclude(req_.info.attacker, galactic::kVictoryStarbaseDestroyed, kNoPlayer);
    } else if (!space_ && req_.land_base_destruction_forces_retreat && base_parts > 0 && base_alive == 0) {
        conclude(req_.info.attacker, galactic::kVictoryEnemyBaseDestroyed, kNoPlayer);
    } else if (!has[1]) {
        conclude(req_.info.defender, galactic::kVictoryAllDestroyed, kNoPlayer);  // also both empty: the defender holds
    } else if (!has[0]) {
        conclude(req_.info.attacker, galactic::kVictoryAllDestroyed, kNoPlayer);
    } else if (cfg_.max_frames > 0 && frame_ >= cfg_.max_frames) {
        finish(req_.info.defender, galactic::kVictoryAllRetreated, req_.info.attacker);  // [port] time limit
    }
}

bool TacticalBattle::victory_relevant(uint32_t type) {
    const objects::GameObjectTypes& types = catalog_->types();
    if (type >= types.size()) return false;
    if (vr_cache_.size() != types.size()) vr_cache_.assign(types.size(), 0);
    uint8_t& c = vr_cache_[type];
    if (c == 0) {
        const auto t = static_cast<objects::TypeIndex>(type);
        const bool r = types.get_bool(t, "Victory_Relevant") || types.get_bool(t, space_ ? "Space_Victory_Relevant" : "Land_Victory_Relevant");
        c = r ? 2 : 1;
    }
    return c == 2;
}

bool TacticalBattle::victory_relevant(const Participant& p) {
    if (p.other_type >= 0) return victory_relevant(static_cast<uint32_t>(p.other_type));  // §3.2 step 3
    const galactic::ConflictTypeInfo& ti = rules_->type(p.type);
    if (ti.members.empty()) return victory_relevant(p.type);
    for (const auto& [mt, n] : ti.members)
        if (n > 0 && victory_relevant(mt)) return true;
    return false;
}

void TacticalBattle::apply_speed_factors() {
    // MOVEMENT 3.4 items 4, 5, 7 (product order: Mod_Multiplier, engines, list 5) [port: one factor].
    const float engines = catalog_->engines_disabled_speed_modifier();
    for (const Unit& u : units_) {
        if (!u.alive || u.retreated || !u.movable) continue;
        float f = combat_->mod_multiplier(u.id, tc::ModKind::Speed);
        if (u.engines_offline || l1_engines_off(u.id)) f = f * engines;  // L1 systems: + timed engine disables
        f = f * (1.0f + combat_->modifier(u.id, tc::ModList::MaxSpeed));
        if (abilities_->immobilize_count(u.id) > 0) f = 0.0f;
        if (combat_->stunned(u.id, frame_)) f = 0.0f;  // OI abilities: land stun suspends the locomotor (0x647290)
        movement_->set_speed_factor(u.id, f);
        movement_->set_speed_override(u.id, abilities_->speed_override(u.id));  // OI host: Berserker_Speed (-1 = none)
    }
}

// REMAINING_GAPS §4.4 / §4.7: flying units strafe and fighters run at their combat target (ordered, else auto).
void TacticalBattle::feed_attack_targets(const objects::ObjectStore& store) {
    for (const Unit& u : units_) {
        if (!u.alive || u.retreated || !u.movable || !movement_->has_unit(u.id)) continue;
        const mv::LocomotorKind k = movement_->view(u.id).locomotor;
        if (k == mv::LocomotorKind::Fighter && space_) {
            feed_fighter_target(u, store);  // FIGHTER_FLYING §4.3: the formation attack target (no point target)
            continue;
        }
        if (k != mv::LocomotorKind::Flying && k != mv::LocomotorKind::Fighter) continue;
        const Unit* t = unit_by_id(combat_->current_target(u.id));
        if (t && t->alive && store.alive(t->h)) {
            const Vec3 p = store.position(t->h);
            movement_->set_attack_target(u.id, true, {p.x, p.y}, p.z);
        } else {
            movement_->set_attack_target(u.id, false);
        }
    }
}

// ---- S7 fighter host (FIGHTER_FLYING.md §2-§5) -------------------------------------------------------------------------

namespace {
// [port] squadron slot offsets (team list +0x78; its writer is untraced): the leader at 0, the others line abreast in
// world y at +-40, +-80, ... in team index order.
mv::F3 fighter_slot(size_t n) {
    if (n == 0) return {};
    const float k = static_cast<float>((n + 1) / 2) * 40.0f;
    return {0.0f, (n % 2 == 1) ? k : -k, 0.0f};
}
}  // namespace

// Serial, before the movement tick: every fighter gets its XML values once (set_fighter_params), and every live TEAM
// container with fighter members is (re)registered as a squadron whenever its live member list (ascending id = team
// index order) differs from the MovementSystem's - so map-start companies, reinforcements, hangar launches and
// replenished wingmen all join before their first fighter frame. Removed containers drop their squadron.
void TacticalBattle::fighter_sync() {
    if (!space_ || !movement_) return;
    const objects::GameObjectTypes& types = catalog_->types();
    std::vector<std::vector<objects::ObjectId>> want(containers_.size());
    for (Unit& u : units_) {
        if (!u.alive || u.retreated || !u.movable || !movement_->has_unit(u.id)) continue;
        if (movement_->view(u.id).locomotor != mv::LocomotorKind::Fighter) continue;
        if (!u.fighter_seen) {
            u.fighter_seen = true;
            movement_->set_fighter_params(u.id, mv::fighter_params_from_type(types, u.type));
        }
        if (u.container < containers_.size()) want[u.container].push_back(u.id);
    }
    for (size_t c = 0; c < containers_.size(); ++c) {
        const Container& ct = containers_[c];
        const mv::FighterSquad* s = movement_->fighter_squad(ct.id);
        if (ct.removed || !ct.team || want[c].empty()) {
            if (s) movement_->remove_fighter_squad(ct.id);
            continue;
        }
        if (s && s->members == want[c]) continue;
        mv::FighterSquadDesc d;
        d.id = ct.id;
        d.members = want[c];
        for (size_t n = 0; n < d.members.size(); ++n) d.slots.push_back(fighter_slot(n));
        d.tolerance = ct.squad_type < types.size() ? mv::squadron_tolerance_from_type(types, ct.squad_type) : 25.0f;
        movement_->set_fighter_squad(d);
    }
}

// FIGHTER_FLYING §4.3 step 2: the formation attack target (0x41a2e0) from the combat target (ordered, else auto). A
// squadron's target comes from its leader; a target that is a member of a registered squadron is that squadron (a
// team: dogfight §4.5); otherwise the object with its combat radius (0x397780) and, for an ordered hard point, the
// hard point's position (0x396ce0: the combat attachment point, unit-local x forward / z up). hidden = FOW_Object_Hidden
// for the attacker's owner with force 1 (0x35f470). No combat target clears it (the locomotor then retargets from its
// dogfight cell and reports Retarget).
void TacticalBattle::feed_fighter_target(const Unit& u, const objects::ObjectStore& store) {
    mv::ObjectId addr = u.id;
    if (u.container < containers_.size()) {
        const objects::ObjectId cid = containers_[u.container].id;
        if (const mv::FighterSquad* s = movement_->fighter_squad(cid)) {
            if (s->members.empty() || s->members.front() != u.id) return;
            addr = cid;
        }
    }
    mv::FighterTarget ft;
    const Unit* t = unit_by_id(combat_->current_target(u.id));
    if (t && t->alive && !t->retreated && store.alive(t->h) && combat_->has_unit(t->id)) {
        const mv::FighterSquad* ts = t->container < containers_.size() ? movement_->fighter_squad(containers_[t->container].id) : nullptr;
        if (ts) {
            ft.id = ts->id;
        } else {
            ft.id = t->id;
            const tactical::Vec3 p = combat_->position(t->id);
            ft.pos = {p.x, p.y, p.z};
            const tc::CombatData& cd = combat_->data();
            const uint32_t di = combat_->desc_index(t->id);
            if (di < cd.units.size()) {
                const tc::UnitDesc& ud = cd.units[di];
                ft.radius = ud.radius;
                const int32_t hp = combat_->ordered_target(u.id) == t->id ? combat_->ordered_hardpoint(u.id) : -1;
                if (hp >= 0 && static_cast<size_t>(hp) < ud.hardpoints.size() && ud.hardpoints[static_cast<size_t>(hp)] < cd.hardpoints.size() &&
                    combat_->hardpoint_targetable(t->id, static_cast<uint32_t>(hp))) {
                    const tactical::Vec3 l = cd.hardpoints[ud.hardpoints[static_cast<size_t>(hp)]].attach_offset;
                    const tactical::Vec3 f = combat_->facing(t->id);
                    float fx = f.x, fy = f.y;
                    const float len2 = fx * fx + fy * fy;
                    if (len2 > 0.0f) {
                        const float inv = 1.0f / dmath::sqrt(len2);
                        fx = fx * inv;
                        fy = fy * inv;
                    } else {
                        fx = 1.0f;
                        fy = 0.0f;
                    }
                    ft.hard_point = hp;
                    ft.pos = {p.x + (l.x * fx - l.y * fy), p.y + (l.x * fy + l.y * fx), p.z + l.z};
                }
            }
        }
        ft.hidden = fog_object_hidden(store.owner(u.h), ft.id, true);
    }
    movement_->set_fighter_target(addr, ft);
}

objects::ObjectId TacticalBattle::fighter_combat_unit(objects::ObjectId id) const {
    for (const mv::FighterSquad& s : movement_->fighter_squads())
        if (s.id == id) return s.members.empty() ? objects::kNoObjectId : s.members.front();
    return id;
}

// The locomotor's orders (0x38d730) as combat attack orders, in event order: Provoke (§4.5 step 2.2) - squadron `who`
// attacks `target`, unless `who` has unit ability 0x29 (ION_CANNON_SHOT) or 0x22 (LUCKY_SHOT) active (0x39b950; the
// spec names them by number only); Retarget (§4.3) - container `who` attacks a dogfight-cell entry. Every live member
// of `who` (or `who` itself when it is a unit) is ordered at the target object (a squadron: its leader).
void TacticalBattle::forward_fighter_events() {
    for (const mv::FighterEvent& e : movement_->fighter_events()) {
        if (e.kind == mv::FighterEvent::Provoke && abilities_ && (abilities_->is_active(e.who, 0x29) || abilities_->is_active(e.who, 0x22)))
            continue;
        const objects::ObjectId tgt = fighter_combat_unit(e.target);
        if (tgt == objects::kNoObjectId || !combat_->has_unit(tgt) || !combat_->alive(tgt)) continue;
        auto order = [&](objects::ObjectId m) {
            if (m == tgt || !combat_->has_unit(m) || !combat_->alive(m)) return;
            if (!combat_->is_enemy(combat_->owner(m), combat_->owner(tgt))) return;
            combat_->order_attack(m, tgt);
        };
        bool squad = false;
        for (const mv::FighterSquad& s : movement_->fighter_squads())
            if (s.id == e.who) {
                squad = true;
                for (const mv::ObjectId m : s.members) order(m);
            }
        if (!squad) order(e.who);
    }
}

void TacticalBattle::drive_animation(SimRng& rng, uint64_t gframe) {
#ifdef ALAMO_SIM_HAVE_ANIM
    // R3 anim: WalkLocomotor (0x61f840, A.7 walk / move / turn rows) and the idle behaviour (0x62e570) for battle
    // units, through SimAnimation::service_locomotor (rules in anim_behaviors.hpp); Berserker state first.
    anim::AnimWorld* w = hooks_ ? hooks_->world() : nullptr;
    const GameMode* gm = hooks_ ? hooks_->anim_mode() : nullptr;
    if (!anim_ || !w || !gm) return;
    for (const Unit& u : units_) {
        if (!u.alive || u.retreated || !w->contains(u.id)) continue;
        anim_->set_berserker(*gm, u.id, abilities_->is_active(u.id, ta::kBerserker));
        anim::LocomotorInput in;
        in.movable = u.movable;
        if (u.movable) {
            const mv::UnitView v = movement_->view(u.id);
            if (v.locomotor != mv::LocomotorKind::Default) {  // REMAINING_GAPS §4.3-4.7: the locomotor picks the clip
                if (v.anim != mv::kAnimNone && w->current_type(u.id) != v.anim) {
                    anim::PlayRequest req;
                    req.type = v.anim;  // variant 0, no draw; one-shot [port: the locomotor re-requests looping states]
                    if (w->play(u.id, req, nullptr, gframe) == anim::PlayResult::Played) ++stats_.anim_plays;
                }
                continue;
            }
            in.speed = v.speed;
            in.max_speed = v.max_speed;
            in.yaw_deg = v.yaw;
        }
        stats_.anim_plays += anim_->service_locomotor(*gm, u.id, in, cfg_.walk_speed_ratio, cfg_.min_move_anim_ratio, rng, gframe);
    }
#else
    (void)rng, (void)gframe;
#endif
}

void TacticalBattle::spawn_particle(TacticalMode& mode, int32_t type, Vec3 pos, int32_t owner) {
    // R4 B.1 layer 1: a PARTICLE GameObject (consumes an id), removed at frame + Particle_Lifetime_Frames.
    if (!cfg_.particles || type < 0 || static_cast<size_t>(type) >= catalog_->types().size()) return;
    objects::SpawnParams sp;
    sp.type = static_cast<objects::TypeIndex>(type);
    sp.owner = owner;
    sp.position = pos;
    sp.facing = {1.0f, 0.0f, 0.0f};
    const objects::ObjectHandle h = mode.primary().create(sp);  // not in the spatial trees (never a target)
    int32_t life = catalog_->types().get_int(sp.type, "Particle_Lifetime_Frames");
    if (life <= 0) life = static_cast<int32_t>(cfg_.particle_default_lifetime);
    particles_.push_back({mode.primary().id(h), h, frame_ + static_cast<uint32_t>(life)});
    ++stats_.particles;
}

objects::ObjectHandle TacticalBattle::spawn_death_clone(TacticalMode& mode, int32_t type, Vec3 pos, Vec3 facing, int32_t owner) {
    if (type < 0 || static_cast<size_t>(type) >= catalog_->types().size()) return {};
    objects::SpawnParams sp;
    sp.type = static_cast<objects::TypeIndex>(type);
    sp.owner = owner;
    sp.position = pos;
    sp.facing = facing;
    return mode.spawn_death_clone(sp);
}

bool TacticalBattle::death_clone_corpse_path(uint32_t dying_type, int32_t clone_type, int32_t key) const {
    // S9_DEATH_CLONE_SHIELDS.md §1.6 [L]: the clone is made by the mode's other object manager (mode +0x20, the
    // secondary / local store of OBJECT_RUNTIME §1 [M]) when the dying type is in category Infantry, the clone type has
    // none of HIDE_WHEN_FOGGED (0x19), IMPOSING_PRESENCE (0x40), LAND_OBSTACLE (0x14), PROXIMITY_MINE (0x55) and the
    // key is not 4 (Force lightning). [port] The 0x5051d0 test is not read (taken as passing). A secondary-store object
    // takes no sim id, so the port makes no object at all: the clone lives only as the dying unit's corpse row
    // (SimAnimation::corpse_clone). Vanilla: the typed infantry clones (Infantry_*_Death_Clone_*: melt, disruptor) take
    // it; the own-type infantry clones keep the primary path (their types have HIDE_WHEN_FOGGED).
    const objects::GameObjectTypes& types = catalog_->types();
    if (clone_type < 0 || static_cast<size_t>(clone_type) >= types.size() || dying_type >= types.size() || key == 4) return false;
    if (!infantry_category_) {
        const auto m = types.enums().get(objects::EnumId::GameObjectCategory).lookup("Infantry");
        infantry_category_ = m ? static_cast<uint64_t>(*m) : ~uint64_t{0};
    }
    if (*infantry_category_ == ~uint64_t{0} || (types.get_mask(dying_type, "CategoryMask") & *infantry_category_) == 0) return false;
    const auto ct = static_cast<objects::TypeIndex>(clone_type);
    for (const objects::BehaviorTypeId b : {objects::BehaviorTypeId{0x19}, objects::BehaviorTypeId{0x40}, objects::BehaviorTypeId{0x14},
                                            objects::BehaviorTypeId{0x55}})
        if (types.has_behavior(ct, objects::BehaviorSet::Union, b)) return false;
    return true;
}

objects::ObjectHandle TacticalBattle::spawn_prop(TacticalMode& mode, int32_t type, Vec3 pos, Vec3 facing, int32_t owner) {
    if (type < 0 || static_cast<size_t>(type) >= catalog_->types().size()) return {};
    objects::SpawnParams sp;
    sp.type = static_cast<objects::TypeIndex>(type);
    sp.owner = owner;
    sp.position = pos;
    sp.facing = facing;
    return mode.spawn(sp, 1.0f);
}

void TacticalBattle::consume_events(TacticalMode& mode, SimRng& rng) {
    objects::ObjectStore& store = mode.primary();
    // a. damage events -> DAMAGE_TRACKING (call order, before the deaths disable the victims).
#ifdef ALAMO_SIM_HAVE_AI_TACTICAL
    // A1_READINGS §7 [H; M flag names]: amount = the actual hull + shield loss (shield absorbed + hull damage; a hit
    // routed to a hard point changes neither), recorded only when > 0 and only for DAMAGE_TRACKING victims. A per-attacker
    // entry needs the source type's Is_Valid_Target (+0x6b) and Collidable_By_Projectile_Living (+0x41); its key is the
    // source's TEAM parent when it has one. [port] The killing hit's overkill is not clamped (the victim leaves the store
    // this tick); the running total / last-damage frame (+0x8 / +0x50) are not kept (nothing reads them yet).
    {
        const objects::GameObjectTypes& dtypes = catalog_->types();
        for (const tc::DamageEvent& e : combat_->damage_events()) {
            if (e.source == tc::kNoObject || !damage_tracking_.tracks(static_cast<int32_t>(e.victim))) continue;
            const float amount = e.absorbed + (e.hardpoint < 0 ? e.to_hull : 0.0f);
            if (!(amount > 0.0f)) continue;
            const objects::ObjectHandle sh = store.find(static_cast<objects::ObjectId>(e.source));
            if (!store.alive(sh)) continue;
            const objects::TypeIndex st = store.type(sh);
            if (st >= dtypes.size() || !dtypes.get_bool(st, "Is_Valid_Target") || !dtypes.get_bool(st, "Collidable_By_Projectile_Living"))
                continue;
            auto key = static_cast<int32_t>(e.source);
            if (const Unit* su = unit_by_id(static_cast<objects::ObjectId>(e.source)); su && su->container != 0xFFFFFFFFu &&
                                                                                       containers_[su->container].team)
                key = static_cast<int32_t>(containers_[su->container].id);
            damage_tracking_.record(static_cast<int32_t>(e.victim), key, amount);
        }
    }
#endif
    // b. projectile detonations -> PARTICLE objects: detonation particle, then the synced hit pick.
    for (const tc::HitEvent& e : combat_->hit_events()) {
        if (e.projectile_desc >= tables_->projectiles.size()) continue;
        const BattleTables::ProjectileVisuals& pv = tables_->projectiles[e.projectile_desc];
        const Vec3 at = sv(e.position);
        if (e.victim == tc::kNoObject) {
            spawn_particle(mode, pv.lifetime, at, e.owner);
            continue;
        }
        const int32_t det = e.absorbed ? pv.absorbed : (e.armor_reduced && pv.armor_reduced >= 0 ? pv.armor_reduced : pv.detonation);
        spawn_particle(mode, det, at, e.owner);
#ifdef ALAMO_SIM_HAVE_ANIM
        hooks_->anim_hit(e);  // R3 anim: flinch
#endif
        const objects::ObjectHandle vh = store.find(e.victim);
        if (e.pick < 0 || !store.exists(vh)) continue;
        if (const BattleTables::UnitVisuals* vis = visuals_of_type(static_cast<uint32_t>(store.type(vh)))) {
            const std::vector<int32_t>& list = e.absorbed ? vis->shield_hit : vis->hit;
            if (static_cast<size_t>(e.pick) < list.size()) spawn_particle(mode, list[static_cast<size_t>(e.pick)], at, store.owner(vh));
        }
    }
    // c. hardpoint destructions: Death_Explosion_Particles, then Death_Breakoff_Prop (each an id).
    for (const tc::HardpointEvent& e : combat_->hardpoint_events()) {
        ++stats_.hardpoints_destroyed;
        const objects::ObjectHandle oh = store.find(e.owner);
        if (!store.exists(oh)) continue;
        Vec3 at = store.position(oh);
        // battle host: at the hard point's attachment bone (bind pose, BattleCatalog model data; unit centre without it):
        // unit-local (x forward, y left, z up) rotated by the unit's yaw.
        if (combat_->has_unit(e.owner)) {
            const tc::UnitDesc& ud = combat_->data().units[combat_->desc_index(e.owner)];
            if (e.index < ud.hardpoints.size() && ud.hardpoints[e.index] < combat_->data().hardpoints.size()) {
                const auto off = combat_->data().hardpoints[ud.hardpoints[e.index]].attach_offset;
                const Vec3 f = store.facing(oh);
                const float len = dmath::sqrt(f.x * f.x + f.y * f.y);
                const float fx = len > 0.0f ? f.x / len : 1.0f, fy = len > 0.0f ? f.y / len : 0.0f;
                at = Vec3{at.x + fx * off.x - fy * off.y, at.y + fy * off.x + fx * off.y, at.z + off.z};
            }
        }
        spawn_particle(mode, e.explosion_type, at, store.owner(oh));
        if (e.breakoff_type >= 0 && spawn_prop(mode, e.breakoff_type, at, store.facing(oh), store.owner(oh))) ++stats_.props;
    }
    // d. deaths: destroy, death clone object, first Death_Explosions entry [port: pick rule untraced].
    for (const tc::DeathEvent& d : combat_->deaths()) {
        Unit* u = unit_by_id(d.id);
        if (!u || !u->alive) continue;  // not a battle unit (dummy targets are invulnerable)
        u->alive = false;
        --parts_[u->participant].alive;
        hg_unit_removed(u->id);  // hangars: carrier / single launched ship gone; engine loops forgotten
        // battle host: object removal record (death)
        bt_removals_.push_back({store.owner(u->h), u->type, u->container != kNoContainer ? containers_[u->container].type : u->type, false});
        const Vec3 at = store.position(u->h);
        const Vec3 face = store.facing(u->h);
        const int32_t owner = store.owner(u->h);
        mode.destroy(u->h);
        if (u->movable) movement_->remove_unit(u->id);
#ifdef ALAMO_SIM_HAVE_AI_TACTICAL
        if (u->tracked) damage_tracking_.disable(u->id);
#endif
        ++stats_.deaths;
        // Die step 8 clone (0x29f270, S9_DEATH_CLONE_SHIELDS.md §1.1 / §1.6 [H]): every tactical death with kind != 15
        // makes one - the picked Death_Clone type, else the dying type itself (S9 t04) - at the dying object's transform,
        // owned by its player, with +0x3a0 |= 0xc0 (refix t01: TacticalMode::spawn_death_clone; the store's active()
        // searches skip it), carrying the death sequence (anim: the clone's own, by the killing kind). refix t03: the
        // infantry corpse path takes no object (death_clone_corpse_path).
        objects::ObjectHandle ch{};
        bool corpse_path = false;
        if (d.death_clone >= 0) {
            corpse_path = death_clone_corpse_path(u->type, d.death_clone, d.clone_key);
            if (!corpse_path) ch = spawn_death_clone(mode, d.death_clone, at, face, owner);
            if (ch) ++stats_.death_clones;
        }
        // refixf t04: the clone's DeathBehavior (spin-away, death clip, Remove_Upon_Death, persistence, fade) is planned
        // here from data and the battle removes the clone object at the planned frame - identical with and without the
        // animation service, which only plays the planned clip (schedule_death_clone).
        const bool carried_by_clone = static_cast<bool>(ch);
        if (ch) schedule_death_clone(mode, ch, d.death_clone, d.kind, rng);
#ifdef ALAMO_SIM_HAVE_ANIM
        {  // R3 anim: death clip / corpse of the unit (unless a clone carries it), or the infantry corpse path
            bool carried = carried_by_clone;
            if (!ch && corpse_path) carried = hooks_->anim_corpse_clone(u->id, d.death_clone, d.kind);
            hooks_->anim_death(u->id, carried, d.kind);
        }
#else
        (void)carried_by_clone;
#endif
        if (const BattleTables::UnitVisuals* vis = visuals_of_type(u->type))
            if (!vis->death_explosions.empty()) spawn_particle(mode, vis->death_explosions.front(), at, owner);
        unit_gone(mode, *u);
    }
    // e. damaged smoke (render attachment).
    for (const tc::SmokeEvent& e : combat_->smoke_events()) {
        const objects::ObjectHandle h = store.find(e.id);
        if (!store.exists(h)) continue;
        const BattleTables::UnitVisuals* vis = visuals_of_type(static_cast<uint32_t>(store.type(h)));
        effects_.push_back({e.id, vis ? vis->smoke : std::string(), e.percent, frame_});
        ++stats_.smoke;
    }
    // f. ability events.
    engines_restarted_.clear();
    for (const ta::AbilityEvent& e : abilities_->events()) {
        ++stats_.ability_events;
        if (e.kind == ta::AbilityEventKind::Activated) ++stats_.activations;
        if (e.kind == ta::AbilityEventKind::EnginesOffline)
            if (Unit* u = unit_by_id(e.id)) {
                engine_loops_.engines_offline(e.id, frame_);  // §3.3: both engine loops off
                if (e.value == 1) {  // destroyed: Locomotor_Set_Engines(destroyed = 1), permanent
                    u->engines_offline = true;
                    l1_engines_destroyed(e.id);
                } else {  // disabled by the station-upgrade copy: EngineControl::disable(id, -1) (HARDPOINT_DISABLED §9.1)
                    l1_engines_.disable(e.id, -1.0f, frame_);
                    l1_used_ = true;
                }
            }
        if (e.kind == ta::AbilityEventKind::EnginesOnline)
            if (Unit* u = unit_by_id(e.id)) {  // an ENGINE hardpoint repaired to max (0x393980 -> 0x554640)
                if (l1_engines_.repair_online(e.id, frame_)) l1_used_ = true;
                u->engines_offline = false;
                engines_restarted_.push_back(e.id);
                engine_loops_.engines_online(e.id, frame_);  // §3.3: idle loop on
            }
        if (e.kind == ta::AbilityEventKind::SpawnObject) spawn_ability_object(mode, e);  // fog follow-up: FOW_REVEAL_PING
    }
    oi_events(mode);  // OI abilities: ability moves / stops / owner changes, ability spawns, death survivors
    abilities_->clear_events();
}

// --- fog follow-up: S9 SpawnObject (FOW_REVEAL_PING, FOG_OF_WAR §3.3): the record's Spawned_Object_Type at the
// target's position (the user's when the target is gone) for the user's owner. The shipped ping projectiles have
// Max_Speed 0 / no flight distance and detonate at once [M]: the object lives one tick (PARTICLE-style expiry) and
// reveals through its REVEAL behaviour meanwhile (fog_ping). It consumes an object id.
void TacticalBattle::spawn_ability_object(TacticalMode& mode, const ta::AbilityEvent& e) {
    const ta::UnitAbilityRecord* r = abilities_->record(e.id, e.type);
    if (!r || r->spawned_object_type.empty()) return;
    const objects::TypeIndex t = catalog_->types().raw().find(r->spawned_object_type);
    if (t == objects::kNone || t >= catalog_->types().size()) return;
    objects::ObjectStore& store = mode.primary();
    const objects::ObjectHandle user = store.find(e.id);
    objects::ObjectHandle at = store.find(e.target);
    if (!store.alive(at)) at = user;
    if (!store.alive(at)) return;
    const objects::ObjectHandle h =
        spawn_prop(mode, static_cast<int32_t>(t), store.position(at), store.facing(at), store.exists(user) ? store.owner(user) : -1);
    if (!h) return;
    particles_.push_back({store.id(h), h, frame_ + 1});
    fog_ping(store.id(h));
}
// --- end fog follow-up ---

// --- OI tactical: crush (MOVEMENT.md 5.5) - 1,000,000 damage of the crusher type's Squash_Damage_Type ---
void TacticalBattle::oi_apply_crush(uint32_t frame) {
    for (const mv::CrushEvent& e : oi_crush_) {
        const Unit* cr = unit_by_id(e.crusher);
        if (!cr || !combat_->alive(e.victim)) continue;
        tc::DamageRequest r;
        r.victim = e.victim;
        r.amount = mv::kCrushDamage;
        r.source = e.crusher;
        std::string_view name = catalog_->types().get_string(cr->type, "Squash_Damage_Type");
        while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) name.remove_prefix(1);
        while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.remove_suffix(1);
        r.damage_type = name.empty() ? tc::kNoTypeId : tables_->combat.damage.damage_index(name);
        combat_->take_damage(r, frame);
    }
    oi_crush_.clear();
}
// --- end OI tactical ---

// refixf t04: DeathBehavior of a death clone object (ANIM_PARTICLES §A.7 death / corpse rows, TACTICAL_GAPS §1.9 [H]):
// spin-away decision, death clip (Specific_Death_Anim_Type / _Index, by damage kind), removal at once (no clip, no spin,
// Remove_Upon_Death), else the corpse countdown (death clip length, Death_Persistence_Duration < 0 = forever, then
// Death_Fade_Time) - anim::plan_death, with the clip data of the clone's model read from the files (the clip length is
// sim-visible, §A.8). The draws (spin float, random variant) happen here, at the spawn, in the sim's draw order, with or
// without the animation service; the service plays the planned clip and draws nothing more for the clone. [port] The
// original runs this in the clone's DEATH behaviour (0x58cdb0 / 0x58d430) and removes the object there; the port keeps
// the removal frame with the battle so a headless run removes the same objects on the same frame as alamo_game.
void TacticalBattle::schedule_death_clone(TacticalMode& mode, objects::ObjectHandle clone, int32_t clone_type, uint8_t kind, SimRng& rng) {
    const auto t = static_cast<objects::TypeIndex>(clone_type);
#ifdef ALAMO_SIM_HAVE_ANIM
    SimAnimation* src = anim_ ? anim_ : clone_models_;
    if (!src && catalog_->files()) {
        if (!own_clone_models_) own_clone_models_ = std::make_unique<SimAnimation>(catalog_->types(), *catalog_->files());
        src = own_clone_models_.get();
    }
    const anim::DeathPlan plan = src ? src->plan_clone_death(t, mode.kind(), kind, rng)
                                     : anim::plan_death(nullptr, anim::death_params(anim::type_anim_params(catalog_->types(), t, mode.kind())),
                                                        kind, rng);
    const bool remove_now = plan.remove_now, forever = plan.forever;
    const uint64_t lifetime = plan.lifetime;
#else
    // [port] a build without engine/anim has no clip data and no spin-away: Remove_Upon_Death -> at once, else
    // Death_Persistence_Duration (< 0 forever, default) + Death_Fade_Time (default 0.25 s), int(30 x s) each.
    (void)rng, (void)kind;
    const objects::GameObjectTypes& types = catalog_->types();
    const auto seconds = [&types, t](const char* field, float def) {
        return types.get_string(t, field).empty() ? def : types.get_float(t, field);
    };
    const auto frames = [](float s) { return s > 0.0f ? static_cast<uint64_t>(s * 30.0f) : uint64_t{0}; };
    const bool remove_now = types.get_bool(t, "Remove_Upon_Death");
    const float persist = seconds("Death_Persistence_Duration", -1.0f);
    const bool forever = !remove_now && persist < 0.0f;
    const uint64_t lifetime = frames(persist) + frames(seconds("Death_Fade_Time", 0.25f));
#endif
    if (remove_now || (!forever && lifetime == 0)) {
        mode.destroy(clone);
        ++stats_.death_clones_removed;
        return;
    }
    uint32_t at = kForeverFrame;
    if (!forever) at = lifetime >= static_cast<uint64_t>(kForeverFrame - 1u - frame_) ? kForeverFrame - 1u : frame_ + static_cast<uint32_t>(lifetime);
    death_clones_.push_back({mode.primary().id(clone), clone, at});
#ifdef ALAMO_SIM_HAVE_ANIM
    hooks_->anim_clone(clone, kind, &plan);
#endif
}

void TacticalBattle::remove_death_clones(TacticalMode& mode, bool all) {
    // Due clones go at their planned frame; at the battle's end (all) every clone left - forever ones included - goes with
    // it [port]: the tactical store is dropped when the mode pops (BT §3.1), and nothing of a clone (a removing object, no
    // battle unit) reaches the galactic write-back (units / participants only), the removal records or a later save.
    objects::ObjectStore& store = mode.primary();
    size_t w = 0;
    for (size_t i = 0; i < death_clones_.size(); ++i) {
        if (all || frame_ >= death_clones_[i].frame) {
            if (store.alive(death_clones_[i].h)) {
                mode.destroy(death_clones_[i].h);
                ++stats_.death_clones_removed;
            }
            continue;
        }
        death_clones_[w++] = death_clones_[i];
    }
    death_clones_.resize(w);
}

void TacticalBattle::expire_objects(TacticalMode& mode) {
    remove_death_clones(mode, false);  // refixf t04
    objects::ObjectStore& store = mode.primary();
    size_t w = 0;
    for (size_t i = 0; i < particles_.size(); ++i) {
        if (frame_ >= particles_[i].frame) {
            if (store.alive(particles_[i].h)) store.destroy(particles_[i].h);
            continue;
        }
        particles_[w++] = particles_[i];
    }
    particles_.resize(w);
    // Dummy targets: removed once no unit holds a pending ability on them (or after dummy_lifetime) [port].
    w = 0;
    for (size_t i = 0; i < dummies_.size(); ++i) {
        const TimedObject d = dummies_[i];
        bool used = frame_ <= d.frame;
        if (!used && frame_ < d.frame + cfg_.dummy_lifetime)
            for (const Unit& u : units_)
                used = used || (u.alive && !u.retreated && abilities_->pending_target(u.id) == d.id);
        if (used) {
            dummies_[w++] = d;
            continue;
        }
        abilities_->remove(d.id);
        if (store.alive(d.h)) store.destroy(d.h);
    }
    dummies_.resize(w);
}

void TacticalBattle::tick(TacticalMode& mode, SimRng& rng, const ParallelFor& pf, double sim_time, uint64_t global_frame) {
    if (finished_) {  // finished outside a tick (auto-resolve stop): the clones go before the mode pops (refixf t04)
        if (!death_clones_.empty()) remove_death_clones(mode, true);
        return;
    }
    ++frame_;
    oi_global_frame_ = global_frame;
    stats_.frames = frame_;
    story_reinforced_.clear();  // L1/S4 wave 2b story hook: this tick's reserve deployments
    story_garrisoned_.clear();  // S4: this tick's garrison entries / structure captures
    story_captured_.clear();
    objects::ObjectStore& store = mode.primary();
    l1_weather_tick(rng);  // L1 systems: land weather service (before the common tick)
    fog_decay();  // fog of war: common-tick step 4, before the object services (owner: fog agent)
    hg_production(mode, rng);  // station contents / skirmish tactical production (before the object services)
    profile::lap("tac.setup");
    // 1. reinforcements, 2. stand-in orders, 3. retreat coordinator.
    reinforce(mode, rng, true);
    if (cfg_.order_interval > 0 && (frame_ - 1) % cfg_.order_interval == 0) issue_orders(mode, rng);
    retreat_policy();
    if (retreat_side_ >= 0 && !retreat_complete_ && frame_ >= retreat_done_frame_) {
        bt_retreat_attrition(static_cast<uint8_t>(retreat_side_), rng);  // battle host: vf12 at the countdown end (§4.9)
        complete_retreat(mode, static_cast<uint8_t>(retreat_side_));
    }
    profile::lap("tac.orders");
    // 4. speed factors, movement, positions into the store and the trees, locomotor animation.
    apply_speed_factors();
    fighter_sync();  // S7 fighters: new squadrons (map start, reinforcements, hangar launches, wingmen) and params
    feed_attack_targets(store);
    if (space_) avoid_snapshot(store);  // SPACE_AVOIDANCE: committed member transforms for the fighter avoidance hook
    profile::lap("tac.pre_move");
    movement_->tick(frame_, rng, pf);
    movement_->commit(store);
    profile::lap("tac.movement");
    forward_fighter_events();  // S7 fighters: Provoke / Retarget -> combat attack orders (before the combat tick)
    for (const objects::ObjectId id : movement_->despawned())  // §4.6: land bombers at their path end, no death effects
        if (const Unit* u = unit_by_id(id)) bt_kill_silently(mode, u->h);
    oi_crush_.assign(movement_->crush_events().begin(), movement_->crush_events().end());  // OI tactical
    l1_motion(mode, rng);  // L1 systems: hyperspace jumps, engine re-enables
    hg_engine_service();   // engine loops: SimpleSpace locomotor services (moving first, then idle)
    bt_team_positions(mode);  // battle host: TEAM containers at their members' box centre (A1_READINGS §29.4)
    profile::lap("tac.motion_post");
    for (const Unit& u : units_)
        if (u.alive && !u.retreated && u.movable && store.alive(u.h))
            mode.trees().update(static_cast<tactical::EntityHandle>(u.id), mode.bounds_of(u.h));
    mode.trees().maintain(sim_time, pf);
    profile::lap("tac.trees");
    drive_animation(rng, global_frame);
    profile::lap("tac.anim");
    // 5. combat (abilities run inside its serial phases), events.
    for (const Unit& u : units_) {
        if (!u.alive || u.retreated || !u.movable) continue;
        combat_->set_position(u.id, tv(store.position(u.h)));
        combat_->set_facing(u.id, tv(facing_of_yaw(movement_->view(u.id).yaw)));
    }
    s4_positions(mode);  // S4: garrisoned units sit on their holders
    profile::lap("tac.combat_pre");
    hooks_->set_tick(&rng, global_frame);
    abilities_->tick(frame_, mode.trees(), rng, pf);
    profile::lap("tac.combat");
    consume_events(mode, rng);
    oi_respawn_due(mode);  // OI abilities: Tactical_Respawn creations due this frame
    profile::lap("tac.events");
    hg_tick(mode, rng);    // hangars: SPAWN_SQUADRON services and the launches they request
    l1_weapons(mode, rng);  // L1 systems: special-weapon volleys, TSW state machines
    profile::lap("tac.services");
    expire_objects(mode);
    s4_tick(mode, rng);  // S4: garrison enters / exits / ejects, structure capture
    pads_tick(mode);     // Int-final: build pads (construction completion, children gone)
    bt_shield_stamps(mode);  // battle host: base-shield secondary occupants follow the live shields (land)
    service_damage_tracking();
    profile::lap("tac.cleanup");
    fog_service(mode);  // fog of war: REVEAL sources, attacker flashes (owner: fog agent)
    profile::lap("tac.fog");
    advance_queues(mode, rng);  // [port: Gabriel] 5b. queueable unit actions: completed orders start the next entry
    stats_.shots = combat_->stats().shots;
    stats_.hits = combat_->stats().hits;
    // 6. victory.
    check_victory();
    if (finished_ && !death_clones_.empty()) remove_death_clones(mode, true);  // refixf t04: the battle's clones end with it
    space_obstacles_sync(mode);  // refixf t02: this tick's new / dead / retreated SPACE_OBSTACLE units
    hg_publish_engine_requests();  // engine loops: this tick's requests for the audio host
    profile::lap("tac.victory");
}

// ---- commands / owner ---------------------------------------------------------------------------------------------

std::vector<objects::ObjectId> TacticalBattle::owned_ids(const TacticalMode& mode, uint8_t player, const std::vector<uint32_t>& wire) const {
    std::vector<objects::ObjectId> out;
    const objects::ObjectStore& store = mode.primary();
    for (uint32_t w : wire) {
        if (w == cmd::kNoObject || w > 0x7FFFFFFFu) continue;
        const objects::ObjectHandle h = store.find(static_cast<objects::ObjectId>(w));
        if (!store.active(h) || store.owner(h) != static_cast<int32_t>(player)) continue;
        const objects::ObjectId id = store.id(h);
        if (!abilities_->has_object(id) || std::find(out.begin(), out.end(), id) != out.end()) continue;
        out.push_back(id);
    }
    return out;
}

// [port: Gabriel] queueable unit actions: a queued ability command appends one Ability entry per eligible user
// (idle users with an empty queue run it at once, as one command); a plain one clears its users' queues first.
cmd::RejectReason TacticalBattle::handle_command(TacticalMode& mode, uint8_t player, const cmd::EventData& ev, SimRng& rng) {
    using R = cmd::RejectReason;
    using T = cmd::EventType;
    if (finished_ || !abilities_) return R::NotAllowed;
    const T t = ev.type();
    if (t == T::Garrison || t == T::MoveToGarrison) return s4_command(mode, player, ev, rng);  // S4 garrisons
    // battle host: TacticalBuildEvent (33) / TacticalSell (34) - L1_S4_REMAINING §3.6; AI players build without a charge.
    if (t == T::Production) return hg_production_command(mode, player, ev);  // skirmish station production (else Unhandled)
    if (t == T::TacticalBuild) {
        const uint32_t pw = ev.u("pad"), tw = ev.u("type_index");
        if (pw == cmd::kNoObject || pw > 0x7FFFFFFFu) return R::InvalidTarget;
        const bool ai = player >= galaxy_->players().size() || !galaxy_->players()[player].human;
        return tactical_build(mode, player, static_cast<objects::ObjectId>(pw), static_cast<objects::TypeIndex>(tw), rng, ai);
    }
    if (t == T::TacticalSell) {
        const uint32_t ow = ev.u("object");
        if (ow == cmd::kNoObject || ow > 0x7FFFFFFFu) return R::InvalidUnit;
        return tactical_sell(mode, player, static_cast<objects::ObjectId>(ow));
    }
    // --- OI abilities: RepairHardpointEvent (45, 0x4308b0): adds the event player as a repairer of the hardpoint; no
    // ownership or credit check at command time, no cancel (TACTICAL_GAPS §1.12) ---
    if (t == T::RepairHardpoint) {
        const uint32_t ow = ev.u("object");
        if (ow == cmd::kNoObject || ow > 0x7FFFFFFFu) return R::InvalidUnit;
        return abilities_->repair_hardpoint(static_cast<objects::ObjectId>(ow), ev.u("hardpoint"), static_cast<int32_t>(player))
                   ? R::None
                   : R::InvalidTarget;
    }
    // --- end OI abilities ---
    if (t != T::SetUnitAbilityMode && t != T::TacticalSpecialAbility && t != T::TacticalSpecialAbilityWithDummyTarget &&
        t != T::SetAbilityAutofire)
        return R::Unhandled;
    const char* list = (t == T::TacticalSpecialAbility || t == T::TacticalSpecialAbilityWithDummyTarget) ? "users" : "units";
    if (!ev.queued()) {
        if (!queues_.empty()) clear_queues(owned_ids(mode, player, ev.ids(list)));
        return handle_ability(mode, player, ev, rng);
    }
    const std::vector<objects::ObjectId> ids = owned_ids(mode, player, ev.ids(list));
    if (ids.empty()) return R::InvalidUnit;
    const objects::ObjectStore& store = mode.primary();
    QueuedOrder q;
    q.kind = QueuedOrder::Kind::Ability;
    q.player = player;
    if (t == T::TacticalSpecialAbility) {
        const uint32_t w = ev.u("target");
        const objects::ObjectHandle th = w <= 0x7FFFFFFFu ? store.find(static_cast<objects::ObjectId>(w)) : objects::ObjectHandle{};
        if (!store.alive(th)) return R::InvalidTarget;
        q.target = store.id(th);
        const Vec3 p = store.position(th);
        q.x = p.x;
        q.y = p.y;
    } else if (t == T::TacticalSpecialAbilityWithDummyTarget) {
        if (dummy_type_ < 0) return R::NotAllowed;
        q.x = ev.f("x");
        q.y = ev.f("y");
    }
    q.ability = ev;
    q.ability.set_queued(false);
    const std::vector<objects::ObjectId> now = enqueue(ids, q);
    if (now.empty()) return R::None;
    cmd::EventData run = q.ability;
    std::vector<uint32_t> wire;
    for (const objects::ObjectId id : now) wire.push_back(static_cast<uint32_t>(id));
    run.set_ids(list, std::move(wire));
    return handle_ability(mode, player, run, rng);
}

cmd::RejectReason TacticalBattle::handle_ability(TacticalMode& mode, uint8_t player, const cmd::EventData& ev, SimRng& rng) {
    using R = cmd::RejectReason;
    using T = cmd::EventType;
    if (finished_ || !abilities_) return R::NotAllowed;
    objects::ObjectStore& store = mode.primary();
    auto object_of = [&store](uint32_t w) -> tc::ObjectId {
        if (w == cmd::kNoObject || w > 0x7FFFFFFFu) return tc::kNoObject;
        const objects::ObjectHandle h = store.find(static_cast<objects::ObjectId>(w));
        return store.alive(h) ? store.id(h) : tc::kNoObject;
    };
    switch (ev.type()) {
        case T::SetUnitAbilityMode: {  // 43
            const auto ids = owned_ids(mode, player, ev.ids("units"));
            if (ids.empty()) return R::InvalidUnit;
            ++stats_.ability_commands;
            const uint32_t n = abilities_->set_unit_ability_mode(ids, static_cast<uint8_t>(ev.u("ability")), ev.u("on") != 0, frame_,
                                                                 object_of(ev.u("object")));
            return n > 0 ? R::None : R::AbilityNotReady;
        }
        case T::TacticalSpecialAbility: {  // 40
            const auto users = owned_ids(mode, player, ev.ids("users"));
            const tc::ObjectId target = object_of(ev.u("target"));
            if (users.empty()) return R::InvalidUnit;
            if (target == tc::kNoObject) return R::InvalidTarget;
            ++stats_.ability_commands;
            const uint32_t hp = ev.u("id2");  // [port] the hardpoint index field (COMMANDS 2.2: "id32")
            const uint32_t n = abilities_->targeted_ability(users, static_cast<uint8_t>(ev.u("ability")), target,
                                                            hp < 64 ? static_cast<int32_t>(hp) : -1, frame_);
            return n > 0 ? R::None : R::AbilityNotReady;
        }
        case T::TacticalSpecialAbilityWithDummyTarget: {  // 57: Dummy_Barrage_Target at the ground point
            const auto users = owned_ids(mode, player, ev.ids("users"));
            if (users.empty()) return R::InvalidUnit;
            if (dummy_type_ < 0) return R::NotAllowed;
            const Vec3 at{ev.f("x"), ev.f("y"), ev.f("z")};
            const objects::ObjectId dummy = spawn_dummy_target(mode, static_cast<int32_t>(player), at, rng);  // wave 2b: shared
            ++stats_.ability_commands;
            const uint32_t n = abilities_->targeted_ability(users, static_cast<uint8_t>(ev.u("ability")), dummy, -1, frame_);
            return n > 0 ? R::None : R::AbilityNotReady;
        }
        case T::SetAbilityAutofire: {  // 58
            const auto ids = owned_ids(mode, player, ev.ids("units"));
            if (ids.empty()) return R::InvalidUnit;
            ++stats_.ability_commands;
            abilities_->set_autofire(ids, static_cast<uint8_t>(ev.u("ability")), ev.u("on") != 0);
            return R::None;
        }
        default: return R::Unhandled;
    }
}

// --- L1/S4 wave 2b story hook: the command-57 Dummy_Barrage_Target (moved out of handle_command, unchanged) ---
// [port] owned by the issuer's enemy side (attack orders need an enemy target), never auto-targeted, invulnerable;
// not in the spatial trees.
objects::ObjectId TacticalBattle::spawn_dummy_target(TacticalMode& mode, int32_t player, Vec3 at, SimRng& rng) {
    if (dummy_type_ < 0 || !abilities_) return objects::kNoObjectId;
    objects::ObjectStore& store = mode.primary();
    const int32_t owner = player == req_.info.attacker ? req_.info.defender : req_.info.attacker;
    objects::SpawnParams sp;
    sp.type = static_cast<objects::TypeIndex>(dummy_type_);
    sp.owner = owner;
    sp.position = at;
    const objects::ObjectHandle h = store.create(sp);
    tc::UnitSpawn cs;
    cs.id = store.id(h);
    cs.desc = desc_of(static_cast<uint32_t>(dummy_type_));
    cs.owner = owner;
    cs.position = tv(at);
    cs.flags = tc::kUnitInvulnerable | tc::kUnitSkipTargeting;
    abilities_->add_unit(cs, cs.desc, frame_, rng);
    dummies_.push_back({cs.id, h, frame_});
    ++stats_.dummies;
    return cs.id;
}
// --- end L1/S4 wave 2b ---

bool TacticalBattle::change_owner(TacticalMode& mode, objects::ObjectHandle h, int32_t owner) {
    objects::ObjectStore& store = mode.primary();
    if (!store.alive(h) || owner < 0) return false;
    const objects::ObjectId id = store.id(h);
    if (!abilities_ || !abilities_->has_object(id)) return false;
    const int32_t old_owner = store.owner(h);
    if (!store.set_owner(h, owner)) return false;
    mode.trees().set_owner(static_cast<tactical::EntityHandle>(id), owner);
    abilities_->set_owner(id, owner);
    l1_sw_.set_owner(id, owner);  // L1 systems
    if (movement_) movement_->set_owner(id, owner);  // OI tactical: crush hostility
    fog_owner_changed(mode, h, old_owner);  // fog of war: REVEAL vf18 (owner: fog agent)
    s4_owner_changed(mode, id, old_owner, owner);  // S4: garrison owners (STORY_CAPTURE_HERO has no dispatcher: L1_S4_REMAINING §4.3)
    if (Unit* u = unit_by_id(id)) {
        if (owner == req_.info.defender) u->side = 0;
        else if (owner == req_.info.attacker) u->side = 1;
    }
    return true;
}

bool TacticalBattle::script_change_owner(TacticalMode& mode, objects::ObjectHandle h, int32_t owner) {
    objects::ObjectStore& store = mode.primary();
    if (!store.alive(h) || owner < 0) return false;
    if (change_owner(mode, h, owner)) return true;
    const objects::ObjectId id = store.id(h);
    const int32_t old_owner = store.owner(h);
    if (!store.set_owner(h, owner)) return false;
    mode.trees().set_owner(static_cast<tactical::EntityHandle>(id), owner);
    fog_owner_changed(mode, h, old_owner);
    s4_owner_changed(mode, id, old_owner, owner);
    return true;
}

// ---- reporting / CRC ----------------------------------------------------------------------------------------------

bool TacticalBattle::unit_attitude(objects::ObjectId id, float& roll_rad, float& pitch_rad) const {
    float r = 0.0f, p = 0.0f;
    const bool ok = movement_ && movement_->attitude(id, r, p);
    constexpr float kDegToRad = 0.017453292f;  // FIGHTER_FLYING §1.1 k
    roll_rad = r * kDegToRad;
    pitch_rad = p * kDegToRad;
    return ok;
}

std::string TacticalBattle::summary() const {
    auto faction = [this](int32_t p) -> std::string {
        if (p < 0 || static_cast<size_t>(p) >= galaxy_->players().size()) return "-";
        const uint32_t f = galaxy_->players()[static_cast<size_t>(p)].faction;
        return f < galaxy_->rules().factions.size() ? galaxy_->rules().factions[f].name : "?";
    };
    return std::string(space_ ? "space" : "land") + " battle " + req_.map + ": " + std::to_string(stats_.frames) + " frames, " +
           std::to_string(stats_.spawned) + " spawned (" + std::to_string(stats_.reinforcements) + " reinforcements), " +
           std::to_string(stats_.deaths) + " deaths, " + std::to_string(combat_ ? combat_->stats().scans : 0) + " scans, " + std::to_string(stats_.shots) + " shots / " + std::to_string(stats_.hits) +
           " hits, " + std::to_string(stats_.hardpoints_destroyed) + " hardpoints destroyed, " + std::to_string(stats_.activations) +
           " ability activations, " + std::to_string(stats_.particles) + " particle objects, " + std::to_string(stats_.props + stats_.death_clones) +
           " props/clones, winner " + faction(result_.winner) + ", victory " + victory_name(result_.victory) + ", retreated " +
           faction(result_.retreated) + ", galactic losses " + std::to_string(result_.destroyed.size()) +
           (result_.starbase_destroyed ? " + starbase" : "");
}

void TacticalBattle::hash(StateHasher& h) const {
    h.u32(frame_);
    h.boolean(finished_);
    h.i32(retreat_side_);
    h.u32(retreat_done_frame_);
    h.boolean(retreat_complete_);
    if (end_hold_until_ != 0) {  // absent = older streams unchanged (end_hold_frames 0 never sets it)
        h.u32(end_hold_until_);
        h.i32(end_winner_);
        h.u8(end_victory_);
        h.i32(end_retreated_);
    }
    h.u32(placed_[0]);
    h.u32(placed_[1]);
    h.u32(static_cast<uint32_t>(parts_.size()));
    for (const Participant& p : parts_) {
        h.i32(p.galactic_id);
        h.boolean(p.deployed);
        h.u32(p.spawned);
        h.u32(p.alive);
    }
    h.u32(static_cast<uint32_t>(units_.size()));
    for (const Unit& u : units_) {
        h.i32(u.id);
        h.boolean(u.alive);
        h.boolean(u.retreated);
    }
    h.i32(result_.winner);
    h.u8(result_.victory);
    h.i32(result_.retreated);
    h.boolean(result_.starbase_destroyed);
    h.u32(static_cast<uint32_t>(result_.destroyed.size()));
#ifdef ALAMO_SIM_HAVE_AI_TACTICAL
    damage_tracking_.hash(h);
#else
    h.u32(0);  // empty DAMAGE_TRACKING store (same layout without engine/ai)
#endif
    if (queue_used_) hash_queues(h);  // [port: Gabriel] order queues (absent until an order was queued)
    s4_hash(h);                       // S4: garrisons / capture points (absent while none exist)
    if (bt_used_) bt_hash(h);         // battle host: pad units, HIJACK reserves, lead object, attrition (absent until used)
}

void TacticalBattle::hash_abilities(StateHasher& h) const {
    if (abilities_) abilities_->hash(h);
    if (combat_) combat_->hash_extended(h);
    if (movement_) movement_->hash_speed(h);
    h.u32(static_cast<uint32_t>(units_.size()));
    for (const Unit& u : units_) {
        h.u8(u.side);
        h.boolean(u.engines_offline);
    }
    h.u32(static_cast<uint32_t>(containers_.size()));
    for (const Container& c : containers_) {
        h.i32(c.id);
        h.u32(c.alive);
    }
    h.u32(static_cast<uint32_t>(particles_.size()));
    for (const TimedObject& p : particles_) {
        h.i32(p.id);
        h.u32(p.frame);
    }
    h.u32(static_cast<uint32_t>(dummies_.size()));
    for (const TimedObject& d : dummies_) {
        h.i32(d.id);
        h.u32(d.frame);
    }
    if (!death_clones_.empty()) {  // refixf t04 (absent while none)
        h.u32(static_cast<uint32_t>(death_clones_.size()));
        for (const TimedObject& c : death_clones_) {
            h.i32(c.id);
            h.u32(c.frame);
        }
    }
    if (l1_used_) l1_hash(h);  // L1 systems (absent until a pack / jump / engine entry existed)
    if (pads_used_) pads_hash(h);  // Int-final: build pads (absent until a pad was used)
    if (!oi_respawns_.empty()) oi_hash(h);  // OI abilities: pending Tactical_Respawn creations (absent while none)
    if (hg_used_) hg_hash(h);  // hangars: carriers, launches, upgrade tech (absent until a carrier / upgrade existed)
}

}  // namespace alamo::sim
