#include "alamo/sim/tactical_battle.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>

#include "alamo/core/ascii.hpp"
#include "alamo/core/dmath.hpp"
#include "alamo/core/hash.hpp"
#include "alamo/core/profile.hpp"
#include "alamo/galactic/special_data.hpp"  // P4 tactical: special_name_crc (Black Market gate)
#include "alamo/objects/game_object_types.hpp"
#include "alamo/sim/map_spawn.hpp"
#include "alamo/tactical/movement/land_path.hpp"  // bkt1 t01: zone_connected (the staging search)
#include "alamo/tactical/movement/space_avoid.hpp"
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
constexpr objects::BehaviorTypeId kWallBehavior = 0x17;            // WALL (S8 t13: EmpireWall stamp bit)
constexpr uint32_t kDamageTrackingInterval = 30;
#endif
constexpr uint32_t kNoContainer = 0xFFFFFFFFu;

Vec3 facing_of_yaw(float yaw_deg) {
    float s = 0.0f, c = 1.0f;
    dmath::sincos(yaw_deg * (dmath::kPiF / 180.0f), s, c);
    return {c, s, 0.0f};
}

float yaw_of(Vec3 f) { return dmath::atan2(f.y, f.x) * (180.0f / dmath::kPiF); }

// s78j t01 (TACTICAL_COMBAT §3.1.1 "Units leaving a land landing object"): 0x20acd0((0, 0, 0), O) - the level facing
// along the unload offset O (yaw atan2(O.y, O.x); yaw 0 when O's xy are zero).
Vec3 offset_facing(float ox, float oy) { return facing_of_yaw(ox == 0.0f && oy == 0.0f ? 0.0f : yaw_of(Vec3{ox, oy, 0.0f})); }

tactical::Vec3 tv(Vec3 v) { return tactical::Vec3{v.x, v.y, v.z}; }
Vec3 sv(tactical::Vec3 v) { return Vec3{v.x, v.y, v.z}; }

// S8 t09 / t13 (MOVEMENT §1.3): the LAND_OBSTACLE stamper's model hooks, one source for map placements and run-time objects:
// the model bounding box (BattleCatalog::model_half_extents) and the Fine_Tune_Occupied_Passability ray against the type's
// Land_Model_Name collision data (BattleCatalog::avoid_model(type, false)). Ray model: no model files in this run (pure
// headless) -> Unknown, untrimmed [port]; an empty Land_Model_Name -> None (the footprint rules for a type without a land
// model, S8 t13); a model that loads -> Model, even without collision meshes (then no sample is hit: retail's ray query
// hits only collision-flagged meshes); a named model that does not load -> Unknown [port]. The ray (S8 t13, vf +0x80 mode
// 0 mask 1: mv::avoid_ray_any) is the segment (x, y, -1000) -> (x, y, +1000) against the collision-flagged meshes whose bone
// is visible, through the model placed by space_avoid's transform (position, facing Rz(yaw) (0, -1), scale). Const data.
mv::MapStampHooks land_stamp_hooks(const BattleCatalog* cat) {
    mv::MapStampHooks hooks;
    hooks.model_half_extents = [cat](uint32_t ty, float& hx, float& hy, float& hz) { return cat->model_half_extents(ty, hx, hy, hz); };
    hooks.ray_model = [cat](uint32_t ty) {
        if (!cat->files() || ty >= cat->types().size()) return mv::StampRayModel::Unknown;
        std::string_view name = cat->types().get_string(ty, "Land_Model_Name");
        while (!name.empty() && (name.front() == ' ' || name.front() == '\t' || name.front() == '\r' || name.front() == '\n'))
            name.remove_prefix(1);
        while (!name.empty() && (name.back() == ' ' || name.back() == '\t' || name.back() == '\r' || name.back() == '\n'))
            name.remove_suffix(1);
        if (name.empty()) return mv::StampRayModel::None;
        return cat->avoid_model(ty, false) ? mv::StampRayModel::Model : mv::StampRayModel::Unknown;
    };
    hooks.mesh_ray_hit = [cat](uint32_t ty, const mv::StampPose& pose, mv::V2 xy) {
        const mv::AvoidModel* m = cat->avoid_model(ty, false);
        if (!m) return true;
        mv::AvoidObstacle o;
        o.pos = {pose.position.x, pose.position.y, pose.z};
        const mv::V2 d = mv::dir_from_deg(pose.yaw_deg);
        o.facing = {d.y, -d.x};
        o.scale = pose.scale;
        o.model = m;
        mv::avoid_place(o);
        return mv::avoid_ray_any(o, {xy.x, xy.y, -1000.0f}, {xy.x, xy.y, 1000.0f});
    };
    return hooks;
}

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
    uint64_t global_frame() const { return gframe_; }  // s78k t05: test_play_clip
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
        b_->hero_clash_apply_hits(frame);  // s78n t06: the hero-clash exchange damage of this tick (HERO_CLASH §3.6)
        a_->before_units(frame, rng);
    }
    void unit_begin(tc::ObjectId id, uint32_t frame, SimRng& rng) override { a_->unit_begin(id, frame, rng); }
    void unit_after_targeting(tc::ObjectId id, uint32_t frame, SimRng& rng) override { a_->unit_after_targeting(id, frame, rng); }
    // s78f t03 (TACTICAL_COMBAT §3.1.1 "Slot 47"): the attack-idle pose 0x569400 - clip 6 Attack Idle, index 0, looping,
    // speed 1, blend = one synced Get_Float(0.2, 0.75), when (arg 0 or Locomotor_Has_Animation_Priority clear), the
    // locomotor is not moving, the current clip is not a flinch (79-86) and not Attack (5) / Attack Idle (6) - one
    // short-circuit conjunction, so while clip 5 / 6 plays nothing is drawn or restarted (s78j t02 [H], reverting s78i
    // t01). The draw is spent whether or not the model has the clip (the blend is the call's argument; a model without
    // clip 6 keeps its old clip and draws every service). [M] The animation gate 0x397040 is read as false. Without an
    // animation world nothing is played or drawn [port].
    void attack_idle(tc::ObjectId id, bool arg, uint32_t frame, SimRng& rng) override {
        (void)frame;
#ifdef ALAMO_SIM_HAVE_ANIM
        if (!world_ || !world_->contains(id)) return;
        const tc::CombatSystem& c = *b_->combat_;
        if (c.flags(id) & tc::kUnitMoving) return;
        if (arg) {
            const int32_t t = c.data().units[c.desc_index(id)].object_type;
            const objects::GameObjectTypes& ty = b_->catalog_->types();
            if (t >= 0 && static_cast<size_t>(t) < ty.size() && ty.get_bool(static_cast<objects::TypeIndex>(t), "Locomotor_Has_Animation_Priority"))
                return;
        }
        const int32_t cur = world_->current_type(id);
        if (anim::is_flinch(cur)) return;
        // s78j t02 (TACTICAL_COMBAT §3.1.1 "Order of the tests" [H]): the clip test comes before the draw. (The s78i t01
        // probe IDLE47N never ran slot 47: Prevent_All_Fire suspends TARGETING / WEAPON.)
        if (cur == anim::kAnimAttack || cur == anim::kAnimAttackIdle) return;
        const float blend = rng.range_float(0.2f, 0.75f);
        anim::PlayRequest r;
        r.type = anim::kAnimAttackIdle;
        r.index = 0;
        r.loop = true;
        r.blend_seconds = blend;
        r.speed = 1.0f;
        world_->play(id, r, &rng, gframe_);
#else
        (void)id, (void)arg, (void)rng;
#endif
    }
    // s78f t03: the facing snap / [port] turn step of the targeting service moves the locomotor's yaw (combat's facing is
    // re-read from the movement view before each tick).
    void facing_set(tc::ObjectId id, tactical::Vec3 f) override {
        if (b_->movement_ && b_->movement_->has_unit(id)) b_->movement_->snap_yaw(id, dmath::atan2(f.y, f.x) * (180.0f / dmath::kPiF));
    }
    // s78f t03 / t08: the TURRET aim of a land turret unit (MovementSystem S8 t24 model, one frame behind the combat
    // target; -2 beyond the rotate extent); units without the model (no movement unit, space) answer -1 (treated as aimed).
    int8_t turret_aim_state(tc::ObjectId id) const override {
        if (!b_->movement_ || !b_->movement_->has_unit(id)) return -1;
        return b_->movement_->view(id).turret_aim;
    }
    // s78f t08: the pending targeted ability (TD +0x148: AbilitySystem's pending order) and the deploy request 0x391260
    // (the Walk locomotor's Deploy(true), as the DEPLOY ability's; the change reaches the abilities through oi_deploy_sync).
    tc::ObjectId pending_ability_target(tc::ObjectId id) const override {
        return b_->abilities_ ? b_->abilities_->pending_target(id) : tc::kNoObject;
    }
    void request_deploy(tc::ObjectId id, uint32_t frame) override {
        if (b_->movement_ && b_->movement_->has_unit(id)) (void)b_->movement_->deploy(id, true, frame);
    }
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
        // s78n t26.1 (MOVEMENT §3.2.1 item 3, Take_Damage 0x3a9e30): a hit deploys an idle Walk unit with
        // Max_Dist_Walk_While_Deployed > 0 (the Destroyer Droid)
        if (b_->movement_ && b_->movement_->has_unit(victim)) b_->movement_->damage_deploy(victim, frame);
        return a_->damage_listener(victim, source, projectile_desc, frame, rng);
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
    bool weather_affected(int32_t player) const override { return a_->weather_affected(player); }  // cu8 t01 (Weatherproof)
    // s78n t05 (HERO_CLASH §7): the convert victim test's occupied-build-pad step (no pad state before a pad is used).
    bool occupied_build_pad(tc::ObjectId id) const override {
        return b_->pads_used_ && b_->pad_occupied(id);
    }
    // cu8f t02: energy-pool engine disable (ion projectiles, CU8F_T01_T02_T07_RE.md §2.4).
    void engines_drained(tc::ObjectId id, float seconds, uint32_t /*frame*/) override { b_->energy_drain_engines(id, seconds); }
    void units_removed() override { a_->units_removed(); }
    // OI abilities: detonation effects / land stuns (TACTICAL_GAPS §1.10)
    void unit_effect(tc::ObjectId id, tc::UnitEffect e, int32_t player, tactical::Vec3 p, uint32_t frame) override {
        // q6 (OBJECT_SCRIPTS §10.2): the Invulnerability ability adds its frames to the one INVULNERABLE behaviour, so a
        // script-invulnerable unit (Make_Invulnerable(true): duration 0, kUnitInvulnerable) it hits is invulnerable for
        // the ability's frames only and loses it when they run out. [port] the timed form stands for the behaviour from
        // here; the Skip_Targeting stand-in targets (kUnitInvulnerable too) are never script objects and are left alone.
        if (e == tc::UnitEffect::Invulnerable && b_->combat_) {
            const uint32_t f = b_->combat_->flags(id);
            if ((f & tc::kUnitInvulnerable) && !(f & tc::kUnitSkipTargeting)) b_->combat_->set_flag(id, tc::kUnitInvulnerable, false);
        }
        a_->unit_effect(id, e, player, p, frame);
    }
    // --- fog follow-up: S8 targeting filter step 4 / special-weapon force 1 (read-only: parallel gather phase) ---
    bool target_hidden(int32_t player, tc::ObjectId target, bool force) const override {
        return b_->fog_object_hidden(player, target, force);
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
                               const galactic::ConflictRules& rules, galactic::BattleRequest request, BattleConfig cfg,
                               const ParallelFor& pf)
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
    tables_ = std::make_unique<BattleTables>();  // battle host: model bones
    tables_build_ = catalog.begin_battle(*tables_, space_);
    tables_build_->pf = pf;
    tables_extend();
}

TacticalBattle::~TacticalBattle() = default;

void TacticalBattle::tables_extend() {
    // OPT3 t09: setup appends types several times; each step builds only the new ones (BattleCatalog::extend_battle) instead
    // of rebuilding every type from the start. Same tables and descriptor indices either way. cu9 t12: the constructor
    // always makes the build and keeps it for the battle's life, so there is no full-rebuild fallback.
    catalog_->extend_battle(*tables_build_, types_used_, descs_);
}

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

// cu8j t02 (MOVEMENT §4.7.1 locomotor table): the active locomotor's vf +0x220 (admitted) and vf +0x238 (kind) by its
// behaviour token, the mode's behaviour list first (as locomotor_kind_from_type). Walk / Bike / LandTeamInfantry /
// LandTeamContainer: kind 1; SimpleSpace / Fighter / Flying / LandBomber: kind 2; JetPack: kind 1 [port: on the ground,
// as spawned]; Team: kind 2 when a member admits (-1 here); Starship / Fleet / Simple and no locomotor: not admitted.
std::string_view TacticalBattle::mv_locomotor_token(uint32_t type) const {
    const objects::GameObjectTypes& types = catalog_->types();
    if (type >= types.size()) return {};
    for (const std::string_view tag : {space_ ? std::string_view("SpaceBehavior") : std::string_view("LandBehavior"), std::string_view("Behavior")}) {
        for (const std::string_view raw : types.get_list(type, tag)) {
            const std::string_view t = ascii_trim(raw);
            constexpr std::string_view kSuffix = "LOCOMOTOR";  // any *_LOCOMOTOR token is the locomotor slot
            if (t.size() >= kSuffix.size() && ascii_iequals(t.substr(t.size() - kSuffix.size()), kSuffix)) return t;
        }
    }
    return {};
}

int8_t TacticalBattle::mv_adopt_kind(uint32_t type) {
    const objects::GameObjectTypes& types = catalog_->types();
    if (type >= types.size()) return 0;
    if (adopt_kind_.size() < types.size()) adopt_kind_.resize(types.size(), int8_t{0});
    if (adopt_kind_[type] != 0) return static_cast<int8_t>(adopt_kind_[type] - 1);
    int8_t k = 0;
    const std::string_view t = mv_locomotor_token(type);
    if (ascii_iequals(t, "WALK_LOCOMOTOR") || ascii_iequals(t, "BIKE_LOCOMOTOR") || ascii_iequals(t, "JETPACK_LOCOMOTOR") ||
        ascii_iequals(t, "LAND_TEAM_INFANTRY_LOCOMOTOR") || ascii_iequals(t, "LAND_TEAM_CONTAINER_LOCOMOTOR"))
        k = 1;
    else if (ascii_iequals(t, "SIMPLE_SPACE_LOCOMOTOR") || ascii_iequals(t, "FIGHTER_LOCOMOTOR") ||
             ascii_iequals(t, "FLYING_LOCOMOTOR") || ascii_iequals(t, "LANDBOMBER_LOCOMOTOR"))
        k = 2;
    else if (ascii_iequals(t, "TEAM_LOCOMOTOR"))
        k = -1;
    adopt_kind_[type] = static_cast<int8_t>(k + 1);
    return k;
}

// cu8l t01 (CU8K_T02_T06_RE §1.2 / §1.5 / §4 rule 1): the locomotors whose behaviour init (vf9, run by the behaviour
// attach 0x38cb30 at creation) registers the object itself - a new coordinator holding a Move_To its own position:
// LandTeamContainer (0x5abac0) on land, SimpleSpace (0x625090) in space.
bool TacticalBattle::mv_self_registers(uint32_t type) const {
    const std::string_view t = mv_locomotor_token(type);
    return ascii_iequals(t, space_ ? std::string_view("SIMPLE_SPACE_LOCOMOTOR") : std::string_view("LAND_TEAM_CONTAINER_LOCOMOTOR"));
}

bool TacticalBattle::mv_adopt(const TacticalMode& mode, objects::ObjectHandle h, SimRng& rng, bool leave) {
    if (!movement_) return false;
    const objects::ObjectStore& st = mode.primary();
    if (!st.alive(h)) return false;
    const objects::ObjectId id = st.id(h);
    if (const Unit* u = unit_by_id(id); u && u->container != kNoContainer) return false;  // a TEAM member: never
    int8_t kind = mv_adopt_kind(static_cast<uint32_t>(st.type(h)));
    if (kind < 0) {  // TEAM_LOCOMOTOR (0x6305a0): vf +0x220 = any member's, vf +0x238 = 2
        kind = 0;
        for (size_t ci = 0; ci < containers_.size() && kind == 0; ++ci) {
            if (containers_[ci].id != id) continue;
            for (const Unit& m : units_)
                if (m.container == ci && m.alive && !m.retreated && mv_adopt_kind(m.type) > 0) {
                    kind = 2;
                    break;
                }
        }
    }
    if (kind != (space_ ? 2 : 1)) return false;
    if (movement_->has_unit(id)) return leave ? movement_->stop_and_hold(id, rng) : movement_->adopt(id, rng);
    // [port] an object outside the movement system (a TEAM container, an inert map object) keeps no coordinator record:
    // each adopt draws.
    (void)mv::MovementSystem::formation_ctor_draw(rng);
    return true;
}

// cu8l t02 (CU8K_T02_T06_RE §3 "movement lock", rule 6; A1_READINGS §28.8): the first lock 0x3a8e50 (counter 0 -> 1). An
// object in a formation is split off (0x4f84e0: one draw; the adopt then finds it in a coordinator: no second draw [M]);
// one in a coordinator without a formation draws nothing; one without a coordinator is adopted (0x39bcb0 predicate: one
// draw when admitted). [port] A TEAM member draws nothing (its container holds the formation); a TEAM container is read
// from its members (the movement system's units): any of them waiting for a composition or held by an adopt -> no draw,
// any in a formation -> the split's draw, else the container's adopt.
bool TacticalBattle::mv_first_lock(const TacticalMode& mode, objects::ObjectId id, SimRng& rng) {
    using Hold = mv::MovementSystem::CoordinatorHold;
    if (!movement_ || finished_) return false;
    if (const Unit* u = unit_by_id(id)) {
        if (!u->alive || u->retreated || u->container != kNoContainer) return false;
        if (u->movable && movement_->split_from_formation(id, rng)) return true;
        if (u->movable && movement_->coordinator_hold(id) != Hold::None) return false;
        return mv_adopt(mode, u->h, rng);
    }
    for (size_t ci = 0; ci < containers_.size(); ++ci) {
        if (containers_[ci].id != id || containers_[ci].removed) continue;
        if (movement_->has_unit(id)) {  // s78n t03: a land squad container is a mover with its own coordinator
            if (movement_->split_from_formation(id, rng)) return true;
            if (movement_->coordinator_hold(id) != Hold::None) return false;
            return mv_adopt(mode, containers_[ci].h, rng);
        }
        Hold hold = Hold::None;
        for (const Unit& m : units_)
            if (m.container == ci && m.alive && !m.retreated && m.movable) {
                const Hold mh = movement_->coordinator_hold(m.id);
                if (mh == Hold::NoFormation) hold = Hold::NoFormation;
                else if (mh == Hold::Formation && hold == Hold::None) hold = Hold::Formation;
            }
        if (hold == Hold::NoFormation) return false;
        if (hold == Hold::Formation) {
            (void)mv::MovementSystem::formation_ctor_draw(rng);  // the container's split
            return true;
        }
        return mv_adopt(mode, containers_[ci].h, rng);
    }
    return false;
}

void TacticalBattle::script_first_lock(TacticalMode& mode, objects::ObjectId id) {
    if (bt_rng_) (void)mv_first_lock(mode, id, *bt_rng_);
}

bool TacticalBattle::select_all_entry(objects::ObjectId id, int32_t owner, std::vector<objects::ObjectId>& members) const {
    if (owner >= 0 && owner == retreating_player()) return false;  // the retreat coordinator's list [port: the whole side]
    if (garrison_holder_of(id) != objects::kNoObjectId) return false;  // garrisoned: parent = the host
    const auto it = std::lower_bound(units_.begin(), units_.end(), id, [](const Unit& u, objects::ObjectId v) { return u.id < v; });
    if (it != units_.end() && it->id == id && it->container != kNoContainer) return false;  // a TEAM member
    for (size_t ci = 0; ci < containers_.size(); ++ci) {
        if (containers_[ci].id != id || !containers_[ci].team) continue;
        for (const Unit& u : units_)
            if (u.container == ci && u.alive && !u.retreated) members.push_back(u.id);
        break;
    }
    return true;
}

// FIGHTER_FLYING §6.3.1 type data: Uses_Multiple_Locomotors (+0x74), Stationary_Space_Layer (+0x16e0), the query
// half-extents 0x396850 and the land model's (type +0x2a0, no fallback to other modes' models) box z for the hit value.
// s78n t10 (TACTICAL_COMBAT §5.4.6 port spec item 1): the half-extents are 0x371be0 - both Custom_Hard extents > 0 ->
// x Scale_Factor; else no land model -> (0, 0); else per axis the custom value or the |collision-bound half x / y|
// (AvoidModel::bound_h, not |max| of the all-mesh bind-pose box), x Scale_Factor - and the hit value reads the collision
// bound's centre z and half z, so clearance_top = (c.z + h.z) x Scale_Factor + O.z. No team or deployed branch: flyers
// and Corvette-layer objects are neither. [port] a named land model that does not load (no art data): per axis the
// custom value, else 0, and no hit value. Pure function of the type and the model files: cached per type, not saved.
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
        const mv::AvoidModel* am = catalog_->avoid_model(t, false);
        const bool named = !ascii_trim(types.get_string(t, "Land_Model_Name")).empty();
        ci.has_box = am != nullptr;
        if (am) {
            ci.box_lo_z = am->bound_c.z;
            ci.box_hi_z = am->bound_h.z;
        }
        ci.half = mv::clearance_half_extents(types.get_float(t, "Custom_Hard_XExtent"), types.get_float(t, "Custom_Hard_YExtent"),
                                             ci.scale_factor, am != nullptr || named, am ? am->bound_h.x : 0.0f,
                                             am ? am->bound_h.y : 0.0f);
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
// switch (off while contained) is not modelled: space members are never garrisoned. S7 t08 (SPACE_AVOIDANCE §5.5): no
// other game-object path switches it off (capture points and orbital props keep colliding), so nothing else to filter.
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
#ifdef ALAMO_SIM_HAVE_ASSETS
    // refixf t07 (SPACE_AVOIDANCE §3.1 / §3.2 / §7.2): the tree boxes and the precise projectile test read the type
    // model's collision data (Space_Model_Name / Land_Model_Name, as the fighter avoidance does). A model that does not
    // load gives no data and its type keeps the extent box and box hits [port]. [port] So does a type without a model
    // name (retail: no model, box [P, P + 0.1], never hit): the synthetic test data has no model names, and among the FoC
    // projectile-collidable types only Dummy_Barrage_Target and abstract base types have none.
    if (catalog_->files()) {
        const BattleCatalog* cat = catalog_;
        const bool space = map_.is_space();
        mode.set_model_source([cat, space](objects::TypeIndex t) {
            TacticalMode::ModelRef r;
            r.model = cat->avoid_model(t, space);
            r.known = r.model != nullptr;
            return r;
        });
    }
#endif
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
    tables_build_->pf = pf;  // OPT3 t09
    bt_rng_ = &rng;            // battle host: pad objects join the battle with their add_unit draws
    bt_players_ = players;
    bt_map_prepass();          // REMAINING_GAPS §3.1: base-level filter + branched-map discard; map types -> tables
    bt_add_pad_types(map_);    // battle host: the map's pad UC / constructed types join the tables (before combat exists)
    oi_add_ability_types();    // OI abilities: ability-spawned / survivor types join the tables (before combat exists)
    hg_add_types();            // hangars: launched squadrons / ships and the station chain join the tables
#ifdef ALAMO_SIM_HAVE_ASSETS
    // OPT3 t05 / t09: the model files of the map's placements (after the prepass: a land skirmish's team markers are its pads
    // now) and the participants load in parallel here instead of one type at a time inside the spawns and stamps below
    // (the first battle tick's largest cost); results unchanged.
    if (catalog_->files()) {
        const TypeCrcIndex index(catalog_->types());
        std::vector<uint32_t> types;
        types.reserve(map_.objects.size() + parts_.size());
        for (const terrain::ObjectPlacement& pl : map_.objects)
            if (const objects::TypeIndex t = index.find(pl.type_crc); t != objects::kNone) types.push_back(static_cast<uint32_t>(t));
        for (const Participant& p : parts_) types.push_back(p.type);
        catalog_->preload_models(types, map_.is_space(), pf);
    }
#endif
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
        drop_facing_[0] = drop_facing_[1] = Vec3{};  // a1h t06: mode +0x288 starts zeroed (no marker -> yaw 0)
    }
    // Movement world.
    mv::MovementConfig mc;
    mc.mode = space_ ? mv::MoveMode::Space : mv::MoveMode::Land;
    movement_ = std::make_unique<mv::MovementSystem>(mc);
    // cu8j t01 (MOVEMENT §4.6.2): the movement coordinator update runs at the end of the tick (TacticalBattle::tick), after
    // the object services, as retail's common tick step 10; the movement tick does not run it.
    movement_->set_deferred_composition(true);
    // OI tactical: crush hostility = the combat relation (player +0x370 table, MOVEMENT.md 5.5).
    movement_->set_hostility([this](int32_t a, int32_t b) { return combat_ && combat_->is_enemy(a, b); });
    // s78n t14 part B (FIGHTER_FLYING §7.1, 0x5c8890): Scatters_From_Crushers of the owner's faction ([port] false for an
    // owner without a galactic player).
    movement_->set_scatters_fn([this](int32_t owner) {
        if (!galaxy_ || !rules_ || owner < 0 || static_cast<size_t>(owner) >= galaxy_->players().size()) return false;
        return rules_->faction(galaxy_->players()[static_cast<size_t>(owner)].faction).scatters_from_crushers;
    });
    // cu8l t03 (CU8K_T03_T08_T09_RE §1.10 rule 1): the movement lock of the start fleet's hyperspace arrival (arrivals_).
    movement_->set_locked_fn([this](mv::ObjectId key) { return arrival_locked(key); });
    // s78n t10 (TACTICAL_COMBAT §5.4.6 port spec item 4): the space composition's candidate boxes are the world AABB
    // 0x3982b0 - the Space_Model_Name collision bound through the unit's yaw and Scale_Factor (as avoid_snapshot builds
    // the avoidance members); a type without a space model name has no model (the 0.1 box at the position). [port] no
    // model files in this run, or a named model that does not load: no data (MovementSystem keeps its stand-in).
    if (space_)
        movement_->set_model_bound_fn([this](mv::ObjectId id, const mv::AvoidModel*& model, float& scale) {
            const Unit* u = unit_by_id(id);
            const objects::GameObjectTypes& types = catalog_->types();
            if (!u || u->type >= types.size() || !catalog_->files()) return false;
            scale = types.columns().scale_factor[u->type];
            model = nullptr;
            if (ascii_trim(types.get_string(u->type, "Space_Model_Name")).empty()) return true;
            model = catalog_->avoid_model(u->type, true);
            return model != nullptr;
        });
    // S8 t24 (MOVEMENT §3.2 "Turret recentre around deploy"): a Deploys unit's TURRET aims at its live current combat
    // target (0x632050 [inferred: whenever the unit has one]); the raw position, no z adjust [inferred].
    movement_->set_turret_aim_fn([this](objects::ObjectId id, float& x, float& y, float& z) {
        if (!combat_ || !combat_->has_unit(id)) return false;
        const tc::ObjectId t = combat_->current_target(id);
        if (t == tc::kNoObject || !combat_->has_unit(t) || !combat_->alive(t)) return false;
        const auto p = combat_->position(t);
        x = p.x;
        y = p.y;
        z = p.z;
        return true;
    });
#ifdef ALAMO_SIM_HAVE_ANIM
    // REMAINING_GAPS §4.3-4.7: special-locomotor anim lengths = the model's clip (variant 0) in sim frames (30 / s).
    movement_->set_anim_length_fn([this](objects::ObjectId id, int32_t type) -> uint32_t {
        const anim::AnimWorld* w = hooks_ ? hooks_->world() : nullptr;
        const anim::AnimModel* m = w ? w->model(id) : nullptr;
        const anim::ClipRef* c = m ? m->clip(type, 0) : nullptr;
        if (!c || !c->clip || c->clip->fps <= 0.0f) return 0u;
        return static_cast<uint32_t>(std::ceil(static_cast<double>(c->clip->frame_count) * 30.0 / static_cast<double>(c->clip->fps)));
    });
    // S8 t11 (MOVEMENT §3.2): the Walk deploy clips (anim 0x19 / 0x1A, variant 0 [port]: retail draws a synced variant).
    movement_->set_deploy_clip_fn([this](objects::ObjectId id, int32_t type, uint32_t& frames, float& fps) {
        const anim::AnimWorld* w = hooks_ ? hooks_->world() : nullptr;
        // S8 t11 follow-up: the drawn variant once it plays (SimAnimation::deploy, drive_animation), else variant 0.
        const anim::SlotView sv = w ? w->slot(id) : anim::SlotView{};
        const anim::AnimModel* m = w ? w->model(id) : nullptr;
        const anim::ClipRef* c = sv.type == type && sv.clip ? sv.clip : (m ? m->clip(type, 0) : nullptr);
        if (!c || !c->clip || c->clip->fps <= 0.0f) return false;
        frames = c->clip->frame_count;
        fps = c->clip->fps;
        return true;
    });
#endif
    if (space_) {
        // S7 t07 (MOVEMENT §2.2): SPACE_OBSTACLE types without an XML radius are sized by their space model's collision
        // bound (the capture / construction pods), as in space_obstacles_sync.
        mv::SpaceObstacleHooks so_hooks;
        const BattleCatalog* so_cat = catalog_;
        so_hooks.model_half_extents = [so_cat](uint32_t t, float& hx, float& hy) { return so_cat->space_model_half_extents(t, hx, hy); };
        mv::space_from_map(movement_->space(), map_, &types, &so_hooks);
        // FIGHTER_FLYING §5: the fighter cell grids build from the playable box at the first tick with a fighter; a
        // map without one gets the fallback box above.
        if (!movement_->space().has_box()) movement_->space().set_box({mn[0], mn[1]}, {mx[0], mx[1]});
        // SPACE_AVOIDANCE: the fighter avoidance hook over this tick's tree members (avoid_snapshot).
        movement_->set_fighter_avoid_fn([this](mv::ObjectId, mv::F3 c, mv::F3 h, mv::F3 a, mv::F3 b, mv::F3& hit) {
            return mv::space_avoid(avoid_members_, c, h, a, b, hit);
        });
        fighter_formation_hooks();  // s78f t02
    } else {
        mv::PassabilityGrid grid = mv::grid_from_map(map_);
        // battle host: model bounds for the LAND_OBSTACLE footprints without Obstacle_Width / _Height and the
        // Fine_Tune_Occupied_Passability ray (MOVEMENT 1.3; S8 t09: the same hooks as land_obstacle_attach).
        const mv::MapStampHooks stamp_hooks = land_stamp_hooks(catalog_);
        mv::stamp_map_obstacles(grid, map_, types, &stamp_hooks, pf);  // OPT3 t11: footprints sampled in parallel
        movement_->set_grid(std::move(grid), pf);
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
        // cu8 t03: the AI difficulty adjustment (TACTICAL_COMBAT §3.6 step 12 / §4.3) and the faction's retreat damage
        // factor for this mode (§3.6 step 9).
        if (const galactic::DifficultyRules* d = galaxy_->ai_difficulty(static_cast<int32_t>(p))) {
            infos[p].ai_damage_multiplier = d->damage_multiplier;
            infos[p].faction_health = d->health_multiplier;
            infos[p].faction_shield = d->shield_multiplier;
        }
        const galactic::ConflictFactionInfo& fi = rules_->faction(galaxy_->players()[p].faction);
        infos[p].retreat_damage_mod = space_ ? fi.space_retreat_damage_mod : fi.land_retreat_damage_mod;
        // TACTICAL_COMBAT §2.2: the target scan skips players whose faction is Is_Neutral (player +0x68 -> faction +0x108).
        infos[p].active = !bt_owner_neutral(static_cast<int32_t>(p));
    }
    combat_ = std::make_unique<tc::CombatSystem>(tables_->combat, cc, std::move(infos));
    // S7 t30 (TACTICAL_COMBAT §2.10) [H]: the relation table (0 friendly, 1 enemy, 2 neutral) from the skirmish / campaign
    // rules (bt_relation). Every combat path reads "enemy" = 1 (auto-target, formation target, the human attack, projectile
    // hits, area damage, stun, turret drop), so a relation-2 player (Is_Neutral faction) is attacked by nobody and attacks
    // nobody.
    for (uint32_t p = 0; p < players; ++p)
        for (uint32_t q = 0; q < players; ++q)
            if (q != p)
                combat_->set_enemy(static_cast<int32_t>(p), static_cast<int32_t>(q),
                                   bt_relation(static_cast<int32_t>(p), static_cast<int32_t>(q)) == 1);
    combat_->reserve(256, 2048, 32, 256);
    attach_rocket_terrain();  // refixf t01
    combat_->set_trees(&mode.trees());  // S8 t06: between-tick detonations (also right after a load)
    if (!space_) {
        // S8 t01 (TACTICAL_GAPS 3.1 slot 43): LAND_OBSTACLE targets' aim walk over the cells their stamp occupies (map
        // placements' stamps move to the spawned objects in bt_map_load; S8 t02: objects spawned later are stamped by
        // land_obstacles_sync). The cell centre is always written (ObstacleCellFn contract). A stamp with no cells (water
        // rule / Fine_Tune / off the grid) answers Free: retail gates the walk on the LAND_OBSTACLE behaviour (obj +0x346),
        // not on the cell count, so the walk starts on an unoccupied cell, snaps to it and uses E = sqrt(200). NoFootprint
        // (half-extent rule) only for an object never stamped [port: a type without Obstacle_Width / _Height and without
        // model extents].
        const mv::MovementSystem* ms = movement_.get();
        combat_->set_obstacle_cell_query([ms](tc::ObjectId id, tactical::Vec3 p, tactical::Vec3& c) {
            const mv::PassabilityGrid& g = ms->grid();
            const int32_t cx = mv::world_to_cell(p.x), cy = mv::world_to_cell(p.y);
            const mv::V2 cc = mv::cell_center(cx, cy);
            c = tactical::Vec3{cc.x, cc.y, p.z};
            if (!g.stamped(id)) return tc::ObstacleCell::NoFootprint;
            return g.occupies(id, cx, cy) ? tc::ObstacleCell::Occupied : tc::ObstacleCell::Free;
        });
        // S8 t10 (LOB_SOLVER §5.1): a garrisoned shooter's host for the garrison gun bones (obj +0x2b0).
        combat_->set_garrison_host_query([this](tc::ObjectId id) -> tc::ObjectId { return garrison_holder_of(id); });
    }
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
    if (mode.has_model_source()) {
        // refixf t07 (TACTICAL_COMBAT §3.4, HOTSPOTS 0x2abcf0): per Query_Ray entry, the precise model test (SPACE_AVOIDANCE
        // §5, the object's committed pose: position, planar facing, Scale_Factor), else, with Collision_Box_Modifier > 1,
        // the start, end and midpoint of the segment against a sphere of radius max_extent x modifier. [port] max_extent =
        // the largest half extent of the object's §3.1 world box and the sphere is centred on the position (neither is
        // pinned in docs/re); the hit fraction is the first point inside (start 0, end 1, midpoint 0.5). Read-only: the
        // sweeps run on job workers (resolve_models() runs serially before them).
        const TacticalMode* tm = &mode;
        combat_->set_precise_hit([tm](const tc::PreciseQuery& q, tc::PreciseResult& r) {
            const objects::ObjectStore& st = tm->primary();
            const objects::ObjectHandle h = st.find(q.victim);
            if (!st.exists(h)) return tc::PreciseOutcome::NotModelled;
            mv::AvoidObstacle o;
            if (!tm->obstacle_of(h, o, false)) return tc::PreciseOutcome::NotModelled;
            o.collide = q.collide;
            const mv::F3 a{q.a.x, q.a.y, q.a.z}, b{q.b.x, q.b.y, q.b.z};
            mv::AvoidSegmentHit hit;
            if (mv::avoid_precise_segment(o, a, b, q.mask, q.shield_up ? 2u : 0u, hit)) {
                r.t = hit.t;
                r.mesh = hit.mesh >= 0 ? &o.model->meshes[static_cast<size_t>(hit.mesh)].name : nullptr;
                return tc::PreciseOutcome::Hit;
            }
            const float k = tm->collision_box_modifier(st.type(h));
            if (k > 1.0f) {
                const float rad = std::max(std::max(o.box_h.x, o.box_h.y), o.box_h.z) * k;
                const mv::F3 pts[3] = {a, b, {(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, (a.z + b.z) * 0.5f}};
                const float ts[3] = {0.0f, 1.0f, 0.5f};
                for (int32_t i = 0; i < 3; ++i) {
                    const float dx = pts[i].x - o.pos.x, dy = pts[i].y - o.pos.y, dz = pts[i].z - o.pos.z;
                    if (dmath::sqrt((dx * dx + dy * dy) + dz * dz) <= rad) {
                        r.t = ts[i];
                        r.mesh = nullptr;
                        return tc::PreciseOutcome::Hit;
                    }
                }
            }
            return tc::PreciseOutcome::Miss;
        });
    }
    if (space_) {  // S8 t17 (FIGHTER_FLYING §4.10 A): the scan cube R + Rr of fighters with a formation (kUnitScanQuery)
        const TacticalMode* tm = &mode;
        combat_->set_scan_range_query([this, tm](tc::ObjectId u, tc::ObjectId c, float range, float& rr) {
            return fighter_scan_range(tm->primary(), u, c, range, rr);
        });
    }
    if (!space_)  // s78n t20 (FIGHTER_FLYING §4.11 E, 0x40edd0): can_hit's land line-of-fire term 0x3b5ad0 (L1_S4_REMAINING §1.2)
        combat_->set_shield_sides_query([this](tactical::Vec3 a, tactical::Vec3 b, bool shield_damage) {
            return opposite_sides_of_shield(sv(a), sv(b), -1, true, shield_damage);
        });
    if (!space_ && movement_) {
        // s78n t25.3 (MOVEMENT §4.3.2 items 6 / 7, 0x40edd0 / 0x410e90; TACTICAL_COMBAT §2.8 B 4): the movement's fire
        // test - can the mover hit the target from p: slots 43 / 46 and the line of fire from p at the ground height
        // (CombatSystem::can_hit_from; a squad container through its first live member). Yes or NoTarget (nothing
        // tested) = it can fire. s78n t25.4: los_only (a guard block's trim, item 7: the movement tests the guard range)
        // answers the line of fire alone (CombatSystem::line_of_fire_from).
        movement_->set_fire_test([this](mv::ObjectId mover, mv::V2 p, mv::ObjectId target, bool los_only, mv::V2 g, float gz) {
            const objects::ObjectId unit = attack_lead(mover);
            if (unit == objects::kNoObjectId || !combat_->has_unit(unit)) return true;
            tactical::Vec3 from = combat_->can_hit_origin(unit);
            from.x = p.x;
            from.y = p.y;
            // bkt1 t06 (b0079, MOVEMENT §4.3.2 item 7 / 0x40f7c0): from p at the terrain height, or the water height for a
            // Water-capable class (the mover's movement mask has the Water bit) where the water lies above; to G at its
            // height (the movement's goal point); CombatSystem raises both by their Ranged_Target_Z_Adjust
            from.z = map_.terrain.sample_height(p.x, p.y);
            if ((movement_->mask_of(mover) & mv::kPassWater) != 0u && map_.terrain.header.water_type != terrain::WaterType::None)
                from.z = std::max(from.z, map_.terrain.header.water_height);
            const tactical::Vec3 to{g.x, g.y, gz};
            const tc::CanHit c = los_only ? combat_->line_of_fire_from(unit, from, target, &to) : combat_->can_hit_from(unit, from, target, &to);
            return c == tc::CanHit::Yes || c == tc::CanHit::NoTarget;
        });
        // s78n t26 review round 2 (MOVEMENT §4.3 correction [H]): the formation layout's rank key, Get attack range
        // 0x3749d0 (the value land_attack_order reads); a squad container through its first live member
        movement_->set_attack_range_query([this](mv::ObjectId id) {
            const objects::ObjectId unit = attack_lead(id);
            return unit != objects::kNoObjectId && combat_->has_unit(unit) ? combat_->attack_range(unit) : 0.0f;
        });
    }
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
    // cu8g t02 (CU8G_T02_ENV_PICK_RE §1 / §3 step 7): the environment pick, the map loader's last synced draw - after the
    // chunk-0x102 objects and their spawn-table draws, before every post-load step (skirmish base / start forces, the
    // campaign starbase / structures). Not saved: a loaded battle's read_part(State) takes the map's start index instead.
    env_index_ = pick_environment(map_.environments, rng);
    // cu8i t02 (CU8G_T02_ENV_PICK_RE §7 / §8 item 7): Load_Map's tail, after the pick: the deferred GOM post-load pass
    // (0x2b4440 -> 0x3952e0 -> 0x39bcb0) puts each movable object without a movement coordinator into a new one, which
    // creates a formation: one FormationClass ctor draw int(1, FPS) per object, in object order, whatever the owner: the
    // port's inert Is_Neutral map units count too (_MP_LAND_ENDOR: its one Land_Speeder, retail 1 draw). cu8j t02
    // (MOVEMENT §4.7.3): land and space alike, through the full adopt predicate (mv_adopt: the mode's locomotor kind).
    {
        const objects::ObjectStore& st = mode.primary();
        for (const objects::ObjectHandle h : st.all_objects()) (void)mv_adopt(mode, h, rng);
    }
    bt_pad_restore(mode);        // §6.5: persistent pad children
    bt_skirmish_base(mode);      // t50: the land skirmish's pre-built base
    l1_setup(mode);            // L1 systems: special weapons / hyperspace / engines (before the deploys)
    bt_place_forces(mode, rng, pf);  // §3.2: starbase, structures, defender / attacker start forces
    bt_landing_points(mode);     // §3.3: the attacker's map-owned reinforcement points
    mode.finish_load(pf);
    space_obstacles_sync(mode);  // refixf t02: the starbase / structures as space static entries (MOVEMENT §2.2)
    land_obstacles_sync(mode, pf);   // S8 t02: LAND_OBSTACLE objects spawned during the setup (MOVEMENT §1.3)
    bt_shield_stamps(mode);  // battle host: base-shield secondary occupants (land)
    fog_after_deploy(mode);  // fog of war: dense mask, battle scale (owner: fog agent)
    vm_setup(mode);          // story t06: victory monitor records (§2.5) and the start's victory-relevant objects
    // OPT3 t09: the setup's tables are complete. q6 t12: the build is kept (serial preload from now on) so a type first
    // spawned by a game-object script can join the live tables (script_add_types); nothing else extends them after setup.
    tables_build_->pf = serial_for;
}

// S8 t17 (FIGHTER_FLYING §4.10 B): each combat unit's squadron for the shared team target. Derived state (not saved):
// refreshed at the tick start and after a load, so a command applied before the first tick after a load sees the same
// groups as an uninterrupted run.
void TacticalBattle::share_groups_sync() {
    if (!space_ || !movement_ || !combat_) return;
    for (const Unit& u : units_)
        if (combat_->has_unit(u.id))
            combat_->set_share_group(u.id, u.container < containers_.size() && movement_->fighter_squad(containers_[u.container].id)
                                               ? containers_[u.container].id
                                               : tc::kNoObject);
}

void TacticalBattle::attach_rocket_terrain() {
    // refixf t01 (S9_DEATH_CLONE_SHIELDS.md §2.4.5 mode 1, §2.4.6): land spline rockets follow the heightmap and stay
    // inside its extent box (0x1320d0: centre (W c / 2, H c / 2), half sizes W c / 2, H c / 2). [M] W c, H c are taken as
    // the port grid's covered extent ((width - 1) x kVertexSpacing, origin 0: the frame of Terrain::sample_height).
    // Space battles and maps without a heightmap: flat ground at z 0, no box. Not saved: re-attached after a load.
    if (!combat_) return;
    tc::RocketTerrain rt;
    const terrain::Terrain& tr = map_.terrain;
    if (!space_ && tr.present && tr.width >= 2 && tr.height >= 2) {
        const terrain::Terrain* tp = &tr;
        rt.height = [tp](float x, float y) { return tp->sample_height(x, y); };
        const float w = static_cast<float>(tr.width - 1) * terrain::kVertexSpacing;
        const float h = static_cast<float>(tr.height - 1) * terrain::kVertexSpacing;
        rt.has_box = true;
        rt.cx = 0.5f * w;
        rt.cy = 0.5f * h;
        rt.hx = 0.5f * w;
        rt.hy = 0.5f * h;
        // S8 t06 (LOB_SOLVER.md §6.6): the grid for the projectile ground test (cell triangles + water plane), in the
        // same frame (origin 0, kVertexSpacing cells, int16 heights x kHeightScale).
        rt.grid.cell = terrain::kVertexSpacing;
        rt.grid.width = tr.width;
        rt.grid.height = tr.height;
        rt.grid.heights = tr.heights;
        rt.grid.height_scale = terrain::kHeightScale;
        rt.grid.water_height = tr.header.water_height;
        rt.grid.water_type = static_cast<uint32_t>(tr.header.water_type);
    }
    combat_->set_rocket_terrain(std::move(rt));
}

Vec3 TacticalBattle::slot_position(uint8_t side, uint32_t mask) {
    const uint32_t k = placed_[side]++;
    const float spacing = space_ ? cfg_.space_spacing : cfg_.land_spacing;
    const float row = static_cast<float>(k / 6u), col = static_cast<float>(k % 6u) - 2.5f;
    const Vec3 f = facing_[side];
    const Vec3 right{-f.y, f.x, 0.0f};
    const Vec3 p{anchor_[side].x - f.x * row * spacing + right.x * col * spacing,
                 anchor_[side].y - f.y * row * spacing + right.y * col * spacing, 0.0f};
    return snap_passable(p, mask, true);
}

Vec3 TacticalBattle::snap_passable(Vec3 p, uint32_t mask, bool always) const {
    float mn[2], mx[2];
    if (map_.playable_box(mn, mx)) {
        p.x = std::clamp(p.x, mn[0], mx[0]);
        p.y = std::clamp(p.y, mn[1], mx[1]);
    }
    if (!space_ && movement_) {
        const mv::PassabilityGrid& g = movement_->grid();
        if (!always && g.passable_world({p.x, p.y}, mask)) {
            p.z = map_.terrain.sample_height(p.x, p.y);
            return p;
        }
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

void TacticalBattle::deploy(TacticalMode& mode, uint32_t pi, SimRng& rng, uint32_t member_first, uint32_t member_count) {
    Participant& P = parts_[pi];
    const bool range = member_first != 0 || member_count != 0xFFFFFFFFu;  // a1i t04: one landing unload step
    if (P.deployed && !range) return;
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
    const size_t mk_end = std::min(members_types.size(), static_cast<size_t>(member_first) + member_count);
    for (size_t mk = member_first; mk < mk_end; ++mk) {
        const uint32_t t = members_types[mk];
        mv::UnitDesc md = mv::unit_desc_from_type(types, t);
        md.locomotor = mv::locomotor_kind_from_type(types, t, space_ ? mv::MoveMode::Space : mv::MoveMode::Land);  // REMAINING_GAPS §4.3-4.7
        Vec3 pos;
        Vec3 face = facing_[side];
        // s78i t02 (TACTICAL_COMBAT §3.1.1 "other spawns"): a reserve drop (0x2b5fe0 land / 0x2bf2d0 space) gives every
        // object it creates the owner's entry-facing record (mode +0x288; yaw 0 when unset). A landing object's company
        // takes the per-member unload facing of drop_face_ (s78j t01).
        if (P.reserve && !range && !deploy_unload_) {
            const Vec3 df = side < 2 ? drop_facing_[side] : Vec3{};  // as spawn_landing: the participant's side record
            face = df.x == 0.0f && df.y == 0.0f ? facing_of_yaw(0.0f) : df;
        }
        if (mk < drop_face_.size()) face = drop_face_[mk];
        const bool lead_sw = space_ && bt_have_ds_ && !P.reserve && bt_lead_ == objects::kNoObjectId && bt_is_super_weapon(t);
        if (spot < P.spots.size()) {  // §3.2: the marker chosen for the starbase / structure / step-3 type
            pos = P.spots[spot].first;
            face = P.spots[spot].second;
            ++spot;
        } else if (P.starbase) {
            pos = {anchor_[side].x - facing_[side].x * 600.0f, anchor_[side].y - facing_[side].y * 600.0f, 0.0f};
        } else if (lead_sw) {
            pos = bt_ds_pos_;  // battle host: the spawned super-weapon at DEATH_STAR_POSITION (TACTICAL_GAPS §4.8) [M]
        } else if (!drop_pos_.empty()) {  // a1g t04 follow-up: order_reinforce's land drop layout (member order)
            // a1h t06: the slots passed land_drop_valid, so they are used as they are (no clamp, no nearest-cell move).
            const size_t k = mk;
            if (k < drop_pos_.size()) {
                pos = drop_pos_[k];
                if (!space_) pos.z = map_.terrain.sample_height(pos.x, pos.y);
            } else {
                pos = slot_position(side, movement_->classes().mask(md.movement_class));
            }
        } else {
            pos = slot_position(side, space_ ? 0u : movement_->classes().mask(md.movement_class));
        }
        objects::SpawnParams sp;
        sp.type = t;
        sp.owner = P.owner;
        sp.position = pos;
        sp.facing = face;
        sp.parent_mode_id = P.built ? objects::kNoObjectId : P.galactic_id;  // q6 t19: a pool entry from tactical production
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
            mv_type_extents(md, t);  // s78n t20
            movement_->add_unit(md);
        }
        if (side < 2 && u.movable && !P.reserve && !P.built && !stats_.start_placed[side]) {  // s78n t20 (stats only)
            stats_.start_placed[side] = true;
            stats_.first_start[side] = pos;
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
            // cu8l t01 (CU8K_T02_T06_RE §1.2, §4 rules 1 / 4): a land team container's LandTeamContainer locomotor
            // registers it at creation (0x5abac0: its own coordinator, Move_To its own position, no formation): no draw
            // now; the coordinator composes at the next step 10 (one int(1, FPS), coordinator creation order) unless an
            // order takes the members first. s78n t03 (TACTICAL_COMBAT §5.4.4 item 2): the container is a movement unit
            // walking its own path, its members steering to their slots, so its own coordinator holds it. [port] A squad
            // with a member that cannot steer (mv_squad_add false) keeps the members standing in for the container.
            if (!space_ && movement_ && mv_self_registers(team_obj)) {
                if (mv_squad_add(mode, containers_.size() - 1, team_obj)) {
                    const mv::ObjectId self[] = {ct.id};
                    (void)movement_->hold_from_birth(self);
                } else {
                    (void)movement_->hold_from_birth(groups.back());
                }
            }
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
        // s78f t07 (TACTICAL_COMBAT §3.1.1 "Team targeting copies"): a land team member joins combat with its team (the
        // container's object id); the chunk's first member (list order, the lowest id, as slot 31's 0x4ec820) is the
        // team_leader standing in for the container, which is no combat unit here. This runs the TeamTargeting copy,
        // team scoring and the Take_Damage redirect / share and convert team rules. [port] Space squadrons keep their
        // FIGHTER_FLYING §4.10 B shared target and are not registered (no space team targeting service).
        if (!space_ && u.container != kNoContainer) {
            cs.team = static_cast<uint32_t>(containers_[u.container].id);
            cs.team_leader = i == first || units_[i - 1].container != u.container;
        }
        // s78g t06 (TACTICAL_COMBAT §3.6 step 2, no mode gate): a space squadron member still counts in its squadron for
        // the Take_Damage redirect / share rules (vanilla Luke_XWing in Red Squadron, TIE_Prototype in Vader's squadron).
        if (space_ && u.container != kNoContainer) cs.damage_team = static_cast<uint32_t>(containers_[u.container].id);
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
    // cu8j t02 (MOVEMENT §4.7.3): the team helper 0x2a5a60 adopts each chunk's container (0x39bcb0, one FormationClass
    // ctor draw when it qualifies) right after creating it, before the next chunk: the adopt follows each container's
    // creation draws. The skirmish company placer runs the helper only for a Create_Team first member (the port's space
    // containers of other companies are not adopted).
    // cu8l t01 (CU8K_T02_T06_RE §4 rule 4): it draws only in space (TeamLocomotor squadrons); a land container already
    // sits in the coordinator its locomotor made (above), so the predicate's check 5 fails: no draw.
    // cu8l t02 (CU8K_T02_T06_RE §3 row 0x6a1f90, rule 8): Lua Spawn_Unit's company path runs the same placer 0x29e960 and
    // helper, so a script-spawned space container adopts at once too (with the placer's Create_Team condition [M]).
    const bool helper = (cfg_.skirmish || deploy_script_) ? units_.size() > first && units_[first].type < types.size() &&
                                                                types.get_bool(units_[first].type, "Create_Team")
                                                          : true;
    for (size_t k = 0; k < groups.size(); ++k) {  // OI abilities: + the container's SAP-init draws
        abilities_->add_container(containers_[c0 + k].id, desc_of(team_beh), P.owner, groups[k], frame_, &rng);
        if (space_ && helper) (void)mv_adopt(mode, containers_[c0 + k].h, rng);
    }
    // cu8l t01 (CU8K_T02_T06_RE §1.5, §4 rules 1 / 3) [port interim rule, untraced: the space skirmish trace is still
    // missing]: a ship whose locomotor is SimpleSpace registers itself at creation like a land container (0x625090): no
    // draw now, one space composition (0x5d58f0) at the next step 10, in unit order. [port] Not for a SimpleSpace team
    // member [L: whether its own registration survives joining the team is not read]. cu8l t03: the campaign start forces
    // too - the fleet placement then teleports (the pending composition empties: no draw) and re-adopts every placed
    // object, so only an unplaced ship composes; and the campaign spawn's adopt below finds it in that coordinator (no
    // draw: retail traced no 0x2a1bcd adopt for the Kashyyyk ships).
    if (space_ && movement_)
        for (size_t i = first; i < units_.size(); ++i)
            if (units_[i].container == kNoContainer && units_[i].movable && mv_self_registers(units_[i].type)) {
                const objects::ObjectId one[] = {units_[i].id};
                (void)movement_->hold_from_birth(one);
            }
    // cu8j t02: the campaign spawn 0x2a00b0 (start forces of a galactic battle) then adopts each member not in a team, one
    // draw each (land vehicles, heroes, ...). Skirmish start forces and reinforcements do not.
    if (!cfg_.skirmish && !P.reserve && !deploy_script_ && !range)
        for (size_t i = first; i < units_.size(); ++i)
            if (units_[i].container == kNoContainer) (void)mv_adopt(mode, units_[i].h, rng);
}

// refixf t02 (MOVEMENT §2.2 [H]): SpaceObstacleBehaviorClass inserts its object into the space occupancy as a static entry on
// attach and removes it on detach. Map placements enter through mv::space_from_map (ids mv::map_stamp_id(placement)); S7 t13:
// bt_map_load moves each entry to the object spawned for it (SpaceOccupancy::rekey_static; a Map_Load_Spawn_Table pick is
// re-sized by the type spawned, a placeholder that spawns nothing loses its entry), so a map-adopted unit's entry leaves with
// it. Battle units whose type has SPACE_OBSTACLE (the starbase, space structures, upgraded / built stations) enter here under
// their object id, so the destination search (§2.4) and the planner keep ships off them. Before, the starbase was no obstacle:
// an Attack_Move / Move_To on it (one order per task-force unit) sent each ship to its exact centre, the ships stacked on one
// point and Project_By_Unit_Range gave nil (foc-station seeds 1 and 4). [port] attach = the first sync (end of setup, end of
// each tick) with the unit live, detach = the first sync after it died or retreated; footprint as space_from_map (radius) at
// the attach position + Space_Obstacle_Offset rotated by the facing [M: rotation sense]. S7 t07: a type without an XML radius
// gets its space model's collision-bound extent (MOVEMENT §2.2); the model's collide switch is not consulted (SPACE_AVOIDANCE
// §5.5).
void TacticalBattle::space_obstacles_sync(const TacticalMode& mode) {
    if (!space_ || !movement_) return;
    mv::SpaceOccupancy& occ = movement_->space();
    const objects::ObjectStore& st = mode.primary();
    // S8 t09: an entry re-keyed by bt_map_load to an object that is not a battle unit (an asteroid / prop with
    // SPACE_OBSTACLE) leaves with that object, as land_obstacles_sync does for stamps (ascending id).
    for (const mv::ObjectId id : occ.static_ids())
        if (id > 0 && !st.alive(st.find(id))) occ.remove_static(id);
    for (const Unit& u : units_) {
        if (!u.alive || u.retreated || !st.alive(u.h)) {
            occ.remove_static(u.id);
            continue;
        }
        if (!occ.has_static(u.id)) space_obstacle_attach(mode, u.h);
    }
}

bool TacticalBattle::space_obstacle_attach(const TacticalMode& mode, objects::ObjectHandle h) {
    const objects::ObjectStore& st = mode.primary();
    if (!space_ || !movement_ || !st.alive(h)) return false;
    const objects::GameObjectTypes& types = catalog_->types();
    mv::SpaceObstacleHooks so_hooks;
    const BattleCatalog* so_cat = catalog_;
    so_hooks.model_half_extents = [so_cat](uint32_t t, float& hx, float& hy) { return so_cat->space_model_half_extents(t, hx, hy); };
    const auto t = static_cast<uint32_t>(st.type(h));
    float r = 0.0f;
    uint32_t bits = 0;
    if (!mv::space_obstacle_footprint(types, t, r, bits, &so_hooks)) return false;
    const Vec3 p = st.position(h), f = st.facing(h);
    const float len = dmath::sqrt(f.x * f.x + f.y * f.y);
    const mv::V2 fu{len > 0.0f ? f.x / len : 1.0f, len > 0.0f ? f.y / len : 0.0f};
    movement_->space().add_static(st.id(h), mv::space_obstacle_center(types, t, {p.x, p.y}, fu), r, bits);
    return true;
}

// S8 t02 (MOVEMENT §1.3 [H]): LandObstacleBehaviorClass stamps its object's footprint into the passability grid on attach and
// unstamps it on detach. Map placements are stamped at setup (mv::stamp_map_obstacles) and their stamps move to the objects
// spawned for them in bt_map_load; everything spawned later (the starbase / structures / persisted pad children at setup,
// built pads, reinforcement structures, script spawns, death clones whose type has LAND_OBSTACLE) is stamped here under its
// object id, and every stamped object that left the store (died, sold, removed) is unstamped. [port] attach / detach = this
// sync: end of setup (also after the structures deploy, before the start forces: their spawn ring search sees the
// structures), end of each tick (the next tick's pathing and targeting see the change). Gone first (ascending id), then new
// (the store's LAND_OBSTACLE attach order); footprint as stamp_map_obstacles at the object's position, yaw (facing = Rz(yaw)
// (0, -1), as map placements) and height, with the same hooks (land_stamp_hooks: Obstacle_Proxy_Type and the
// Fine_Tune_Occupied_Passability ray apply to every stamp, S8 t09). A death clone is in the LAND_OBSTACLE table iff it kept
// the behaviour through TacticalMode::spawn_death_clone's strip (Death_Clone_Is_Obstacle, MOVEMENT §1.3 [H]). Placement
// stamps never stay under a placement id here: bt_map_load hands each to its object or unstamps it (skipped company,
// no-spawn placeholder).
void TacticalBattle::land_obstacles_sync(const TacticalMode& mode, const ParallelFor& pf) {
    if (space_ || !movement_) return;
    const objects::ObjectStore& st = mode.primary();
    mv::PassabilityGrid& grid = movement_->grid();
    for (const mv::ObjectId id : grid.stamped_ids())
        if (id > 0 && !st.alive(st.find(id))) grid.unstamp(id);
    for (const objects::ObjectHandle h : st.behavior_table(objects::beh::LandObstacle).insertion)
        if (st.alive(h) && !grid.stamped(st.id(h))) land_obstacle_attach(mode, h, pf);
}

bool TacticalBattle::land_obstacle_attach(const TacticalMode& mode, objects::ObjectHandle h, const ParallelFor& pf) {
    const objects::ObjectStore& st = mode.primary();
    if (space_ || !movement_ || !st.alive(h)) return false;
    const objects::GameObjectTypes& types = catalog_->types();
    const auto t = static_cast<uint32_t>(st.type(h));
    if (t >= types.size()) return false;
    if (land_obstacle_none_.size() != types.size()) land_obstacle_none_.assign(types.size(), 0);
    if (land_obstacle_none_[t]) return false;
    const mv::MapStampHooks hooks = land_stamp_hooks(catalog_);
    const Vec3 p = st.position(h), f = st.facing(h);
    // yaw of the facing Rz(yaw) (0, -1) = (sin yaw, -cos yaw): the map placements' convention (bt_map_load yaw_facing)
    const float yaw = (f.x == 0.0f && f.y == 0.0f) ? 0.0f : dmath::atan2(f.x, -f.y) * (180.0f / dmath::kPiF);
    mv::ObstacleFootprint fp;
    if (!mv::land_obstacle_footprint(types, t, {p.x, p.y}, yaw, p.z, &hooks, fp)) {
        land_obstacle_none_[t] = 1;  // const type data (sizes, proxy, ray model): never retried
        return false;
    }
    // S8 t13 (MOVEMENT §1.3 "proxy bits"): the bits come from the object's own live behaviours (0x397350), never the proxy.
    fp.bits = mv::land_obstacle_bits(static_cast<uint32_t>(types.get_mask(t, "OverridePassability")), true,
                                     st.has_behavior(h, kWallBehavior));
    // OPT3 t12: the Fine_Tune rays (a whole-mesh test per 10-unit sample; a skirmish base structure has ~400 samples on
    // ~14k triangles) run through pf. The hook is const catalog data (as stamp_map_obstacles).
    movement_->grid().stamp(st.id(h), fp, pf);
    return true;
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
    if (arrival_held()) return;  // story t12: reinforcements wait in hyperspace
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
        if (!space_ && !standin_owner(store.owner(u.h))) continue;  // bkt1 t07 (b0084): land third parties idle
        const Vec3 up = store.position(u.h);
        const Unit* best = nullptr;
        float best_d2 = 0.0f;
        for (const Unit& e : units_) {
            if (!e.alive || e.retreated || e.side == u.side) continue;
            // s78n t25.3 [port]: on land the pick is an enemy of the unit's owner (the auto-attack push 0x41fd30 names what
            // the targeting scan picked, never a neutral object)
            if (!space_ && combat_ && !combat_->is_enemy(store.owner(u.h), store.owner(e.h))) continue;
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
        // in range: hold, combat fires. s78n t20 stage A [port] (FIGHTER_FLYING §4.11 A, arrived re-check only): an arrived
        // unit that cannot hit its nearest enemy from where it stands approaches again (no land stop here).
        if (dist <= 0.8f * range && !standin_recheck(u, best->id)) continue;
        if (movement_locked(u.id)) continue;  // OI follow-up t03: a locked unit keeps its order (§28.9)
        if (!(dist > 0.0f)) continue;  // s78n t20 review: on the enemy's position there is no approach line
        const Vec3 ep = store.position(best->h);
        if (!space_) {  // s78n t25.3 [port] (MOVEMENT §4.3.2 item 2): the auto-attack block push 0x41fd30, one unit
            const objects::ObjectId one[] = {u.id};
            (void)land_attack_order(mode, one, best->id, ep, 0, false, rng);
            ++stats_.orders;
            continue;
        }
        // s78n t20 interim (space): the approach aims with the unit's own range (no 100 floor)
        const float k = (dist - 0.7f * (u.range > 0.0f ? u.range : range)) / dist;
        mv::MoveOrder o;
        o.units = {u.id};
        o.destination = {up.x + (ep.x - up.x) * k, up.y + (ep.y - up.y) * k};
        const tactical::Aabb& b = mode.bounds();
        o.destination.x = std::clamp(o.destination.x, b.min.x, b.max.x);
        o.destination.y = std::clamp(o.destination.y, b.min.y, b.max.y);
        o.skip_destination_check = true;  // s78n t20 fix (MOVEMENT 4.3.1 item 2): an attack approach, as attack_approach
        movement_->move(o, rng, frame_);
        fighter_order(o.units, 0, Vec3{o.destination.x, o.destination.y, 0.0f});  // S7 t22
        ++stats_.orders;
    }
}

// ---- OI follow-up t03: the movement lock (A1_READINGS §28.9) -------------------------------------------------------

void TacticalBattle::set_script_movement_lock(const void* key, std::function<bool(objects::ObjectId)> fn) {
    std::erase_if(script_locks_, [key](const auto& e) { return e.first == key; });
    if (fn) script_locks_.emplace_back(key, std::move(fn));
}

bool TacticalBattle::script_movement_locked(objects::ObjectId id) const {
    for (const auto& [key, f] : script_locks_)
        if (f(id)) return true;
    return false;
}

bool TacticalBattle::movement_locked(objects::ObjectId id) const {
    if (abilities_ && abilities_->immobilize_count(id) > 0) return true;
    // s78n t06 (HERO_CLASH §3.2): a hero in a clash has its movement orders locked (0x3a8e50(h, 1) with +0x3b4).
    if (hero_clash_locked(id)) return true;
    if (!landings_.empty() && landing_locked(id)) return true;  // a1i t04: the unload exit move (§35.2)
    return script_movement_locked(id);
}

bool TacticalBattle::hero_clash_locked(objects::ObjectId id) const {
    // s78n t06 review: the clash's share of the lock - +0x3b4 set and not released by a Lua TEAM Unlock (A1_READINGS §28.9
    // item 4, kUnitHeroClashUnlocked).
    if (!combat_ || !combat_->has_unit(id)) return false;
    const uint32_t f = combat_->flags(id);
    return (f & tc::kUnitHeroClash) && !(f & tc::kUnitHeroClashUnlocked);
}

bool TacticalBattle::release_hero_clash_lock(objects::ObjectId id) {
    if (!hero_clash_locked(id)) return false;
    combat_->set_flag(id, tc::kUnitHeroClashUnlocked, true);
    return true;
}

void TacticalBattle::drop_locked(std::vector<objects::ObjectId>& ids) const {
    ids.erase(std::remove_if(ids.begin(), ids.end(), [this](objects::ObjectId id) { return movement_locked(id); }), ids.end());
}

// ---- integration fixes: human order API (rules of BattleAi::execute_order) -----------------------------------------

bool TacticalBattle::standin_owner(int32_t player) const {
    if (req_.info.attacker < 0 && req_.info.defender < 0) return player >= 0;  // a battle built without sides: everyone
    return player >= 0 && (player == req_.info.attacker || player == req_.info.defender);
}

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
                               float facing_deg, bool attack_move, uint8_t flags, bool switch_locomotor,
                               objects::ObjectId goal_object, bool no_destination_check) {
    // s78n t26.1 (MOVEMENT §3.2.1 item 2, the group move 0x2a7b80): a deployed manual deployer is skipped entirely - its
    // orders (garrison enter, attack, team target) are kept and it is not moved
    std::vector<objects::ObjectId> admitted;
    if (!space_ && movement_) {
        for (const objects::ObjectId id : ids)
            if (!movement_->move_skips(id)) admitted.push_back(id);
        if (admitted.size() != ids.size()) ids = admitted;
        if (ids.empty()) return;
    }
    s4_cancel(ids);  // S4: a move replaces a pending garrison enter
    team_attack_drop(ids);  // q6: an ordered move clears the team target (FIGHTER_FLYING §4.10 B writer 3)
    const tactical::Aabb& bb = mode.bounds();
    mv::MoveOrder o;
    o.units.assign(ids.begin(), ids.end());
    // P1_DOUBLE_CLICK_MOVE.md §3.2: the event flags, unchanged; s78f t10: plus the attack-move bit, as the queued path and
    // the host's formation record (fighter_order below) carry it, so the fighter leash reads the same block.
    o.flags = static_cast<uint8_t>(flags | (attack_move ? mv::kMoveFlagAttackMove : 0));
    o.switch_locomotor = switch_locomotor;  // S7 t10: MoveToPosition's 1-bit (MOVEMENT §6.2)
    o.destination = {std::clamp(x, bb.min.x, bb.max.x), std::clamp(y, bb.min.y, bb.max.y)};
    if (std::isfinite(facing_deg)) o.facing_deg = facing_deg;
    o.goal_object = goal_object == objects::kNoObjectId ? -1 : goal_object;  // S7 t37 (movement: -1 = none)
    // s78n t20 (MOVEMENT §4.3.1 item 2): an object goal goes to the attack-arc slot test, not to the destination check
    o.skip_destination_check = no_destination_check || goal_object != objects::kNoObjectId;
    movement_->move(o, rng, frame_);
    for (const objects::ObjectId id : o.units) {
        if (!attack_move) combat_->clear_order(id);
        // S8 t17 (FIGHTER_FLYING §4.10 B writer 3): an ordered move (0x39f800) clears the squadron's team target - every
        // member's Set_Target(null) and its hard points'.
        if (const Unit* mu = attack_move || !space_ ? nullptr : unit_by_id(id);
            mu && mu->container < containers_.size() && movement_->fighter_squad(containers_[mu->container].id))
            combat_->clear_targets(id);
        combat_->set_attack_goal(id, tc::kNoObject);  // refixf t06: a position block has no attack goal (§2.8)
    }
    // S7 t22: the squadrons' formation (block flags: 0x01 attack-move, 0x02 guard-move; P1_DOUBLE_CLICK §3.3)
    fighter_order(o.units, static_cast<uint8_t>((attack_move ? mv::kMoveFlagAttackMove : 0) | flags), Vec3{o.destination.x, o.destination.y, 0.0f});
}

// Attack rule of BattleAi::execute_order: order_attack, then (space) a move into 70 % of max(range, 100) beyond 80 %, or
// (land, s78n t25.3) one attack-block order per resolved target (land_attack_order).
uint32_t TacticalBattle::exec_attack(TacticalMode& mode, std::span<const objects::ObjectId> ids, objects::ObjectId target, int32_t hp,
                                     SimRng& rng) {
    const objects::ObjectStore& st = mode.primary();
    const Unit* t = unit_by_id(target);
    const Container* ct = t ? nullptr : team_target(target);  // q6: a TEAM container (TeamAttack, tactical_battle.hpp)
    if (!t && !ct) return 0;
    std::vector<TeamPick> picks;
    uint32_t n = 0;
    // s78n t25.3 (MOVEMENT §4.3.2 item 2): on land the attackers of each resolved target (a TEAM pick per attacker; groups
    // in order of first appearance) form one movement order with the attack block
    std::vector<std::pair<objects::ObjectId, std::vector<objects::ObjectId>>> groups;
    for (const objects::ObjectId id : ids) {
        const Unit* u = unit_by_id(id);
        if (!u || u->id == target || !combat_->has_unit(u->id)) continue;
        const Unit* tu = t;
        if (ct) {
            const objects::ObjectId m = team_pick(mode, *ct, st.position(u->h), combat_->share_group(u->id), picks);
            tu = m != objects::kNoObjectId ? unit_by_id(m) : nullptr;
            if (!tu) continue;
        }
        const Vec3 tp = st.position(tu->h);
        combat_->order_attack(u->id, tu->id, ct ? -1 : hp);
        if (ct) team_attack_set(u->id, ct->id, tu->id, false);
        const objects::ObjectId block = ct && movement_->fighter_squad(ct->id) ? ct->id : tu->id;
        if (!movement_locked(u->id))  // S7 t22: an object block (flags 0); a locked unit keeps its formation (as Move_To)
            fighter_order(std::span<const objects::ObjectId>(&u->id, 1), 0, tp, block);
        ++n;
        if (space_) {
            attack_approach(mode, *u, tp, rng, block, tu->id);
            continue;
        }
        auto g = std::find_if(groups.begin(), groups.end(), [&](const auto& x) { return x.first == tu->id; });
        if (g == groups.end()) g = groups.insert(groups.end(), {tu->id, {}});
        g->second.push_back(u->id);
        attack_block_set(u->id, ct ? ct->id : tu->id, 0, tp);  // s78n t20 stage A
    }
    for (const auto& [tid, us] : groups)
        (void)land_attack_order(mode, us, tid, st.position(unit_by_id(tid)->h), 0, false, rng);
    return n;
}

void TacticalBattle::attack_approach(TacticalMode& mode, const Unit& u, Vec3 tp, SimRng& rng, objects::ObjectId goal,
                                     objects::ObjectId target, bool replan) {
    if (!u.movable || movement_locked(u.id)) return;  // a locked unit fires from where it stands (§28.9 [M])
    if (!space_ && target != objects::kNoObjectId && combat_) {  // s78n t25.3 (MOVEMENT §4.3.2): the attack block
        const objects::ObjectId one[] = {u.id};
        (void)land_attack_order(mode, one, target, tp, 0, replan, rng);
        return;
    }
    const tactical::Aabb& bb = mode.bounds();
    const Vec3 up = mode.primary().position(u.h);
    const float dx = tp.x - up.x, dy = tp.y - up.y, d = dmath::sqrt(dx * dx + dy * dy);
    const float range = std::max(u.range, 100.0f);
    // s78f review (FIGHTER_FLYING §4.11 B 1 / B 3 "new group move"): Move_To(object) gives a fighter a fresh formation
    // whose only block is the object block (an earlier stack, e.g. a carrier guard, is gone): the move's block names the
    // target (`goal`); in range, nobody moves and the formation is still replaced (fighter_new_order).
    const bool fighter = goal != objects::kNoObjectId && space_ && movement_->has_unit(u.id) &&
                         movement_->view(u.id).locomotor == mv::LocomotorKind::Fighter;
    if (d > 0.8f * range) {
        const float k = (d - 0.7f * range) / d;
        mv::MoveOrder o;
        o.units = {u.id};
        o.destination = {std::clamp(up.x + dx * k, bb.min.x, bb.max.x), std::clamp(up.y + dy * k, bb.min.y, bb.max.y)};
        if (fighter) o.goal_object = goal;
        // s78n t20 (MOVEMENT §4.3.1 item 2): the approach stands for an object-goal block - no destination check (space;
        // s78n t25.3: land attack approaches are the attack block above)
        o.skip_destination_check = true;
        movement_->move(o, rng, frame_);
    } else if (fighter) {
        mv::MoveOrder o;
        o.units = {u.id};
        o.destination = {std::clamp(tp.x, bb.min.x, bb.max.x), std::clamp(tp.y, bb.min.y, bb.max.y)};
        o.goal_object = goal;
        movement_->fighter_new_order(o, frame_);
    }
}

// ---- q6: attack on a TEAM container (tactical_battle.hpp TeamAttack) -------------------------------------------------

const TacticalBattle::Container* TacticalBattle::team_target(objects::ObjectId id) const {
    if (id == objects::kNoObjectId) return nullptr;
    for (const Container& c : containers_)
        if (c.id == id) return c.team && !c.removed && c.alive > 0 ? &c : nullptr;
    return nullptr;
}

objects::ObjectId TacticalBattle::team_pick(const TacticalMode& mode, const Container& c, Vec3 from, objects::ObjectId group,
                                            std::vector<TeamPick>& picks) const {
    if (group != objects::kNoObjectId)
        for (const TeamPick& p : picks)
            if (p.group == group && p.container == c.id) return p.member;
    const objects::ObjectStore& st = mode.primary();
    const auto ci = static_cast<uint32_t>(&c - containers_.data());
    objects::ObjectId best = objects::kNoObjectId;
    float bd = 0.0f;
    for (const Unit& m : units_) {  // ascending id: the first strict minimum keeps the lower id on a tie [port]
        if (m.container != ci || !m.alive || m.retreated || !combat_->has_unit(m.id) || !combat_->alive(m.id)) continue;
        if (combat_->flags(m.id) & (tc::kUnitGarrisoned | tc::kUnitSkipTargeting)) continue;  // 0x5680b0 test 2 / untargetable
        const Vec3 p = st.position(m.h);
        const float dx = p.x - from.x, dy = p.y - from.y, d = dx * dx + dy * dy;
        if (best == objects::kNoObjectId || d < bd) {
            best = m.id;
            bd = d;
        }
    }
    if (group != objects::kNoObjectId && best != objects::kNoObjectId) picks.push_back({group, c.id, best});
    return best;
}

void TacticalBattle::team_attack_set(objects::ObjectId attacker, objects::ObjectId container, objects::ObjectId member, bool goal) {
    auto it = std::lower_bound(team_attacks_.begin(), team_attacks_.end(), attacker,
                               [](const TeamAttack& r, objects::ObjectId v) { return r.attacker < v; });
    if (it == team_attacks_.end() || it->attacker != attacker) it = team_attacks_.insert(it, TeamAttack{});
    *it = TeamAttack{attacker, container, member, goal};
}

void TacticalBattle::team_attack_drop(std::span<const objects::ObjectId> ids) {
    attack_block_drop(ids);  // s78n t20 stage A: the same writers end the land attack block
    if (team_attacks_.empty()) return;
    for (const objects::ObjectId id : ids) {
        const auto it = std::lower_bound(team_attacks_.begin(), team_attacks_.end(), id,
                                         [](const TeamAttack& r, objects::ObjectId v) { return r.attacker < v; });
        if (it != team_attacks_.end() && it->attacker == id) team_attacks_.erase(it);
    }
}

// Serial, after this tick's combat events (deaths, container removals) and before the order queues advance. Per record in
// attacker order: the order ends when the container is gone or no longer hostile, or when combat no longer holds a member of
// it (the order was dropped: fog, stealth, an owner change; or replaced). Otherwise the attacker's combat target becomes the
// current pick; a pick after the old member died also re-runs the order's approach move (and the squadron goal when the
// goal was the member) [port: the original's attacker follows the container object block; the port moves once per pick].
void TacticalBattle::team_attack_service(TacticalMode& mode, SimRng& rng) {
    if (team_attacks_.empty() || !combat_) return;
    const objects::ObjectStore& st = mode.primary();
    std::vector<TeamPick> picks;
    const auto live_unit = [this](const Unit* x) { return x && x->alive && !x->retreated && combat_->has_unit(x->id) && combat_->alive(x->id); };
    const auto keep = [&](TeamAttack& r) {
        const Unit* u = unit_by_id(r.attacker);
        if (!live_unit(u)) return false;
        const Container* ct = team_target(r.container);
        if (!ct) {
            // The container left with its last member: the order ends with it. A land team member runs no scan of its own
            // (s78f t03, TeamTargeting is the container's), so nothing else drops the dead member it still names.
            if (combat_->ordered_target(u->id) == r.member && !live_unit(unit_by_id(r.member))) {
                combat_->clear_order(u->id);
                if (r.goal) combat_->set_attack_goal(u->id, tc::kNoObject);
            }
            return false;
        }
        if (!combat_->is_enemy(st.owner(u->h), st.owner(ct->h))) return false;
        const auto ci = static_cast<uint32_t>(ct - containers_.data());
        const Unit* mu = unit_by_id(r.member);
        const bool member_live = live_unit(mu);
        const objects::ObjectId cur = combat_->ordered_target(u->id);
        bool lost = !member_live;
        if (cur != r.member) {
            const Unit* cu = cur != tc::kNoObject ? unit_by_id(cur) : nullptr;
            if (cu && cu->container == ci) {
                r.member = cur;  // a squadmate's order (combat share group) named another member
                lost = !live_unit(cu);
            } else if (cur != tc::kNoObject || member_live) {
                return false;  // dropped or replaced
            }
        }
        const objects::ObjectId m = team_pick(mode, *ct, st.position(u->h), combat_->share_group(u->id), picks);
        if (m == objects::kNoObjectId) {  // no member can be attacked (all garrisoned): the order ends, the target goes
            combat_->clear_order(u->id);
            if (r.goal) combat_->set_attack_goal(u->id, tc::kNoObject);
            return false;
        }
        if (m == cur) return true;
        combat_->order_attack(u->id, m, -1);
        if (r.goal) combat_->set_attack_goal(u->id, m, -1);
        r.member = m;
        if (lost) {
            const Vec3 tp = st.position(unit_by_id(m)->h);
            if (!movement_locked(u->id) && !movement_->fighter_squad(ct->id))
                fighter_order(std::span<const objects::ObjectId>(&u->id, 1), 0, tp, m);
            attack_approach(mode, *u, tp, rng, objects::kNoObjectId, m, true);  // s78n t25.3: land - a re-plan (slot kept)
        }
        return true;
    };
    size_t w = 0;
    for (size_t i = 0; i < team_attacks_.size(); ++i)
        if (keep(team_attacks_[i])) team_attacks_[w++] = team_attacks_[i];
    team_attacks_.resize(w);
}

// ---- s78n t20 stage A: land attack blocks (tactical_battle.hpp AttackBlock) -------------------------------------------

bool TacticalBattle::standin_recheck(const Unit& u, objects::ObjectId enemy) const {
    if (space_ || !combat_ || !movement_ || !combat_->has_unit(u.id)) return false;
    const objects::ObjectId mover = attack_mover(u.id);
    if (!movement_->has_unit(mover)) return false;
    const mv::UnitView v = movement_->view(mover);
    if (v.has_goal || v.pushed) return false;
    const tc::CanHit c = attack_can_hit(u.id, enemy);
    return c != tc::CanHit::Yes && c != tc::CanHit::NoTarget;
}

objects::ObjectId TacticalBattle::attack_mover(objects::ObjectId unit) const {
    objects::ObjectId c = objects::kNoObjectId;
    mv::V2 slot;
    return movement_ && movement_->team_slot(unit, c, slot) && c >= 0 ? c : unit;
}

tc::CanHit TacticalBattle::attack_can_hit(objects::ObjectId unit, objects::ObjectId target) const {
    const objects::ObjectId mover = attack_mover(unit);
    if (mover == unit || !movement_ || !movement_->has_unit(mover)) return combat_->can_hit(unit, target);
    tactical::Vec3 from = combat_->can_hit_origin(unit);  // its z (the lob solver's start height)
    const mv::V2 p = movement_->view(mover).pos;
    from.x = p.x;
    from.y = p.y;
    return combat_->can_hit_from(unit, from, target);
}

void TacticalBattle::attack_block_set(objects::ObjectId unit, objects::ObjectId target, uint8_t flags, Vec3 goal) {
    if (space_ || !movement_ || target == objects::kNoObjectId) return;
    const Unit* u = unit_by_id(unit);
    if (!u || !u->movable) return;
    const objects::ObjectId mover = attack_mover(unit);
    if (!movement_->has_unit(mover)) return;
    auto it = std::lower_bound(attack_blocks_.begin(), attack_blocks_.end(), mover,
                               [](const AttackBlock& r, objects::ObjectId v) { return r.mover < v; });
    if (it == attack_blocks_.end() || it->mover != mover) it = attack_blocks_.insert(it, AttackBlock{});
    // [port] the first check 30 frames after the order (the formation timer F +0x1d8 is set at its first state-1 step)
    *it = AttackBlock{mover, target, flags, frame_ + 30u, goal.x, goal.y};
}

void TacticalBattle::attack_block_drop(std::span<const objects::ObjectId> ids) {
    if (attack_blocks_.empty()) return;
    for (const objects::ObjectId id : ids) {
        const objects::ObjectId mover = attack_mover(id);
        const auto it = std::lower_bound(attack_blocks_.begin(), attack_blocks_.end(), mover,
                                         [](const AttackBlock& r, objects::ObjectId v) { return r.mover < v; });
        if (it != attack_blocks_.end() && it->mover == mover) attack_blocks_.erase(it);
    }
}

objects::ObjectId TacticalBattle::attack_lead(objects::ObjectId mover) {
    if (!movement_ || !movement_->is_land_team_container(mover)) return mover;
    for (const auto& [mid, slot] : movement_->land_team_slots(mover)) {
        (void)slot;
        const Unit* m = unit_by_id(mid);
        if (m && m->alive && !m->retreated && combat_->has_unit(mid)) return mid;
    }
    return objects::kNoObjectId;
}

bool TacticalBattle::pending_ability_ranges(objects::ObjectId unit, float& min_range, float& max_range, objects::ObjectId& on) const {
    // the unit's pending targeted ability, else its squad container's (AbilitySystem keeps a squad's on the container)
    on = objects::kNoObjectId;
    if (!abilities_) return false;
    for (const objects::ObjectId id : {unit, attack_mover(unit)})
        if (abilities_->pending_ranges(id, min_range, max_range)) {
            on = abilities_->pending_target(id);
            return true;
        }
    return false;
}

Vec3 TacticalBattle::attack_stage_goal(const TacticalMode& mode, std::span<const objects::ObjectId> ids, objects::ObjectId target,
                                       Vec3 tp, size_t movers) {
    // bkt1 t01 (b0068; 0x40e260, BKT1_ATTACK_RE §1.2 [H], TACTICAL_COMBAT §2.8; the AI's Attack_Move staging and stage
    // A's per-plan staging merged). M = the first unit with the largest minimum range: its type's
    // Targeting_Min_Attack_Distance, or its pending targeted ability's Activation_Min_Range when > 0. Only when M is inside
    // that min range of T is G moved:
    //   - crush case: a one-member formation whose M has SQUASH and T Is_Squashable (or M Is_Supercrusher and T
    //     Is_Squashable_By_Supercrusher) -> G = T's position (no hostility test);
    //   - else d = min + 0.2 x (Get_Base_Attack_Range(M) - min) and G = the free-position search 0x440f90 (mode 3) around
    //     M: rings centred on M from R0 = max(0, min - |M - G|) (3-D), step 1.0 growing by 0.8 up to min(1.5 r, 10)
    //     (r = M's hard footprint radius; maxStep 5 when r = 0), 20 rings, n = trunc(2 pi R / r) + 1 samples (at least
    //     4 after ring 0), counter-clockwise from the T -> M direction; the first sample >= d from G (3-D, the sample at
    //     M's z), inside the map extents and in M's zone for M's own movement class wins. None: G stays at T's point.
    // (the gate: slot 46 with the ability's Activation_Min_Range when M's pending ability is on T, else the type's)
    objects::ObjectId m = objects::kNoObjectId;
    float mn = 0.0f, gate = 0.0f;
    for (const objects::ObjectId id : ids) {
        if (!combat_->has_unit(id)) continue;
        const float tmin = combat_->min_attack_distance(id);
        float v = tmin, g = tmin;
        float amin = 0.0f, amax = 0.0f;
        objects::ObjectId on = objects::kNoObjectId;
        if (pending_ability_ranges(id, amin, amax, on) && amin > 0.0f) {
            v = std::max(v, amin);
            if (on == target) g = amin;
        }
        if (v > mn) {
            mn = v;
            gate = g;
            m = id;
        }
    }
    if (m == objects::kNoObjectId || !combat_->inside_min_range_of(m, target, gate)) return tp;
    const Unit* mu = unit_by_id(m);
    if (!mu) return tp;
    const objects::GameObjectTypes& types = catalog_->types();
    const Unit* tu = unit_by_id(target);
    if (movers == 1 && attack_mover(m) == m && tu && mu->type < types.size() && tu->type < types.size() &&
        types.has_behavior(mu->type, objects::BehaviorSet::Union, 0x26)) {  // SQUASH
        const bool squash = types.get_bool(tu->type, "Is_Squashable") ||
                            (types.get_bool(mu->type, "Is_Supercrusher") && types.get_bool(tu->type, "Is_Squashable_By_Supercrusher"));
        if (squash) return tp;
    }
    const float d = mn + 0.2f * (combat_->base_attack_range_of(m) - mn);
    const Vec3 mp = mode.primary().position(mu->h);
    const objects::ObjectId mover = attack_mover(m);
    const uint32_t mask = movement_ && movement_->has_unit(mover) ? movement_->mask_of(mover) : 0u;
    float hx = 0.0f, hy = 0.0f;
    unit_half_extents(*mu, hx, hy);
    const float r = dmath::sqrt(hx * hx + hy * hy);
    const float max_step = std::min(r > 0.0f ? 1.5f * r : 5.0f, 10.0f);
    const tactical::Aabb box = map_extents(mode);
    const float yaw = dmath::atan2(mp.y - tp.y, mp.x - tp.x);
    const auto dist3 = [](Vec3 a, Vec3 b) {
        const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
        return dmath::sqrt((dx * dx + dy * dy) + dz * dz);
    };
    float R = std::max(0.0f, mn - dist3(mp, tp));
    float step = 1.0f;
    for (int ring = 0; ring < 20; ++ring) {
        int32_t n = r > 0.0f ? static_cast<int32_t>(std::min(R * 2.0f * dmath::kPiF / r, 4096.0f)) + 1 : 1;
        if (ring > 0) n = std::max<int32_t>(4, n);
        for (int32_t k = 0; k < n; ++k) {
            const float th = 2.0f * dmath::kPiF * static_cast<float>(k) / static_cast<float>(n) + yaw;
            const Vec3 q{mp.x + R * dmath::cos(th), mp.y + R * dmath::sin(th), mp.z};
            if (d > dist3(q, tp)) continue;
            if (q.x < box.min.x || q.x > box.max.x || q.y < box.min.y || q.y > box.max.y) continue;
            if (!movement_ || !mv::zone_connected(movement_->regions(), mask, {mp.x, mp.y}, {q.x, q.y})) continue;
            return q;  // land_attack_order reads its x / y; the fire trim's end height is C's (bkt1 t06)
        }
        R += step;
        step = std::min(step + 0.8f, max_step);
    }
    return tp;
}

TacticalBattle::AttackPlan TacticalBattle::land_attack_order(TacticalMode& mode, std::span<const objects::ObjectId> ids,
                                                            objects::ObjectId target, Vec3 tp, uint8_t flags, bool replan,
                                                            SimRng& rng, bool guard, bool repath) {
    if (space_ || !movement_ || !combat_) return AttackPlan::None;
    const objects::ObjectStore& st = mode.primary();
    const Unit* tu = unit_by_id(target);
    mv::MoveOrder o;
    std::vector<objects::ObjectId> movers;
    for (const objects::ObjectId id : ids) {
        const Unit* u = unit_by_id(id);
        if (!u || !u->alive || u->retreated || !u->movable || movement_locked(id)) continue;
        const objects::ObjectId mover = attack_mover(id);
        if (!movement_->has_unit(mover)) continue;
        if (std::find(o.units.begin(), o.units.end(), id) == o.units.end()) o.units.push_back(id);
        if (std::find(movers.begin(), movers.end(), mover) == movers.end()) movers.push_back(mover);
    }
    if (o.units.empty()) return AttackPlan::None;
    // The target's footprint half extents (0x396850): its movement unit's, else the type's hard extents.
    float thx = 0.0f, thy = 0.0f;
    if (movement_->has_unit(target)) {
        const mv::V2 h = movement_->collision_half(target);
        thx = h.x;
        thy = h.y;
    } else if (tu) {
        unit_half_extents(*tu, thx, thy);
    }
    // s78n t25.4 [port]: a guard block is not staged (G = C; 0x40e260's min-range rule is for an attack on T)
    const Vec3 g = guard ? tp : attack_stage_goal(mode, o.units, target, tp, movers.size());
    const tactical::Aabb& bb = mode.bounds();
    o.destination = {std::clamp(g.x, bb.min.x, bb.max.x), std::clamp(g.y, bb.min.y, bb.max.y)};
    o.flags = static_cast<uint8_t>(flags & ~kAttackBlockNoLock);  // bkt1 t05: the host-only bit stays here
    o.goal_object = -1;  // land: the block's object is attack.target
    o.attack.target = target;
    o.attack.c = {tp.x, tp.y};
    o.attack.cz = tp.z;
    o.attack.g = o.destination;
    o.attack.t_hx = thx;
    o.attack.t_hy = thy;
    o.attack.replan = replan;
    o.attack.guard = guard;  // s78n t25.4
    // 0x410e90's gate: the target point not fogged for the first unit's owner, and not a stealthed target the first unit
    // cannot target (Target_Stealth_Units)
    const Unit* first = unit_by_id(o.units.front());
    bool trim = !fog_point_fogged(st.owner(first->h), tp.x, tp.y, false);
    if (trim && abilities_ && abilities_->is_stealthed(target, frame_)) {
        const objects::GameObjectTypes& types = catalog_->types();
        trim = first->type < types.size() && types.get_bool(first->type, "Target_Stealth_Units");
    }
    o.attack.trim = trim;
    // s78n t26 review round 3 (FIGHTER_FLYING §4.11 E [H], 0x40edd0): c is false for a target hidden for the first unit's
    // owner or stealthed against its type - no item-6 stop; G 5: a re-path keeps the standing in-range members
    bool hidden = false, stealthed = false;
    attack_target_unseen(st, *first, target, hidden, stealthed);
    o.attack.unseen = hidden || stealthed;
    o.attack.repath = repath && replan;
    for (const objects::ObjectId mover : movers) {
        const objects::ObjectId lead = attack_lead(mover);
        mv::LandAttackUnit e;
        e.id = mover;
        if (lead != objects::kNoObjectId && combat_->has_unit(lead)) {
            e.range = combat_->attack_range(lead);  // 0x3749d0 (Get_Base_Attack_Range)
            // 0x4f6e40 (§4.3.2 item 1; bkt1 t01, BKT1_ATTACK_RE §1.1 [H]): a pending targeted ability with an
            // Activation_Max_Range > 0 gives min(range, that range) - no extent, no guard cap; else + min(hx, hy) of the
            // target's extents ([port] land blocks name no hard point: the hard-point radius term is not reached)
            float amin = 0.0f, amax = 0.0f;
            objects::ObjectId on = objects::kNoObjectId;
            if (pending_ability_ranges(lead, amin, amax, on) && amax > 0.0f) {
                e.engage = std::min(e.range, amax);
            } else {
                e.engage = e.range + std::min(thx, thy);
                // s78n t25.4 (item 1): a guard block's engagement range is capped at Land_Guard_Range (GC +0x11ec4)
                if (guard) e.engage = std::min(e.engage, movement_->constants().land_guard_range);
            }
            const tc::CanHit c = attack_can_hit(lead, target);
            e.in_range = c == tc::CanHit::Yes || c == tc::CanHit::LineBlocked;  // slots 44 / 45: range only
            e.melee = combat_->melee_attacker(lead);
            // s78o (b0071): TARGETING - a weapon range, or a grenade arc (can_hit answers more than NoTarget)
            e.targeting = e.range > 0.0f || c != tc::CanHit::NoTarget;
        } else {
            e.targeting = false;  // not a combat unit: no TARGETING
        }
        o.attack.units.push_back(e);
    }
    const uint32_t stops = movement_->stats().attack_stops;
    movement_->move(o, rng, frame_);
    for (const objects::ObjectId mover : movers)
        if (movement_->view(mover).has_goal) return AttackPlan::Moved;
    return movement_->stats().attack_stops != stops ? AttackPlan::Stopped : AttackPlan::None;
}

void TacticalBattle::attack_target_unseen(const objects::ObjectStore& st, const Unit& unit, objects::ObjectId target,
                                          bool& hidden, bool& stealthed) {
    // the block's object: a squad member's container (FIGHTER_FLYING §4.11 D 1 / D 7: blocks name the team container; its
    // fog test uses the team box, FOG_OF_WAR §5.2), else the unit
    objects::ObjectId obj = target;
    const Unit* tu = unit_by_id(target);
    if (tu && tu->container < containers_.size() && containers_[tu->container].id != objects::kNoObjectId)
        obj = containers_[tu->container].id;
    hidden = fog_object_hidden(st.owner(unit.h), obj, true);
    stealthed = false;
    if (abilities_ && (abilities_->is_stealthed(target, frame_) || (obj != target && abilities_->is_stealthed(obj, frame_)))) {
        const objects::GameObjectTypes& types = catalog_->types();
        stealthed = !(unit.type < types.size() && types.get_bool(unit.type, "Target_Stealth_Units"));
    }
}

void TacticalBattle::attack_block_service(TacticalMode& mode, SimRng& rng) {
    if (attack_blocks_.empty() || space_ || !combat_ || !movement_) return;
    const objects::ObjectStore& st = mode.primary();
    const auto live = [&](const Unit* x) { return x && x->alive && !x->retreated && st.alive(x->h) && combat_->has_unit(x->id); };
    struct Checked {  // s78n t25.3: this pass's checked blocks (ascending mover)
        objects::ObjectId mover, lead, tid;
        bool due, plan, halted;
        bool locked = false;                   // bkt1 t03: the block's lead is movement-locked (not a formation lead)
        bool repath = false, dropped = false;  // s78n t26 review round 3: a moving formation's re-path (G 5); C 3's drop
        bool at_p = false;                     // C 3: the plan goes toward p (an unseen target after arrival)
        Vec3 p{};
    };
    std::vector<Checked> checked;
    const auto keep = [&](AttackBlock& r) {
        if (!movement_->has_unit(r.mover)) return false;
        const Container* ct = team_target(r.target);
        if (!ct && !live(unit_by_id(r.target))) return false;  // the target died (signal 0x10)
        // The unit the target is attacked through: the target itself, or (a TEAM container) the member the lead's order names.
        const auto names = [&](const Unit* x) -> objects::ObjectId {
            if (!live(x)) return objects::kNoObjectId;
            for (const tc::ObjectId o : {combat_->ordered_target(x->id), combat_->attack_goal(x->id)}) {
                if (o == tc::kNoObject) continue;
                if (!ct) {
                    if (o == r.target) return o;
                    continue;
                }
                const Unit* ou = unit_by_id(o);
                if (live(ou) && ou->container < containers_.size() && &containers_[ou->container] == ct) return o;
            }
            return objects::kNoObjectId;
        };
        const Unit* lead = nullptr;
        objects::ObjectId tid = objects::kNoObjectId;
        if (movement_->is_land_team_container(r.mover)) {
            for (const auto& [mid, slot] : movement_->land_team_slots(r.mover)) {
                (void)slot;
                const Unit* m = unit_by_id(mid);
                tid = names(m);
                if (tid != objects::kNoObjectId) {
                    lead = m;
                    break;
                }
            }
        } else {
            lead = unit_by_id(r.mover);
            tid = names(lead);
        }
        // bkt1 t05 (b0075, BKT1_ATTACK_RE §3 [H]): a unit Attack_Move on a non-hostile object - no unit's order names it
        // (nothing locks it), the block stands for the movement's object block alone: lead = the mover (a squad through
        // its first live member), C = the object
        if ((r.flags & kAttackBlockNoLock) != 0 && !ct) {
            const objects::ObjectId l = attack_lead(r.mover);
            lead = l == objects::kNoObjectId ? nullptr : unit_by_id(l);
            tid = r.target;
        }
        if (!lead || tid == objects::kNoObjectId) return false;  // the order no longer names the target
        // s78n t26 review round 2: the checks are the attack formation's (below); here the timer gate only. bkt1 t03: a
        // locked lead no longer suspends its formation here - the formation's lead is picked below among unlocked members.
        checked.push_back({r.mover, lead->id, tid, frame_ >= r.next, false, false, movement_locked(lead->id), false, false, false, Vec3{}});
        return true;
    };
    size_t w = 0;
    for (size_t i = 0; i < attack_blocks_.size(); ++i)
        if (keep(attack_blocks_[i])) attack_blocks_[w++] = attack_blocks_[i];
    attack_blocks_.resize(w);
    const auto bi = [&](objects::ObjectId mover) {
        return std::lower_bound(attack_blocks_.begin(), attack_blocks_.end(), mover,
                                [](const AttackBlock& b, objects::ObjectId v) { return b.mover < v; });
    };
    // s78n t26 review round 2 (FIGHTER_FLYING §4.11 A / E [H]): the checks are per attack formation F - the blocks on the
    // same target whose movers share UnitView::atk_form (0: the block alone), in ascending mover order (lead: bkt1 t03
    // below). F is checked when its lead's block is due (0x410a60 / 0x4192b0: frame + int(FPS +
    // 0.5)); every member's timer then resets. c = 0x40edd0(F): EVERY member can hit from where it stands (each block's
    // lead: a squad through its first member that names the target, D 5); moving = some member has a goal; stopping =
    // the lead's Walk state 0x14 (End Move) or 0x15 (Pre Stopped: [port] never observed, the port's walk passes it within
    // the frame), or the block has flag 0x01 and the lead has no path (0x39ade0: no record on its path stack).
    //   - arrived (no member has a goal), no member on a collision-owned top record, and not c -> plan again (0x410a60);
    //   - moving, c, not stopping and D <= R -> the land stop: every member with a goal halts at once;
    //   - moving and (stopping and not c, or D > R) -> plan again.
    // bkt1 t03 (b0073, FIGHTER_FLYING §4.11 G 3 [H]): F's lead = the member nearest the 3-D centroid L of F's movers
    // (summed in member order; strict <, first wins) among the movers that stand off their goal point (the unit their
    // lead's order names, tid: D 5's tested member of a TEAM container target), [port] skipping movement-locked ones (hero clash, immobilize, landing, Lua lock:
    // they keep their order, A1_READINGS §28.9); fallback the first unlocked member. Every member locked: F is not
    // checked, the timers wait. F is checked when its lead's block is due.
    std::vector<uint8_t> grouped(checked.size(), 0);
    const auto pos3 = [&](objects::ObjectId id) {
        const mv::V2 p = movement_->view(id).pos;
        const objects::ObjectHandle h = st.find(id);
        return Vec3{p.x, p.y, st.alive(h) ? st.position(h).z : 0.0f};
    };
    for (size_t i0 = 0; i0 < checked.size(); ++i0) {
        if (grouped[i0]) continue;
        grouped[i0] = 1;
        const uint32_t form = movement_->view(checked[i0].mover).atk_form;
        std::vector<size_t> g{i0};
        if (form != 0)
            for (size_t j = i0 + 1; j < checked.size(); ++j)
                if (!grouped[j] && bi(checked[j].mover)->target == bi(checked[i0].mover)->target &&
                    movement_->view(checked[j].mover).atk_form == form) {
                    grouped[j] = 1;
                    g.push_back(j);
                }
        size_t i = checked.size();
        {
            Vec3 L{};
            for (const size_t j : g) {
                const Vec3 q = pos3(checked[j].mover);
                L.x += q.x;
                L.y += q.y;
                L.z += q.z;
            }
            const float inv = 1.0f / static_cast<float>(g.size());
            L = Vec3{L.x * inv, L.y * inv, L.z * inv};
            float best = 0.0f;
            size_t first_free = checked.size();
            for (const size_t j : g) {
                if (checked[j].locked) continue;
                if (first_free == checked.size()) first_free = j;
                const Vec3 q = pos3(checked[j].mover);
                const Vec3 tc = st.position(unit_by_id(checked[j].tid)->h);
                if (q.x == tc.x && q.y == tc.y && q.z == tc.z) continue;  // its goal point is where it stands
                const float dx = q.x - L.x, dy = q.y - L.y, dz = q.z - L.z;
                const float dd = (dx * dx + dy * dy) + dz * dz;
                if (i == checked.size() || dd < best) {
                    best = dd;
                    i = j;
                }
            }
            if (i == checked.size()) i = first_free;
        }
        if (i == checked.size()) {  // every member locked: the formation waits
            for (const size_t j : g)
                if (checked[j].due) bi(checked[j].mover)->next = frame_ + 30u;
            continue;
        }
        const auto ri = bi(checked[i].mover);
        if (!checked[i].due) continue;
        for (const size_t j : g) bi(checked[j].mover)->next = frame_ + 30u;
        bool can = true, moving = false, pushed = false;
        for (const size_t j : g) {
            const tc::CanHit c = attack_can_hit(checked[j].lead, checked[j].tid);
            can = can && (c == tc::CanHit::Yes || c == tc::CanHit::NoTarget);  // NoTarget: nothing tested (0x40edd0 true)
            const mv::UnitView vj = movement_->view(checked[j].mover);
            moving = moving || vj.has_goal;
            pushed = pushed || vj.pushed;
        }
        const Unit* lead = unit_by_id(checked[i].lead);
        const Vec3 tp = st.position(unit_by_id(checked[i].tid)->h);
        const float gx = tp.x - ri->gx, gy = tp.y - ri->gy, R = 0.5f * lead->range;
        const bool far = gx * gx + gy * gy > R * R;
        // s78n t26 review round 3 (FIGHTER_FLYING §4.11 E [H]): c (0x40edd0) is false when the object is hidden for
        // members[0]'s owner or stealthed and members[0]'s type lacks Target_Stealth_Units ([port] the target unit the
        // lead's order names, tid: a TEAM container through that member)
        bool hidden = false, stealthed = false;
        attack_target_unseen(st, *lead, checked[i].tid, hidden, stealthed);
        if (hidden || stealthed) can = false;
        if (!moving) {
            if (pushed) continue;
            if (hidden || stealthed) {
                // §4.11 C 3 [H] (states 3 / 4, n = 1, the object hidden or stealthed for the lead): P = the object's goal
                // point 0x583200 - hidden: its position ([M] the block's stored point is the 1e18 default); stealthed: the
                // lead's own position (the member nearest the centroid L, G 3; [port] stealth is tested first). d = 2-D
                // |L - P|: d >= 40 -> plan again toward P; d < 40 -> the block becomes a position block at L (Set_Move kind 1,
                // clear) and every member's attack order is cleared (0x38d730(member, null) [M]). [port] checked on the
                // block's 30-frame timer.
                mv::V2 L{0.0f, 0.0f};
                for (const size_t j : g) L = L + movement_->view(checked[j].mover).pos;
                L = L * (1.0f / static_cast<float>(g.size()));
                Vec3 P = tp;
                if (stealthed) {
                    float best = 0.0f;
                    bool have = false;
                    for (const size_t j : g) {
                        const mv::V2 q = movement_->view(checked[j].mover).pos;
                        const float dd = (q.x - L.x) * (q.x - L.x) + (q.y - L.y) * (q.y - L.y);
                        if (!have || dd < best) {
                            best = dd;
                            have = true;
                            P = {q.x, q.y, tp.z};
                        }
                    }
                }
                const float dx = L.x - P.x, dy = L.y - P.y;
                if (dx * dx + dy * dy >= 40.0f * 40.0f) {
                    checked[i].plan = true;
                    checked[i].at_p = true;
                    checked[i].p = P;
                } else {
                    for (const size_t j : g) checked[j].dropped = true;
                }
                continue;
            }
            checked[i].plan = !can;  // arrived and some member cannot hit (FIGHTER_FLYING §4.11 A)
            continue;
        }
        // §4.11 C 1 / C 4 [H]: a hidden object - 0x4165f0 is false (no re-path) and c is false (no land stop); step 5 pops
        // nothing (n = 1): the formation walks on along its current path
        if (hidden) continue;
        const mv::UnitView lv = movement_->view(checked[i].mover);
        const bool stopping =
            lv.walk_state == mv::LocoState::WalkEndMove || ((ri->flags & mv::kMoveFlagAttackMove) != 0 && !lv.has_goal);
        if (can && !stopping && !far) {  // §4.11 E: the land stop of F
            std::vector<objects::ObjectId> ids;
            for (const size_t j : g)
                if (movement_->view(checked[j].mover).has_goal) ids.push_back(checked[j].mover);
            movement_->halt(ids, frame_);  // s78n t20 review: no speed zeroing, the walk brakes through End Move
            ++stats_.attack_land_stops;
            for (const size_t j : g) checked[j].halted = true;
        } else if ((stopping && !can) || far) {
            checked[i].plan = true;
            checked[i].repath = true;  // 0x4192b0 step 4's re-path: G 5
        }
    }
    // s78n t25.3 (MOVEMENT §4.3.2 item 8, §4.6.2): the re-plans, in ascending mover order. A re-plan is the formation's:
    // it takes the lead of every block on the same target whose mover is in the same attack formation (atk_form != 0)
    // and was not land-stopped this pass; their timers reset together. No new formation (no ctor draw), no slot test.
    std::vector<uint8_t> done(checked.size(), 0);
    for (size_t i = 0; i < checked.size(); ++i) {
        if (!checked[i].plan || done[i]) continue;
        const auto ri = bi(checked[i].mover);
        const objects::ObjectId target = ri->target;
        const uint8_t flags = ri->flags;
        const uint32_t form = movement_->view(checked[i].mover).atk_form;
        const Vec3 tp = checked[i].at_p ? checked[i].p : st.position(unit_by_id(checked[i].tid)->h);
        std::vector<objects::ObjectId> leads;
        for (size_t j = 0; j < checked.size(); ++j) {
            if (done[j] || checked[j].halted || checked[j].dropped) continue;
            if (j != i && (form == 0 || bi(checked[j].mover)->target != target || movement_->view(checked[j].mover).atk_form != form))
                continue;
            done[j] = 1;
            AttackBlock& rj = *bi(checked[j].mover);
            rj.next = frame_ + 30u;
            rj.gx = tp.x;
            rj.gy = tp.y;
            leads.push_back(checked[j].lead);
        }
        if (land_attack_order(mode, leads, checked[i].tid, tp, flags, true, rng, false, checked[i].repath) == AttackPlan::Moved)
            ++stats_.attack_reissues;
    }
    // s78n t26 review round 3 (FIGHTER_FLYING §4.11 C 3 [H]): the dropped blocks - each unit of the mover (a squad
    // container's members) loses its attack order and attack goal (a position block has none, TACTICAL_COMBAT §2.8), the
    // movement its attack block; the record goes. [port] no move toward L (the formation stands within 40 of it).
    std::vector<objects::ObjectId> gone;
    for (const Checked& c : checked)
        if (c.dropped) gone.push_back(c.mover);
    if (gone.empty()) return;
    for (const objects::ObjectId mover : gone) {
        std::vector<objects::ObjectId> us;
        if (movement_->is_land_team_container(mover)) {
            for (const auto& [mid, slot] : movement_->land_team_slots(mover)) {
                (void)slot;
                us.push_back(mid);
            }
        } else {
            us.push_back(mover);
        }
        for (const objects::ObjectId u : us) {
            if (!combat_->has_unit(u)) continue;
            combat_->clear_order(u);
            combat_->set_attack_goal(u, tc::kNoObject);
        }
        const auto it = bi(mover);
        if (it != attack_blocks_.end() && it->mover == mover) attack_blocks_.erase(it);
        ++stats_.attack_unseen_drops;
    }
    movement_->clear_attack_block(gone);
}

cmd::RejectReason TacticalBattle::order_move(TacticalMode& mode, uint8_t player, std::span<const objects::ObjectId> units, float x,
                                             float y, SimRng& rng, float facing_deg, bool attack_move, bool queued,
                                             uint8_t move_flags, bool switch_locomotor) {
    using R = cmd::RejectReason;
    if (finished_ || !movement_) return R::NotAllowed;
    const auto own = owned_units<Unit>(mode.primary(), player, units, [this](objects::ObjectId id) { return unit_by_id(id); }, true);
    if (own.empty()) return R::InvalidUnit;
    if (!std::isfinite(x) || !std::isfinite(y)) return R::InvalidPosition;
    std::vector<objects::ObjectId> ids;
    for (const Unit* u : own) ids.push_back(u->id);
    if (abilities_)  // OI abilities: order cancel 0x38f5d0 (BARRAGE / LUCKY / ION / HUNT off on a new order)
        for (const objects::ObjectId id : ids) abilities_->order_cancel(id, frame_);
    drop_locked(ids);  // OI follow-up t03: the group move leaves locked units out (after the cancel, §28.9)
    if (ids.empty()) return R::None;
    if (!queued) {
        clear_queues(ids);
        raise_order_replaced(ids);  // S7 t31: signal 0xb
        exec_move(mode, ids, x, y, rng, facing_deg, attack_move, move_flags, switch_locomotor);
        return R::None;
    }
    // MOVEMENT §6.2: the swap comes first, then the waypoint queue (each swapped unit continues its re-issued order).
    if (switch_locomotor)
        for (const objects::ObjectId id : ids) movement_->use_secondary_locomotor(id, true);
    const tactical::Aabb& bb = mode.bounds();
    // s78f t02 (MOVEMENT §6.2, FIGHTER_FLYING §4.11 B 1): a space fighter whose squadron (or own) formation exists takes the
    // shift-queued move into its block stack (the waypoint append 0x420f70: inserted at the bottom with 0x04, run after
    // everything queued; a guard above it ends once its object stops) instead of the host's order queue [port: Gabriel's
    // queue keeps the other units].
    if (space_) {
        std::vector<objects::ObjectId> wp, rest;
        for (const objects::ObjectId id : ids) {
            const Unit* u = unit_by_id(id);
            const bool fighter = u && movement_->has_unit(id) && movement_->view(id).locomotor == mv::LocomotorKind::Fighter;
            if (fighter && movement_->fighter_formation(fighter_key(*u))) wp.push_back(id);
            else rest.push_back(id);
        }
        if (!wp.empty()) {
            mv::MoveOrder o;
            o.units = wp;
            o.flags = static_cast<uint8_t>(move_flags | mv::kMoveFlagQueue | (attack_move ? mv::kMoveFlagAttackMove : 0));
            o.destination = {std::clamp(x, bb.min.x, bb.max.x), std::clamp(y, bb.min.y, bb.max.y)};
            if (std::isfinite(facing_deg)) o.facing_deg = facing_deg;
            o.waypoint = true;
            movement_->move(o, rng, frame_);
            // s78f review: the block went into the formation's stack (it does not replace the current order); the host
            // record (fighter_order) is only created when missing, and then mirrors the stack (fighter_sync).
            std::vector<objects::ObjectId> fresh;
            for (const objects::ObjectId id : wp)
                if (const Unit* u = unit_by_id(id); u && !fighter_formation(fighter_key(*u))) fresh.push_back(id);
            if (!fresh.empty())
                fighter_order(fresh, o.flags, Vec3{o.destination.x, o.destination.y, 0.0f});
        }
        ids = std::move(rest);
        if (ids.empty()) return R::None;
    }
    QueuedOrder q;
    q.kind = QueuedOrder::Kind::Move;
    q.player = player;
    q.x = std::clamp(x, bb.min.x, bb.max.x);
    q.y = std::clamp(y, bb.min.y, bb.max.y);
    if (std::isfinite(facing_deg)) q.facing = facing_deg;
    q.attack_move = attack_move;
    q.flags = move_flags;
    const std::vector<objects::ObjectId> now = enqueue(ids, q);
    raise_order_replaced(now);  // S7 t31: the units that start it now
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
        if (movement_locked(u->id)) continue;  // OI follow-up t03: the refused move path (§28.9 [M])
        raise_order_replaced(u->id);  // S7 t31 [M: as Turn_To_Face 0x581cd0]
        team_attack_drop(std::span<const objects::ObjectId>(&u->id, 1));  // s78n t20 review: the attack block ends too
        const Vec3 p = st.position(u->h);
        mv::MoveOrder o;
        o.units = {u->id};
        o.destination = {p.x, p.y};
        o.facing_deg = facing_deg;
        movement_->move(o, rng, frame_);
        fighter_order(o.units, 0, Vec3{p.x, p.y, 0.0f});  // S7 t22
    }
    return R::None;
}

cmd::RejectReason TacticalBattle::order_attack(TacticalMode& mode, uint8_t player, std::span<const objects::ObjectId> units,
                                               objects::ObjectId target, SimRng& rng, int32_t hardpoint, bool queued) {
    using R = cmd::RejectReason;
    if (finished_ || !combat_) return R::NotAllowed;
    const objects::ObjectStore& st = mode.primary();
    const Unit* t = unit_by_id(target);
    // q6: a TEAM container is a valid target on its own (A1_READINGS §33.2.1 row 1; TeamAttack in tactical_battle.hpp)
    const Container* ct = t ? nullptr : team_target(target);
    if (ct ? !combat_->is_enemy(player, st.owner(ct->h)) : (!t || !t->alive || t->retreated || !combat_->is_enemy(player, st.owner(t->h))))
        return R::InvalidTarget;
    const int32_t hp = (t && hardpoint >= 0 && static_cast<uint32_t>(hardpoint) < combat_->hardpoint_count(t->id)) ? hardpoint : -1;
    std::vector<objects::ObjectId> ids;
    for (const Unit* u : owned_units<Unit>(st, player, units, [this](objects::ObjectId id) { return unit_by_id(id); }, false))
        if (u->id != target && combat_->has_unit(u->id)) ids.push_back(u->id);
    if (ids.empty()) return R::InvalidUnit;
    if (abilities_)  // OI abilities: order cancel 0x38f5d0
        for (const objects::ObjectId id : ids) abilities_->order_cancel(id, frame_);
    if (!queued) {
        clear_queues(ids);
        raise_order_replaced(ids);  // S7 t31: signal 0xb
        team_attack_drop(ids);  // q6
        exec_attack(mode, ids, target, hp, rng);
        return R::None;
    }
    QueuedOrder q;
    q.kind = QueuedOrder::Kind::Attack;
    q.player = player;
    q.target = target;
    q.hardpoint = hp;
    const std::vector<objects::ObjectId> now = enqueue(ids, q);
    raise_order_replaced(now);  // S7 t31
    team_attack_drop(now);  // q6
    if (!now.empty()) exec_attack(mode, now, target, hp, rng);
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
    raise_order_replaced(now);  // S7 t31: the MoveToObject executor raises 0xb
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
        if (combat_->has_unit(u->id)) {
            combat_->clear_order(u->id);
            combat_->set_attack_goal(u->id, tc::kNoObject);  // refixf t06
        }
    }
    clear_queues(all);  // [port: Gabriel] Stop clears the queue
    team_attack_drop(all);  // q6
    fighter_order_clear(all);  // S7 t22: Stop leaves the formation [M]
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
        if (arrival_held()) return R::NotAllowed;  // story t12: the hyperspace hold (space only)
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
            // battle host (fog agent ask): space drop validity 0x4d6fd0 - fogged (force 1) / inside a non-allied prevention
            // radius -> refused (TACTICAL_GAPS §4.2).
            if (!fog_reinforce_point_ok(static_cast<int32_t>(player), x, y)) return R::InvalidPosition;
        }
        if (!space_) {
            // a1h t06 (TACTICAL_GAPS §4.2.2 item 5): Execute re-runs the drop validity (force 0, landing rule on, movers
            // ignored only for an AI issuer); a refused drop is refused whole. Then the company layout around (x, y),
            // turned by the side's yaw - the slots just tested.
            const bool human = player < galaxy_->players().size() && galaxy_->players()[player].human;
            if (!land_drop_valid(mode, static_cast<int32_t>(player), galactic_id, x, y, human, catalog_->use_reinforcement_points()))
                return R::InvalidPosition;
            // a1i t05 (A1_READINGS §35.4 item 3): campaign refusals after the validity (silent): a type that stays in its
            // transport during ground battles, or one the battle planet restricts (0x373490). [port] A transport whose
            // contents hold such a type cannot be in the port's pool (it holds companies); the "battle decided" gate is [M].
            if (!cfg_.skirmish) {
                const objects::GameObjectTypes& types = catalog_->types();
                if (p.type < types.size() && types.get_bool(p.type, "Stay_In_Transport_During_Ground_Battle")) return R::NotAllowed;
                if (galaxy_->planet_restricts(req_.info.planet, p.type)) return R::NotAllowed;
            }
            const float yaw = drop_yaw(p.side);
            const LandDropShape shape = land_drop_shape(p.type);
            LandDropLayout lay = land_drop_layout(shape, x, y, yaw);
            if (lay.ok && p.other_type < 0) {
                // a1i t04 (A1_READINGS §35.3): a company with a Company_Transport_Unit comes down in its landing object at
                // (x, y); the members leave it for the unload offsets (0x643130: the team grid from 3.0 x the team
                // bounds, turned by the landing's yaw) [port: placed there at once, snapped to a passable cell of their
                // movement class; no exit move]. Without one the members drop at the validity layout's slots [port].
                Landing landing = spawn_landing(mode, p, x, y);
                landing.participant = i;
                drop_pos_.clear();
                // q6 t19 (SKIRMISH_SETUP §3.1 item 3): in skirmish a pool type without a Company_Transport_Unit cannot be
                // dropped (0x2b5fe0 returns null: no landing object, no members).
                if (landing.id == objects::kNoObjectId && cfg_.skirmish) return R::NotAllowed;
                if (landing.id != objects::kNoObjectId) {
                    lay = land_drop_layout(shape, x, y, yaw, 3.0f);
                    const objects::GameObjectTypes& types = catalog_->types();
                    for (const LandDropSlot& s : lay.slots) {
                        const mv::UnitDesc md = mv::unit_desc_from_type(types, s.type);
                        drop_pos_.push_back(snap_passable(Vec3{s.x, s.y, 0.0f}, movement_->classes().mask(md.movement_class), false));
                    }
                    // Exit targets: the step's team offset / vehicle slot, on a cell passable for its first member.
                    for (size_t g = 0, m = 0; g < lay.group_x.size(); ++g) {
                        const uint32_t t = m < lay.slots.size() ? lay.slots[m].type : 0u;
                        const mv::UnitDesc md = mv::unit_desc_from_type(types, t);
                        landing.targets.push_back(
                            snap_passable(Vec3{lay.group_x[g], lay.group_y[g], 0.0f}, movement_->classes().mask(md.movement_class), false));
                        m += shape.create_team ? std::min<size_t>(std::max<uint32_t>(shape.squad, 1u), lay.slots.size()) : 1u;
                    }
                    // s78j t01 (TACTICAL_COMBAT §3.1.1 "Units leaving a land landing object") [H], the untimed unload
                    // placing everything at once: team step j faces along its offset O_j (the layout's, before the
                    // passable-cell snap) from the landing centre, step 0 the landing's facing; a vehicle placed at its
                    // slot ends on the landing's facing (0x3a8710).
                    const Vec3 psi = mode.primary().facing(landing.h);
                    drop_face_.assign(lay.slots.size(), psi);
                    if (shape.create_team) {
                        const size_t k = std::min<size_t>(std::max<uint32_t>(shape.squad, 1u), lay.slots.size());
                        for (size_t g = 1, m = k; g < lay.group_x.size() && m < lay.slots.size(); ++g, m += k)
                            for (size_t q = m; q < std::min(m + k, lay.slots.size()); ++q)
                                drop_face_[q] = offset_facing(lay.group_x[g] - x, lay.group_y[g] - y);
                    }
                    if (cfg_.landing_unload_timed) {
                        drop_face_.clear();  // landing_service picks the facings step by step
                        // §35.2: the company stays inside; landing_service runs the state machine. The reserve leaves the
                        // pool now (deployed), its members spawn step by step.
                        landing.timed = true;
                        drop_pos_.clear();
                        p.deployed = true;
                        landings_.push_back(std::move(landing));
                        ++stats_.reinforcements;
                        return R::None;
                    }
                } else {
                    for (const LandDropSlot& s : lay.slots) drop_pos_.push_back(Vec3{s.x, s.y, 0.0f});
                }
                const size_t first = units_.size();
                deploy_unload_ = landing.id != objects::kNoObjectId;
                deploy(mode, i, rng);
                deploy_unload_ = false;
                drop_pos_.clear();
                drop_face_.clear();
                if (landing.id != objects::kNoObjectId) {
                    // [port] Every unload step runs at the order (the tactical AI takes the units at once, §35.3): the
                    // signal 0x19 record is posted now; the landing object stays for the steps' timers, then leaves.
                    LandingUnload rec;
                    rec.landing = landing.id;
                    rec.galactic_id = p.galactic_id;
                    rec.owner = p.owner;
                    rec.carried_type = p.type;
                    for (size_t k = first; k < units_.size(); ++k) rec.units.push_back(units_[k].id);
                    landings_unloaded_.push_back(std::move(rec));
                    landing.state = 3;
                    landing.remove_frame = frame_ + (static_cast<uint32_t>(landing.targets.size()) + 1u) * landing_step_frames(p.owner);
                    landings_.push_back(std::move(landing));
                }
                ++stats_.reinforcements;
                return R::None;
            }
        }
        if (space_ && p.other_type < 0) {  // a1i t05: Find_Free_Placement per member (A1_READINGS §35.4 item 4)
            std::vector<Vec3> spots;
            if (space_member_spots(mode, p, x, y, spots)) {
                drop_pos_ = std::move(spots);
                deploy(mode, i, rng);
                drop_pos_.clear();
                ++stats_.reinforcements;
                return R::None;
            }
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

// ---- a1g t04 follow-up: land company drop layout (TACTICAL_GAPS §4.2.1) ----
namespace {
using Off2 = std::array<float, 2>;

// 0x20acd0: yaw of the vector 0 -> (x, y) in degrees, [0, 360); 0 for the zero vector.
float drop_yaw_of(float x, float y) {
    if (x == 0.0f && y == 0.0f) return 0.0f;
    float d = dmath::atan2(y, x) * 57.295776f;
    if (d < 0.0f) d += 360.0f;
    return d;
}

Off2 drop_rotate(float deg, float x, float y) {
    float sn = 0.0f, cs = 1.0f;
    dmath::sincos(deg * (dmath::kPiF / 180.0f), sn, cs);
    return {x * cs - y * sn, x * sn + y * cs};
}

// 0x40f970(cell = (a, b), n): rows of perRow slots, centred; output in world units (x 20), bounds in cell units.
bool drop_grid(float a, float b, uint32_t n, std::vector<Off2>& out, float& bx, float& by) {
    out.clear();
    if (b == 0.0f || n == 0) return false;
    const float side = std::sqrt(std::max(1.0f, static_cast<float>(n) * b * a * 1.8f));
    const float fr = side / b;
    const uint32_t per_row = fr >= 1.0f ? static_cast<uint32_t>(fr) : 1u;
    const uint32_t rows = (n + per_row - 1u) / per_row;
    float x = (static_cast<float>(rows) * a - a) / 2.0f;
    uint32_t left = n;
    for (uint32_t r = 0; r < rows; ++r) {
        const uint32_t k = std::min(per_row, left);
        const float gap = (side - static_cast<float>(k) * b) / static_cast<float>(k + 1u);
        float y = gap - side / 2.0f;
        for (uint32_t j = 0; j < k; ++j) {
            y += b / 2.0f;
            out.push_back({x * 20.0f, y * 20.0f});
            y += b / 2.0f + gap;
        }
        left -= k;
        x -= a;
    }
    bx = static_cast<float>(rows) * a / 2.0f;
    by = side / 2.0f;
    return true;
}

// 0x5abc00((ex, ey), g, s): ceil(sqrt(s)) columns along y per row, rows across x (row 0 = front, most +x), centred.
// Bounds (cell units): the slots' joint box half extents x 0.05.
void drop_team(float ex, float ey, float g, uint32_t s, std::vector<Off2>& out, float& bx, float& by) {
    out.clear();
    uint32_t c = static_cast<uint32_t>(std::sqrt(static_cast<float>(s)));
    if (c * c != s) ++c;
    if (c == 0) c = 1;
    const uint32_t rows = (s + c - 1u) / c;
    const float px = 2.0f * ex + g, py = 2.0f * ey + g;
    float lo_x = 0.0f, hi_x = 0.0f, lo_y = 0.0f, hi_y = 0.0f;
    uint32_t left = s;
    for (uint32_t r = 0; r < rows; ++r) {
        const uint32_t k = std::min(c, left);
        const float x = (static_cast<float>(rows - 1u) / 2.0f - static_cast<float>(r)) * px;
        for (uint32_t j = 0; j < k; ++j) {
            const float y = k == 1u ? 0.0f : -static_cast<float>(k - 1u) * py / 2.0f + static_cast<float>(j) * py;
            out.push_back({x, y});
            hi_x = std::max(hi_x, x + ex);
            lo_x = std::min(lo_x, x - ex);
            hi_y = std::max(hi_y, y + ey);
            lo_y = std::min(lo_y, y - ey);
        }
        left -= k;
    }
    bx = (hi_x - lo_x) / 2.0f * 0.05f;
    by = (hi_y - lo_y) / 2.0f * 0.05f;
}

bool drop_token_is(std::string_view list, std::string_view token) {  // case-insensitive token in a , / space list
    size_t i = 0;
    auto sep = [](char ch) { return ch == ',' || ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n'; };
    while (i < list.size()) {
        while (i < list.size() && sep(list[i])) ++i;
        size_t j = i;
        while (j < list.size() && !sep(list[j])) ++j;
        if (ascii_iequals(list.substr(i, j - i), token)) return true;
        i = j;
    }
    return false;
}
}  // namespace

LandDropLayout land_drop_layout(const LandDropShape& shape, float px, float py, float yaw_deg, float team_factor) {
    LandDropLayout out;
    const size_t n = shape.members.size();
    if (n == 0 || shape.hx.size() != n || shape.hy.size() != n) return out;
    const float fx = shape.hx[0], fy = shape.hy[0], fs = shape.formation_spacing;
    std::vector<Off2> local;
    float bx = 0.0f, by = 0.0f;
    if (!shape.create_team) {  // vehicles: one grid over all members, cell = (2 hx / 20 + FS, 2 hy / 20 + FS) of F
        if (!drop_grid(2.0f * fx / 20.0f + fs, 2.0f * fy / 20.0f + fs, static_cast<uint32_t>(n), local, bx, by)) return out;
    } else {
        const uint32_t sq = std::min<uint32_t>(std::max<uint32_t>(shape.squad, 1u), static_cast<uint32_t>(n));
        const uint32_t teams = (static_cast<uint32_t>(n) + sq - 1u) / sq;
        std::vector<Off2> team, offs;
        float tbx = 0.0f, tby = 0.0f;
        if (shape.container_locomotor)
            drop_team(fx, fy, 20.0f * fs, sq, team, tbx, tby);
        else if (!drop_grid(2.0f * fx / 20.0f + fs, 2.0f * fy / 20.0f + fs, sq, team, tbx, tby))
            return out;
        if (!drop_grid(team_factor * tbx, team_factor * tby, teams, offs, bx, by)) return out;
        local.resize(n);
        for (size_t i = 0; i < n; ++i) {
            const size_t t = i / sq, j = i - t * sq;
            const Off2 o = offs[t];
            const Off2 m = drop_rotate(drop_yaw_of(o[0], o[1]), team[j][0], team[j][1]);  // each team faces its offset
            local[i] = {o[0] + m[0], o[1] + m[1]};
        }
        for (uint32_t t = 0; t < teams; ++t) {  // a1i t04: one exit target per team (0x643130)
            const Off2 r = drop_rotate(yaw_deg, offs[t][0], offs[t][1]);
            out.group_x.push_back(px + r[0]);
            out.group_y.push_back(py + r[1]);
        }
    }
    out.half_x = bx * 20.0f;
    out.half_y = by * 20.0f;
    out.slots.resize(n);
    for (size_t i = 0; i < n; ++i) {
        const Off2 r = drop_rotate(yaw_deg, local[i][0], local[i][1]);
        LandDropSlot& sl = out.slots[i];
        sl.type = shape.members[i];
        sl.x = px + r[0];
        sl.y = py + r[1];
        sl.hx = shape.hx[i];
        sl.hy = shape.hy[i];
        sl.yaw_deg = drop_yaw_of(r[0], r[1]);
        if (!shape.create_team) {  // vehicles: one exit target per member
            out.group_x.push_back(sl.x);
            out.group_y.push_back(sl.y);
        }
    }
    out.ok = true;
    return out;
}

bool drop_boxes_overlap(const DropBox& a, const DropBox& b) {
    const Off2 ax = drop_rotate(a.yaw_deg, 1.0f, 0.0f), ay = drop_rotate(a.yaw_deg, 0.0f, 1.0f);
    const Off2 bu = drop_rotate(b.yaw_deg, 1.0f, 0.0f), bv = drop_rotate(b.yaw_deg, 0.0f, 1.0f);
    const float dx = b.cx - a.cx, dy = b.cy - a.cy;
    const Off2 axes[4] = {ax, ay, bu, bv};
    for (const Off2& u : axes) {
        const float ra = a.hx * std::fabs(ax[0] * u[0] + ax[1] * u[1]) + a.hy * std::fabs(ay[0] * u[0] + ay[1] * u[1]);
        const float rb = b.hx * std::fabs(bu[0] * u[0] + bu[1] * u[1]) + b.hy * std::fabs(bv[0] * u[0] + bv[1] * u[1]);
        if (std::fabs(dx * u[0] + dy * u[1]) >= ra + rb) return false;  // separating axis
    }
    return true;
}

bool land_drop_radius_ok(float x, float y, std::span<const LandDropRadiusObject> objects, bool use_reinforcement_points) {
    bool enabled = !use_reinforcement_points;
    for (const LandDropRadiusObject& o : objects) {
        const float dx = x - o.x, dy = y - o.y, d2 = dx * dx + dy * dy;
        if (o.allied) {
            if (o.enabling > 0.0f && d2 <= o.enabling * o.enabling) enabled = true;
        } else if (o.prevention > 0.0f && d2 < o.prevention * o.prevention && (!o.powered || o.owner_base_power)) {
            return false;  // prevention: a POWERED object blocks only while its owner has base power
        }
    }
    return enabled;
}

void drop_half_extents(const objects::GameObjectTypes& types, const BattleCatalog* catalog, uint32_t type, float& hx, float& hy) {
    hx = hy = 0.0f;
    if (type >= types.size()) return;
    float sc = types.get_float(type, "Scale_Factor");
    if (!(sc > 0.0f)) sc = 1.0f;
    const float cx = types.get_float(type, "Custom_Hard_XExtent"), cy = types.get_float(type, "Custom_Hard_YExtent");
    if (cx > 0.0f && cy > 0.0f) {
        hx = cx * sc;
        hy = cy * sc;
        return;
    }
    // s78n t10 (TACTICAL_COMBAT §5.4.6 item 1 and port spec item 2, 0x371be0 with the land model; replaces a1h t06's
    // |max| of the bind-pose box with a fallback to the other modes' models): no land model (0x3729f0 reads the land slot
    // only) -> (0, 0), even with one custom set; else per axis the hard extent when > 0, else the |collision-bound half|
    // of the land model. [port] a named land model that does not load (no art data, or no catalog): per axis the hard
    // extent, else 0.
    if (ascii_trim(types.get_string(type, "Land_Model_Name")).empty()) return;
    const tactical::movement::AvoidModel* am = catalog ? catalog->avoid_model(type, false) : nullptr;
    hx = (cx > 0.0f ? cx : am ? std::fabs(am->bound_h.x) : 0.0f) * sc;
    hy = (cy > 0.0f ? cy : am ? std::fabs(am->bound_h.y) : 0.0f) * sc;
}

void drop_deployed_extents(const objects::GameObjectTypes& types, const BattleCatalog* catalog, uint32_t type, float& hx, float& hy) {
    drop_half_extents(types, catalog, type, hx, hy);
    if (type >= types.size()) return;
    const float dx = types.get_float(type, "Custom_Hard_XExtent_Deployed");
    if (!(dx > 0.0f)) return;  // 0x371d20: the deployed pair applies only when the deployed x extent is set
    float sc = types.get_float(type, "Scale_Factor");
    if (!(sc > 0.0f)) sc = 1.0f;
    hx = dx * sc;
    hy = types.get_float(type, "Custom_Hard_YExtent_Deployed") * sc;  // also when <= 0
}

LandDropShape land_drop_shape(const objects::GameObjectTypes& types, const BattleCatalog* catalog, const galactic::ConflictRules& rules,
                              uint32_t company_type) {
    LandDropShape s;
    const galactic::ConflictTypeInfo& ti = rules.type(company_type);
    if (ti.members.empty()) {
        s.members.push_back(company_type);
    } else {
        for (const auto& [mt, n] : ti.members)
            for (int32_t i = 0; i < n; ++i) s.members.push_back(mt);
    }
    if (s.members.empty()) return s;
    if (const uint32_t f = s.members.front(); f < types.size()) s.create_team = types.get_bool(f, "Create_Team");  // original member
    // a1h t06 (TACTICAL_GAPS §4.2.2 item 2): a member without a land model that names a Unique_Ground_Unit (+0x1b60; FoC:
    // General_Veers -> Veers_AT_AT_Walker) is laid out, passability-tested and box-tested as that type.
    auto trimmed = [](std::string_view v) {
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t' || v.front() == '\r' || v.front() == '\n')) v.remove_prefix(1);
        while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r' || v.back() == '\n')) v.remove_suffix(1);
        return v;
    };
    for (uint32_t& t : s.members) {
        if (t >= types.size() || !trimmed(types.get_string(t, "Land_Model_Name")).empty()) continue;
        const std::string_view name = trimmed(types.get_string(t, "Unique_Ground_Unit"));
        const objects::TypeIndex u = name.empty() ? objects::kNone : types.resolve_name(name);
        if (u != objects::kNone && u < types.size()) t = u;
    }
    for (const uint32_t t : s.members) {
        float hx = 0.0f, hy = 0.0f;
        drop_half_extents(types, catalog, t, hx, hy);
        s.hx.push_back(hx);
        s.hy.push_back(hy);
    }
    if (const uint32_t f = s.members.front(); f < types.size()) s.formation_spacing = types.get_float(f, "FormationSpacing");
    if (company_type < types.size()) {
        const int32_t k = types.get_int(company_type, "Max_Squad_Size");  // +0x1e44, default 10 (bt_team)
        s.squad = k > 0 ? static_cast<uint32_t>(k) : 10u;
        std::string_view name = types.get_string(company_type, "Create_Team_Type");
        while (!name.empty() && (name.front() == ' ' || name.front() == '\t' || name.front() == '\r' || name.front() == '\n')) name.remove_prefix(1);
        while (!name.empty() && (name.back() == ' ' || name.back() == '\t' || name.back() == '\r' || name.back() == '\n')) name.remove_suffix(1);
        const objects::TypeIndex tt = name.empty() ? objects::kNone : types.resolve_name(name);
        // LandBehavior (+0x718) lists LAND_TEAM_CONTAINER_LOCOMOTOR (its behaviour id is shared with the other
        // locomotors, so the list text is read).
        if (tt != objects::kNone && tt < types.size())
            s.container_locomotor = drop_token_is(types.get_string(tt, "LandBehavior"), "LAND_TEAM_CONTAINER_LOCOMOTOR");
    }
    return s;
}

LandDropShape TacticalBattle::land_drop_shape(uint32_t company_type) const {
    return sim::land_drop_shape(catalog_->types(), catalog_, *rules_, company_type);
}

void TacticalBattle::drop_half_extents(uint32_t type, float& hx, float& hy) const {
    sim::drop_half_extents(catalog_->types(), catalog_, type, hx, hy);
}

bool TacticalBattle::mv_squad_add(TacticalMode& mode, size_t ci, uint32_t object_type) {
    // s78n t03 (TACTICAL_COMBAT §5.4.4 item 2, MOVEMENT §3.7, FIGHTER_FLYING §7): the LandTeamContainer locomotor. The
    // container (created at its members' mean with the first member's facing) becomes a mover of its own: free-position
    // search at add_unit, its own path and walk at the team speed (apply_speed_factors), its members steering to slots.
    if (space_ || !movement_ || ci >= containers_.size()) return false;
    const objects::GameObjectTypes& types = catalog_->types();
    if (object_type >= types.size()) return false;
    const Container& ct = containers_[ci];
    std::vector<mv::ObjectId> ids;
    std::vector<const Unit*> mem;
    for (const Unit& u : units_) {  // ascending id = the chunk's list order (team index order)
        if (u.container != ci) continue;
        if (!u.alive || u.retreated || !u.movable || !movement_->has_unit(u.id) || u.type >= types.size()) return false;
        if (movement_->view(u.id).locomotor != mv::LocomotorKind::Default || types.get_bool(u.type, "Uses_Multiple_Locomotors"))
            return false;
        ids.push_back(u.id);
        mem.push_back(&u);
    }
    if (ids.empty()) return false;
    // Slots (pack +0x114 / +0x118): the drop layout 0x5abc00 with the first member's half-extents and g = 20 x its
    // FormationSpacing (TACTICAL_GAPS §4.2.1), x along the container's facing, y across. s78n t14 (FIGHTER_FLYING §7.2): it
    // only seeds set_land_team - the container's first tick lays the slots out by its ContainerArrangement (Rows 0x5ac970 /
    // Dartboard 0x5ac2a0, MovementSystem::lay_out_land_team) with its own facing.
    const uint32_t f = mem.front()->type;
    float fx = 0.0f, fy = 0.0f;
    type_half_extents(f, fx, fy);
    std::vector<Off2> offs;
    float bx = 0.0f, by = 0.0f;
    drop_team(fx, fy, 20.0f * types.get_float(f, "FormationSpacing"), static_cast<uint32_t>(ids.size()), offs, bx, by);
    if (offs.size() != ids.size()) return false;
    std::vector<mv::V2> slots(ids.size());
    std::vector<ta::SquadSlot> box(ids.size());
    for (size_t i = 0; i < ids.size(); ++i) {
        slots[i] = {offs[i][0], offs[i][1]};
        box[i].along = offs[i][0];
        box[i].side = offs[i][1];
        unit_half_extents(*mem[i], box[i].hx, box[i].hy);
    }
    float jx = 0.0f, jy = 0.0f;
    ta::squad_joint_half_extents(box, types.get_float(object_type, "MaxJiggleDistance"), jx, jy);
    const objects::ObjectStore& st = mode.primary();
    const Vec3 p = st.position(ct.h);
    mv::UnitDesc d = mv::unit_desc_from_type(types, object_type);
    const mv::UnitDesc fm = mv::unit_desc_from_type(types, f);
    d.id = ct.id;
    d.pos = {p.x, p.y};
    d.z = p.z;
    d.yaw_deg = yaw_of(st.facing(ct.h));
    d.owner = ct.owner;
    d.team_container = true;
    d.team_member = false;
    d.locomotor = mv::LocomotorKind::Default;
    d.uses_multiple_locomotors = d.has_secondary_locomotor = false;
    // s78n t14 part B (FIGHTER_FLYING §7.5 item 1): the container keeps its type's UnitCollisionClass, Is_Squashable and
    // MovementPredictionInterval - it is a land collision checker / partner, and a crusher pushes it aside (crush-flee).
    d.crusher = d.supercrusher = false;
    d.turret = false;
    d.movement_class = fm.movement_class;  // [port] the container type's +0x1688 [M]: the first member's class
    // s78n t20 (MOVEMENT §5.4.4 item 3): the container TYPE's extents 0x371be0 - FoC's team container types set no hard
    // extents and [port] their models are not checked: (0, 0)
    d.type_hx = d.type_hy = 0.0f;
    // The formation layout reads the container's hard radius 0x397780: sqrt(hx^2 + hy^2) of the joint box.
    const float r = dmath::sqrt(jx * jx + jy * jy);
    if (r > 0.0f) d.radius = r;
    if (types.get_string(object_type, "Max_Speed").empty()) {  // [port] untagged: the members' minimum (the team cap rules)
        float mn = 3.0e38f;
        for (const Unit* u : mem) mn = std::min(mn, mv::unit_desc_from_type(types, u->type).max_speed);
        d.max_speed = mn;
    }
    if (!movement_->add_unit(d)) return false;
    if (!movement_->set_land_team(ct.id, ids, slots)) {
        (void)movement_->remove_unit(ct.id);
        return false;
    }
    for (const Unit* u : mem) mv_team_shape(*u);  // s78n t14: the layout's per-member half-extents
    const mv::UnitView v = movement_->view(ct.id);
    if (v.pos.x != p.x || v.pos.y != p.y)  // the free-position search moved it off a blocked centroid
        mode.primary().set_position(ct.h, Vec3{v.pos.x, v.pos.y, map_.terrain.sample_height(v.pos.x, v.pos.y)});
    return true;
}

void TacticalBattle::mv_team_shape(const Unit& u) {
    // s78n t14 (FIGHTER_FLYING §7.2): Rows reads each member's half-extents 0x396b70 / 0x396a30 (unit_half_extents),
    // Dartboard member 0's type half-extents 0x371be0 (type_half_extents).
    if (space_ || !movement_ || !movement_->has_unit(u.id)) return;
    float hx = 0.0f, hy = 0.0f, thx = 0.0f, thy = 0.0f;
    unit_half_extents(u, hx, hy);
    type_half_extents(u.type, thx, thy);
    (void)movement_->set_team_member_shape(u.id, hx, hy, thx, thy);
}

float TacticalBattle::drop_yaw(uint8_t side) const {
    if (side >= 2) return 0.0f;  // third players: no record written
    const Vec3 f = drop_facing_[side];
    return f.x == 0.0f && f.y == 0.0f ? 0.0f : yaw_of(f);
}

Vec3 TacticalBattle::player_drop_facing(int32_t player) const {
    for (uint8_t side = 0; side < 2; ++side) {
        if (side_player(side) != player) continue;
        const Vec3 f = drop_facing_[side];
        return f.x == 0.0f && f.y == 0.0f ? facing_of_yaw(0.0f) : f;
    }
    return facing_of_yaw(0.0f);  // a third player: no record written (yaw 0)
}

// a1i t04 (A1_READINGS §35.1 / §35.3): the landing object of a land reserve drop. L = the company's
// Company_Transport_Unit[0], then its Landing_Transport_Variant when set; none -> no landing object (kNoObjectId). Created
// at (x, y), z = terrain + L's Hover_Offset [the +0xd8 vehicle-height term is [M]], facing = the side's entry-facing
// record, parent id = the reserve's galactic id; TRANSPORT_LANDING attached, carried type = the company type. Removal
// frame: the unload steps (one per team of min(Max_Squad_Size, n) Create_Team members or per other member) plus the finish
// step, each 2 s x (1 + s), s = the owner's Reinforcement_Deploy_Time_Multiplier list (0x4ad3a0: (int)(30 x seconds));
// [port] the Land / Takeoff clips take no time and there is no clearance wait (§35.2 state 4).
TacticalBattle::Landing TacticalBattle::spawn_landing(TacticalMode& mode, const Participant& p, float x, float y) {
    Landing out;
    const objects::GameObjectTypes& types = catalog_->types();
    if (p.type >= types.size()) return out;
    const std::vector<objects::TypeIndex> tr = types.get_type_refs(p.type, "Company_Transport_Unit");
    objects::TypeIndex l = tr.empty() ? objects::kNone : tr.front();  // the first entry (0x400f30)
    if (l == objects::kNone || l >= types.size()) return out;
    std::string_view vname = types.get_string(l, "Landing_Transport_Variant");  // one name (+0x1e68)
    while (!vname.empty() && std::isspace(static_cast<unsigned char>(vname.front())) != 0) vname.remove_prefix(1);
    while (!vname.empty() && std::isspace(static_cast<unsigned char>(vname.back())) != 0) vname.remove_suffix(1);
    if (const objects::TypeIndex v = vname.empty() ? objects::kNone : types.resolve_name(vname); v != objects::kNone && v < types.size())
        l = v;
    objects::SpawnParams sp;
    sp.type = l;
    sp.owner = p.owner;
    sp.position = {x, y, map_.terrain.sample_height(x, y) + types.get_float(l, "Hover_Offset")};
    const Vec3 f = p.side < 2 ? drop_facing_[p.side] : Vec3{};
    sp.facing = f.x == 0.0f && f.y == 0.0f ? facing_of_yaw(0.0f) : f;
    sp.parent_mode_id = p.built ? objects::kNoObjectId : p.galactic_id;  // q6 t19: tactical production pool entries
    const objects::ObjectHandle h = mode.spawn(sp, 1.0f);
    objects::ObjectStore& st = mode.primary();
    st.add_behavior(h, objects::beh::TransportLanding);
    st.set_carried_type(h, p.type);
    out.id = st.id(h);
    out.h = h;
    return out;
}

uint32_t TacticalBattle::landing_step_frames(int32_t player) const {
    const float s = abilities_ ? abilities_->player_list(player, tactical::abilities::PlayerList::ReinforcementDeploy) : 0.0f;
    const float secs = 2.0f * (1.0f + s);
    return secs > 0.0f ? static_cast<uint32_t>(30.0f * secs) : 0u;  // 0x4ad3a0: (int)(FPS x seconds)
}

bool TacticalBattle::landing_hidden(objects::ObjectId landing) const {
    for (const Landing& l : landings_)
        if (l.id == landing) return l.state == 4;
    return false;
}

uint32_t TacticalBattle::landings_in_flight(int32_t player) const {
    uint32_t n = 0;
    for (const Landing& l : landings_)
        if (l.timed && l.state != 3 && parts_[l.participant].owner == player) ++n;
    return n;
}

bool TacticalBattle::landing_locked(objects::ObjectId id) const {
    for (const Landing& l : landings_)
        if (l.timed && l.state == 2 && std::find(l.units.begin(), l.units.end(), id) != l.units.end()) return true;
    return false;
}

// a1i t04 (A1_READINGS §35.2, timed mode): state 4 waits (hidden) until no other landing in state 1 / 2 overlaps its
// footprint (drop_half_extents box along the yaw [port: the footprint AABB with z +-100 of 0x38de80 as an oriented box; only
// this battle's landings are seen]); state 1 [port: the Land clip takes no time] turns into state 2 with the step timer;
// each expiry spawns one team chunk (min(k, members left)) or one vehicle at the landing position (deploy member range)
// and sends it on a group move to its exit target [port: no forced-move flag in the movement system], locked
// (landing_locked); after the last member the next expiry finishes: unlock, the signal 0x19 record (landings_unloaded),
// state 3 and the take-off [port: no Takeoff clip: removed at once]. Untimed landings are only removed at their frame.
void TacticalBattle::landing_service(TacticalMode& mode, SimRng& rng) {
    objects::ObjectStore& store = mode.primary();
    const objects::GameObjectTypes& types = catalog_->types();
    auto footprint = [&](const Landing& l) {
        DropBox b;
        const Vec3 q = store.position(l.h), f = store.facing(l.h);
        b.cx = q.x;
        b.cy = q.y;
        b.yaw_deg = f.x == 0.0f && f.y == 0.0f ? 0.0f : yaw_of(f);
        sim::drop_half_extents(types, catalog_, store.type(l.h), b.hx, b.hy);
        return b;
    };
    auto unloaded = [&](const Landing& l) {  // 0x6438a0: the signal 0x19 record (ReinforcementsUnloadedDataClass)
        const Participant& P = parts_[l.participant];
        LandingUnload rec;
        rec.landing = l.id;
        rec.galactic_id = P.galactic_id;
        rec.owner = P.owner;
        rec.carried_type = P.type;
        rec.units = l.units;
        landings_unloaded_.push_back(std::move(rec));
    };
    for (size_t li = 0; li < landings_.size(); ++li) {
        Landing& l = landings_[li];
        if (!store.alive(l.h)) {
            // a1j t01 (A1_READINGS §36): a timed landing destroyed before its finish. Its behaviour's detach (vf15 0x6442d0,
            // run when the object is deleted) finishes the unload when a unit is out (+0xb0 != 0): unlock, signal 0x19 with
            // the units out so far. With none out the original sends nothing and the Reinforce status waits forever
            // [port: the record is sent with no units, so the status's in-flight count still reaches 0]. The members not
            // yet out are lost.
            if (l.timed && l.state != 3) unloaded(l);
            l.units.clear();
            l.state = 3;
            l.remove_frame = 0;
            continue;
        }
        if (!l.timed) continue;
        const Participant& P = parts_[l.participant];
        if (l.state == 4) {
            bool blocked = false;
            const DropBox mine = footprint(l);
            for (size_t o = 0; o < landings_.size() && !blocked; ++o)
                if (o != li && (landings_[o].state == 1 || landings_[o].state == 2) && store.alive(landings_[o].h))
                    blocked = drop_boxes_overlap(mine, footprint(landings_[o]));
            if (blocked) continue;
            l.state = 1;  // 0x642c70: shown, landing dust [port: no particle]
        }
        if (l.state == 1) {  // [port] the Land clip passes 98 % at once
            l.state = 2;
            l.timer_start = frame_;
            l.timer_len = landing_step_frames(P.owner);
        }
        if (l.state != 2 || frame_ < l.timer_start + l.timer_len) continue;
        const std::vector<uint32_t> members = spawn_types(P);
        const auto n = static_cast<uint32_t>(members.size());
        if (l.unloaded >= n) {  // 0x6438a0: finish
            unloaded(l);
            l.units.clear();  // unlocked
            l.state = 3;
            l.remove_frame = frame_;
            continue;
        }
        // 0x644330: one team chunk or one vehicle at the landing position.
        const int32_t mss = types.get_int(P.type, "Max_Squad_Size");
        const uint32_t k = std::min(mss > 0 ? static_cast<uint32_t>(mss) : 10u, std::max(n, 1u));
        const uint32_t i = l.unloaded;
        const uint32_t cnt = members[i] < types.size() && types.get_bool(members[i], "Create_Team") ? std::min(k, n - i) : 1u;
        const Vec3 at = store.position(l.h);
        drop_pos_.assign(n, Vec3{at.x, at.y, 0.0f});
        // s78j t01 (TACTICAL_COMBAT §3.1.1 "Units leaving a land landing object") [H]: step j's units are created at the
        // landing centre facing along the step's offset O_j (0x20acd0((0, 0, 0), O_j)); a team's step 0 takes the
        // landing's own facing instead (vehicles have no step-0 override). O_j = the unload layout's step offset before the
        // passable-cell snap, rebuilt here from the landing position and the side's drop yaw (as order_reinforce built it).
        const Vec3 psi = store.facing(l.h);
        const bool team = cnt > 1 || (members[i] < types.size() && types.get_bool(members[i], "Create_Team"));
        {
            const LandDropLayout lay = land_drop_layout(land_drop_shape(P.type), at.x, at.y, drop_yaw(P.side), 3.0f);
            const uint32_t j = l.next_offset;
            Vec3 f = psi;
            if (lay.ok && j < lay.group_x.size() && !(team && j == 0)) f = offset_facing(lay.group_x[j] - at.x, lay.group_y[j] - at.y);
            drop_face_.assign(n, f);
        }
        const size_t first = units_.size();
        const uint32_t pi = l.participant;  // deploy may grow parts_-held state; keep the index
        deploy(mode, pi, rng, i, cnt);
        drop_pos_.clear();
        drop_face_.clear();
        Landing& lr = landings_[li];
        mv::MoveOrder o;
        for (size_t u = first; u < units_.size(); ++u) {
            lr.units.push_back(units_[u].id);
            if (units_[u].movable) o.units.push_back(units_[u].id);
        }
        if (!o.units.empty() && lr.next_offset < lr.targets.size()) {
            o.destination = {lr.targets[lr.next_offset].x, lr.targets[lr.next_offset].y};
            // s78k t01 (TACTICAL_COMBAT §3.1.1 "Units leaving a land landing object", MOVEMENT §6.1 / §3.5.1) [H]: the exit
            // move has flags 0x20 (a straight 2-point path without passability checks); a vehicle's block carries the
            // landing's yaw as its final facing (+0x5c = 1), so it turns to psi on arrival when more than 10 degrees off;
            // a team's block has none (+0x5c = 0) and keeps its travel heading.
            o.flags = mv::kMoveFlagSkipValidation;
            if (!team) o.final_facing_deg = yaw_of(psi);
            movement_->move(o, rng, frame_);
            ++stats_.orders;
        }
        lr.unloaded += cnt;
        ++lr.next_offset;
        lr.timer_start = frame_;
        lr.timer_len = landing_step_frames(P.owner);
    }
    // Take-off: removed at the frame (untimed: after the steps' timers; timed: at the finish).
    size_t w = 0;
    for (size_t i = 0; i < landings_.size(); ++i) {
        if (landings_[i].state == 3 && frame_ >= landings_[i].remove_frame) {
            if (store.alive(landings_[i].h)) mode.destroy(landings_[i].h);
            continue;
        }
        if (w != i) landings_[w] = std::move(landings_[i]);
        ++w;
    }
    landings_.resize(w);
}

bool find_free_placement(float px, float py, const FreePlacementBox& local, std::span<const FreePlacementBox> blockers, float radius,
                         float& out_x, float& out_y, float start_deg, const std::function<bool(float x, float y)>& accept) {
    const float step = 1.2f * std::max(std::fabs(local.max_x - local.min_x), std::fabs(local.max_y - local.min_y));
    for (float r = 0.0f; r < radius; r += step) {
        for (int32_t k = 0; k <= 16; ++k) {
            float sn = 0.0f, cs = 1.0f;
            dmath::sincos((start_deg + static_cast<float>(k) * 22.5f) * (dmath::kPiF / 180.0f), sn, cs);
            const float qx = px + r * cs, qy = py + r * sn;
            const FreePlacementBox q{qx + local.min_x, qy + local.min_y, qx + local.max_x, qy + local.max_y};
            bool blocked = accept && !accept(qx, qy);
            for (const FreePlacementBox& o : blockers)
                if (q.min_x < o.max_x && o.min_x < q.max_x && q.min_y < o.max_y && o.min_y < q.max_y) {
                    blocked = true;
                    break;
                }
            if (!blocked) {
                out_x = qx;
                out_y = qy;
                return true;
            }
            if (r == 0.0f) break;  // the centre is one test
        }
        if (!(step > 0.0f)) break;
    }
    return false;
}

bool TacticalBattle::space_member_spots(TacticalMode& mode, const Participant& p, float x, float y, std::vector<Vec3>& out) const {
    out.clear();
    const objects::GameObjectTypes& types = catalog_->types();
    const std::vector<uint32_t> members = spawn_types(p);
    const Vec3 at{x, y, 0.0f};
    if (members.size() <= 1) {  // a single unit: at P
        out.assign(members.size(), at);
        return true;
    }
    auto has_model = [&](uint32_t t) {
        std::string_view v = types.get_string(t, "Space_Model_Name");
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t' || v.front() == '\r' || v.front() == '\n')) v.remove_prefix(1);
        return !v.empty();
    };
    constexpr uint64_t kLayer20 = 0x20;  // Space_Layer 0x20: placed at P, never a blocker
    // Member boxes first: a member whose model is named but not loaded sends the whole drop to the rows of 6 [port].
    std::vector<FreePlacementBox> local(members.size());
    std::vector<uint8_t> at_p(members.size(), 0);
    for (size_t i = 0; i < members.size(); ++i) {
        const uint32_t t = members[i];
        if (t >= types.size() || !has_model(t) || types.get_mask(t, "Space_Layer") == kLayer20) {
            at_p[i] = 1;
            continue;
        }
        const BattleCatalog::ModelInfo* m = catalog_->model_info(t, true);
        if (!m || !m->has_bounds) return false;
        local[i] = {m->min[0], m->min[1], m->max[0], m->max[1]};
    }
    // Blockers: the object tree (all owners): live, not dying, not Is_Decoration, no TRANSPORT_LANDING / LAND_OBSTACLE /
    // PROJECTILE, Space_Layer != 0x20, with a model; their tree box in x / y [M: the type byte +0x3e test is not applied].
    const objects::ObjectStore& st = mode.primary();
    std::vector<FreePlacementBox> blockers;
    for (const objects::ObjectHandle h : st.all_objects()) {
        if (!st.alive(h) || st.dying(h)) continue;
        const objects::TypeIndex t = st.type(h);
        if (t >= types.size() || types.get_bool(t, "Is_Decoration") || !has_model(t)) continue;
        if (st.has_behavior(h, objects::beh::TransportLanding) || st.has_behavior(h, objects::beh::LandObstacle) ||
            st.has_behavior(h, objects::beh::Projectile))
            continue;
        if (types.get_mask(t, "Space_Layer") == kLayer20) continue;
        const tactical::Aabb b = mode.bounds_of(h);
        blockers.push_back({b.min.x, b.min.y, b.max.x, b.max.y});
    }
    // Members one at a time: each placed member blocks the later ones.
    for (size_t i = 0; i < members.size(); ++i) {
        if (at_p[i]) {
            out.push_back(at);
            continue;
        }
        float qx = 0.0f, qy = 0.0f;
        if (!find_free_placement(x, y, local[i], blockers, 2500.0f, qx, qy)) {
            qx = x;  // none: (0, 0, 0), replaced by P (z 0)
            qy = y;
        }
        out.push_back(Vec3{qx, qy, 0.0f});
        blockers.push_back({qx + local[i].min_x, qy + local[i].min_y, qx + local[i].max_x, qy + local[i].max_y});
    }
    return true;
}

// ---- land drop validity (header; TACTICAL_GAPS §4.2.1 tests 1-4, §4.2.2 items 2-4) ----
bool TacticalBattle::land_drop_valid(TacticalMode& mode, int32_t player, objects::ObjectId key, float x, float y, bool human,
                                     bool use_reinforcement_points) const {
    if (space_ || !movement_) return true;  // land only
    if (!std::isfinite(x) || !std::isfinite(y)) return false;
    const Participant* part = nullptr;
    for (const Participant& q : parts_)
        if (q.galactic_id == key && q.owner == player && q.reserve && !q.deployed) {
            part = &q;
            break;
        }
    if (!part) return false;
    const float yaw = drop_yaw(part->side);
    const LandDropLayout lay = land_drop_layout(land_drop_shape(part->type), x, y, yaw);
    if (!lay.ok) return false;
    const objects::GameObjectTypes& types = catalog_->types();
    const objects::ObjectStore& st = mode.primary();
    const mv::PassabilityGrid& grid = movement_->grid();
    float mn[2], mx[2];
    const bool box = map_.playable_box(mn, mx);
    // 1-2. passable for the member's movement class (inside the playable box [port]); not fogged for the player.
    for (const LandDropSlot& s : lay.slots) {
        if (box && (s.x < mn[0] || s.x > mx[0] || s.y < mn[1] || s.y > mx[1])) return false;
        if (s.type >= types.size()) return false;
        const mv::UnitDesc md = mv::unit_desc_from_type(types, s.type);
        if (!grid.passable_world({s.x, s.y}, movement_->classes().mask(md.movement_class))) return false;
        if (fog_point_fogged(player, s.x, s.y, true)) return false;
    }
    // 3. footprints: the formation box (broad phase), then each slot's own box against each blocking object's box.
    static const objects::BehaviorTypeId kMine = objects::behavior_type_id("PROXIMITY_MINE");
    static const objects::BehaviorTypeId kLanding = objects::behavior_type_id("TRANSPORT_LANDING");
    const DropBox formation{x, y, lay.half_x, lay.half_y, yaw};
    std::vector<LandDropRadiusObject> radius;
    for (const objects::ObjectHandle h : st.all_objects()) {  // GOM order
        if (!st.alive(h)) continue;
        const objects::TypeIndex t = st.type(h);
        if (t >= types.size()) continue;
        const int32_t o = st.owner(h);
        const Vec3 q = st.position(h);
        // 4. radius inputs (GOM list +0x2f0): dying / removing (+0x3a0 & 0x42) skipped.
        if (st.active(h)) {
            LandDropRadiusObject ro;
            ro.x = q.x;
            ro.y = q.y;
            ro.allied = o >= 0 && (o == player || !combat_->is_enemy(player, o));  // 0x2823c0 / 0x39a230
            ro.enabling = types.get_float(t, "Reinforcement_Enabling_Radius");
            ro.prevention = types.get_float(t, "Reinforcement_Prevention_Radius");
            ro.powered = st.has_behavior(h, objects::beh::Powered);
            ro.owner_base_power = o >= 0 && combat_->has_base_power(o);
            if (ro.enabling > 0.0f || ro.prevention > 0.0f) radius.push_back(ro);
        }
        // a1h t06 (§4.2.2 item 4): the object tree GOM +0xd8 - Collidable_By_Projectile_Living (a death clone: _Dead),
        // any owner; never proximity mines; an AI issuer ignores movers; a human issuer ignores landing transports (item 3).
        if (!types.get_bool(t, st.dying(h) ? "Collidable_By_Projectile_Dead" : "Collidable_By_Projectile_Living")) continue;
        if (kMine != objects::kNoBehavior && st.has_behavior(h, kMine)) continue;
        const bool obstacle = st.has_behavior(h, objects::beh::LandObstacle);
        if (!human && st.has_behavior(h, objects::beh::Locomotor) && !obstacle) continue;
        // a1i t05 (A1_READINGS §35.4 item 2): a persistent cinematic landing (pack +0xc9) still blocks a human drop.
        if (human && kLanding != objects::kNoBehavior && st.has_behavior(h, kLanding) && !st.landing_persist(h)) continue;
        // Footprint 0x38de80: centre + R(yaw)(Custom_Hard_X/YExtent_Offset) x Scale_Factor, half extents 0x371be0, along yaw.
        const Vec3 f = st.facing(h);
        DropBox xb;
        xb.yaw_deg = f.x == 0.0f && f.y == 0.0f ? 0.0f : yaw_of(f);
        float sc = types.get_float(t, "Scale_Factor");
        if (!(sc > 0.0f)) sc = 1.0f;
        const float ox = types.get_float(t, "Custom_Hard_XExtent_Offset") * sc, oy = types.get_float(t, "Custom_Hard_YExtent_Offset") * sc;
        float sn = 0.0f, cs = 1.0f;
        dmath::sincos(xb.yaw_deg * (dmath::kPiF / 180.0f), sn, cs);
        xb.cx = q.x + ox * cs - oy * sn;
        xb.cy = q.y + ox * sn + oy * cs;
        // a1i t05 (§35.4 item 1, 0x396850 rule 2): a Deploys type with a locomotor while it deploys / is deployed uses
        // its deployed extents. s78n t10 (TACTICAL_COMBAT §5.4.6 item 4.3): Walk state 0x18 Deploying or 0x19 Deployed of
        // a movement-backed unit (as unit_half_extents), else [port] DEPLOY active. [M, not ported] a land squad
        // container's joint box as a blocker (whether 0x3b6170's tree query returns containers was not traced).
        bool dep = false;
        if (types.get_bool(t, "Deploys") && st.has_behavior(h, objects::beh::Locomotor)) {
            if (movement_ && movement_->has_unit(st.id(h))) {
                const mv::LocoState ds = movement_->deploy_state(st.id(h));
                dep = ds == mv::LocoState::WalkDeploying || ds == mv::LocoState::WalkDeployed;
            } else {
                dep = abilities_ && abilities_->deployed(st.id(h));
            }
        }
        if (dep)
            sim::drop_deployed_extents(types, catalog_, t, xb.hx, xb.hy);
        else
            sim::drop_half_extents(types, catalog_, t, xb.hx, xb.hy);
        if (!drop_boxes_overlap(formation, xb)) continue;
        for (const LandDropSlot& s : lay.slots)
            if (drop_boxes_overlap(DropBox{s.x, s.y, s.hx, s.hy, s.yaw_deg}, xb)) return false;  // footprint taken
    }
    // 4. reinforcement radii (check-radius: AI and human drops are not forced).
    for (const LandDropSlot& s : lay.slots)
        if (!land_drop_radius_ok(s.x, s.y, radius, use_reinforcement_points)) return false;
    return true;
}
// ---- end a1g t04 follow-up ----


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
                if (!t)  // q6: a TEAM container target
                    if (const Container* ct = team_target(o.target)) return combat_->is_enemy(o.player, st.owner(ct->h));
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
            // s78n t06 review (A1_READINGS §28.9, HERO_CLASH §3.2): a movement-locked unit (hero clash, script lock, ability
            // immobilize, landing exit) takes no queued order. [port] Its queue is dropped, not kept for after the lock:
            // retail's lock callers leave every coordinator (Stop, the clash Begin 0x6abde0) or replace the move (Confuse),
            // which discards the formation's queued moves, and a locked unit's coordinator Move_To is dropped.
            if (movement_locked(q.id)) {
                q.orders.clear();
                continue;
            }
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
                    if (const Container* ct = t ? nullptr : team_target(o.target)) {  // q6: a TEAM container target
                        if (combat_->is_enemy(o.player, st.owner(ct->h))) exec_attack(mode, s.ids, o.target, -1, rng);
                        break;
                    }
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

bool TacticalBattle::retreat_prevented(int32_t player) const {
    // cu8 t01 (TACTICAL_GAPS §1.5, BATTLE_TRANSITIONS §3.4 request check): an interdict / Retreat_Prevention entry that
    // targets the player refuses the request (0x282750 space / 0x2825e0 land).
    if (abilities_ && abilities_->retreat_blocked(player)) return true;
    // cu8f t07 (CU8F_T01_T02_T07_RE.md §3.2): space only, the fallback query 0x2a9ff0 - behaviour 0x2C
    // TACTICAL_SUPER_WEAPON in the requesting player's own object list, no distance or hostility test: a player who owns
    // such an object in the battle (FoC: Death_Star, Death_Star_II) cannot retreat (reason 6). Land has no such query.
    // [port] live (not dead, not retreated) battle units, current owner.
    if (!space_ || !combat_) return false;
    for (const Unit& u : units_) {
        if (!u.alive || u.retreated || !bt_is_super_weapon(u.type)) continue;
        int32_t owner = -1;
        if (combat_->has_unit(u.id)) {
            owner = combat_->owner(u.id);
        } else if (l1_mode_ && l1_mode_->primary().alive(u.h)) {
            owner = l1_mode_->primary().owner(u.h);
        }
        if (owner == player) return true;
    }
    return false;
}

bool TacticalBattle::request_retreat(int32_t player) {
    const int32_t side = player == req_.info.defender ? 0 : player == req_.info.attacker ? 1 : -1;
    if (side < 0 || finished_ || vm_declared_ || frame_ < retreat_allowed_frame_ || retreat_side_ >= 0) return false;
    if (retreat_prevented(player)) return false;
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
        if (movement_) (void)movement_->remove_unit(c.id);  // s78n t03: a squad container's own mover
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
        else {
            (void)movement_->remove_unit(u.id);  // s78n t23 step 0: a registered mine obstacle (no-op otherwise)
            movement_->object_removed(u.id);     // s78f review
        }
#ifdef ALAMO_SIM_HAVE_AI_TACTICAL
        if (u.tracked) damage_tracking_.disable(u.id);
#endif
        unit_gone(mode, u);
        // [port] with an end hold the map stays on screen: the ship is gone (no jump-out flight yet), not left frozen there.
        if (cfg_.end_hold_frames > 0) mode.destroy(u.h);
    }
    retreat_complete_ = true;
    vm_retreat_complete(side_player(side));  // story t06: 0x340750 (holds released, 0xc tested)
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
    if (!side || finished_ || vm_declared_ || frame_ < retreat_allowed_frame_ || retreat_side_ >= 0) return false;
    if (retreat_prevented(player)) return false;  // cu8 t01
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

std::vector<TacticalBattle::RemovalRecord> TacticalBattle::unit_losses() const {
    std::vector<RemovalRecord> out;
    for (const RemovalRecord& r : bt_removals_) {
        if (r.survived) continue;
        const bool team = std::any_of(containers_.begin(), containers_.end(), [&r](const Container& c) { return c.type == r.object_type; });
        if (!team) out.push_back(r);
    }
    return out;
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

void TacticalBattle::apply_speed_factors() {
    // MOVEMENT 3.4 items 4, 5, 7 (product order: Mod_Multiplier, engines, list 5) [port: one factor].
    const float engines = catalog_->engines_disabled_speed_modifier();
    // s78k t03 (TACTICAL_COMBAT §4.4 "Who the squad leader is", MOVEMENT §3.4 / §3.7, FIGHTER_FLYING §4.11 item 4): a TEAM
    // container's locomotor runs at the container's effective max speed - the minimum member Max_Speed (0x4049e0) x the
    // container's factors, whose SPEED Mod_Multiplier is the squad value 0x398010 returns for the container
    // (team_mod_multiplier, keyed by the container id: land teams and space squadrons alike, no mode gate). [port] The
    // container has no mover, so the movement layer applies it (set_team): a land member walks at most at that speed, a
    // squadron's formation speed is that speed. Every member keeps its own product for item 4 (a member never takes the
    // squad value; this replaces s78j t03's stand-in that gave members the team value). The container's engines / list 5
    // are not modelled (the port's containers take neither), so its factor is the Mod_Multiplier alone. One value per
    // team per tick (sorted (team, value) pairs, probed only).
    std::vector<std::pair<uint32_t, float>> team_speed;
    // s78n t14 (FIGHTER_FLYING §7.3 item 6): the land squads with a current target (0x397a40(K, 0) [M]: the container's,
    // else a member's - members read their head's target), ascending container id.
    std::vector<mv::ObjectId> targeted;
    const auto team_mult = [&](uint32_t team) {
        auto it = std::lower_bound(team_speed.begin(), team_speed.end(), team,
                                   [](const std::pair<uint32_t, float>& e, uint32_t t) { return e.first < t; });
        if (it == team_speed.end() || it->first != team)
            it = team_speed.insert(it, {team, combat_->team_mod_multiplier(team, tc::ModKind::Speed)});
        return it->second;
    };
    for (const Unit& u : units_) {
        if (!u.alive || u.retreated || !u.movable) continue;
        float f = combat_->mod_multiplier(u.id, tc::ModKind::Speed);
        if (u.engines_offline || l1_engines_off(u.id)) f = f * engines;  // L1 systems: + timed engine disables
        f = f * (1.0f + combat_->modifier(u.id, tc::ModList::MaxSpeed));
        // OI follow-up t03 (A1_READINGS §28.9): the movement lock does not slow the unit (no speed-0 freeze); its users
        // stop it first and new move orders skip it (movement_locked).
        if (combat_->stunned(u.id, frame_)) f = 0.0f;  // OI abilities: land stun suspends the locomotor (0x647290)
        movement_->set_speed_factor(u.id, f);
        movement_->set_speed_override(u.id, abilities_->speed_override(u.id));  // OI host: Berserker_Speed (-1 = none)
        const bool in_team = u.container < containers_.size() && containers_[u.container].team && !containers_[u.container].removed;
        const auto team = in_team ? static_cast<uint32_t>(containers_[u.container].id) : 0u;
        // s78l t01 (MOVEMENT §3.4 "Team cap, exact"): the container type's raw Max_Speed is in the team minimum (-1: no
        // tag / not in a team). [port] the container's further chain factors have no host state here (fleeing is the movement
        // layer's, s78n t14 part B).
        float cmax = -1.0f;
        if (in_team && containers_[u.container].type < catalog_->types().size()) {
            const auto ct = static_cast<objects::TypeIndex>(containers_[u.container].type);
            if (!catalog_->types().get_string(ct, "Max_Speed").empty()) cmax = catalog_->types().get_float(ct, "Max_Speed");
        }
        movement_->set_team(u.id, in_team ? containers_[u.container].id : mv::ObjectId{-1}, in_team ? team_mult(team) : 1.0f, cmax);
        if (in_team && !space_ && combat_ && combat_->has_unit(u.id) && combat_->current_target(u.id) != tc::kNoObject)
            targeted.push_back(containers_[u.container].id);
    }
    std::sort(targeted.begin(), targeted.end());
    // s78n t03: a movement-backed land squad container walks at the team speed - the same value its members' set_team
    // carries (the members' minimum with its own Max_Speed, x its factor).
    for (const Container& c : containers_) {
        if (space_ || !c.team || c.removed || !movement_->has_unit(c.id)) continue;
        float cmax = -1.0f;
        if (c.type < catalog_->types().size()) {
            const auto ct = static_cast<objects::TypeIndex>(c.type);
            if (!catalog_->types().get_string(ct, "Max_Speed").empty()) cmax = catalog_->types().get_float(ct, "Max_Speed");
        }
        movement_->set_team(c.id, c.id, team_mult(static_cast<uint32_t>(c.id)), cmax);
        // s78n t14 (FIGHTER_FLYING §7.2 / §7.3): the squad's SPREAD_OUT state (a change re-lays its slots) and whether it has
        // a target (its members then do not recover to their slots).
        (void)movement_->set_team_spread_out(c.id, abilities_ && abilities_->spread_out(c.id));
        const bool tgt = (combat_ && combat_->has_unit(c.id) && combat_->current_target(c.id) != tc::kNoObject) ||
                         std::binary_search(targeted.begin(), targeted.end(), c.id);
        (void)movement_->set_team_target(c.id, tgt);
    }
    feed_pair_gates();
}

// s78n t18 (MOVEMENT §5.3 "Pair gates"): the garrison-host gate and "hidden" of the land contact tests
// (MovementSystem::set_pair_gate). A unit walking to / entering a host takes no collision object with that host; a
// squad garrisons as its container (FIGHTER_FLYING §7.4): its host is its first live member's, and while every live
// member is inside the container is hidden (no layer object, it does not walk, it takes no order). The host a unit
// has left is kept by set_garrison_exit (s4_leave / s4_apply) until its next order.
// s78n t18 review: the movement system does not save these, and its move() reads "hidden" in the command phase. They are
// a function of saved state only (units, containers, the garrison system), so they are set before the movement tick
// (apply_speed_factors), after every batch of garrison events (s4_apply), at the end of the battle tick (a death or a
// retreat changes who is "every live member") and after a load (read_part): an order given right after a load meets
// the same gates as it would have in the saving run.
void TacticalBattle::feed_pair_gates() {
    if (space_ || !movement_) return;
    namespace tg = alamo::tactical::garrison;
    const auto host_of = [](tg::ObjectId h) { return h == tg::kNoObject ? mv::ObjectId{-1} : static_cast<mv::ObjectId>(h); };
    std::vector<tg::ObjectId> link(containers_.size(), tg::kNoObject);
    std::vector<uint8_t> outside(containers_.size(), 0), inside(containers_.size(), 0);
    for (const Unit& u : units_) {
        if (!u.alive || u.retreated) continue;
        const tg::ObjectId in = garrison_.holder_of(u.id), pend = garrison_.pending_holder(u.id);
        if (u.movable && movement_->has_unit(u.id)) (void)movement_->set_pair_gate(u.id, host_of(pend), false);
        if (u.container >= containers_.size()) continue;
        const Container& c = containers_[u.container];
        if (!c.team || c.removed) continue;
        (in == tg::kNoObject ? outside : inside)[u.container] = 1;
        const tg::ObjectId h = in != tg::kNoObject ? in : pend;
        if (h != tg::kNoObject && link[u.container] == tg::kNoObject) link[u.container] = h;
    }
    for (size_t i = 0; i < containers_.size(); ++i) {
        const Container& c = containers_[i];
        if (!c.team || c.removed || !movement_->has_unit(c.id)) continue;
        (void)movement_->set_pair_gate(c.id, host_of(link[i]), inside[i] != 0 && outside[i] == 0);
    }
}

// s78n t20 (MOVEMENT §5.4.3 rule 4, §5.4.4 item 1, §5.4.5 items 1 / 3; MovementSystem::set_collision_info). The current
// target 0x397a40 of a single unit is its combat target (ordered, else acquired); of a squad container its own, else its
// first live member's that has one (team index = id order). A target that is a member of a movement-backed squad is
// named by its container (the layer object the collision records see), at the member's position. Attack range 0x3749d0 =
// Get_Max_Attack_Range; "short-range" 0x372210 = melee_attacker; the target's radius 0x397780 is added [port] for every
// unit the combat system knows (the weapon-list test of 0x685e10 is [M]). A container reads its targeting member, else
// its first live member, for the range and the melee test. Hero clash: the unit's own flag (+0x3b4).
void TacticalBattle::feed_collision_info(const objects::ObjectStore& store) {
    if (space_ || !movement_ || !combat_) return;
    using Info = mv::MovementSystem::CollisionInfo;
    const auto fill = [&](objects::ObjectId src, Info& ci) {
        if (!combat_->has_unit(src)) return;
        ci.attack_range = combat_->attack_range(src);
        ci.short_range = combat_->melee_attacker(src);
        const tc::ObjectId t = combat_->current_target(src);
        const Unit* tu = t != tc::kNoObject ? unit_by_id(t) : nullptr;
        if (!tu || !tu->alive || !store.alive(tu->h)) return;
        const Vec3 tp = store.position(tu->h);
        ci.target = tu->id;
        if (tu->container < containers_.size()) {
            const Container& tk = containers_[tu->container];
            if (tk.team && !tk.removed && movement_->is_land_team_container(tk.id)) ci.target = tk.id;
        }
        ci.target_pos = {tp.x, tp.y};
        ci.target_radius = movement_->collision_radius(ci.target);
    };
    for (const Unit& u : units_) {
        if (!u.alive || u.retreated || !u.movable || !movement_->has_unit(u.id)) continue;
        Info ci;
        fill(u.id, ci);
        ci.hero_clash = combat_->has_unit(u.id) && (combat_->flags(u.id) & tc::kUnitHeroClash) != 0;
        (void)movement_->set_collision_info(u.id, ci);
    }
    std::vector<objects::ObjectId> first(containers_.size(), objects::kNoObjectId), aiming(containers_.size(), objects::kNoObjectId);
    for (const Unit& u : units_) {
        if (!u.alive || u.retreated || u.container >= containers_.size()) continue;
        if (first[u.container] == objects::kNoObjectId) first[u.container] = u.id;
        if (aiming[u.container] == objects::kNoObjectId && combat_->has_unit(u.id) && combat_->current_target(u.id) != tc::kNoObject)
            aiming[u.container] = u.id;
    }
    for (size_t i = 0; i < containers_.size(); ++i) {
        const Container& c = containers_[i];
        if (!c.team || c.removed || !movement_->is_land_team_container(c.id)) continue;
        Info ci;
        if (combat_->has_unit(c.id) && combat_->current_target(c.id) != tc::kNoObject) fill(c.id, ci);
        else if (aiming[i] != objects::kNoObjectId) fill(aiming[i], ci);
        else if (first[i] != objects::kNoObjectId) fill(first[i], ci);
        ci.hero_clash = combat_->has_unit(c.id) && (combat_->flags(c.id) & tc::kUnitHeroClash) != 0;
        (void)movement_->set_collision_info(c.id, ci);
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
        // S7 t37 / s78f t02 (A1_READINGS §33.4.1 step 3, FIGHTER_FLYING §4.11 D 2): T = the type of the first team member
        // (a fighter); Space_Layer 0 or Disregard_Space_Layer_For_Guard_Attack_Move -> its guard of a moving object
        // follows it. The chase ranges (0x4165f0's u = the container) are the squadron type's.
        if (const Unit* m0 = unit_by_id(d.members.front()); m0 && m0->type < types.size()) {
            const objects::TypeIndex mt = static_cast<objects::TypeIndex>(m0->type);
            d.guard_follow = types.get_mask(mt, "Space_Layer") == 0 || types.get_bool(mt, "Disregard_Space_Layer_For_Guard_Attack_Move");
        }
        if (ct.squad_type < types.size()) d.chase = mv::fighter_chase_ranges_from_type(types, ct.squad_type);
        movement_->set_fighter_squad(d);
    }
    // S7 t22: a formation goes with its squadron (or fighter).
    std::erase_if(fformations_, [this](const FighterFormationRecord& r) {
        if (movement_->fighter_squad(r.key)) return false;
        const Unit* x = unit_by_id(r.key);
        return !x || !x->alive || x->retreated || !movement_->has_unit(x->id);
    });
    // S7 t25 / t37 / s78f t02 (§4.10 E, the coordinator's target listener 0x4f77e0 on signals 0x10 / 0x1a / 0x2e): a
    // guarded object that is gone turns the guard block into a position block (kind 1, object null); the flags stay 0x02,
    // so the squadron keeps a position guard with Guard_Chase_Range. The movement layer's formation (mv::FighterFormation)
    // does the conversion at the object's removal (position = the last waypoint of the formation's path). s78f review: the
    // record follows the formation's CURRENT block that is not an auto block (fighter_order_view: the order's own block -
    // e.g. the guard while it is on top - keeps the order's values; a queued waypoint takes over once it is popped), not
    // blocks[0]. A dead object still in the movement layer drops the record's object.
    for (FighterFormationRecord& r : fformations_) {
        if (movement_->fighter_formation(r.key)) {  // the record = the view (unchanged while the order's block is current)
            const FighterOrderView v = fighter_order_view(r);
            r.flags = v.flags;
            r.goal = v.goal;
            r.goal_object = v.goal_object;
        }
        if (r.goal_object == objects::kNoObjectId || movement_->fighter_squad(r.goal_object)) continue;
        const Unit* g = unit_by_id(r.goal_object);
        if (!g || !g->alive || g->retreated) r.goal_object = objects::kNoObjectId;
    }
}

// The order's own block (the one Move_To built) carries no 0x08 and sits above every queued block (the waypoint append
// inserts at the bottom; a patrol block re-queued by a pop gets 0x08): while it is the current block the record's values
// stand for it [port: not every host order path hands the movement layer its block flags / object]; a queued block (0x08)
// is read from the stack. A dead object the movement layer already converted gives the conversion point.
FighterOrderView TacticalBattle::fighter_order_view(const FighterFormationRecord& r) const {
    FighterOrderView v;
    v.first_flags = v.flags = r.flags;
    v.goal = r.goal;
    v.goal_object = r.goal_object;
    const mv::FighterFormation* f = movement_ ? movement_->fighter_formation(r.key) : nullptr;
    if (!f || f->blocks.empty()) return v;
    constexpr uint8_t kMask = mv::kMoveFlagAttackMove | mv::kMoveFlagGuardMove;
    const auto queued = [](const mv::FighterBlock& b) { return (b.flags & mv::kMoveFlagQueue) != 0; };
    if (queued(f->blocks.front())) v.first_flags = static_cast<uint8_t>(f->blocks.front().flags & kMask);
    const mv::FighterBlock* c = &f->blocks.front();  // the bottom block is never an auto block (Push keeps it)
    for (size_t i = f->blocks.size(); i-- > 0;)
        if (!f->blocks[i].pushed) {
            c = &f->blocks[i];
            break;
        }
    if (queued(*c)) {
        v.flags = static_cast<uint8_t>(c->flags & kMask);
        v.goal = Vec3{c->point.x, c->point.y, 0.0f};  // an object block: the order-time point; readers follow the object
        v.goal_object = c->kind == mv::kFighterBlockObject ? c->object : objects::kNoObjectId;
    } else if (c->kind == mv::kFighterBlockPosition && r.goal_object != objects::kNoObjectId) {
        const auto it = std::lower_bound(units_.begin(), units_.end(), r.goal_object,
                                         [](const Unit& x, objects::ObjectId id) { return x.id < id; });
        const bool live = it != units_.end() && it->id == r.goal_object && it->alive && !it->retreated;
        const std::span<const mv::FighterSquad> sq = movement_->fighter_squads();
        const auto si = std::lower_bound(sq.begin(), sq.end(), r.goal_object, [](const mv::FighterSquad& q, objects::ObjectId id) { return q.id < id; });
        if (!live && !(si != sq.end() && si->id == r.goal_object)) {
            v.goal = Vec3{c->point.x, c->point.y, 0.0f};
            v.goal_object = objects::kNoObjectId;
        }
    }
    return v;
}

std::vector<objects::ObjectId> TacticalBattle::fighter_pushed(objects::ObjectId key) const {
    std::vector<objects::ObjectId> out;
    if (!movement_) return out;
    if (const mv::FighterFormation* f = movement_->fighter_formation(key))
        for (const mv::FighterBlock& b : f->blocks)
            if (b.pushed && b.kind == mv::kFighterBlockObject) out.push_back(b.object);
    return out;
}

// s78f t02 (FIGHTER_FLYING §4.11 C / D / E): the movement layer's fighter formations read the fog (FOW_Object_Hidden with
// force 1, 0x35f470), an object's current attack target (0x397a40; a squadron container: its first member's, the shared
// team target [port]), the member range test (TARGETING slots 43 / 46: not inside the min range, centre distance <= the
// max attack range [M: no target radius]) and the positions of objects that are not movers (stations, structures).
void TacticalBattle::fighter_formation_hooks() {
    mv::FighterFormationHooks h;
    h.hidden = [this](int32_t owner, mv::ObjectId id) { return fog_object_hidden(owner, id, true); };
    h.attack_target = [this](mv::ObjectId id) -> mv::ObjectId {
        const objects::ObjectId cu = fighter_combat_unit(id);
        if (cu == objects::kNoObjectId || !combat_ || !combat_->has_unit(cu)) return -1;
        const objects::ObjectId t = combat_->current_target(cu);
        return t == objects::kNoObjectId ? -1 : t;
    };
    h.in_range = [this](mv::ObjectId m, mv::ObjectId o) {
        const objects::ObjectId ou = fighter_combat_unit(o);
        if (!combat_ || !combat_->has_unit(m) || ou == objects::kNoObjectId || !combat_->has_unit(ou)) return false;
        if (combat_->inside_min_range(m, ou)) return false;
        const tactical::Vec3 a = combat_->position(m), b = combat_->position(ou);
        const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
        const float r = combat_->attack_range(m);
        return (dx * dx + dy * dy) + dz * dz <= r * r;
    };
    h.position = [this](mv::ObjectId id, mv::F3& p) {
        // s78f review: a dead or retreated object (combat keeps dead units until its compaction) has no position, so a
        // block naming it is dropped (the death itself also reaches MovementSystem::object_removed).
        const Unit* x = unit_by_id(id);
        if (!combat_ || !combat_->has_unit(id) || !x || !x->alive || x->retreated) return false;
        const tactical::Vec3 v = combat_->position(id);
        p = {v.x, v.y, v.z};
        return true;
    };
    movement_->set_fighter_formation_hooks(std::move(h));
}

// FIGHTER_FLYING §2 step 6 / §4.3 step 2 / §4.9: the formation attack target Formation_Attack_Target(F, &hp, -1)
// (0x41a2e0, TACTICAL_COMBAT §2.8: newest valid block first; a block qualifies with an object hostile to the leader's
// owner, guard blocks never). The fighter states read only this goal, never the combat target (§4.9). A squadron's
// target comes from its leader. fighter_formation_target (tactical_battle.hpp) picks it:
//   1. the ordered combat target (Attack / Attack_Target, the set path of a task-force Attack_Move, path B's lock, the
//      locomotor's own Provoke / Retarget orders) [port: the ordered slot stands for the order's object block and the
//      locked team target; while it is set the team target is locked, so §4.9 step 2 pushes no auto block];
//   2. else the newest pushed auto attack block that is live and hostile (S7 t22: §4.9 step 2, fighter_push - the only
//      way an auto target moves a squadron; it is newer than the order's own block);
//   3. else the attack goal (the order's object block: Attack_Move(object)) when hostile to the leader's owner;
//   4. else none: 0x41a2e0 finds no object (a position, guard or no formation) and the squadron keeps Moving (§4.1) or
//      parks (§4.2); its weapons still fire at targets of opportunity (combat).
// S7 t12's "moving -> none" rule and t12's idle "combat auto target unless hidden" stand-in are gone: a plain Move_To
// does not peel off at enemies along the way because the chase test measures Idle_Chase_Range from its destination
// (fighter_chase_test rule 5), an Attack_Move(position) does (Attack_Move_Response_Range, rule 4).
// A target that is a member of a registered squadron is that squadron (a team: dogfight §4.5); otherwise the object with
// its combat radius (0x397780) and, for an ordered hard point, the hard point's position (0x396ce0: the combat attachment
// point, unit-local x forward / z up). hidden = FOW_Object_Hidden for the attacker's owner with force 1 (0x35f470): no
// filter here, so a fogged goal toggles Idle <-> Directed_Combat every frame as in retail (§4.9 step 3). No target clears
// it (the locomotor then retargets from its dogfight cell and reports Retarget).
objects::ObjectId fighter_formation_target(const FighterTargetInputs& in) {
    if (in.ordered != objects::kNoObjectId) return in.ordered;
    if (in.pushed != objects::kNoObjectId) return in.pushed;
    if (in.goal != objects::kNoObjectId && in.goal_hostile) return in.goal;
    return objects::kNoObjectId;
}

uint8_t fighter_chase_test(const FighterChaseInputs& in) {
    if (!in.formation || in.candidate_is_goal) return 0;  // rule 1
    float rr = (in.flags & mv::kMoveFlagGuardMove) != 0    ? in.guard_range
               : (in.flags & mv::kMoveFlagAttackMove) != 0 ? in.attack_move_range
                                                           : in.idle_range;  // rule 2
    if (rr < 0.0f) rr = 0.0f;
    uint8_t base = 0;
    if (in.candidate_attacker) {  // rule 3
        rr += in.extension;
        base = 1;
    } else if (in.guarded_moving) {
        return 0;
    }
    if (in.moving && (in.flags & mv::kMoveFlagAttackMove) != 0) return 2;  // rule 4 [M: within Rr of its own path point]
    if (!in.last_guard && !in.single_position) return 0;                    // rule 5
    const auto within = [](Vec3 a, Vec3 b, float r) {
        const float dx = a.x - b.x, dy = a.y - b.y;
        return dx * dx + dy * dy <= r * r;
    };
    if (in.moving && !within(in.unit, in.goal, rr)) return 0;  // rule 6 (state 3 skips it)
    if (within(in.candidate, in.goal, rr)) return 1;           // rule 7
    return within(in.candidate, in.goal, rr + in.attack_range) ? static_cast<uint8_t>(base + 1) : 0;
}

const FighterFormationRecord* TacticalBattle::fighter_formation(objects::ObjectId key) const {
    const auto it = std::lower_bound(fformations_.begin(), fformations_.end(), key,
                                     [](const FighterFormationRecord& r, objects::ObjectId k) { return r.key < k; });
    return it != fformations_.end() && it->key == key ? &*it : nullptr;
}

FighterFormationRecord* TacticalBattle::fighter_formation_mut(objects::ObjectId key) {
    return const_cast<FighterFormationRecord*>(std::as_const(*this).fighter_formation(key));
}

objects::ObjectId TacticalBattle::fighter_key(const Unit& u) const {
    if (u.container < containers_.size() && containers_[u.container].team) return containers_[u.container].id;
    return u.id;
}

objects::ObjectId TacticalBattle::fighter_goal_key(objects::ObjectId id) {
    const Unit* x = id != objects::kNoObjectId ? unit_by_id(id) : nullptr;
    if (x && x->container < containers_.size() && movement_->fighter_squad(containers_[x->container].id)) return containers_[x->container].id;
    return id;
}

bool TacticalBattle::fighter_goal_live(objects::ObjectId key, const objects::ObjectStore& store) {
    if (key == objects::kNoObjectId) return false;
    if (const mv::FighterSquad* s = movement_->fighter_squad(key)) return !s->members.empty();
    const Unit* x = unit_by_id(key);
    return x && x->alive && !x->retreated && store.alive(x->h) && combat_->has_unit(x->id);
}

// A move / attack order replaces the formation of every fighter squadron it names (one record per squadron).
void TacticalBattle::fighter_order(std::span<const objects::ObjectId> ids, uint8_t flags, Vec3 goal, objects::ObjectId goal_object) {
    if (!space_ || !movement_) return;
    for (const objects::ObjectId id : ids) {
        const Unit* u = unit_by_id(id);
        if (!u || !u->alive || u->retreated || !movement_->has_unit(id) || movement_->view(id).locomotor != mv::LocomotorKind::Fighter) continue;
        const objects::ObjectId key = fighter_key(*u);
        FighterFormationRecord* r = fighter_formation_mut(key);
        if (!r) {
            const auto it = std::lower_bound(fformations_.begin(), fformations_.end(), key,
                                             [](const FighterFormationRecord& a, objects::ObjectId k) { return a.key < k; });
            FighterFormationRecord nr;
            nr.key = key;
            r = &*fformations_.insert(it, std::move(nr));
        }
        r->flags = static_cast<uint8_t>(flags & (mv::kMoveFlagAttackMove | mv::kMoveFlagGuardMove));
        r->goal = goal;
        r->goal_object = goal_object;
    }
}

void TacticalBattle::fighter_order_clear(std::span<const objects::ObjectId> ids) {
    for (const objects::ObjectId id : ids)
        if (const Unit* u = unit_by_id(id)) {
            const objects::ObjectId key = fighter_key(*u);
            std::erase_if(fformations_, [key](const FighterFormationRecord& r) { return r.key == key; });
        }
}

// §4.9 step 2 (Service_Space -> Auto_Engage, LocomotorCommon 0x42d0f0 on the container's pack). t = the squadron's
// unlocked auto target: none while the leader holds a live ordered (locked team) target; else the first member's in
// team index order (the squadron shares the first member's pick, 0x406370 [port: combat keeps per-member auto targets]).
// t (as its squadron when it is a team member) is pushed as the newest auto block when it is not the formation's goal
// (newest valid block, -1) already and fighter_chase_test passes [port: rr = 1's detach into a formation of its own is
// moot, the record is per squadron; the mode's hold table 0x4944e0 is not modelled]. No formation: nothing is pushed.
// s78f t02 (FIGHTER_FLYING §4.11 B 3): the block goes into the movement layer's block stack (MovementSystem::fighter_push,
// 0x41fd30 with its pre-pop: it replaces an earlier auto block, never a guard); the movement layer pops it when the leash
// (0x4165f0) fails or its object is hidden, so the auto blocks are read back from there (fighter_pushed).
objects::ObjectId TacticalBattle::fighter_push(const Unit& leader, objects::ObjectId key, const objects::ObjectStore& store) {
    FighterFormationRecord* r = fighter_formation_mut(key);
    if (!r) return objects::kNoObjectId;
    const int32_t me = store.owner(leader.h);
    const auto hostile = [&](objects::ObjectId k) {
        const objects::ObjectId cu = fighter_combat_unit(k);
        return cu != objects::kNoObjectId && combat_->has_unit(cu) && combat_->is_enemy(me, combat_->owner(cu));
    };
    const auto newest = [&]() -> objects::ObjectId {
        const std::vector<objects::ObjectId> pushed = fighter_pushed(key);
        for (auto it = pushed.rbegin(); it != pushed.rend(); ++it)
            if (fighter_goal_live(*it, store) && hostile(*it)) return *it;
        return objects::kNoObjectId;
    };
    const auto top_pushed = [&]() -> objects::ObjectId {  // the formation's top block when it is a live auto block
        const mv::FighterFormation* f = movement_->fighter_formation(key);
        if (!f || f->blocks.empty() || !f->blocks.back().pushed) return objects::kNoObjectId;
        return fighter_goal_live(f->blocks.back().object, store) ? f->blocks.back().object : objects::kNoObjectId;
    };
    if (combat_->has_unit(leader.id) && fighter_goal_live(combat_->ordered_target(leader.id), store)) return newest();
    std::span<const objects::ObjectId> members(&leader.id, 1);
    const mv::FighterSquad* sq = movement_->fighter_squad(key);
    if (sq) members = sq->members;
    objects::ObjectId t = objects::kNoObjectId;
    bool any_target = false;
    for (const objects::ObjectId m : members) {
        if (!combat_->has_unit(m)) continue;
        any_target = any_target || combat_->current_target(m) != objects::kNoObjectId;
        if (combat_->ordered_target(m) != objects::kNoObjectId) continue;
        const objects::ObjectId a = combat_->auto_target(m);
        if (a != objects::kNoObjectId && fighter_goal_live(a, store)) {
            t = a;
            break;
        }
    }
    if (t == objects::kNoObjectId) {
        // S7 t25 (§4.9 step 2, LocomotorCommon 0x42d0f0): Auto_Engage(null) with a formation goal -> 0x4105a0 [M: ends the
        // pushed block] - the squadron's members hold no target at all (dropped by enter_idle, fog, death, an owner
        // change): the newest auto block goes (dead blocks were erased above), so a fogged or converted pushed goal does
        // not hold the squadron.
        if (!any_target && top_pushed() != objects::kNoObjectId) movement_->fighter_pop(key);
        return newest();
    }
    const objects::ObjectId tk = fighter_goal_key(t);
    if (!hostile(tk)) return newest();  // an owner change: the targeting service drops it at its next due frame
    objects::ObjectId g = newest();
    const objects::ObjectId order_goal = fighter_goal_key(combat_->attack_goal(leader.id));
    if (g == objects::kNoObjectId && fighter_goal_live(order_goal, store) && hostile(order_goal)) g = order_goal;
    if (tk == g) return g;
    // S7 t25 (§4.10 C): Auto_Engage reaches each member's fighter locomotor (TeamLocomotor 0x630140 -> vf +0x260
    // 0x5cb580) after the container's own push: t a squadron, the member's lock timer expired -> lock the first member of
    // t's squadron (index order) whose own timer expired; both timers 10 s, the victim's own lock dropped. No block.
    if (const mv::FighterSquad* ts = sq ? movement_->fighter_squad(tk) : nullptr) {
        const std::vector<objects::ObjectId> mine(sq->members.begin(), sq->members.end());
        const std::vector<objects::ObjectId> theirs(ts->members.begin(), ts->members.end());
        for (const objects::ObjectId m : mine) {
            mv::FighterState* fs = movement_->fighter_state(m);
            if (!fs || fs->lock_start + fs->lock_frames > frame_) continue;
            for (const objects::ObjectId x : theirs) {
                mv::FighterState* xs = movement_->fighter_state(x);
                if (!xs || xs->lock_start + xs->lock_frames > frame_) continue;
                fs->lock = x;
                fs->lock_start = frame_;
                fs->lock_frames = mv::kFighterLockFrames;
                xs->lock = -1;
                xs->lock_start = frame_;
                xs->lock_frames = mv::kFighterLockFrames;
                break;
            }
        }
    }
    const mv::UnitView lv = movement_->view(leader.id);
    const objects::ObjectId last_push = top_pushed();
    const bool live_push = last_push != objects::kNoObjectId;  // a live pushed auto block is the last block
    const FighterOrderView ov = fighter_order_view(*r);           // s78f review: the movement stack's order block
    FighterChaseInputs ci;
    ci.formation = true;
    ci.flags = ov.first_flags;  // Rr and the attack-move branch read the first block
    ci.moving = lv.has_goal && !lv.arrived;
    ci.last_guard = (ov.flags & mv::kMoveFlagGuardMove) != 0 && !live_push;
    ci.single_position = !ci.moving && !live_push && ov.goal_object == objects::kNoObjectId && !fighter_goal_live(order_goal, store);
    ci.candidate_is_goal = tk == order_goal || tk == fighter_goal_key(ov.goal_object) || (live_push && tk == last_push);
    // Rule 3: the guarded object (a guard block's object) and whether c attacks it or the squadron.
    const Unit* guarded = (ov.flags & mv::kMoveFlagGuardMove) != 0 && ov.goal_object != objects::kNoObjectId ? unit_by_id(ov.goal_object) : nullptr;
    if (guarded && (!guarded->alive || guarded->retreated || !store.alive(guarded->h))) guarded = nullptr;
    if (guarded && movement_->has_unit(guarded->id)) {
        const mv::UnitView gv = movement_->view(guarded->id);
        ci.guarded_moving = gv.has_goal && gv.speed > 0.0f;
    }
    {
        std::vector<objects::ObjectId> attackers;  // c, or every member of c's squadron
        if (const mv::FighterSquad* cs = movement_->fighter_squad(tk)) attackers.assign(cs->members.begin(), cs->members.end());
        else attackers.push_back(t);
        for (const objects::ObjectId a : attackers) {
            if (!combat_->has_unit(a)) continue;
            const objects::ObjectId at = combat_->current_target(a);
            if (at == objects::kNoObjectId) continue;
            if ((guarded && at == guarded->id) || std::find(members.begin(), members.end(), at) != members.end()) ci.candidate_attacker = true;
        }
    }
    ci.unit = store.position(leader.h);
    // Rr: the squadron type's (a fighter without one: its own type's); cached per type.
    const objects::GameObjectTypes& types = catalog_->types();
    uint32_t ty = leader.type;
    if (sq && leader.container < containers_.size() && containers_[leader.container].squad_type < types.size())
        ty = containers_[leader.container].squad_type;
    if (ty < types.size()) {
        if (fchase_have_.size() != types.size()) {
            fchase_have_.assign(types.size(), 0);
            fchase_cache_.assign(types.size(), {});
        }
        if (!fchase_have_[ty]) {
            const auto ti = static_cast<objects::TypeIndex>(ty);
            fchase_cache_[ty] = {types.get_float(ti, "Guard_Chase_Range"), types.get_float(ti, "Idle_Chase_Range"),
                                 types.get_float(ti, "Attack_Move_Response_Range"),
                                 types.get_float(ti, "Autonomous_Move_Extension_Vs_Attacker")};
            fchase_have_[ty] = 1;
        }
        ci.guard_range = fchase_cache_[ty][0];
        ci.idle_range = fchase_cache_[ty][1];
        ci.attack_move_range = fchase_cache_[ty][2];
        ci.extension = fchase_cache_[ty][3];
    }
    ci.attack_range = combat_->attack_range(leader.id);
    ci.goal = ov.goal;
    if (const Unit* go = ov.goal_object != objects::kNoObjectId ? unit_by_id(ov.goal_object) : nullptr;
        go && go->alive && !go->retreated && store.alive(go->h))
        ci.goal = store.position(go->h);  // an object block: the slot goal follows the object
    const Unit* tu = unit_by_id(t);
    ci.candidate = store.position(tu->h);
    if (fighter_chase_test(ci) != 0) movement_->fighter_push(key, tk);  // 0x41cf70: the auto attack block {kind 3, t, +0x5d = 1}
    return newest();
}

// S8 t17 (FIGHTER_FLYING §4.10 A, TACTICAL_COMBAT §2.2): Get_Scan_Range 0x566f50 / the per-candidate acceptance of the
// combat scan for a fighter (a squadron member: its squadron's formation). The inputs are fighter_push's for the auto
// target c; c = none: no goal / attacker match and c at the slot goal (rule 7: "c = null -> 1"). Rr = the first block's
// range (+ Autonomous_Move_Extension_Vs_Attacker for an attacker). Read-only: combat calls it from its parallel gather
// phase (nothing else runs then); the dead pushed blocks fighter_push erases are skipped instead.
uint8_t TacticalBattle::fighter_scan_range(const objects::ObjectStore& store, objects::ObjectId member, objects::ObjectId cand,
                                           float range, float& rr) const {
    rr = 0.0f;
    const auto unit_c = [this](objects::ObjectId id) -> const Unit* {
        const auto it = std::lower_bound(units_.begin(), units_.end(), id, [](const Unit& x, objects::ObjectId v) { return x.id < v; });
        return it == units_.end() || it->id != id ? nullptr : &*it;
    };
    const auto squad_c = [this](objects::ObjectId id) -> const mv::FighterSquad* {
        const std::span<const mv::FighterSquad> s = movement_->fighter_squads();
        const auto it = std::lower_bound(s.begin(), s.end(), id, [](const mv::FighterSquad& q, objects::ObjectId v) { return q.id < v; });
        return it == s.end() || it->id != id ? nullptr : &*it;
    };
    const auto key_of = [&](objects::ObjectId id) {  // fighter_goal_key
        const Unit* x = id != objects::kNoObjectId ? unit_c(id) : nullptr;
        if (x && x->container < containers_.size() && squad_c(containers_[x->container].id)) return containers_[x->container].id;
        return id;
    };
    const auto goal_live = [&](objects::ObjectId k) {  // fighter_goal_live
        if (k == objects::kNoObjectId) return false;
        if (const mv::FighterSquad* s = squad_c(k)) return !s->members.empty();
        const Unit* x = unit_c(k);
        return x && x->alive && !x->retreated && store.alive(x->h) && combat_->has_unit(x->id);
    };
    const Unit* u = unit_c(member);
    if (!u) return 0;
    const FighterFormationRecord* r = fighter_formation(fighter_key(*u));
    if (!r) return 0;
    const mv::FighterSquad* sq = squad_c(r->key);
    const objects::ObjectId lid = sq && !sq->members.empty() ? sq->members.front() : u->id;
    const Unit* leader = unit_c(lid);
    if (!leader || !movement_->has_unit(lid)) return 0;
    std::span<const objects::ObjectId> members(&leader->id, 1);
    if (sq) members = sq->members;
    objects::ObjectId last_push = objects::kNoObjectId;  // s78f t02: the movement formation's top, when a live auto block
    if (const mv::FighterFormation* ff = movement_->fighter_formation(r->key); ff && !ff->blocks.empty() && ff->blocks.back().pushed &&
                                                                               goal_live(ff->blocks.back().object))
        last_push = ff->blocks.back().object;
    const objects::ObjectId order_goal = key_of(combat_->attack_goal(lid));
    const mv::UnitView lv = movement_->view(lid);
    const FighterOrderView ov = fighter_order_view(*r);  // s78f review: the movement stack's order block
    FighterChaseInputs ci;
    ci.formation = true;
    ci.flags = ov.first_flags;
    ci.moving = lv.has_goal && !lv.arrived;
    ci.last_guard = (ov.flags & mv::kMoveFlagGuardMove) != 0 && last_push == objects::kNoObjectId;
    ci.single_position = !ci.moving && last_push == objects::kNoObjectId && ov.goal_object == objects::kNoObjectId && !goal_live(order_goal);
    const Unit* guarded = (ov.flags & mv::kMoveFlagGuardMove) != 0 && ov.goal_object != objects::kNoObjectId ? unit_c(ov.goal_object) : nullptr;
    if (guarded && (!guarded->alive || guarded->retreated || !store.alive(guarded->h))) guarded = nullptr;
    if (guarded && movement_->has_unit(guarded->id)) {
        const mv::UnitView gv = movement_->view(guarded->id);
        ci.guarded_moving = gv.has_goal && gv.speed > 0.0f;
    }
    ci.unit = store.position(leader->h);
    ci.goal = ov.goal;
    if (const Unit* go = ov.goal_object != objects::kNoObjectId ? unit_c(ov.goal_object) : nullptr;
        go && go->alive && !go->retreated && store.alive(go->h))
        ci.goal = store.position(go->h);
    ci.attack_range = range;
    uint32_t ty = leader->type;
    if (sq && leader->container < containers_.size() && containers_[leader->container].squad_type < catalog_->types().size())
        ty = containers_[leader->container].squad_type;
    if (ty < fchase_have_.size() && fchase_have_[ty]) {
        ci.guard_range = fchase_cache_[ty][0];
        ci.idle_range = fchase_cache_[ty][1];
        ci.attack_move_range = fchase_cache_[ty][2];
        ci.extension = fchase_cache_[ty][3];
    } else if (ty < catalog_->types().size()) {
        const objects::GameObjectTypes& types = catalog_->types();
        const auto ti = static_cast<objects::TypeIndex>(ty);
        ci.guard_range = types.get_float(ti, "Guard_Chase_Range");
        ci.idle_range = types.get_float(ti, "Idle_Chase_Range");
        ci.attack_move_range = types.get_float(ti, "Attack_Move_Response_Range");
        ci.extension = types.get_float(ti, "Autonomous_Move_Extension_Vs_Attacker");
    }
    if (cand == objects::kNoObjectId) {
        ci.candidate = ci.goal;
    } else {
        const Unit* c = unit_c(cand);
        if (!c || !store.alive(c->h)) return 0;
        const objects::ObjectId tk = key_of(cand);
        ci.candidate_is_goal = tk == order_goal || tk == key_of(ov.goal_object) || tk == last_push;
        std::span<const objects::ObjectId> attackers(&c->id, 1);
        if (const mv::FighterSquad* cs = squad_c(tk)) attackers = cs->members;
        for (const objects::ObjectId a : attackers) {
            const objects::ObjectId at = combat_->has_unit(a) ? combat_->current_target(a) : objects::kNoObjectId;
            if (at == objects::kNoObjectId) continue;
            if ((guarded && at == guarded->id) || std::find(members.begin(), members.end(), at) != members.end()) ci.candidate_attacker = true;
        }
        ci.candidate = store.position(c->h);
    }
    const uint8_t res = fighter_chase_test(ci);
    if (res != 0) {
        rr = (ci.flags & mv::kMoveFlagGuardMove) != 0 ? ci.guard_range : (ci.flags & mv::kMoveFlagAttackMove) != 0 ? ci.attack_move_range : ci.idle_range;
        if (rr < 0.0f) rr = 0.0f;
        if (ci.candidate_attacker) rr += ci.extension;
    }
    return res;
}

void TacticalBattle::feed_fighter_target(const Unit& u, const objects::ObjectStore& store) {
    mv::ObjectId addr = u.id;
    if (u.container < containers_.size()) {
        const objects::ObjectId cid = containers_[u.container].id;
        if (const mv::FighterSquad* s = movement_->fighter_squad(cid)) {
            if (s->members.empty() || s->members.front() != u.id) return;
            addr = cid;
        }
    }
    const auto live = [&](objects::ObjectId id) -> const Unit* {
        const Unit* x = id != objects::kNoObjectId ? unit_by_id(id) : nullptr;
        return x && x->alive && !x->retreated && store.alive(x->h) && combat_->has_unit(x->id) ? x : nullptr;
    };
    const int32_t me = store.owner(u.h);
    FighterTargetInputs in;
    if (live(combat_->ordered_target(u.id))) in.ordered = combat_->ordered_target(u.id);
    in.pushed = fighter_push(u, addr == u.id ? fighter_key(u) : addr, store);
    if (const Unit* g = live(combat_->attack_goal(u.id))) {
        in.goal = g->id;
        in.goal_hostile = combat_->is_enemy(me, store.owner(g->h));
    }
    mv::FighterTarget ft;
    const objects::ObjectId chosen = fighter_formation_target(in);
    if (chosen != objects::kNoObjectId && movement_->fighter_squad(chosen)) {  // a pushed squadron (a team: dogfight)
        ft.id = chosen;
        ft.hidden = fog_object_hidden(me, ft.id, true);
    } else if (const Unit* t = live(chosen)) {
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
        ft.hidden = fog_object_hidden(me, ft.id, true);
    }
    movement_->set_fighter_target(addr, ft);
}

// enter_idle (§4.7 step 4, 0x5c9ca0 -> 0x406370(team, null)): the squadron's team target is dropped - every member's
// TARGETING Set_Target(null), the hard points' targets too - so the targeting service rescans on its next due frame; the
// formation and its blocks are kept (§4.7, §4.9 step 3). S7 t25 (§4.10 D): enter_idle comes from the movement tick's
// EnterIdle events, raised only at its four retail call sites (Directed_Combat NoTarget, Moving path complete); the t22
// state-transition detection (which also caught the Dead_Stop / hyperspace ends) is gone. A member's ordered target that
// stands for its order's object block (the Attack / Attack_Move object) is kept [port: the block is the ordered slot];
// Provoke / Retarget orders (team targets only in retail) are dropped.
void TacticalBattle::fighter_enter_idle() {
    if (!space_ || !movement_) return;
    std::vector<objects::ObjectId> keys;  // first-seen order (event order)
    for (const mv::FighterEvent& e : movement_->fighter_events()) {
        if (e.kind != mv::FighterEvent::EnterIdle) continue;
        const Unit* u = unit_by_id(e.who);
        if (!u) continue;
        const objects::ObjectId key = fighter_key(*u);
        if (std::find(keys.begin(), keys.end(), key) == keys.end()) keys.push_back(key);
    }
    for (const objects::ObjectId key : keys) {
        std::span<const objects::ObjectId> members(&key, 1);
        if (const mv::FighterSquad* s = movement_->fighter_squad(key)) members = s->members;
        const FighterFormationRecord* r = fighter_formation(key);
        for (const objects::ObjectId m : members) {
            if (!combat_->has_unit(m) || combat_->current_target(m) == objects::kNoObjectId) continue;
            const objects::ObjectId o = combat_->ordered_target(m);
            if (o != objects::kNoObjectId && (o == combat_->attack_goal(m) || (r && o == r->goal_object))) continue;
            combat_->clear_targets(m);
        }
    }
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
        if (e.kind == mv::FighterEvent::EnterIdle) continue;  // fighter_enter_idle
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

const TacticalBattle::PresenceTypeInfo& TacticalBattle::presence_info(uint32_t type) {
    // s78l t03: Presence_Induced_Animations entries ("<anim type>, <type>, <type>, ..."). [port] repeated tags reach the
    // port as one flat token list, so a token naming a ModelAnimType starts the next entry.
    if (presence_cache_.size() < catalog_->types().size()) presence_cache_.resize(catalog_->types().size());
    static const PresenceTypeInfo kNone{};
    if (type >= presence_cache_.size()) return kNone;
    PresenceTypeInfo& info = presence_cache_[type];
    if (info.parsed) return info;
    info.parsed = true;
#ifdef ALAMO_SIM_HAVE_ANIM
    const objects::GameObjectTypes& ty = catalog_->types();
    const auto t = static_cast<objects::TypeIndex>(type);
    info.range = ty.get_float(t, "Presence_Range");
    for (std::string_view tok : ty.get_list(t, "Presence_Induced_Animations")) {
        while (!tok.empty() && std::isspace(static_cast<unsigned char>(tok.front()))) tok.remove_prefix(1);
        while (!tok.empty() && std::isspace(static_cast<unsigned char>(tok.back()))) tok.remove_suffix(1);
        if (tok.empty()) continue;
        const int32_t a = anim::find_anim_type(tok);
        if (a >= 0) {
            info.anims.push_back({a, {}});
        } else if (!info.anims.empty()) {
            const objects::TypeIndex r = ty.resolve_name(tok);
            if (r != objects::kNone) info.anims.back().second.push_back(static_cast<uint32_t>(r));
        }
    }
#endif
    return info;
}

void TacticalBattle::imposing_presence(objects::ObjectStore& store, const std::vector<uint8_t>& gated) {
#ifdef ALAMO_SIM_HAVE_ANIM
    // s78l t03 (TACTICAL_COMBAT slot 47 "s78l t03" (c), TACTICAL_GAPS 6.2): IMPOSING_PRESENCE 0x624ab0 on its interval-30
    // schedule (the store's: first due frame drawn at the attach, TACTICAL_GAPS 6.4). Each due presence object P (alive,
    // the behaviour enabled; death clones keep it) refreshes, for every battle unit R with IDLE inside the box of half size
    // P's Presence_Range around P, not dying, whose type lists P's type in Presence_Induced_Animations, R's record of P to
    // int(FPS x Unit_Presence_Relaxation_Time). Skipped for an R with a target (TARGETING, a team member: its container's;
    // suspended or not) or that the moving gate stops. No draws; the order of the writes does not change the result.
    const GameMode* gm = hooks_ ? hooks_->anim_mode() : nullptr;
    if (!anim_ || !gm || &gm->primary() != &store) return;
    const float fr = static_cast<float>(anim::kSimTicksPerSecond) * cfg_.presence_relaxation_seconds;
    const int32_t frames = fr >= 1.0f ? (fr < 1.0e9f ? static_cast<int32_t>(fr) : 1000000000) : 0;
    if (frames < 1) return;
    anim::AnimBehaviors& beh = anim_->behaviors(*gm);
    const uint32_t mf = store.mode_frame();
    const objects::BehaviorTypeId kImposingPresence = 0x40;
    for (const objects::ObjectHandle p : store.all_objects()) {
        if (!store.alive(p)) continue;
        const uint32_t bi = store.behavior_index(p, kImposingPresence);
        if (bi == objects::kNone || !store.behavior_enabled(p, bi) || store.behavior_next_service(p, bi) > mf) continue;
        store.set_behavior_next_service(p, bi, mf + std::max<uint32_t>(1u, store.behavior_interval(p, bi)));
        const uint32_t pt = store.type(p);
        const float range = presence_info(pt).range;
        const Vec3 pp = store.position(p);
        const objects::ObjectId pid = store.id(p);
        for (size_t i = 0; i < units_.size(); ++i) {
            const Unit& u = units_[i];
            if (!u.alive || u.retreated || !store.active(u.h) || !store.has_behavior(u.h, objects::beh::Idle)) continue;
            if (i < gated.size() && gated[i]) continue;
            if (combat_->has_unit(u.id) && combat_->current_target(u.id) != tc::kNoObject) continue;
            const Vec3 q = store.position(u.h);
            if (std::fabs(q.x - pp.x) > range || std::fabs(q.y - pp.y) > range || std::fabs(q.z - pp.z) > range) continue;
            bool listed = false;
            for (const auto& e : presence_info(store.type(u.h)).anims)
                listed = listed || std::find(e.second.begin(), e.second.end(), pt) != e.second.end();
            if (listed) beh.presence_refresh(u.id, pid, frames);
        }
    }
#else
    (void)store, (void)gated;
#endif
}

int32_t TacticalBattle::presence_pick(const objects::ObjectStore& st, int32_t unit, const anim::IdleRecord* records, size_t count) {
#ifdef ALAMO_SIM_HAVE_ANIM
    // The default idle type's presence step (0x395920 step 2): among the records whose object still exists the largest
    // value wins; on <= an allied record (0x2823e0) replaces a non-allied best (ascending presence id); the idle = the
    // first Presence_Induced_Animations entry of the unit's type listing the winner's type, else none (the plain idle).
    const objects::ObjectHandle uh = st.find(unit);
    if (!st.exists(uh)) return anim::kNoAnimType;
    const int32_t uo = st.owner(uh);
    bool have = false, best_allied = false;
    int32_t best_frames = 0;
    objects::TypeIndex best_type = objects::kNone;
    for (const anim::IdleRecord& r : std::span<const anim::IdleRecord>(records, count)) {
        const objects::ObjectHandle ph = st.find(r.presence);
        if (!st.exists(ph)) continue;
        const int32_t po = st.owner(ph);
        // 0x2823e0: relation == 0 (self or Ally); a neutral (not an enemy) presence owner is not allied.
        const bool allied = po == uo || (galaxy_ && galaxy_->allied(po, uo));
        if (!have || r.frames > best_frames || (allied && !best_allied)) {
            have = true;
            best_frames = r.frames;
            best_allied = allied;
            best_type = st.type(ph);
        }
    }
    if (!have) return anim::kNoAnimType;
    for (const auto& e : presence_info(st.type(uh)).anims)
        if (std::find(e.second.begin(), e.second.end(), static_cast<uint32_t>(best_type)) != e.second.end()) return e.first;
    return anim::kNoAnimType;
#else
    (void)st, (void)unit, (void)records, (void)count;
    return -1;
#endif
}

void TacticalBattle::play_turn_clip_events(SimRng& rng, uint64_t gframe) {
    std::vector<mv::MovementSystem::TurnClipEvent> ev = movement_->take_turn_clip_events();
#ifdef ALAMO_SIM_HAVE_ANIM
    // s78l t02 (MOVEMENT §3.5.1 "Arrival turn, details"), in the movement's order (ascending id): a clip event plays
    // `anim` with index 0xFFFF (synced variant draw only when the clip has >= 2 variants), loop 0, at ev.speed, blend
    // ev.blend when >= 0 (the locomotor caps it with its [port] constant for GC 0x140b16d28), else the clip's global ([port] the move blend tunable: the
    // globals 0x140b398fc / 0x140b39900 are unread); an idle event plays the default idle type (slot 47: index 0,
    // Loop_Idle_Anim_00, [port] the idle blend for 0x140b39908, speed 1) unless the unit burns.
    anim::AnimWorld* w = hooks_ ? hooks_->world() : nullptr;
    const GameMode* gm = hooks_ ? hooks_->anim_mode() : nullptr;
    if (!anim_ || !w || !gm) return;

    const objects::ObjectStore& st = gm->primary();
    const anim::PresenceIdleFn presence = [this, &st](anim::ObjectId unit, std::span<const anim::IdleRecord> recs) {
        return presence_pick(st, unit, recs.data(), recs.size());
    };
    for (const mv::MovementSystem::TurnClipEvent& e : ev) {
        if (!w->contains(e.id)) continue;
        if (e.idle) {
            if (combat_->has_unit(e.id) && combat_->burning(e.id)) continue;
            if (anim_->play_default_idle(*gm, e.id, gframe, &presence)) ++stats_.anim_plays;
            continue;
        }
        if (e.anim < 0) continue;
        anim::PlayRequest r;
        r.type = e.anim;
        r.index = anim::kRandomVariant;
        r.loop = false;
        r.speed = e.speed;
        r.blend_seconds = e.blend >= 0.0f ? e.blend : w->tunables().move_blend_seconds;
        const anim::AnimModel* m = w->model(e.id);
        if (m && m->variant_count(e.anim) < 2) r.index = 0;  // one variant: no draw (0x1ffb40 returns at once)
        if (w->play(e.id, r, &rng, gframe) == anim::PlayResult::Played) ++stats_.anim_plays;
    }
#else
    (void)rng, (void)gframe;
#endif
}

bool TacticalBattle::anim_engaged_clip(objects::ObjectId id) const {
    // cu8g t01 (CU8F_T01_T02_T07_RE.md §2.3): HOLSTER_WEAPON's "engaged" clip test - the current animation type is ATTACK
    // (5) or SPINMOVE (0x68). Without the animation service: false.
#ifdef ALAMO_SIM_HAVE_ANIM
    const anim::AnimWorld* w = hooks_ ? hooks_->world() : nullptr;
    if (!w || !w->contains(id)) return false;
    const int32_t t = w->current_type(id);
    return t == static_cast<int32_t>(anim::kAnimAttack) || t == static_cast<int32_t>(anim::kAnimSpinMove);
#else
    (void)id;
    return false;
#endif
}

void TacticalBattle::drive_animation(objects::ObjectStore& store, SimRng& rng, uint64_t gframe) {
#ifdef ALAMO_SIM_HAVE_ANIM
    // R3 anim: WalkLocomotor (0x61f840, A.7 walk / move / turn rows) and the idle behaviour (0x62e570) for battle
    // units, through SimAnimation::service_locomotor (rules in anim_behaviors.hpp); Berserker state first.
    anim::AnimWorld* w = hooks_ ? hooks_->world() : nullptr;
    const GameMode* gm = hooks_ ? hooks_->anim_mode() : nullptr;
    if (!anim_ || !w || !gm) return;
    const objects::ObjectStore& st = gm->primary();
    const bool land = gm->kind() == objects::ModeKind::Land;
    // s78l t03 (a) (TACTICAL_COMBAT slot 47 "s78l t03"): the IDLE moving gate = the locomotor's vf +0x110. Walk / Bike /
    // Fighter / SimpleSpace: a path on the stack, whatever the speed ([port] has_goal, a pushed walk, or the Walk arrival
    // turn - UnitView::arrival_turn - which retail runs with the path still stacked: MOVEMENT 3.5.1); JetPack: state != Walk Stopped; Flying / LandBomber: [port] the goal (formation tests not
    // modelled); a land team member on the Default locomotor (LandTeamInfantry): its container's, [port] the container
    // follows its first live member, so that member's. No locomotor (not movable): never gated.
    const auto own_moving = [&](const Unit&, const mv::UnitView& v) {
        if (v.locomotor == mv::LocomotorKind::Jetpack) return v.loco_state != mv::LocoState::WalkStopped;
        return v.has_goal || v.pushed || v.arrival_turn;  // s78l t02: the arrival turn keeps the path stacked
    };
    const auto team_of = [&](const Unit& u) -> uint32_t {
        return land && u.container < containers_.size() && containers_[u.container].team && !containers_[u.container].removed
                   ? u.container
                   : 0xFFFFFFFFu;
    };
    // s78n t26.1 review (MOVEMENT §3.2.1 item 3): a walk that ended deployed this tick (0x16 -> 0x19) sets the Deploy clip on
    // its last frame (blend 0.2, rate 1.0; [port] variant 0, no draw), whatever clip plays (Deployed_Walk, a turn, an idle)
    for (const objects::ObjectId id : movement_->deploy_pose_events())
        if (const Unit* du = unit_by_id(id); du && du->alive && !du->retreated && w->contains(id) && anim_->deploy_at_end(*gm, id, gframe))
            ++stats_.anim_plays;
    std::vector<int8_t> head_moving(containers_.size(), -1);  // per team container: its first live member's gate
    idle_gate_.assign(units_.size(), 0);
    for (size_t i = 0; i < units_.size(); ++i) {
        const Unit& u = units_[i];
        if (!u.alive || u.retreated || !u.movable || !movement_->has_unit(u.id)) continue;
        const mv::UnitView v = movement_->view(u.id);
        const bool own = own_moving(u, v);
        const uint32_t c = team_of(u);
        if (c != 0xFFFFFFFFu && head_moving[c] < 0) head_moving[c] = own ? 1 : 0;
        const bool moving = (c != 0xFFFFFFFFu && v.locomotor == mv::LocomotorKind::Default) ? head_moving[c] > 0 : own;
        // Allow_Idle_When_Moving (+0x85) exempts the unit from the gate (the IDLE service and the presence writer alike).
        idle_gate_[i] = static_cast<uint8_t>(moving && !anim_->params_for(store.type(u.h), gm->kind()).allow_idle_when_moving);
    }
    imposing_presence(store, idle_gate_);
    // The default idle type's presence step (0x395920 step 2): among the records whose object still exists the largest
    // value wins; on <= an allied record (0x2823e0) replaces a non-allied best (ascending presence id); the idle = the
    // first Presence_Induced_Animations entry of the unit's type listing the winner's type, else none (the plain idle).
    const anim::PresenceIdleFn presence = [this, &st](anim::ObjectId unit, std::span<const anim::IdleRecord> recs) {
        return presence_pick(st, unit, recs.data(), recs.size());
    };
    for (size_t i = 0; i < units_.size(); ++i) {
        const Unit& u = units_[i];
        if (!u.alive || u.retreated || !w->contains(u.id)) continue;
        anim_->set_berserker(*gm, u.id, abilities_->is_active(u.id, ta::kBerserker));
        // s78k t02 (TACTICAL_COMBAT slot 47 "s78k t02", IdleBehavior 0x62e570) [H]: the IDLE service reads the TARGETING
        // behaviour's target (a land team member: its head's) whether or not it is suspended (Prevent_All_Fire), and
        // draws Get_Int(0, FPS) every service while one is held. current_target keeps the target while suspended.
        const anim::IdleTarget target = !combat_->has_unit(u.id)                         ? anim::IdleTarget::None
                                        : combat_->current_target(u.id) != tc::kNoObject ? anim::IdleTarget::Target
                                                                                         : anim::IdleTarget::NoTarget;
        const bool gated = idle_gate_[i] != 0;
        anim::LocomotorInput in;
        in.movable = u.movable;
        in.on_path = gated;
        in.target = target;
        if (u.movable) {
            const mv::UnitView v = movement_->view(u.id);
            // s78n t26.1 review: only 0x19 with the walking-deployed flag leaves the deploy states to the locomotor clips
            // (clip 115); 0x18 with the flag set (an order shorter than M while deploying) keeps its Deploy clip.
            if (v.deploy_state != mv::LocoState::None && !(v.deploy_state == mv::LocoState::WalkDeployed && v.walk_deployed)) {
                // S8 t11 follow-up (MOVEMENT §3.2, 0x61e930): the Walk deploy states own the animation - the Deploy /
                // Undeploy clip (synced random variant at Deployment_Anim_Rate) starts when the locomotor starts timing
                // it; Deployed keeps the finished clip. [port] a reversed clip is drawn from its first frame.
                // s78l t03: the IDLE deploy gate (deploying / undeploying / deployed) returns with nothing changed.
                // s78n t26.1: the deployed pose after a deployed walk is set above (deploy_pose_events).
                if (v.deploy_state != mv::LocoState::WalkDeployed && v.deploy_clip >= 0 && w->current_type(u.id) != v.deploy_clip &&
                    anim_->deploy(*gm, u.id, v.deploy_clip == static_cast<int32_t>(anim::kAnimUndeploy), rng, gframe))
                    ++stats_.anim_plays;
                continue;
            }
            // s78n t26.1 (MOVEMENT §3.2.1 item 3, state 0x13 0x61f840): walking deployed plays clip 115 (Deployed_Walk) at
            // Deployed_Walk_Anim_Speed x the speed over the UNDEPLOYED effective max (the move step's f, §3.5.2 C.1)
            in.deployed = v.walk_deployed;
            if (v.locomotor != mv::LocomotorKind::Default) {  // REMAINING_GAPS §4.3-4.7: the locomotor picks the clip
                if (v.anim != mv::kAnimNone && w->current_type(u.id) != v.anim) {
                    anim::PlayRequest req;
                    req.type = v.anim;  // variant 0, no draw; one-shot [port: the locomotor re-requests looping states]
                    if (w->play(u.id, req, nullptr, gframe) == anim::PlayResult::Played) ++stats_.anim_plays;
                }
                // s78l t03 (a): then the IDLE service under the same moving gate as every other locomotor.
                if (anim_->service_idle(*gm, u.id, gated, target, rng, gframe, &presence)) ++stats_.anim_plays;
                continue;
            }
            in.speed = v.speed;
            in.max_speed = v.max_speed;
            in.yaw_deg = v.yaw;
            // s78l t02: the arrival turn plays its own clips (play_turn_clip_events), also on the frame it ends. The IDLE gate
            // does not count that frame as moving: the path pops inside the locomotor (behaviour 1) before the IDLE service
            // (behaviour 7) of the same frame (MOVEMENT 3.5.1 step 6).
            in.host_turn = v.arrival_turn || v.arrival_turn_ended;
        }
        stats_.anim_plays += anim_->service_locomotor(*gm, u.id, in, cfg_.walk_speed_ratio, cfg_.min_move_anim_ratio, rng, gframe, &presence);
    }
#else
    (void)store, (void)rng, (void)gframe;
#endif
}

void TacticalBattle::spawn_particle(TacticalMode& mode, int32_t type, Vec3 pos, int32_t owner) {
    // R4 B.1 layer 1: a PARTICLE GameObject (consumes an id), removed at frame + Particle_Lifetime_Frames.
    if (type < 0 || static_cast<size_t>(type) >= catalog_->types().size()) return;
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
    // source's TEAM parent when it has one. §30.2 [H]: the hull part is the clamped loss (hullBefore - max(0, hullAfter),
    // 0 when the cannot-die restore ran), so the killing hit records at most the remaining hull. [port] The running total /
    // last-damage frame (+0x8 / +0x50) are not kept (nothing reads them yet).
    {
        const objects::GameObjectTypes& dtypes = catalog_->types();
        for (const tc::DamageEvent& e : combat_->damage_events()) {
            if (e.source == tc::kNoObject || !damage_tracking_.tracks(static_cast<int32_t>(e.victim))) continue;
            const float amount = e.absorbed + (e.hardpoint < 0 ? e.hull_lost : 0.0f);
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
    for (const tc::DeathEvent& d : combat_->deaths()) process_death(mode, d, rng);
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
        // cu8g t01 (CU8F_T01_T02_T07_RE.md §2.3): Saber_Throw's throw snaps the saber holstered (0x635450: suppress, fade 0).
        // cu8k t01 (§2.7 items 1-4): the apply records T (e.point); the throw comes kSaberThrowDelay frames later (anim 0x6c
        // past its half [M]) and spawns the Saber_Name projectile (saber_throw_service); hg_holster_service snaps the saber
        // holstered at the throw, holds the draw while it flies (to the ability's end, resolved by the machine) and snaps
        // drawn only on a real catch. value 0 = the activation failed (the forwarding path's Unhandled): nothing thrown.
        if (e.kind == ta::AbilityEventKind::Unhandled && e.type == ta::kSaberThrow && e.value > 0) {
            SaberThrowState st;
            st.thrower = e.id;
            st.target = e.target;
            st.aim = Vec3{e.point.x, e.point.y, e.point.z};
            st.throw_frame = frame_ + ta::kSaberThrowDelay;
            if (abilities_) abilities_->saber_params(e.id, st.projectile, st.catch_dist);
            sabers_.erase(std::remove_if(sabers_.begin(), sabers_.end(), [&](const SaberThrowState& x) { return x.thrower == e.id; }),
                          sabers_.end());  // a new throw replaces a pending one (as saber_schedule)
            sabers_.push_back(st);
            engine_loops_.saber_schedule(e.id, st.throw_frame, 0u, tactical::hangar::EngineLoops::kSaberOpenEnd);
        }
        // S7 t15 A1 (ABILITIES_HARDPOINTS §2.6): every ability end raises signal 0x29, including the Remote_Bomb toss, which
        // ends at its Toss_Anim end (LOB_SOLVER §3.5 step 4, S8 t06).
        if (e.kind == ta::AbilityEventKind::Ended) ability_finished_.push_back(e.id);
        // S7 t26 (AI_TACTICAL §7.3.3 item 3): DEPLOY raises 0x29 when the deploy (anim 25 -> state 0x19) or undeploy (anim
        // 26 -> state 0) clip ends, at once on a blocked reverse: oi_deploy_sync raises it from the movement deploy events
        // (S8 t11), not the AbilitySystem Deployed event, which fires at the request.
    }
    oi_events(mode);  // OI abilities: ability moves / stops / owner changes, ability spawns, death survivors
    oi_deploy_sync();  // S8 t11 / t12: this pass's deploy requests and jetpack swaps
    abilities_->clear_events();
}

void TacticalBattle::process_death(TacticalMode& mode, const tc::DeathEvent& d, SimRng& rng, bool silent) {
    objects::ObjectStore& store = mode.primary();
    {
        Unit* u = unit_by_id(d.id);
        if (!u || !u->alive) return;  // not a battle unit (dummy targets are invulnerable)
        u->alive = false;
        --parts_[u->participant].alive;
        hg_unit_removed(u->id);  // hangars: carrier / single launched ship gone; engine loops forgotten
        // battle host: object removal record (death)
        bt_removals_.push_back({store.owner(u->h), u->type, u->container != kNoContainer ? containers_[u->container].type : u->type, false});
        const Vec3 at = store.position(u->h);
        const Vec3 face = store.facing(u->h);
        const int32_t owner = store.owner(u->h);
        mode.destroy(u->h);
        vm_deaths_.push_back(u->id);  // story t06: the victory monitor sees this tick's deaths in this order
        s4_hover_death(u->id, at, face);  // s78n t20 batch 2: before the mover (and its hover state) goes
        if (u->movable) movement_->remove_unit(u->id);
        else {
            (void)movement_->remove_unit(u->id);  // s78n t23 step 0: a registered mine obstacle (no-op otherwise)
            movement_->object_removed(u->id);     // s78f review: blocks naming a station / structure (fighter formations)
        }
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
            bool carried = carried_by_clone || silent;  // s78n t12 review: a silent removal plays no death clip
            if (!ch && corpse_path) carried = hooks_->anim_corpse_clone(u->id, d.death_clone, d.kind);
            hooks_->anim_death(u->id, carried, d.kind);
        }
#else
        (void)carried_by_clone;
#endif
        if (const BattleTables::UnitVisuals* vis = visuals_of_type(u->type))
            if (!silent && !vis->death_explosions.empty()) spawn_particle(mode, vis->death_explosions.front(), at, owner);
        unit_gone(mode, *u);
    }
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
    register_clone_obstacle(mode, clone, clone_type);  // s78n t20 batch 1
#ifdef ALAMO_SIM_HAVE_ANIM
    hooks_->anim_clone(clone, kind, &plan);
#endif
}

// s78n t20 batch 1 (MOVEMENT §5.4.5 item 2, §5.2 item 4): the clone is in the land layer as an object without a locomotor
// (obj +0x333 = -1): never Moving, never a checker, the obstacle b of the movers' collision records. Its footprint is
// its type's (Custom_Hard_X/YExtent x Scale_Factor, else the radius; UnitCollisionClass through Variant_Of_Existing_Type).
// [port] registered only on land and only for clones that stay (a Remove_Upon_Death clone is destroyed before). s78n t23
// step 0: also a Proximity_Mine created by an ability (oi_spawn_unit, the battle unit's death removes it); not yet a
// resting Proj_Remote_Bomb (a CombatSystem projectile, no battle object).
void TacticalBattle::register_clone_obstacle(TacticalMode& mode, objects::ObjectHandle clone, int32_t clone_type) {
    if (space_ || !movement_ || clone_type < 0) return;
    const objects::GameObjectTypes& types = catalog_->types();
    const auto t = static_cast<objects::TypeIndex>(clone_type);
    if (t >= types.size() || !types.get_bool(t, "IsDeathCloneObstacle")) return;
    mv::UnitDesc md = mv::unit_desc_from_type(types, t);
    if (md.collision_class < 0) return;
    const objects::ObjectStore& store = mode.primary();
    const Vec3 pos = store.position(clone), face = store.facing(clone);
    md.id = store.id(clone);
    md.pos = {pos.x, pos.y};
    md.z = pos.z;
    md.yaw_deg = yaw_of(face);
    md.owner = store.owner(clone);
    md.no_locomotor = true;
    md.death_clone_obstacle = true;
    // the locomotor-side data of the variant's base type does not apply (the clone's behaviours were stripped)
    md.locomotor = mv::LocomotorKind::Default;
    md.crusher = md.supercrusher = false;
    md.turret = false;
    md.hover_height = 0.0f;
    md.uses_multiple_locomotors = md.has_secondary_locomotor = false;
    mv_type_extents(md, t);
    movement_->add_unit(md);
}

void TacticalBattle::kill_pushed_clones(TacticalMode& mode) {
    if (space_ || !movement_ || movement_->push_kills().empty()) return;
    // 0x391310(b, 1): the clone dies by the normal death. [port] it is destroyed here (its own death plan ends at once;
    // a clone has no death clone of its own), and leaves the movement system and the battle's clone list.
    const std::vector<objects::ObjectId> ids(movement_->push_kills().begin(), movement_->push_kills().end());
    objects::ObjectStore& store = mode.primary();
    for (const objects::ObjectId id : ids) {
        movement_->remove_unit(id);
        const auto it = std::find_if(death_clones_.begin(), death_clones_.end(), [id](const TimedObject& o) { return o.id == id; });
        if (it == death_clones_.end()) continue;
        if (store.alive(it->h)) {
            mode.destroy(it->h);
            ++stats_.death_clones_removed;
        }
        death_clones_.erase(it);
    }
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
            if (movement_) movement_->remove_unit(death_clones_[i].id);  // s78n t20 batch 1 (a registered clone obstacle)
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
    // s78n t12 review: deaths raised between ticks by command-phase ability effects - the vehicle thief's eject (unit
    // ability 46 / 18 off: Take_Damage kind 13 kills the stolen vehicle), a STIM_PACK that kills its user - are still in
    // the combat system's list, which the coming combat tick clears. Process them now, in list order, like the combat
    // tick's own deaths (process_death skips a unit that was processed already: last tick's deaths, script_damage's).
    // [port] before this the host never saw such a death (the object stayed, dead in combat only).
    if (combat_ && !combat_->deaths().empty()) {
        // s78n t19: the hooks get this tick's context first, so what a late death starts (its death clip, a corpse
        // clone) is stamped with the current frame, not with the tick before
        if (hooks_) hooks_->set_tick(&rng, global_frame);
        const std::vector<tc::DeathEvent> late(combat_->deaths().begin(), combat_->deaths().end());
        for (const tc::DeathEvent& d : late) process_death(mode, d, rng);
    }
    story_reinforced_.clear();  // L1/S4 wave 2b story hook: this tick's reserve deployments
    ability_finished_.clear();  // S7 t15 A1: this tick's signal 0x29
    landings_unloaded_.clear();  // a1i t04: signal 0x19 records of the last tick
    production_done_.clear();    // q6 t19: signal 0x22 records of the last tick
    story_garrisoned_.clear();  // S4: this tick's garrison entries / structure captures
    story_captured_.clear();
    share_groups_sync();  // S8 t17 (FIGHTER_FLYING §4.10 B): squadron of the shared team target, before this tick's orders
    objects::ObjectStore& store = mode.primary();
    l1_weather_tick(rng);  // L1 systems: land weather service (before the common tick)
    hero_clash_tick(mode, rng, global_frame);  // s78n t06: land tick step 4, after the weather (HERO_CLASH §2.1)
    fog_decay();  // fog of war: common-tick step 4, before the object services (owner: fog agent)
    hg_production(mode, rng);  // station contents / skirmish tactical production (before the object services)
    profile::lap("tac.setup");
    // 1. reinforcements, 2. stand-in orders, 3. retreat coordinator.
    reinforce(mode, rng, true);
    landing_service(mode, rng);  // a1i t04: landing objects (timed unload steps, take-off)
    if (cfg_.order_interval > 0 && (frame_ - 1) % cfg_.order_interval == 0) issue_orders(mode, rng);
    retreat_policy();
    if (retreat_side_ >= 0 && !retreat_complete_ && frame_ >= retreat_done_frame_) {
        bt_retreat_attrition(static_cast<uint8_t>(retreat_side_), rng);  // battle host: vf12 at the countdown end (§4.9)
        complete_retreat(mode, static_cast<uint8_t>(retreat_side_));
    }
    profile::lap("tac.orders");
    attack_block_service(mode, rng);  // s78n t20 stage A: serial, ascending mover, before the collision feed and the movement tick
    // 4. speed factors, movement, positions into the store and the trees, locomotor animation.
    apply_speed_factors();
    feed_collision_info(store);  // s78n t20: targets / ranges / hero clashes for the land collision decisions
    fighter_sync();  // S7 fighters: new squadrons (map start, reinforcements, hangar launches, wingmen) and params
    feed_attack_targets(store);
    if (space_) avoid_snapshot(store);  // SPACE_AVOIDANCE: committed member transforms for the fighter avoidance hook
    profile::lap("tac.pre_move");
    movement_->tick(frame_, rng, pf);
    movement_->commit(store);
    kill_pushed_clones(mode);  // s78n t20 batch 1: the death clones a collision push killed
    play_turn_clip_events(rng, global_frame);  // s78l t02: the arrival turn's clip plays (synced variant draws), ascending id
    s4_exit_arrivals();  // s78n t26.1: the garrison hosts' +0xf8 hook for the walk-out moves that finished
    oi_deploy_sync();  // S8 t11: deploy clip ends (Is_Active, deploy flags, signal 0x29) before the combat tick
    profile::lap("tac.movement");
    fighter_enter_idle();  // S7 t22 / t25: enter_idle drops the squadrons' team targets (§4.7 step 4, §4.10 D)
    forward_fighter_events();  // S7 fighters: Provoke / Retarget -> combat attack orders (before the combat tick)
    for (const objects::ObjectId id : movement_->despawned())  // §4.6: land bombers at their path end, no death effects
        if (const Unit* u = unit_by_id(id)) bt_kill_silently(mode, u->h);
    oi_crush_.assign(movement_->crush_events().begin(), movement_->crush_events().end());  // OI tactical
    l1_motion(mode, rng);  // L1 systems: hyperspace jumps, engine re-enables
    arrival_service(mode, rng);  // cu8l t03: the start fleet's hyperspace arrivals (timer draws, finishes)
    l1_force_disable(mode);  // cu8 t02: Disable_Force_Abilities_Range emitters (Force-disable stamps)
    hg_engine_service();   // engine loops: SimpleSpace locomotor services (moving first, then idle)
    bt_team_positions(mode);  // battle host: space squadrons / non-steering land squads at their members' box centre
    profile::lap("tac.motion_post");
    mode.resolve_models();  // refixf t07: after a load, before the boxes and the parallel sweeps read the model cache
    for (const Unit& u : units_)
        if (u.alive && !u.retreated && u.movable && store.alive(u.h))
            mode.trees().update(static_cast<tactical::EntityHandle>(u.id), mode.bounds_of(u.h));
    mode.trees().maintain(sim_time, pf);
    profile::lap("tac.trees");
    drive_animation(store, rng, global_frame);
    profile::lap("tac.anim");
    // 5. combat (abilities run inside its serial phases), events.
    for (const Unit& u : units_) {
        if (!u.alive || u.retreated || !u.movable) continue;
        combat_->set_position(u.id, tv(store.position(u.h)));
        const mv::UnitView mvv = movement_->view(u.id);
        combat_->set_facing(u.id, tv(facing_of_yaw(mvv.yaw)));
        // S7 t14 (MOVEMENT §3.1.1 step 6): the locomotor swap disables TARGETING / WEAPON / SPECIAL_WEAPON for the jetpack
        // flight and enables them on the swap back. The swap runs inside MovementSystem (orders, the JET_PACK ability, the
        // jetpack's state 0x16), so the flag mirrors the active locomotor here, before every combat tick.
        // S7 t15 A1: the swap back then raises "unit ability finished" (signal 0x29: task-force event 19, the blocking
        // Activate_Ability); the flag still set from the previous tick marks it.
        if (!mvv.secondary_locomotor && (combat_->flags(u.id) & tc::kUnitWeaponsSuspended) != 0) ability_finished_.push_back(u.id);
        combat_->set_flag(u.id, tc::kUnitWeaponsSuspended, mvv.secondary_locomotor);
        // S8 t11 (TACTICAL_COMBAT §3.1.1 "Gate details" step 6): fighter Idle (0x1C), and Walk Stopped (0x16) on a locomotor
        // whose vf +0x240 is true (Flying, JetPack, LandBomber; [port] Starship: the port's space locomotors keep no 0x16).
        // ROCKET_ATTACK in any Unit_Abilities_Data entry ("ROCKET_ATTACK detection").
        const bool air = mvv.locomotor == mv::LocomotorKind::Flying || mvv.locomotor == mv::LocomotorKind::Jetpack ||
                         mvv.locomotor == mv::LocomotorKind::LandBomber;
        combat_->set_flag(u.id, tc::kUnitFighterIdle, mvv.loco_state == mv::LocoState::FighterIdle);
        combat_->set_flag(u.id, tc::kUnitHoverRest, air && mvv.loco_state == mv::LocoState::WalkStopped);
        // s78n t20 batch 2 (MOVEMENT §5.3 "Hover in full"): a Hover_Height unit raised (state 1) - melee attackers refuse it
        combat_->set_flag(u.id, tc::kUnitHoverRaised, movement_->hover_state(u.id) == 1);
        combat_->set_flag(u.id, tc::kUnitRocketAttack, abilities_ && abilities_->is_active(u.id, 11));
        // s78f t03 (TACTICAL_COMBAT §3.1.1 "Targeting states"): the locomotor is moving (an order in progress or speed), and
        // the model has an Attack clip (0x397ae0(obj, 5)): a moving land unit without TURRET fires only without one.
        combat_->set_flag(u.id, tc::kUnitMoving, mvv.has_goal || mvv.speed > 0.0f);
        bool attack_clip = false;
#ifdef ALAMO_SIM_HAVE_ANIM
        if (const anim::AnimWorld* w = hooks_ ? hooks_->world() : nullptr)
            if (const anim::AnimModel* m = w->contains(u.id) ? w->model(u.id) : nullptr) attack_clip = m->variant_count(anim::kAnimAttack) > 0;
#endif
        combat_->set_flag(u.id, tc::kUnitAttackClip, attack_clip);
        // S8 t17 (FIGHTER_FLYING §4.10 A / B): a squadron member shares its squadron's team target; a fighter whose
        // formation record exists scans R + Rr (fighter_scan_range).
        if (space_) {
            const bool member = u.container < containers_.size() && movement_->fighter_squad(containers_[u.container].id);
            combat_->set_share_group(u.id, member ? containers_[u.container].id : tc::kNoObject);
            combat_->set_flag(u.id, tc::kUnitScanQuery,
                              mvv.locomotor == mv::LocomotorKind::Fighter && fighter_formation(fighter_key(u)) != nullptr);
        }
    }
    s4_positions(mode);  // S4: garrisoned units sit on their holders
    combat_team_positions(store);  // s78n t03: the land teams' TeamTargeting P_U (cleared after the combat tick)
    profile::lap("tac.combat_pre");
    hooks_->set_tick(&rng, global_frame);
    abilities_->tick(frame_, mode.trees(), rng, pf);
    combat_->clear_team_positions();  // s78n t03: readers between ticks use the head's own position (save / load equal)
    profile::lap("tac.combat");
    consume_events(mode, rng);
    oi_respawn_due(mode);  // OI abilities: Tactical_Respawn creations due this frame
    profile::lap("tac.events");
    hg_tick(mode, rng);    // hangars: SPAWN_SQUADRON services and the launches they request
    saber_throw_service(mode);  // cu8k t01: Saber_Throw throws / SWORD_CONTROL / SWORD_CATCH (after the saber's step)
    hg_holster_service();  // cu8g t01: HOLSTER_WEAPON (engine-loop suppression, draw / holster SFX requests)
    l1_weapons(mode, rng);  // L1 systems: special-weapon volleys, TSW state machines
    profile::lap("tac.services");
    expire_objects(mode);
    s4_tick(mode, rng);  // S4: garrison enters / exits / ejects, structure capture
    pads_tick(mode);     // Int-final: build pads (construction completion, children gone)
    sk_income(mode);     // skai: skirmish map battles: the Income_Stream credit trickle
    bt_shield_stamps(mode);  // battle host: base-shield secondary occupants follow the live shields (land)
    service_damage_tracking();
    profile::lap("tac.cleanup");
    fog_service(mode);  // fog of war: REVEAL sources, attacker flashes (owner: fog agent)
    fog_setup_tick();   // FOWF t05: setup phase end (all humans ready / countdown)
    profile::lap("tac.fog");
    team_attack_service(mode, rng);  // q6: attacks on TEAM containers (re-pick, end), before the queues read the orders
    advance_queues(mode, rng);  // [port: Gabriel] 5b. queueable unit actions: completed orders start the next entry
    late_swap_back_signals();   // S7 t27: a jetpack swap back after the combat_pre loop raises signal 0x29 in this tick
    stats_.shots = combat_->stats().shots;
    stats_.hits = combat_->stats().hits;
    // cu8j t01 (MOVEMENT §4.6.2): the movement coordinator update (retail common tick step 10, after the object services):
    // this tick's new coordinators compose their formations (the FormationClass ctor draws, the land stagger draws).
    // Orders given after it (the AI and story services of the host) compose at the next tick's.
    movement_->coordinator_update(frame_, global_frame, rng);
    // 6. victory.
    check_victory(mode);  // story t06: the victory monitor (losses since the last tick, countdown, end)
    if (finished_ && !death_clones_.empty()) remove_death_clones(mode, true);  // refixf t04: the battle's clones end with it
    space_obstacles_sync(mode);  // refixf t02: this tick's new / dead / retreated SPACE_OBSTACLE units
    land_obstacles_sync(mode, pf);   // S8 t02: this tick's new / gone LAND_OBSTACLE objects
    hg_publish_engine_requests();  // engine loops: this tick's requests for the audio host
    feed_pair_gates();  // s78n t18 review: the gates the command phase reads follow this tick's deaths / retreats
    profile::lap("tac.victory");
}

// S7 t27 (MOVEMENT §3.1.1 step 6): the swap back raises signal 0x29 at once. The combat_pre loop sees the swaps of the
// movement tick; a swap back later in the tick (after that loop: ability events, orders, queues) is reported here, in the
// same tick, and the flag cleared so the next combat_pre loop does not report it again.
void TacticalBattle::late_swap_back_signals() {
    if (!movement_ || !combat_) return;
    for (const Unit& u : units_) {
        if (!u.alive || u.retreated || !u.movable || !combat_->has_unit(u.id)) continue;
        if ((combat_->flags(u.id) & tc::kUnitWeaponsSuspended) == 0 || movement_->view(u.id).secondary_locomotor) continue;
        ability_finished_.push_back(u.id);
        combat_->set_flag(u.id, tc::kUnitWeaponsSuspended, false);
    }
}

// ---- S7 t31: signal 0xb (AI_TACTICAL §7.3.3 item 2, A1_READINGS §33.3) ----------------------------------------------

uint32_t TacticalBattle::add_order_replaced_listener(OrderReplacedListener fn) {
    const uint32_t token = next_listener_token_++;
    order_replaced_listeners_.emplace_back(token, std::move(fn));
    return token;
}

void TacticalBattle::remove_order_replaced_listener(uint32_t token) {
    order_replaced_listeners_.erase(std::remove_if(order_replaced_listeners_.begin(), order_replaced_listeners_.end(),
                                                   [token](const auto& l) { return l.first == token; }),
                                    order_replaced_listeners_.end());
}

void TacticalBattle::raise_order_replaced(std::span<const objects::ObjectId> ids) {
    if (order_replaced_listeners_.empty()) return;
    const std::vector<std::pair<uint32_t, OrderReplacedListener>> ls = order_replaced_listeners_;  // a listener may unsubscribe
    for (const objects::ObjectId id : ids)
        for (const auto& l : ls)
            if (l.second) l.second(id);
}

bool TacticalBattle::ability_raises_order_replaced(uint8_t ability) {
    // 0x570a50 / 0x220ed0: 2 3 5 6 13 15 16 17 18 20 21 22 27 31 32 34 37 38 39 41 45 46 50 51 52 53 55 58 59 64 66 67 69 71 73 74
    static constexpr uint8_t kIds[] = {2,  3,  5,  6,  13, 15, 16, 17, 18, 20, 21, 22, 27, 31, 32, 34, 37, 38,
                                       39, 41, 45, 46, 50, 51, 52, 53, 55, 58, 59, 64, 66, 67, 69, 71, 73, 74};
    return std::find(std::begin(kIds), std::end(kIds), ability) != std::end(kIds);
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
    // battle host: TacticalBuildEvent (33) / TacticalSell (34) - L1_S4_REMAINING §3.6; AI players build without a charge in campaign
    // battles and pay like a human in land skirmish (skai t09, tactical_build).
    if (t == T::Production) return hg_production_command(mode, player, ev);  // skirmish station production (else Unhandled)
    if (t == T::SetupPhaseMove || t == T::SetupPhaseTriggerEnd) return fog_setup_command(mode, player, ev);  // FOWF t05
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
    // S8 t26 (TACTICAL_COMBAT §3.1.1 "Fire-at-position order", command 25 execute 0x40a3b0): readiness >= 0.99, point = the
    // command position when "at position" is set or there is no target object, else the target's bare position; then
    // Fire_Lobbing_At_Position (sw_fire_lobbing_at), then the destroy-after sweep (S8 t30: every live object of the
    // weapon's owner with DUMMY_DESTROY_AFTER_SPECIAL_WEAPON_FIRED 0x67, a copy of the owner's list in its order, killed
    // silently) [M: after an accepted order only, as the Lua binding]. [port] Lobbing superweapons of the issuing player
    // only.
    if (t == T::SpecialWeaponFire) {
        const uint32_t ww = ev.u("weapon");
        if (ww == cmd::kNoObject || ww > 0x7FFFFFFFu) return R::InvalidUnit;
        const objects::ObjectStore& store = mode.primary();
        const objects::ObjectHandle wh = store.find(static_cast<objects::ObjectId>(ww));
        if (!store.alive(wh) || store.owner(wh) != static_cast<int32_t>(player)) return R::InvalidUnit;
        const objects::ObjectId wid = store.id(wh);
        if (!combat_ || !combat_->fires_at_position(wid)) return R::Unhandled;
        if (l1_sw_.readiness(wid, frame_) < 0.99f) return R::AbilityNotReady;
        Vec3 at{ev.f("x"), ev.f("y"), ev.f("z")};
        if (ev.u("at_position") == 0) {
            const uint32_t tw = ev.u("target");
            const objects::ObjectHandle th = tw == cmd::kNoObject || tw > 0x7FFFFFFFu ? objects::ObjectHandle{} : store.find(static_cast<objects::ObjectId>(tw));
            if (store.alive(th)) at = store.position(th);
        }
        if (!sw_fire_lobbing_at(mode, wid, at)) return R::NotAllowed;
        static const objects::BehaviorTypeId kDestroyAfter = objects::behavior_type_id("DUMMY_DESTROY_AFTER_SPECIAL_WEAPON_FIRED");
        if (kDestroyAfter != objects::kNoBehavior) {
            const auto owned = store.owned_by(store.owner(wh));
            const std::vector<objects::ObjectHandle> list(owned.begin(), owned.end());
            for (const objects::ObjectHandle h : list)
                if (store.alive(h) && store.has_behavior(h, kDestroyAfter)) bt_kill_silently(mode, h);
        }
        return R::None;
    }
    // cu9g t02 (HUD_GAPS §9.7, command 28 execute 0x5246d0): land only (no bombing-run manager in space). The point is the
    // dequantized wire value (the original's q1..q3 round trip); a valid target starts the run (0x492d60). No RNG.
    // Outcomes: no record / space -> NotAllowed, invalid point -> InvalidPosition (both silent in the original),
    // valid point but the run is not available -> AbilityNotReady (the original logs it and plays the GUI error SFX for
    // the local issuer; the app does that from this reason).
    if (t == T::BombingRun) {
        if (space_ || !l1_mode_) return R::NotAllowed;
        const bool has_record = std::any_of(l1_runs_.begin(), l1_runs_.end(), [&](const L1BombingRun& r) { return r.player == static_cast<int32_t>(player) && r.enabled; });
        if (!has_record) return R::NotAllowed;
        const Vec3 at{ev.f("x"), ev.f("y"), ev.f("z")};
        if (!bombing_run_target_valid(static_cast<int32_t>(player), at)) return R::InvalidPosition;
        if (!bombing_run_available(static_cast<int32_t>(player))) return R::AbilityNotReady;
        return start_bombing_run(mode, static_cast<int32_t>(player), at) ? R::None : R::NotAllowed;
    }
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
            // S8 t10 (LOB_SOLVER §3.5 "MARKER removal"): a Remote_Bomb marker has no lifetime - only the toss kills it
            // (MarkerTossed); it stays when the toss never happens. [port] the other abilities' markers keep the expiry.
            // A refused command (no user took the ability) leaves the marker on the normal expiry.
            ++stats_.ability_commands;
            const uint32_t n = abilities_->targeted_ability(users, static_cast<uint8_t>(ev.u("ability")), dummy, -1, frame_);
            if (n > 0 && ev.u("ability") == static_cast<uint32_t>(ta::kPlaceRemoteBomb) && dummy != objects::kNoObjectId && !dummies_.empty() && dummies_.back().id == dummy)
                dummies_.back().frame = 0xFFFFFFFFu;
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
    if (!owner_change_one(mode, h, owner)) return false;
    team_change_owner(mode, store.id(h), owner);
    return true;
}

bool TacticalBattle::owner_change_one(TacticalMode& mode, objects::ObjectHandle h, int32_t owner) {
    objects::ObjectStore& store = mode.primary();
    if (change_owner(mode, h, owner)) return true;
    const objects::ObjectId id = store.id(h);
    const int32_t old_owner = store.owner(h);
    if (!store.set_owner(h, owner)) return false;
    mode.trees().set_owner(static_cast<tactical::EntityHandle>(id), owner);
    fog_owner_changed(mode, h, old_owner);
    s4_owner_changed(mode, id, old_owner, owner);
    return true;
}

// s78j t04 (TACTICAL_COMBAT §3.5 "Convert and its team rule" step 4, 0x38e5a0): Change_Owner spreads through a team by
// itself - a member calls it on its TEAM container, a container on each member in list order, and an object that already
// has the new owner returns at once. So after `id` (a member or the container) changed, the container follows, then
// every other live member ([port] list order = ascending id, as convert_container's spread in CombatSystem). The
// container's record (Container::owner: loss records, replenish spawns) follows too. CombatSystem::set_owner does not
// spread, so the host does it here for the Lua / story / AI Change_Owner paths.
void TacticalBattle::team_change_owner(TacticalMode& mode, objects::ObjectId id, int32_t owner) {
    objects::ObjectStore& store = mode.primary();
    uint32_t ci = kNoContainer;
    if (const Unit* u = unit_by_id(id)) {
        ci = u->container;
    } else {
        for (size_t c = 0; c < containers_.size(); ++c)
            if (containers_[c].id == id && !containers_[c].removed) ci = static_cast<uint32_t>(c);
    }
    if (ci >= containers_.size()) return;
    Container& ct = containers_[ci];
    ct.owner = owner;
    if (ct.id != id && !ct.removed && store.alive(ct.h) && store.owner(ct.h) != owner) owner_change_one(mode, ct.h, owner);
    for (size_t i = 0; i < units_.size(); ++i) {  // owner_change_one does not add or remove units
        const Unit& m = units_[i];
        if (m.container != ci || m.id == id || !m.alive || m.retreated || !store.alive(m.h) || store.owner(m.h) == owner) continue;
        owner_change_one(mode, m.h, owner);
    }
}

int32_t TacticalBattle::container_owner(objects::ObjectId id) const {
    for (const Container& c : containers_)
        if (c.id == id) return c.owner;
    return -1;
}

bool TacticalBattle::test_attack_idle(objects::ObjectId id, bool arg, SimRng& rng) {
#ifdef ALAMO_SIM_HAVE_ANIM
    if (!hooks_ || !hooks_->world() || !hooks_->world()->contains(id)) return false;
    hooks_->attack_idle(id, arg, frame_, rng);
    return true;
#else
    (void)id, (void)arg, (void)rng;
    return false;
#endif
}

bool TacticalBattle::test_play_clip(objects::ObjectId id, int32_t clip_type) {
#ifdef ALAMO_SIM_HAVE_ANIM
    if (!hooks_ || !hooks_->world() || !hooks_->world()->contains(id)) return false;
    anim::PlayRequest r;
    r.type = clip_type;
    r.index = 0;
    r.loop = true;
    return hooks_->world()->play(id, r, nullptr, hooks_->global_frame()) == anim::PlayResult::Played;
#else
    (void)id, (void)clip_type;
    return false;
#endif
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
           (result_.starbase_destroyed ? " + starbase" : "") +
           (skirmish_land() ? ", skirmish base " + std::to_string(stats_.skirmish_prebuilt) + " pre-built of " + std::to_string(stats_.skirmish_pads) +
                                  " pads, " + std::to_string(stats_.pad_builds) + " structures started"
                            : std::string());
}

void TacticalBattle::hash(StateHasher& h) const {
    h.u32(frame_);
    h.boolean(finished_);
    h.i32(retreat_side_);
    h.u32(retreat_done_frame_);
    h.boolean(retreat_complete_);
    if (vm_cp_armed_) {  // ccpgt t02: the domination timer (absent while not armed)
        h.i32(vm_cp_timer_);
        h.i32(vm_cp_player_);
    }
    if (vm_declared_ || vm_hold_ != 0 || !vm_queue_.empty()) {  // story t06: the victory monitor (absent while idle)
        h.boolean(vm_declared_);
        h.u8(vm_decl_.type);
        h.i32(vm_decl_.player);
        h.i32(vm_countdown_);
        h.i32(vm_hold_);
        h.u32(static_cast<uint32_t>(vm_queue_.size()));
        for (const auto& [type, player] : vm_queue_) {
            h.u8(type);
            h.i32(player);
        }
    }
    if (!attack_blocks_.empty()) {  // s78n t20 stage A (absent while none: space and older land states hash as before)
        h.u32(static_cast<uint32_t>(attack_blocks_.size()));
        for (const AttackBlock& r : attack_blocks_) {
            h.i32(r.mover);
            h.i32(r.target);
            h.u8(r.flags);
            h.u32(r.next);
            h.f32(r.gx);
            h.f32(r.gy);
        }
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
    if (!fformations_.empty()) {      // S7 t22: fighter formations (absent while none)
        h.u32(static_cast<uint32_t>(fformations_.size()));
        for (const FighterFormationRecord& r : fformations_) {
            h.i32(r.key);
            h.u8(r.flags);
            h.f32(r.goal.x);
            h.f32(r.goal.y);
            h.f32(r.goal.z);
            h.i32(r.goal_object);  // s78f t02: the auto blocks are hashed with the movement formation
        }
    }
    if (!hero_clash_.empty()) hero_clash_.hash(h);  // s78n t06: running hero clashes (absent while none)
    if (!landings_.empty()) {         // a1i t04: landing objects waiting for take-off (absent while none)
        h.u32(static_cast<uint32_t>(landings_.size()));
        for (const Landing& l : landings_) {
            h.i32(l.id);
            h.u8(l.state);
            h.u32(l.unloaded);
            h.u32(l.timer_start + l.timer_len);
            h.u32(l.remove_frame);
        }
    }
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
