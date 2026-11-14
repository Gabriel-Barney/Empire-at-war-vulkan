#pragma once
// Shared includes, helpers and types of tactical_battle.cpp (bo3 t03 split); its member definitions are in tactical_battle.cpp and tactical_battle_partN.cpp.
#include "alamo/sim/tactical_battle.hpp"
#include "alamo/core/sim_crc.hpp"

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

namespace bo3split {

#ifdef ALAMO_SIM_HAVE_AI_TACTICAL
inline constexpr objects::BehaviorTypeId kDamageTrackingBehavior = 0x3E;  // DAMAGE_TRACKING (serviced, interval 30)
inline constexpr objects::BehaviorTypeId kWallBehavior = 0x17;            // WALL (S8 t13: EmpireWall stamp bit)
inline constexpr uint32_t kDamageTrackingInterval = 30;
#endif
inline constexpr uint32_t kNoContainer = 0xFFFFFFFFu;
Vec3 facing_of_yaw(float yaw_deg);
float yaw_of(Vec3 f);
Vec3 offset_facing(float ox, float oy);

inline tactical::Vec3 tv(Vec3 v) { return tactical::Vec3{v.x, v.y, v.z}; }
Vec3 sv(tactical::Vec3 v);

// S8 t09 / t13 (MOVEMENT §1.3): the LAND_OBSTACLE stamper's model hooks, one source for map placements and run-time objects:
// the model bounding box (BattleCatalog::model_half_extents) and the Fine_Tune_Occupied_Passability ray against the type's
// Land_Model_Name collision data (BattleCatalog::avoid_model(type, false)). Ray model: no model files in this run (pure
// headless) -> Unknown, untrimmed [port]; an empty Land_Model_Name -> None (the footprint rules for a type without a land
// model, S8 t13); a model that loads -> Model, even without collision meshes (then no sample is hit: retail's ray query
// hits only collision-flagged meshes); a named model that does not load -> Unknown [port]. The ray (S8 t13, vf +0x80 mode
// 0 mask 1: mv::avoid_ray_any) is the segment (x, y, -1000) -> (x, y, +1000) against the collision-flagged meshes whose bone
// is visible, through the model placed by space_avoid's transform (position, facing Rz(yaw) (0, -1), scale). Const data.
inline mv::MapStampHooks land_stamp_hooks(const BattleCatalog* cat) {
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

inline std::string victory_name(uint8_t v) {
    switch (v) {
        case galactic::kVictoryAllDestroyed: return "all-destroyed";
        case galactic::kVictoryAllRetreated: return "all-retreated";
        case galactic::kVictoryStarbaseDestroyed: return "starbase-destroyed";
        default: return std::to_string(v);
    }
}

}  // namespace
using namespace bo3split;

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

namespace bo3split {
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
using namespace bo3split;
// ---- end P host ----

// ---- a1g t04 follow-up: land company drop layout (TACTICAL_GAPS §4.2.1) ----
namespace bo3split {
using Off2 = std::array<float, 2>;
float drop_yaw_of(float x, float y);
Off2 drop_rotate(float deg, float x, float y);
bool drop_grid(float a, float b, uint32_t n, std::vector<Off2>& out, float& bx, float& by);
void drop_team(float ex, float ey, float g, uint32_t s, std::vector<Off2>& out, float& bx, float& by);
bool drop_token_is(std::string_view list, std::string_view token);
}  // namespace
using namespace bo3split;

// ---- S7 fighter host (FIGHTER_FLYING.md §2-§5) -------------------------------------------------------------------------

namespace bo3split {
// [port] squadron slot offsets (team list +0x78; its writer is untraced): the leader at 0, the others line abreast in
// world y at +-40, +-80, ... in team index order.
inline mv::F3 fighter_slot(size_t n) {
    if (n == 0) return {};
    const float k = static_cast<float>((n + 1) / 2) * 40.0f;
    return {0.0f, (n % 2 == 1) ? k : -k, 0.0f};
}
}  // namespace
using namespace bo3split;
}  // namespace alamo::sim
