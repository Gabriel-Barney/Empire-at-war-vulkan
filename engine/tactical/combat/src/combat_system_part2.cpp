// combat_system: part 2 (bo3 t03 split; shared code in combat_system_internal.hpp).
#include "alamo/core/profile.hpp"
#include "combat_system_internal.hpp"

namespace alamo::tactical::combat {
bool CombatSystem::fire_hardpoint(Unit& u, Hardpoint& h, uint32_t frame, SimRng& rng) {
    const HardpointDesc& hd = data_->hardpoints[h.desc];
    const UnitDesc& ud = desc_of(u);
    const Unit* tp = unit(h.target);
    if (!live(tp)) return false;
    const Unit& t = *tp;
    const UnitDesc& td = desc_of(t);
    // 1. gates
    if (!clash_allows(u.id, t.id)) return false;  // s78n t06: Fire_At 0x3825b0 refuses fire outside the clash pairing
    if ((t.flags & kUnitStealthed) && !ud.target_stealth_units && t.owner != u.owner) return false;
    if (hd.fire_category_restrictions & td.category_mask) return false;
    if (!is_enemy(u.owner, t.owner)) return false;
    if (hd.projectile < 0 || static_cast<size_t>(hd.projectile) >= data_->projectiles.size()) return false;
    // L1 systems: a special-weapon hard point never fires at a target hidden for its owner with force 1.
    if (hd.kind == HardpointKind::WeaponSpecial && target_hidden(u.owner, t.id, true)) return false;
    // 2. target hardpoint (§2.9 Q2 step 1): dropped when destroyable and dead; none -> the closest live targetable one to
    //    the shooter, per shot (0x398440 -> 0x394a80(target, pos, 0, 0)).
    int32_t thp = h.target_hp;
    if (thp >= 0 && (static_cast<uint32_t>(thp) >= t.hp_count || hp_dead(hps_[t.hp_begin + static_cast<uint32_t>(thp)]))) thp = -1;
    if (thp < 0 && t.hp_count > 0) thp = closest_hardpoint(t, u.pos, true);
    // S9: ability projectile override / suppression (ion cannon shot, lucky shot).
    int32_t proj = hd.projectile;
    float dscale = 1.0f;  // OI abilities: Blast state gate / projectile / damage multiplier (REMAINING_GAPS §5.5)
    if (hooks_ && !hooks_->fire_gate(u.id, &hd, proj, dscale)) return false;
    if (hooks_ && !hooks_->shot_allowed(u.id, t.id, thp, proj)) return false;
    if (proj < 0 || static_cast<size_t>(proj) >= data_->projectiles.size()) return false;
    // fire bones: A, or the A-B midpoint (C and D never read)
    Vec3 a = local_to_world(u, hd.bone_a);
    Vec3 b = hd.has_bone_b ? local_to_world(u, hd.bone_b) : a;
    if (hooks_) hooks_->fire_bones(u.id, static_cast<uint32_t>(&h - &hps_[u.hp_begin]), a, b);  // S10-int: animated bones
    const Vec3 ref = hd.has_bone_b ? (a + b) * 0.5f : a;
    const float range = fire_range(hd.fire_range, mod_r(u, ModList::FireRange));
    // 3. aim point (§2.9 Q2 step 3): the target hardpoint's bone, else 0x383f70 (range + cone per candidate; none: no shot).
    //    [port] Turret pre-check (Q2 step 2) not modelled: the port has no turret rotation state (the barrel is taken
    //    as on target).
    Vec3 aim0;
    if (thp >= 0) {
        aim0 = hp_world(t, data_->hardpoints[hps_[t.hp_begin + static_cast<uint32_t>(thp)].desc]);
    } else if (!pick_aim_point(u, h, t, ref, range, rng, aim0)) {
        return false;
    }
    // 4. range, XY only
    const float dist = distance_xy(ref, aim0);
    if (!in_fire_range(dist, hd.fire_min_range, range, td.radius)) return false;
    const ProjectileDesc& pd = data_->projectiles[static_cast<size_t>(proj)];
    // S9 energy pool (0x56bee0): a shot with Projectile_Energy_Per_Shot needs that much energy [H];
    // tested before the shot's draws [M].
    if (pd.energy_per_shot > 0.0f && has_energy_pool(u) && u.energy < pd.energy_per_shot) return false;
    // 5. lead (0x399450) on the aim point [M: from the fire-bone reference], then the cone test on the lead point.
    Vec3 aim = lead_point(t, ref, aim0, pd.max_speed * static_cast<float>(cfg_.fps));  // OI tactical: circular lead
    if (!in_cone(u, h, aim)) return false;
    // 8. muzzle: 3 synced Get_Float (arguments lost [M]: per axis between the two bones), else
    //    bone A without B, else Percent(50) picks A or B.
    Vec3 muzzle;
    if (hd.randomize_between_fire_bones) {
        muzzle.x = rng.range_float(a.x, b.x);
        muzzle.y = rng.range_float(a.y, b.y);
        muzzle.z = rng.range_float(a.z, b.z);
    } else if (!hd.has_bone_b) {
        muzzle = a;
    } else {
        muzzle = rng.percent(50) ? a : b;
    }
    // 10. inaccuracy: always 3 synced draws unless disabled (debug flag)
    if (cfg_.inaccuracy_enabled) {
        const float f = inaccuracy_f(cfg_.mode, player(u.owner).ai_accuracy);
        float r = 0.0f;
        bool overridden = false;
        for (int s = 0; s < 2 && !overridden; ++s) {
            const UnitAbilityDesc* ab = ability(u, s);
            if (ab && u.ability_active[static_cast<size_t>(s)] && ab->inaccuracy_override > 0.0f) {
                r = ab->inaccuracy_override;  // returned UNSCALED
                overridden = true;
            }
        }
        // L1 systems: tactical weather (land) scales the inaccuracy by (2 - m); m = 1 leaves it untouched.
        const float ws = cfg_.mode == GameMode::Land ? weather_scale(pd, u.owner) : 1.0f;
        if (!overridden) r = scale_inaccuracy(ud.fire_inaccuracy_fixed_radius, f);
        if (!overridden && ws != 1.0f) r = r * ws;
        if (r > 0.0f) {
            aim = aim + inaccuracy_offset(r, rng);
        } else {
            float dd = 0.0f;
            for (const InaccuracyEntry& e : hd.inaccuracy)
                if (e.category_mask & td.category_mask) {
                    dd = e.distance;
                    break;
                }
            // A1j t03 (0x381dc0, TACTICAL_COMBAT §3.1 step 10): the three draws are made only when the scaled distance,
            // the XY distance and Get_Fire_Range are all > 0.
            float ds = scale_inaccuracy(dd, f);
            if (ws != 1.0f) ds = ds * ws;
            if (ds > 0.0f && dist > 0.0f && range > 0.0f) aim = aim + inaccuracy_offset(inaccuracy_spread(ds, dist, range), rng);
        }
    }
    // 11. spawn (an override projectile keeps the hardpoint's damage / type overrides [M])
    const float damage = (hd.projectile_damage > 0.0f ? hd.projectile_damage : pd.damage) * dscale;
    const uint16_t dtype = hd.damage_type != kNoTypeId ? hd.damage_type : pd.damage_type;
    spawn(u, pd, static_cast<uint32_t>(proj), damage, dtype, muzzle, aim, &t, thp, range, frame);
    if (pd.energy_per_shot > 0.0f && has_energy_pool(u)) {
        u.energy = u.energy - pd.energy_per_shot;
        if (u.energy < 0.0f) u.energy = 0.0f;
    }
    // 12. recharge
    const float W = weapon_delay(u, &hd);
    const float F = fire_rate(u);
    h.pulses -= 1;
    const Recharge r = h.pulses > 0 ? hp_pulse_recharge(hd.pulse_delay, h.pulses, W, F, cfg_.fps)
                                    : hp_full_recharge(hd.min_recharge, hd.max_recharge, hd.pulse_count, W, F, cfg_.fps, rng);
    h.cooldown = r.frames;
    h.pulses = r.pulses;
    ++stats_.shots;
    shot_events_.push_back({u.id, t.id, t.owner});  // OI fog: REVEAL vf26 flash
    if (hooks_) hooks_->fire_done(u.id, &hd, frame);  // OI abilities: Blast charged-shot count
    if (hooks_) hooks_->shot_fired(u.id, t.id, thp, proj, frame);
    return true;
}

void CombatSystem::fire_unit_weapon(Unit& u, uint32_t frame, SimRng& rng) {
    // WeaponBehavior (non-hardpoint weapons) [M for the shot itself; the recharge is H].
    // S7 t15 A2 (MOVEMENT §3.1.1, 0x54d090): the recharge counts the frames since the last service (pack +0x2c), so it
    // keeps elapsing while the behaviour is not serviced (jetpack suspension, stun, no base power): the first service back
    // subtracts the whole gap. [port] Every service subtracts (retail only while the cooldown is non-zero and clamps at
    // 0); ready = <= 0 either way, so the shots are the same and the steady-state counter keeps its S8 CRC format.
    // S8 t12 (TACTICAL_COMBAT §3.1.1 "First service after spawn"): the last-service frame starts at 0, so the first service
    // subtracts the whole current frame number and a unit spawned after its drawn initial delay can fire at once.
    const uint32_t last = u.weapon_last == kNoService ? 0u : u.weapon_last;
    const uint32_t elapsed = frame - last;
    u.weapon_last = frame;
    const int64_t cd = static_cast<int64_t>(u.weapon_cooldown) - static_cast<int64_t>(elapsed);
    u.weapon_cooldown = cd < INT32_MIN ? INT32_MIN : static_cast<int32_t>(cd);
    // S8 t26 (§3.1.1 "Fire-at-position order"): while the order is charged (TD +0x3d) the recharge wait is skipped.
    if (u.weapon_cooldown > 0 && !u.lob_charged) return;
    if (!unit_weapon_allowed(u)) return;  // S8 t08: own-weapon gate 0x54c910 steps 1-6 (TACTICAL_COMBAT §3.1.1)
    const UnitDesc& ud = desc_of(u);
    // Step 7 (S8 t10 / t11, "Gate details"; S8 t26 "Fire-at-position order"): a LOBBING_SUPERWEAPON object fires only while
    // its fire-at-position order is charged (TD +0x3d), and then needs neither targeting state 4 nor a target: the request
    // is the ordered ground point with no target object. Otherwise TD +0x18 must be 4 (attacking: slot 44 && !slot 45,
    // re-tested every service) with a non-null target; else the burst restarts (TD +0x28 := Projectile_Fire_Pulse_Count)
    // and nothing fires. This replaces the port's XY range gate (LOB_SOLVER §8 item 22).
    const bool ground = ud.lobbing_superweapon;
    if (ground && !u.lob_charged) return;
    const Unit* tp = nullptr;
    if (!ground) {
        tp = unit(current_target(u.id));
        // s78f t03: the state of the last targeting service (a team member: the container's raw copy) for this target.
        if (!live(tp) || u.tstate != 4 || u.ttarget != tp->id) {
            u.weapon_pulses = ud.weapon_pulse_count;
            return;
        }
        if ((tp->flags & kUnitStealthed) && !ud.target_stealth_units && tp->owner != u.owner) return;
        if (!is_enemy(u.owner, tp->owner)) return;
        if (!clash_allows(u.id, tp->id)) return;  // s78n t06 review: Fire_At 0x3825b0's pairing rule, as fire_hardpoint
    }
    const ObjectId tid = tp ? tp->id : kNoObject;
    // S9: the ordered hardpoint (command 40 index) of the ordered target, while it is alive.
    int32_t thp = -1;
    if (tp && tp->id == u.ordered_target && u.ordered_hp >= 0 && static_cast<uint32_t>(u.ordered_hp) < tp->hp_count &&
        !hps_[tp->hp_begin + static_cast<uint32_t>(u.ordered_hp)].destroyed)
        thp = u.ordered_hp;
    int32_t proj = fired_weapon_projectile(u);  // OI abilities: weapon index obj +0x324
    float dscale = 1.0f;  // OI abilities: Blast state gate (REMAINING_GAPS §5.5; a weapon never fires while it runs)
    if (hooks_ && !hooks_->fire_gate(u.id, nullptr, proj, dscale)) return;
    if (hooks_ && !hooks_->shot_allowed(u.id, tid, thp, proj)) return;
    if (proj < 0 || static_cast<size_t>(proj) >= data_->projectiles.size()) return;
    const ProjectileDesc& pd = data_->projectiles[static_cast<size_t>(proj)];
    const float range = fire_range(ud.max_attack_distance, mod_r(u, ModList::FireRange));  // 0x54d090: list 8
    // No range test at fire time (LOB_SOLVER §8 item 22 [H]): the targeting state above is the gate; for a grenade the
    // launch solve below can still find no arc from the muzzle (no shot).
    const bool lob = grenade(pd);
    if (pd.energy_per_shot > 0.0f && has_energy_pool(u) && u.energy < pd.energy_per_shot) return;
    // S8 t06 B4 (LOB_SOLVER §5.1): the launch point is the next fire bone <Muzzle_Bone_Name>_NN (round robin per shot; with
    // more than one Projectile_Types entry and more than one bone, the bone of the weapon index), rest pose turned by the
    // unit's facing [port: retail reads the animated pose]. No bone on a known model: the weapon never fires. Bone index
    // outside the list: no fire bone - the position + the shooter's own Ranged_Target_Z_Adjust (0x398440). [port] No
    // model data: straight shots leave from the position, grenades from the position + the z adjust (the pre-t06 rule).
    Vec3 muzzle = u.pos;
    bool on_bone = false;
    if (ud.muzzle_model) {
        const auto n = static_cast<uint32_t>(ud.muzzle_bones.size());
        if (n == 0) return;
        const uint32_t i = u.weapon_bone < n ? u.weapon_bone : 0u;
        u.weapon_bone = i + 1u < n ? i + 1u : 0u;  // advanced before the spawn: a null spawn keeps the new index (§5.2)
        const int64_t bi = ud.weapon_projectiles.size() > 1 && n > 1 ? static_cast<int64_t>(u.weapon_index) : static_cast<int64_t>(i);
        if (bi >= 0 && bi < static_cast<int64_t>(n)) {
            muzzle = local_to_world(u, ud.muzzle_bones[static_cast<size_t>(bi)]);
            on_bone = true;
        }
    }
    if (!on_bone && (lob || ud.muzzle_model)) muzzle.z = muzzle.z + ud.ranged_target_z_adjust;
    // S8 t10 (LOB_SOLVER §5.1): a garrisoned shooter whose host lists gun bones launches from a synced pick among the host's
    // bones within 60 degrees of the target, replacing the request's bone (the own bone count above is still required).
    if (Vec3 gm; garrison_muzzle(u, tp ? tp->pos : u.lob_point, rng, gm)) muzzle = gm;
    // S8 t04 (LOB_SOLVER §5.1 / §5.2): Spawn_Projectile replaces a team-container target by its member nearest the launch
    // point (the hardpoint index is reset). A grenade solves from the launch point toward the un-led target point Q (the
    // member + its Ranged_Target_Z_Adjust, or the ground point as given); no arc = no shot, no cooldown / pulse change
    // (retried next service; a fire-at-position order stays charged). s78f t06 (LOB_SOLVER §5.2 "Order and draws"): the
    // solve comes AFTER the garrison pick above and BEFORE the aim draws below, as in retail - a garrisoned shooter with
    // two or more qualifying host bones has already spent its Get_Int when the solve finds no arc (kept on purpose: the
    // retry spends one draw per service); a non-garrisoned shooter spends none.
    // [port] The projectile keeps the container as its target object.
    const Unit* am = tp ? &lob_member(*tp, muzzle) : nullptr;
    LobSolution sol;
    Vec3 q;
    if (lob) {
        q = am ? lob_target_point(*am) : u.lob_point;
        if (!lob_solve(muzzle, q, pd.max_speed, lob_g(pd), pd.can_lob_slower, sol)) return;
    }
    // S8 t27 (§3.1.1 "Own-weapon aim point", 0x54a270): no target object -> the point as given, no lead; else the
    // shooter's target hardpoint bone, a synced Target_Bones pick or the position + Ranged_Target_Z_Adjust
    // (own_aim_point), then the circular lead. The hardpoint index is the current target's (TD +0x10: the order's or the
    // scan's), reset for a team member.
    Vec3 aim = u.lob_point;
    if (am) {
        int32_t ahp = -1;
        if (am == tp) ahp = tp->id == u.ordered_target ? u.ordered_hp : tp->id == u.auto_target ? u.auto_hp : -1;
        aim = lead_point(*am, muzzle, own_aim_point(*am, ahp, rng), pd.max_speed * static_cast<float>(cfg_.fps));  // circular lead
    }
    if (cfg_.inaccuracy_enabled) {
        // A1j t03: own-weapon inaccuracy 0x549f80 (TACTICAL_COMBAT §3.1.2) [H unless tagged]. Fixed radius r > 0: X and Y
        // draws in (-r, r); the Z draw is made in space only. Otherwise, with a target object: d = the first
        // Targeting_Fire_Inaccuracy entry hitting the target's CategoryMask (scaled like r), and when d > 0 and the 3D
        // muzzle -> led aim distance L != 0: s = d x L / Targeting_Max_Attack_Distance, X, Y, Z draws in (-s, s).
        const float f = inaccuracy_f(cfg_.mode, player(u.owner).ai_accuracy);
        const float ws = cfg_.mode == GameMode::Land ? weather_scale(pd, u.owner) : 1.0f;  // L1 systems: weather
        float r = scale_inaccuracy(ud.fire_inaccuracy_fixed_radius, f);
        if (ws != 1.0f) r = r * ws;
        for (int s = 0; s < 2; ++s) {
            const UnitAbilityDesc* ab = ability(u, s);
            if (ab && u.ability_active[static_cast<size_t>(s)] && ab->inaccuracy_override > 0.0f) {
                r = ab->inaccuracy_override;
                break;
            }
        }
        if (r > 0.0f) {
            Vec3 o;
            o.x = rng.range_float(-r, r);
            o.y = rng.range_float(-r, r);
            if (cfg_.mode == GameMode::Space) o.z = rng.range_float(-r, r);
            aim = aim + o;
        } else if (am) {
            float d = 0.0f;
            for (const InaccuracyEntry& e : ud.fire_inaccuracy)
                if (e.category_mask & desc_of(*am).category_mask) {
                    d = e.distance;
                    break;
                }
            d = scale_inaccuracy(d, f);
            if (ws != 1.0f) d = d * ws;
            const float L = distance3(muzzle, aim);
            // [port] Targeting_Max_Attack_Distance <= 0 would divide by zero in retail: the draws are kept, spread 0.
            if (d > 0.0f && L != 0.0f)
                aim = aim + inaccuracy_offset(ud.max_attack_distance > 0.0f ? d * L / ud.max_attack_distance : 0.0f, rng);
        }
    }
    spawn(u, pd, static_cast<uint32_t>(proj), pd.damage * dscale, pd.damage_type, muzzle, aim, tp, thp, range, frame,
          lob ? &sol : nullptr, q);
    if (ground) {  // S8 t26: a successful spawn clears the order (TD +0x3c / +0x3d / +0x30..+0x38 := 0)
        u.lob_pending = 0;
        u.lob_charged = 0;
        u.lob_point = Vec3{};
    }
    if (pd.energy_per_shot > 0.0f && has_energy_pool(u)) {
        u.energy = u.energy - pd.energy_per_shot;
        if (u.energy < 0.0f) u.energy = 0.0f;
    }
    const float W = weapon_delay(u, nullptr);
    const float F = fire_rate(u);
    u.weapon_pulses -= 1;
    if (u.weapon_pulses > 0) {
        u.weapon_cooldown = weapon_pulse_delay(ud.weapon_pulse_delay, F, cfg_.fps);
    } else {
        const Recharge r = weapon_reset(ud.weapon_recharge, ud.weapon_pulse_count, W, F, cfg_.fps, rng);
        u.weapon_cooldown = r.frames;
        u.weapon_pulses = r.pulses;
    }
    ++stats_.shots;
    // OI fog: REVEAL vf26 flash on the target owner's grid [port: a ground-point shot has no target owner, no flash].
    if (tp) shot_events_.push_back({u.id, tp->id, tp->owner});
    if (hooks_) hooks_->fire_done(u.id, nullptr, frame);
    if (hooks_) hooks_->shot_fired(u.id, tid, thp, proj, frame);
}

bool CombatSystem::own_launch_point(const Unit& u, Vec3& out) const {
    // LOB_SOLVER §3.6 "Charge-up aim" step 2: the weapon's NEXT fire bone (the WEAPON pack's cycle index TD +0xf0 into its
    // bone list; rest pose turned by the facing, as fire_unit_weapon); [port] no model data: the position + its own
    // Ranged_Target_Z_Adjust (the own weapon's no-model launch point). A known model without fire bones: none.
    const UnitDesc& ud = desc_of(u);
    if (ud.muzzle_model) {
        const auto n = static_cast<uint32_t>(ud.muzzle_bones.size());
        if (n == 0) return false;
        out = local_to_world(u, ud.muzzle_bones[u.weapon_bone < n ? u.weapon_bone : 0u]);
        return true;
    }
    out = Vec3{u.pos.x, u.pos.y, u.pos.z + ud.ranged_target_z_adjust};
    return true;
}

bool CombatSystem::lob_turret_aim(Unit& u) {
    // 0x631fe0(turret, ordered point) with the aim update 0x631210 (LOB_SOLVER §3.6 "Charge-up aim", s78f t04) [H unless
    // tagged]: target angles toward the point as given, relative to the body (yaw from the turret bone - [port] the object
    // position: no turret bone offsets in the combat data - minus the facing yaw, wrapped; pitch geometric with up
    // negative, or -theta from Lob_Solve from the weapon's next fire bone for a category-3 projectile, no solution -> not
    // aimed); outside Turret_Rotate_Extent_Degrees (or, unless Turret_XY_Only, Turret_Elevate_Extent_Degrees) -> not aimed.
    // A unit without deploy states always uses the undeployed extents. Then one slew step: pitch and yaw each move
    // toward the target by at most Turret_Rotate_Speed degrees (yaw by the wrapped difference, then wrapped and clamped to
    // +-extent). Aimed (1) only when the current angles equal the target after the step (0x386170: within 0.0001 after
    // mapping negatives into [0, 360)). [port] Turret_Rotate_Speed <= 0: the target is reached at once.
    const UnitDesc& ud = desc_of(u);
    const int32_t proj = fired_weapon_projectile(u);
    if (proj < 0 || static_cast<size_t>(proj) >= data_->projectiles.size()) return false;
    const ProjectileDesc& pd = data_->projectiles[static_cast<size_t>(proj)];
    const Vec3 p = u.lob_point;
    const float dx = p.x - u.pos.x, dy = p.y - u.pos.y, dz = p.z - u.pos.z;
    const float to = (dx == 0.0f && dy == 0.0f) ? 0.0f : dmath::atan2(dy, dx) * kRadToDeg;
    const float yaw = wrap_deg(to - dmath::atan2(u.facing.y, u.facing.x) * kRadToDeg);
    float pitch = -dmath::atan2(dz, dmath::sqrt(dx * dx + dy * dy)) * kRadToDeg;
    if (grenade(pd)) {  // can-aim 0x631a00: a category-3 projectile solves from the next fire bone
        Vec3 from;
        LobSolution s;
        if (!own_launch_point(u, from) || !lob_solve(from, p, pd.max_speed, lob_g(pd), pd.can_lob_slower, s)) return false;
        pitch = -s.angle * kRadToDeg;
    }
    if (abs_f(yaw) > ud.turret_extent) return false;
    if (!ud.turret_xy_only && abs_f(pitch) > ud.turret_elevate_extent) return false;
    const float sp = ud.turret_rotate_speed;
    if (!(sp > 0.0f)) {
        u.lob_tpitch = pitch;
        u.lob_tyaw = yaw;
    } else {
        const float de = pitch - u.lob_tpitch;
        u.lob_tpitch = abs_f(de) <= sp ? pitch : u.lob_tpitch + (de > 0.0f ? sp : -sp);
        const float dyaw = wrap_deg(yaw - u.lob_tyaw);
        const float ny = wrap_deg(abs_f(dyaw) <= sp ? yaw : u.lob_tyaw + (dyaw > 0.0f ? sp : -sp));
        u.lob_tyaw = std::clamp(ny, -ud.turret_extent, ud.turret_extent);
    }
    const auto w = [](float v) { return v < 0.0f ? v + 360.0f : v; };
    return abs_f(w(u.lob_tpitch) - w(pitch)) <= 0.0001f && abs_f(w(u.lob_tyaw) - w(yaw)) <= 0.0001f;
}

void CombatSystem::lobbing_service(Unit& u, uint32_t frame) {
    // S8 t26 (TACTICAL_COMBAT §3.1.1 "Fire-at-position order", LobbingSuperweaponBehavior vf6 0x4ea6f0) [H unless tagged]:
    // while the order is pending and the charge-up stamp is 0, aim the TURRET at the point (0x631fe0: s78f t04, the slew
    // of lob_turret_aim; the charge-up starts on the service where the angles are reached); no TURRET / no solution /
    // outside the extents / still slewing -> wait (no timeout). Once aimed the stamp := now; when now >= stamp +
    // Lobbing_Superweapon_Chargeup_Frames: charged (TD +0x3d) := 1, stamp := 0. After the stamp the turret holds its aim
    // (TD +0xf6 stays set; the next order slews from there). The chargeup / fire SFX and particle are presentation.
    if (!u.lob_pending) return;
    const UnitDesc& ud = desc_of(u);
    if (u.lob_stamp == 0) {
        if (!ud.turret) return;
        if (!lob_turret_aim(u)) return;
        u.lob_stamp = frame;
        lob_events_.push_back({u.id, false, frame, u.lob_point});  // S8 t30: chargeup SFX + particle
    }
    if (u.lob_stamp != 0 && frame >= u.lob_stamp + ud.lobbing_chargeup_frames) {
        u.lob_charged = 1;
        u.lob_stamp = 0;
        lob_events_.push_back({u.id, true, frame, u.lob_point});  // S8 t30: Lobbing_Superweapon_SFXEvent_Fire
    }
}

bool CombatSystem::fires_at_position(ObjectId id) const {
    const Unit* u = unit(id);
    return live(u) && desc_of(*u).lobbing_superweapon && desc_of(*u).weapon_projectile >= 0;
}

bool CombatSystem::order_fire_at_position(ObjectId id, Vec3 point) {
    if (!fires_at_position(id)) return false;
    Unit* u = unit(id);
    u->lob_point = point;  // 0x4ea3f0: TD +0x30..+0x38 := point, +0x3c := 1 (charged / stamp unchanged)
    u->lob_pending = 1;
    return true;
}

uint8_t CombatSystem::fire_at_position_state(ObjectId id, Vec3* point) const {
    const Unit* u = unit(id);
    if (!u || !(u->lob_pending || u->lob_charged)) return 0;
    if (point) *point = u->lob_point;
    return u->lob_charged ? 2 : 1;
}

void CombatSystem::spawn(const Unit& shooter, const ProjectileDesc& pd, uint32_t pdesc, float damage, uint16_t damage_type,
                         Vec3 muzzle, Vec3 aim, const Unit* target, int32_t target_hp, float range, uint32_t frame,
                         const LobSolution* lob, Vec3 q_point) {
    Projectile p;
    p.serial = next_serial_++;
    p.desc = pdesc;
    p.shooter = shooter.id;
    p.owner = shooter.owner;
    p.target = target ? target->id : kNoObject;
    p.target_hp = target_hp;
    p.pos = muzzle;
    p.dir = normalize(aim - muzzle);
    p.speed = pd.max_speed;
    p.spawn_frame = frame;
    p.damage = damage;
    p.damage_type = damage_type;
    p.kind = pd.damage_kind;
    // Maximum travel (+0x6c) = Get_Fire_Range (Projectile_Max_Flight_Distance when <= 0) + target
    // radius + |aim.z - muzzle.z|; 0 -> lifetime expiry.
    // refixf t01 (S9 §2.4.10 step 1, MaxTravelDistance +0x6c): the same rule for rockets [H]; [M] the target radius term
    // is the retail radius helper 0x397780 (+ its container's when garrisoned), here UnitDesc::radius.
    const float base = range > 0.0f ? range : pd.max_flight_distance;
    // S8 t04 (LOB_SOLVER §5.3): a solved grenade's height term is |Q.z - L.z| (the un-led target point).
    const float dz = lob ? abs_f(q_point.z - muzzle.z) : abs_f(aim.z - muzzle.z);
    p.max_travel = base > 0.0f ? base + (target ? desc_of(*target).radius : 0.0f) + dz : 0.0f;  // S8 t26: no target, no radius
    // S8 t04 (LOB_SOLVER §5.3 / §5.4): category 3 flies the closed-form parabola from the launch point. WeaponBehavior
    // shots take the solver's pitch theta and speed s with the yaw of the led, inaccurate aim; hardpoint shots (Fire_At
    // 0x3825b0, no solver) leave straight at the aim point at Max_Speed and gravity pulls them below that line.
    if (grenade(pd)) {
        p.lob = 1;
        p.gravity = lob_g(pd);
        p.launch = muzzle;
        if (lob) {
            p.lob_dir = lob_forward(aim - muzzle, lob->angle);
            p.speed = lob->speed;  // pack +0x90 := the solver's speed
        } else {
            p.lob_dir = p.dir;
        }
        p.dir = p.lob_dir;
    }
    // refixf t01 (S9 §2.4.10 step 1): a spline rocket keeps FireAtPosition = `aim` and the launch point. [M] `aim` is the
    // shared fire path's aim point (target / hardpoint position, circular lead, then the fire-time inaccuracy) standing in
    // for 0x54a270 + 0x549f80 (not re-read). [M/port] Facing: retail leaves along the x axis of the fire-bone (or shooter
    // world) transform; the port has no bone orientations, so the shared path's muzzle -> aim direction (p.dir) is used.
    if (spline_rocket(pd)) {
        p.rocket = 1;
        add_rocket(p, aim, muzzle);
    }
    projectiles_.push_back(p);
}

// ================================================================================================
// projectiles (sections 3.3-3.5)
// ================================================================================================

CombatSystem::SweepHit CombatSystem::sweep_segment(const Projectile& p, Vec3 delta, QueryContext& ctx) const {
    SweepHit best{kNoObject, 0.0f};
    const Ray ray{p.pos, delta, 1.0f};
    const auto np = static_cast<int32_t>(std::min<uint32_t>(player_count(), trees_->player_count()));
    // Find_First_Object_Hit_By_Segment_All_Enemies (0x2abcf0, §3.4): hostile players' trees, Query_Ray, then per entry
    // the precise model test (0x38f900) or the Collision_Box_Modifier sphere (refixf t07: the host's precise_hit_; the
    // query mask is 7 with Projectile_Does_Shield_Damage, else 5). The original returns the first passing entry in tree
    // order; the port takes the minimum (segment fraction, object id) over every passing entry of every hostile tree.
    PreciseQuery pq;
    pq.a = p.pos;
    pq.b = p.pos + delta;
    const ProjectileDesc& pd = data_->projectiles[p.desc];
    pq.mask = pd.does_shield_damage ? 7u : 5u;
    auto scan = [&](int32_t q, SweepHit& out) {
        for (const RayHit& hit : trees_->player(static_cast<uint32_t>(q)).query_ray(ray, kSkipDead, ctx)) {
            const Unit* v = unit(static_cast<ObjectId>(hit.handle));
            if (!live(v)) continue;
            SweepHit c{v->id, hit.t};
            if (precise_hit_) {
                pq.victim = v->id;
                pq.shield_up = v->shield > 0.0f;
                pq.collide = (v->flags & kUnitGarrisoned) == 0;
                PreciseResult r;
                const PreciseOutcome o = precise_hit_(pq, r);
                if (o == PreciseOutcome::Miss) continue;
                if (o == PreciseOutcome::Hit) {
                    c.t = r.t;
                    c.mesh_hp = r.mesh ? hardpoint_by_mesh(*v, *r.mesh) : -1;
                }
            }
            if (out.id == kNoObject || c.t < out.t || (c.t == out.t && c.id < out.id)) out = c;
            // Without the precise test the entries are sorted by (t, id): the first live one is this tree's minimum.
            if (!precise_hit_) break;
        }
    };
    for (int32_t q = 0; q < np; ++q)
        if (is_enemy(p.owner, q)) scan(q, best);
    // §3.4 step 3 (S8 t06): nothing hit and Friendly_Damage_Amount > 0 or Causes_Infection -> the own-player variant
    // 0x2acb90. S8 t10 ("Own-player variant 0x2acb90" [H]): the query over the projectile owner's tree skips only the
    // projectile itself (never in a port tree), NOT the shooter, and yields its first passing candidate (the port's order:
    // the minimum (t, id), as the hostile scan); Sweep then keeps it only when it is the projectile's own target and has no
    // TRANSPORT_LANDING - otherwise there is no own hit (the query does not go on to the next candidate).
    if (best.id == kNoObject && (pd.friendly_damage_amount > 0.0f || pd.causes_infection) && p.owner >= 0 && p.owner < np) {
        SweepHit own{kNoObject, 0.0f};
        scan(p.owner, own);
        if (own.id != kNoObject && own.id == p.target) {
            const Unit* v = unit(own.id);
            if (v && !desc_of(*v).transport_landing) best = own;
        }
    }
    return best;
}

int32_t CombatSystem::hardpoint_by_mesh(const Unit& v, const std::string& mesh) const {
    // 0x394d40 (§2.9 Q1 step 4): the first hardpoint in HardPoints order whose Collision_Mesh equals the name (_stricmp;
    // both sides are upper case); -1 when none.
    if (mesh.empty()) return -1;
    for (uint32_t i = 0; i < v.hp_count; ++i)
        if (data_->hardpoints[hps_[v.hp_begin + i].desc].collision_mesh == mesh) return static_cast<int32_t>(i);
    return -1;
}

void CombatSystem::sweep(uint32_t pi, QueryContext& ctx) {
    const Projectile& p = projectiles_[pi];
    SweepHit best{kNoObject, 0.0f};
    // OI abilities: placed bombs only detonate on the fuse; S8 t04: a stuck sticky grenade no longer sweeps.
    const bool placed = p.fuse >= 0.0f && !p.lob;
    if (p.alive && p.spawn_frame < frame_ && !placed && !p.stuck) {
        if (p.lob) {
            best = sweep_segment(p, lob_next(p, frame_) - p.pos, ctx);  // S8 t04: P -> N(t) (LOB_SOLVER §6.2 step 3)
        } else if (p.rocket == 2) {
            rocket_next_[pi].ok = 0;  // cu8k t01: a thrown saber never sweeps (category 8: no direct hit)
        } else if (p.rocket) {
            // refixf t01: a planned spline rocket sweeps P -> N, N = the path sampled at s + speed (§2.4.2 steps 2 / 5;
            // read-only here, the commit phase advances s). An unplanned one (first flight step) is planned and swept in
            // the serial commit phase (its plan draws).
            RocketNext& rn = rocket_next_[pi];
            rn.ok = 0;
            const Rocket* rk = find_rocket(p.serial);
            Vec3 n;
            if (rk && rk->planned && rk->path.sample(rk->s + p.speed, n)) {
                rn.n = n;
                rn.ok = 1;
                best = sweep_segment(p, n - p.pos, ctx);
            }
        } else {
            const Vec3 delta = p.dir * p.speed;
            best = sweep_segment(p, delta, ctx);
            // A1j t03: Sweep 0x4c97a0 step 1, the land ground test 0x4d0630 of the same segment (TACTICAL_COMBAT §3.4).
            if (straight_ground_test(p, delta)) best.ground = ground_hit(p.pos, p.pos + delta, best.gh);
        }
    }
    sweep_[pi] = best;
}

void CombatSystem::apply_or_queue(const DamageRequest& r, float delay_secs, uint32_t frame) {
    if (delay_secs > 0.0f) {
        Delayed d;
        d.victim = r.victim;
        d.frames = delay_frames(delay_secs, cfg_.fps);
        d.amount = r.amount;
        d.damage_type = r.damage_type;
        d.kind = r.kind;
        d.source_is_projectile = r.source_is_projectile ? 1 : 0;
        d.source = r.source;
        d.projectile_desc = r.projectile_desc;
        d.hardpoint = r.hardpoint;
        d.projectile_target = r.projectile_target;  // s78n t12
        delayed_.push_back(d);
    } else {
        take_damage(r, frame);
    }
}

void CombatSystem::process_hit(Projectile& p, Unit& v, float t, uint32_t frame, SimRng& rng, const Vec3* segment,
                               int32_t mesh_hp) {
    // Process_Hit (0x4cb280) [H unless tagged].
    const ProjectileDesc& pd = data_->projectiles[p.desc];
    const UnitDesc& vd = desc_of(v);
    ++stats_.hits;
    float dmg = p.damage;
    if (dmg == 0.0f) dmg = pd.blast_area_damage;
    const Vec3 impact = segment ? p.pos + *segment * t : p.pos + p.dir * (p.speed * t);
    // refixf t06 (TACTICAL_COMBAT §2.9 Q1 steps 1-3): the hardpoint the hit names.
    int32_t random_hp = -1;
    if (pd.damages_random_hardpoints && v.hp_count > 0) {
        // synced Get_Int(0, count - 1), then probe forward (wrapping) to a live destroyable one
        const auto start = static_cast<uint32_t>(rng.range_int(0, static_cast<int32_t>(v.hp_count) - 1));
        for (uint32_t k = 0; k < v.hp_count; ++k) {
            const uint32_t j = (start + k) % v.hp_count;
            const Hardpoint& h = hps_[v.hp_begin + j];
            if (!hp_dead(h) && data_->hardpoints[h.desc].destroyable) {
                random_hp = static_cast<int32_t>(j);
                break;
            }
        }
    }
    int32_t hp = -1;
    if (v.id == p.target && p.target_hp >= 0) {
        hp = p.target_hp;  // Take_Damage name override: the targeted hardpoint wins (the random draw is still made)
    } else if (cfg_.mode == GameMode::Land) {
        hp = closest_hardpoint(v, impact, true);  // land: closest live targetable to the projectile [M: impact point]
    } else if (pd.damages_random_hardpoints) {
        hp = random_hp;  // space: the random pick's Collision_Mesh
    } else {
        // space: the struck mesh's hardpoint (refixf t07: from the precise test; without one the mesh_hardpoint stand-in)
        hp = mesh_hp == kMeshUnknown ? mesh_hardpoint(v, impact) : mesh_hp;
    }
    DamageRequest r;
    r.victim = v.id;
    r.amount = dmg;
    r.damage_type = p.damage_type;
    r.kind = p.kind;
    r.source = p.shooter;
    r.source_is_projectile = true;
    r.projectile_target = p.target;  // s78n t12
    r.projectile_desc = static_cast<int32_t>(p.desc);
    r.hardpoint = hp;
    r.has_impact = true;
    r.impact = impact;
    // S9 t04 (§1.5): a Causes_Infection hit attaches the INFECTION behaviour (or resumes it: counter - 1). [port] Before
    // Take_Damage (the order inside 0x4cb280 was not traced); the shooter's INFECTION-ability test is left to the data
    // (vanilla: only Infection_Projectile, thrown by that ability). refix t02: the behaviour runs with the shooter type's
    // Infection_Ability parameters; a shooter that is gone or has none starts a parameterless one (flag only) [port].
    if (pd.causes_infection) {
        const Unit* sh = unit(p.shooter);
        const int32_t idesc = sh && desc_of(*sh).infection.has ? static_cast<int32_t>(sh->desc) : -1;
        infect(v, idesc, p.owner, p.shooter, frame);
    }
    bool absorbed_all = false;
    HitEvent he;  // S10-int (R4): the detonation for the host's PARTICLE objects
    he.victim = v.id;
    he.shooter = p.shooter;
    he.owner = p.owner;
    he.projectile_desc = p.desc;
    he.position = impact;
    if (pd.damage_delay_secs > 0.0f) {
        const float delay = rng.range_float(0.25f * pd.damage_delay_secs, pd.damage_delay_secs);  // synced
        apply_or_queue(r, delay, frame);
        he.delayed = true;
    } else {
        const DamageResult res = take_damage(r, frame);
        absorbed_all = res.absorbed > 0.0f && res.to_hull == 0.0f && res.to_hardpoint == 0.0f;  // [M]
        he.armor_reduced = res.hull_armor_factor <= 0.75f;
    }
    // Visual picks are synced draws too (section 0.1).
    if (!absorbed_all) {
        if (vd.hit_particle_count > 0) he.pick = rng.range_int(0, static_cast<int32_t>(vd.hit_particle_count) - 1);
    } else if (vd.shield_hit_particle_count > 0) {
        he.pick = rng.range_int(0, static_cast<int32_t>(vd.shield_hit_particle_count) - 1);
    }
    he.absorbed = absorbed_all;
    hit_events_.push_back(he);
    if (pd.blast_area_damage > 0.0f && pd.blast_area_range > 0.0f) area_damage(p, impact, v.id, frame);
    detonation_effects(p, impact, v.id, frame);  // S9: on-detonation effects (ion stun)
}

bool CombatSystem::effect_match(const Unit& v, uint64_t mask, const std::vector<int32_t>* types) const {
    // OI abilities: the per-effect victim filter (0x39f300 / 0x395c10): CategoryMask & mask, or a listed unit type;
    // mask 0 without types = every category [M].
    const UnitDesc& vd = desc_of(v);
    if (types)
        for (int32_t t : *types)
            if (t >= 0 && t == vd.object_type) return true;
    if (mask == 0) return !types || types->empty();
    return (vd.category_mask & mask) != 0;
}

bool CombatSystem::convert_victim_ok(const Unit& v, uint64_t mask) const {
    // s78n t05 (TACTICAL_COMBAT §3.5 "Convert and its team rule" step 2, HERO_CLASH §7.3: 0x395c10 with
    // Projectile_Convert_Enemy_Targets_Category_Mask), in order: 1. CategoryMask & mask; 2. not a vehicle taken over by a
    // vehicle thief (+0x3a2 & 0x08); 3. no PROXIMITY_MINE; 4. Friendly_Damage_Amount <= 0; 5. not Is_Named_Hero; 6. not
    // a build pad holding a live under-construction or finished object (the host's pad state).
    const UnitDesc& vd = desc_of(v);
    if (!effect_match(v, mask, nullptr)) return false;
    if (v.flags & kUnitStolenVehicle) return false;
    if (vd.proximity_mine) return false;
    if (vd.friendly_damage_amount > 0.0f) return false;
    if (vd.named_hero) return false;
    if (hooks_ && hooks_->occupied_build_pad(v.id)) return false;
    return true;
}

void CombatSystem::effect_victims(int32_t owner, bool enemies, Vec3 center, float radius, std::vector<Unit*>& out) {
    // Per-player GOM queries around the detonation (0x2a8fb0), players in list order, ascending id within a player
    // (port, §1.10); enemies of `owner`, or owner + allies (not enemies) when !enemies. Live units, XY circle [M].
    out.clear();
    if (!(radius > 0.0f) || !trees_) return;
    const auto np = static_cast<int32_t>(std::min<uint32_t>(player_count(), trees_->player_count()));
    for (int32_t q = 0; q < np; ++q) {
        if (enemies != is_enemy(owner, q)) continue;
        for (EntityHandle id : trees_->player(static_cast<uint32_t>(q)).query_circle_xy(CircleXY{center, radius}, kSkipDead, serial_ctx_)) {
            Unit* v = unit(static_cast<ObjectId>(id));
            if (live(v)) out.push_back(v);
        }
    }
}

void CombatSystem::detonation_effects(const Projectile& p, Vec3 center, ObjectId direct, uint32_t frame) {
    // 0x4cb070 order (TACTICAL_GAPS §1.10 [H dispatch, M effects]): stun -> distract -> heal -> convert ->
    // invulnerability -> ion stun -> combat mod; weaken from the detonate paths (0x4d0800) last [M].
    const ProjectileDesc& pd = data_->projectiles[p.desc];
    std::vector<Unit*>& vs = effect_scratch_;
    // Stun (land only): STUNNABLE victims of every enemy player in radius (0x4d01c0 -> 0x647290).
    if (pd.stun && cfg_.mode == GameMode::Land && pd.stun_radius > 0.0f && pd.stun_frames != 0) {
        effect_victims(p.owner, true, center, pd.stun_radius, vs);
        for (Unit* v : vs)
            if (desc_of(*v).stunnable && effect_match(*v, pd.stun_mask, &pd.stun_types)) stun_unit(v->id, pd.stun_frames, frame);
        // Destroy_When_Stunned_Over_Water: the abilities' Stunned hook (ABILITIES_HARDPOINTS §4.12 [port]).
    }
    // Distract: enemies in radius matching the mask get an order toward the detonation (0x2a7b80) [M] - the host's.
    if (pd.distract && pd.distract_radius > 0.0f && hooks_) {
        effect_victims(p.owner, true, center, pd.distract_radius, vs);
        for (Unit* v : vs)
            if (effect_match(*v, pd.distract_mask, nullptr)) hooks_->unit_effect(v->id, UnitEffect::Distracted, p.owner, center, frame);
    }
    // Instant heal: owner and allies in radius: health := min(max, health + int Health_Increase).
    if (pd.heal && pd.heal_radius > 0.0f) {
        effect_victims(p.owner, false, center, pd.heal_radius, vs);
        for (Unit* v : vs) {
            if (!effect_match(*v, pd.heal_mask, nullptr)) continue;
            const float mh = unit_max_hull(*v);
            const float hnew = v->health + static_cast<float>(pd.heal_amount);
            v->health = hnew < mh ? hnew : mh;
            if (hooks_) hooks_->unit_effect(v->id, UnitEffect::Healed, p.owner, center, frame);
        }
    }
    // Convert: enemies in radius -> the victim and its team members: converted flag (decay) + owner := projectile owner.
    if (pd.convert && pd.convert_radius > 0.0f && p.owner >= 0) {
        effect_victims(p.owner, true, center, pd.convert_radius, vs);
        // s78h t04 (TACTICAL_COMBAT §3.5 "Convert and its team rule"): a victim already owned by the new owner is skipped
        // (0x4ca2f0), so a team listed several times converts once. The team is the 0x39b1a0 container: damage_team
        // (= team on land, the squadron in space), so a space squadron converts as a whole as a land team does.
        for (Unit* v : vs) {
            if (!convert_victim_ok(*v, pd.convert_mask) || !live(v)) continue;  // s78n t05: 0x395c10 in full
            if (v->owner == p.owner) continue;
            // s78i t04 (§3.5 steps 3-5): Change_Owner covers the container and every member. Order = the notification
            // order: the victim, then the container when it is a combat unit (a member victim's container; a container
            // victim is the victim), then the other members ([port] list order = ascending id).
            const uint32_t team = convert_container(*v);
            const ObjectId vid = v->id;
            convert_unit(vid, p.owner);
            if (team != 0) {
                if (team <= static_cast<uint32_t>(kNoObject) && static_cast<ObjectId>(team) != vid)
                    convert_unit(static_cast<ObjectId>(team), p.owner);  // no-op unless the container is a live combat unit
                for (Unit& m : units_)
                    if (m.damage_team == team && m.id != vid && static_cast<uint32_t>(m.id) != team && live(&m))
                        convert_unit(m.id, p.owner);
            }
        }
    }
    // Invulnerability: the projectile's target first, then owner + allies in radius, up to Max_Targets (0x4cc010).
    if (pd.invulnerability && pd.invulnerability_frames > 0) {
        int32_t n = 0;
        const int32_t cap = pd.invulnerability_max_targets > 0 ? pd.invulnerability_max_targets : 0x7FFFFFFF;
        auto give = [&](Unit* v) {
            if (n >= cap || !live(v) || v->invuln_end > frame) return;
            make_invulnerable(v->id, pd.invulnerability_frames, frame);
            ++n;
        };
        if (Unit* t = unit(p.target); live(t) && effect_match(*t, pd.invulnerability_mask, nullptr)) give(t);
        effect_victims(p.owner, false, center, pd.invulnerability_radius, vs);
        for (Unit* v : vs)
            if (v->id != p.target && effect_match(*v, pd.invulnerability_mask, nullptr)) give(v);
    }
    // Ion stun (0x4cbbb0): direct victim, or victims in radius (enemy trees, XY circle, id order).
    if (pd.ion_stun) {
        if (!(pd.ion_stun_radius > 0.0f)) {
            Unit* v = unit(direct);
            if (live(v)) ion_stun_unit(*v, p.desc, frame);
        } else {
            effect_victims(p.owner, true, center, pd.ion_stun_radius, vs);
            for (Unit* v : vs) ion_stun_unit(*v, p.desc, frame);
        }
    }
    // Combat mod (0x4c6950): enemies within Projectile_Blast_Area_Range: append-insert into list (code - 1).
    if (pd.combat_mod > 0 && pd.combat_mod <= kModListCount) {
        const auto list = static_cast<ModList>(pd.combat_mod - 1);
        const uint32_t crc = name_crc(pd.name);
        effect_victims(p.owner, true, center, pd.blast_area_range, vs);
        for (Unit* v : vs)
            if (effect_match(*v, pd.combat_mod_mask, nullptr))
                v->mods.insert(list, 0, pd.combat_mod_value, expiry_frame(frame, pd.combat_mod_seconds, cfg_.fps), crc, -1);
    }
    // Weaken (0x4d0800): two replace-by-source inserts of -Take_Damage_Increase and -Cause_Damage_Reduction for the
    // duration [L: which two lists - taken as defense (list 0, used as 1 - r) and damage dealt (list 1)].
    if (pd.weaken && pd.weaken_radius > 0.0f) {
        const uint32_t crc = name_crc(pd.name);
        const uint32_t expiry = expiry_frame(frame, pd.weaken_seconds, cfg_.fps);
        effect_victims(p.owner, true, center, pd.weaken_radius, vs);
        for (Unit* v : vs) {
            if (!effect_match(*v, pd.weaken_mask, nullptr)) continue;
            v->mods.set_by_source(ModList::Defense, 0, -pd.weaken_take_increase, expiry, crc, -1);
            v->mods.set_by_source(ModList::Damage, 0, -pd.weaken_cause_reduction, expiry, crc, -1);
        }
    }
}

bool CombatSystem::ion_stun_unit(Unit& v, uint32_t pdesc, uint32_t frame) {
    // IonStunEffect::Apply (0x62b920) [H].
    const ProjectileDesc& pd = data_->projectiles[pdesc];
    const auto frames = static_cast<int32_t>(static_cast<float>(cfg_.fps) * pd.ion_stun_duration);
    if (frames <= 0 || (v.flags & kUnitLocomotorBusy)) return false;
    float secs = pd.ion_stun_duration;
    if (!pd.ion_stun_stack_duration || v.ion_stun_end <= frame) {
        v.ion_stun_end = frame + static_cast<uint32_t>(frames);
    } else {
        v.ion_stun_end = v.ion_stun_end + static_cast<uint32_t>(frames);
        secs = static_cast<float>(v.ion_stun_end - frame + 1u) / static_cast<float>(cfg_.fps);
    }
    if (hooks_) hooks_->ion_stunned(v.id, frame);  // DEFEND is switched off
    const uint32_t crc = name_crc(pd.name);
    const uint32_t expiry = expiry_frame(frame, secs, cfg_.fps);
    v.mods.set_by_source(ModList::MaxSpeed, 1, -pd.ion_stun_speed_reduction, expiry, crc, -1);
    v.mods.set_by_source(ModList::FireRate, 1, -pd.ion_stun_shot_rate_reduction, expiry, crc, -1);
    return true;
}

void CombatSystem::area_damage(const Projectile& p, Vec3 center, ObjectId direct, uint32_t frame) {
    // Apply_Area_Damage (0x4c7f70) [H unless tagged].
    const ProjectileDesc& pd = data_->projectiles[p.desc];
    const Unit* shooter = unit(p.shooter);
    const float scatter = shooter ? mult(*shooter, ModKind::ScatterRadius) : 1.0f;
    const float R = pd.blast_area_range * scatter;
    if (!(R > 0.0f)) return;
    int32_t victims = 0;
    const auto np = static_cast<int32_t>(std::min<uint32_t>(player_count(), trees_ ? trees_->player_count() : 0u));
    for (int32_t q = 0; q < np; ++q) {
        if (!is_enemy(p.owner, q)) continue;  // Immune_Faction case not modelled
        const auto ids = trees_->player(static_cast<uint32_t>(q)).query_circle_xy(CircleXY{center, R}, kSkipDead, serial_ctx_);
        for (EntityHandle id : ids) {
            Unit* v = unit(static_cast<ObjectId>(id));
            if (!live(v)) continue;
            if (v->id == direct && v->hp_count == 0) continue;
            float d = distance_xy(center, v->pos);
            if (pd.blast_dropoff) {  // cut to the victim's surface [M: segment collision point]
                d = d - desc_of(*v).radius;
                if (d < 0.0f) d = 0.0f;
            }
            const float amount = area_falloff(d, R, pd.blast_dropoff_tiers) * pd.blast_area_damage;
            DamageRequest r;
            r.victim = v->id;
            r.damage_type = p.damage_type;
            r.kind = p.kind;
            r.source = p.shooter;
            r.source_is_projectile = true;
            r.projectile_target = p.target;  // s78n t12: a splash victim is cleared only when it was the target
            r.projectile_desc = static_cast<int32_t>(p.desc);
            if (v->hp_count == 0) {
                r.amount = amount;
                apply_or_queue(r, area_delay_seconds(d, R, cfg_.max_secs_for_ae_delayed_damage), frame);
            } else {
                // split equally over live destroyable hardpoints within R (3D), each with its own delay
                uint32_t n = 0;
                for (uint32_t i = 0; i < v->hp_count; ++i) {
                    const Hardpoint& h = hps_[v->hp_begin + i];
                    const HardpointDesc& hd = data_->hardpoints[h.desc];
                    if (!h.destroyed && hd.destroyable && distance3(center, hp_world(*v, hd)) <= R) ++n;
                }
                if (n == 0) {  // [L] nothing in reach: the hull takes it
                    r.amount = amount;
                    apply_or_queue(r, area_delay_seconds(d, R, cfg_.max_secs_for_ae_delayed_damage), frame);
                } else {
                    const float share = amount / static_cast<float>(n);
                    for (uint32_t i = 0; i < v->hp_count; ++i) {
                        const Hardpoint& h = hps_[v->hp_begin + i];
                        const HardpointDesc& hd = data_->hardpoints[h.desc];
                        if (h.destroyed || !hd.destroyable) continue;
                        const float dh = distance3(center, hp_world(*v, hd));
                        if (!(dh <= R)) continue;
                        DamageRequest rh = r;
                        rh.amount = share;
                        rh.hardpoint = static_cast<int32_t>(i);
                        apply_or_queue(rh, area_delay_seconds(dh, R, cfg_.max_secs_for_ae_delayed_damage), frame);
                    }
                }
            }
            ++victims;
            if (pd.blast_max_victims > 0 && victims >= pd.blast_max_victims) return;
        }
    }
}

// ================================================================================================
// Take_Damage (section 3.6)
// ================================================================================================

float CombatSystem::diminishing(uint32_t frame, const Unit& v) const {
    const auto& c = cfg_.diminishing_firepower;
    if (c.empty()) return 1.0f;
    // First hit: 999999 frames (TACTICAL_GAPS 3.3) -> past the last point.
    const float secs = static_cast<float>(v.ever_hit ? frame - v.last_hit_frame : 999999u) / static_cast<float>(cfg_.fps);
    // OI tactical: natural cubic spline (vf2 0x2c84b0): first x_k > x; none -> last y; k = 0 -> first y.
    const size_t m = dim_x_.size();
    const size_t k = static_cast<size_t>(std::upper_bound(dim_x_.begin(), dim_x_.end(), secs) - dim_x_.begin());
    if (k >= m) return dim_y_[m - 1];
    if (k == 0) return dim_y_[0];
    const float h = dim_x_[k] - dim_x_[k - 1];
    if (!(h > 0.0f)) return dim_y_[k];
    const float a = (dim_x_[k] - secs) / h, b = (secs - dim_x_[k - 1]) / h;
    return a * dim_y_[k - 1] + b * dim_y_[k] + ((a * a * a - a) * dim_y2_[k - 1] + (b * b * b - b) * dim_y2_[k]) * (h * h) / 6.0f;
}

DamageResult CombatSystem::take_damage(const DamageRequest& r, uint32_t frame) { return take_damage_inner(r, frame, 0); }

int32_t CombatSystem::pick_death_clone(const UnitDesc& vd, int32_t key, int32_t sub_key) {
    // Pick_Death_Clone (0x3718f0), index -1 (S9_DEATH_CLONE_SHIELDS.md §1.3 [H]): key 1 with a sub-key reads 0x18 +
    // sub-key; one entry -> it; two or more -> one synced Get_Int(0, n - 1); none -> nothing (no draw).
    if (key == kKindNormal && sub_key != -1) key = kDeathCloneSubKeyBase + sub_key;
    int32_t n = 0;
    for (const DeathCloneEntry& c : vd.death_clones)
        if (c.key == key) ++n;
    if (n == 0) return -1;
    int32_t pick = 0;
    if (n >= 2) {
        SimRng* rng = listener_rng();
        pick = rng ? rng->range_int(0, n - 1) : 0;  // [port] no RNG bound: first entry
    }
    for (const DeathCloneEntry& c : vd.death_clones)
        if (c.key == key && pick-- == 0) return c.type;
    return -1;
}

void CombatSystem::kill(Unit& v, ObjectId killer, uint8_t kind, uint32_t frame, int32_t sub_key, bool removal) {
    // Die (0x39bdb0), ABILITIES_HARDPOINTS.md 4.9 [H unless tagged].
    if (v.flags & kUnitDead) return;
    // 1. dying flag, health 0, killer
    v.flags |= kUnitDead;
    v.killer = killer;
    v.health = 0.0f;
    hand_over_team(v);  // s78g t06: the team keeps a head from this moment on
    // 2. missile-shield / jammer emitter removal: 0x4d07e0 removes ONE entry (TACTICAL_GAPS §1.7)
    v.emitter_radius = 0.0f;
    if (v.emitter_entries > 0) --v.emitter_entries;
    if (hooks_) hooks_->unit_dying(v.id, frame);
    // 3. death signal 0x25
    DeathEvent ev;
    ev.id = v.id;
    ev.killer = killer;
    ev.kind = kind;
    deaths_.push_back(ev);
    const size_t ei = deaths_.size() - 1;
    ++stats_.deaths;
    // 5. live destroyable hardpoints die in list order: HP_Take_Damage(kind 6, scaled base health). s78n t12 review: not
    //    in a world removal without Die (remove_from_world), which runs only step 7.
    for (uint32_t i = 0; i < v.hp_count && !removal; ++i) {
        Hardpoint& h = hps_[v.hp_begin + i];
        const HardpointDesc& hd = data_->hardpoints[h.desc];
        if (!hd.destroyable || h.destroyed || !(h.health > 0.0f)) continue;
        DamageResult tmp;
        hardpoint_damage(v, h, hd.health * cfg_.hardpoint_health_multiplier, killer, tmp, frame, true);
    }
    // 7. world removal (0x391310): Tactical_Respawn, Destruction_Survivors (OI abilities, TACTICAL_GAPS §1.9)
    death_spawns(v, frame);
    if (removal) return;  // s78n t12 review: no death clone (kind 15), no death projectile, no DeathBehavior draw
    // 8. death clone (tactical modes, kind != 15), S9_DEATH_CLONE_SHIELDS.md §1.2 / §1.3 [H]:
    //    key remap (deploying / deployed Deploys walker: 11 for kind 8, else 12; else an active INFECTION: 20), the
    //    existence check (back to the kind when the remapped key has no list; its draw is consumed, its pick dropped),
    //    then the spawn lookup with the Damage_Type sub-key, else key 1, else the dying unit's own type.
    const UnitDesc& vd = desc_of(v);
    if (cfg_.death_clones && kind != kKindNoDeathClone) {
        int32_t key = kind;
        if (deployed_or_deploying(v))  // S8 t11: 0x39a390 (pending or state 0x19; not in a deploy transition)
            key = kind == 8 ? kKindDeployed : kKindDeployedOther;
        else if (v.flags & kUnitInfected)
            key = kKindInfection;
        if (pick_death_clone(vd, key, -1) < 0) key = kind;
        int32_t clone = pick_death_clone(vd, key, sub_key);
        if (clone < 0) clone = pick_death_clone(vd, kKindNormal, -1);
        if (clone < 0) clone = vd.object_type;
        deaths_[ei].death_clone = clone;
        deaths_[ei].clone_key = key;
    }
    // 9. death projectile: one synced Get_Int(0, n - 1) (no draw for n = 1); spawned at the position +
    //    Ranged_Target_Z_Adjust, owned by the dying object's player, along its facing [L].
    if (!vd.death_projectiles.empty()) {
        const auto n = static_cast<int32_t>(vd.death_projectiles.size());
        SimRng* rng = listener_rng();
        const int32_t pick = rng ? rng->range_int(0, n - 1) : 0;  // [port] no RNG bound: first entry
        const int32_t pdi = vd.death_projectiles[static_cast<size_t>(pick)];
        if (pdi >= 0 && static_cast<size_t>(pdi) < data_->projectiles.size()) {
            const ProjectileDesc& pd = data_->projectiles[static_cast<size_t>(pdi)];
            Projectile p;
            p.serial = next_serial_++;
            p.desc = static_cast<uint32_t>(pdi);
            p.shooter = v.id;
            p.owner = v.owner;
            p.pos = Vec3{v.pos.x, v.pos.y, v.pos.z + vd.ranged_target_z_adjust};
            p.dir = normalize(v.facing);
            p.speed = pd.max_speed;
            p.spawn_frame = frame;
            p.damage = pd.damage;
            p.damage_type = pd.damage_type;
            p.kind = pd.damage_kind;
            p.max_travel = pd.max_flight_distance > 0.0f ? pd.max_flight_distance : 0.0f;
            projectiles_.push_back(p);
            deaths_[ei].death_projectile = pdi;
        }
    }
    // DeathBehavior Initialise (vf9 0x58cdb0, TACTICAL_GAPS §1.9 [H]; attached after Die [M: the draw comes after the
    // step 7-9 draws]): with Spin_Away_On_Death one synced Get_Float(0, 1); spin away when draw <= the chance.
    if (vd.spin_away_on_death) {
        SimRng* rng = listener_rng();
        const float u = rng ? rng->range_float(0.0f, 1.0f) : 1.0f;  // [port] no RNG bound: no draw, no spin-away
        deaths_[ei].spin_away = rng != nullptr && u <= vd.spin_away_chance;
    }
}

void CombatSystem::death_spawns(Unit& v, uint32_t frame) {
    // World removal 0x391310 (Die step 7) with obj +0x3a0 bit 0x80 clear (TACTICAL_GAPS §1.9 [H]). The host creates
    // the objects (one id each, SurvivorEvent order); the owner rule is the host's.
    const UnitDesc& vd = desc_of(v);
    if (vd.tactical_respawn_secs > 0.0f && vd.object_type >= 0) {  // 1. delayed creation of the same type
        SurvivorEvent e;
        e.dead = v.id;
        e.type = vd.object_type;
        e.owner = v.owner;
        e.position = v.pos;
        e.facing = v.facing;
        e.respawn = true;
        e.due_frame = frame + static_cast<uint32_t>(static_cast<float>(cfg_.fps) * vd.tactical_respawn_secs);
        survivor_events_.push_back(e);
    }
    if (vd.destruction_survivors.empty()) return;
    SimRng* rng = listener_rng();
    if (!rng) return;  // [port] outside a tick without a bound RNG: no survivors
    // 2. count = synced Get_Int(Minimum, Maximum)
    const int32_t count = rng->range_int(vd.min_survivors, vd.max_survivors);
    const float total = vd.destruction_survivors.back().second;
    for (int32_t i = 0; i < count; ++i) {
        // 3. two inline synced draws per survivor (0x378cb0): spawn when u >= 1 - total; v in [0, total] picks the first
        //    entry with cumulative weight >= v (binary search 0x378b70).
        const float u = rng->inline_unit_float();
        const float pick = rng->inline_unit_float() * total;
        if (!(u >= 1.0f - total)) continue;
        const auto it = std::lower_bound(vd.destruction_survivors.begin(), vd.destruction_survivors.end(), pick,
                                         [](const std::pair<int32_t, float>& a, float x) { return a.second < x; });
        if (it == vd.destruction_survivors.end() || it->first < 0) continue;
        SurvivorEvent e;
        e.dead = v.id;
        e.type = it->first;
        e.owner = v.owner;
        e.position = v.pos;
        e.facing = v.facing;
        survivor_events_.push_back(e);
    }
}

void CombatSystem::hull_damage(Unit& v, float x, ObjectId source, DamageResult& out, uint32_t frame, uint8_t kind,
                               int32_t sub_key) {
    // Apply_Hull_Damage (0x3ab890).
    if (v.flags & kUnitDead) return;
    const UnitDesc& vd = desc_of(v);
    // Fire ignition (4.8): a combustible victim hit with kind 7 that is not burning ignites instead.
    if (vd.combustible && kind == kKindFire && !v.burning) {
        // OI abilities: BURNING (0x2d) attach. Interval 30 (TACTICAL_GAPS §1.8 [H]); base Initialise (§6.4): first due
        // frame = now + synced Get_Int(0, 29). burn_countdown holds the due frame.
        v.burning = 1;
        SimRng* rng = listener_rng();
        const int32_t first = rng ? rng->range_int(0, kBurningInterval - 1) : 0;  // [port] no RNG bound: due at once
        v.burn_countdown = static_cast<int32_t>(frame) + first;
        out.to_hull = 0.0f;
        return;
    }
    const float old = v.health;
    v.health = old - x;
    out.to_hull = x;
    if (vd.damaged_smoke) {  // Damaged_Smoke_Asset_Name at 75 % / 50 % crossed downward [H]
        const float mh = unit_max_hull(v);
        for (const uint8_t pct : {uint8_t{75}, uint8_t{50}}) {
            const float t = mh * (static_cast<float>(pct) / 100.0f);
            if (old > t && v.health <= t) {
                SmokeEvent e;
                e.id = v.id;
                e.percent = pct;
                smoke_events_.push_back(e);
            }
        }
    }
    out.hull_lost = old - (v.health > 0.0f ? v.health : 0.0f);  // A1_READINGS §30.2: the clamped hull loss
    if (v.health <= 0.0f) {
        if (v.flags & kUnitCannotDie) {
            v.health = old > 1.0f ? old : 1.0f;  // max(1, old)
            out.hull_lost = 0.0f;
        } else {
            kill(v, source, kind, frame, sub_key);
            out.killed = true;
        }
    }
}

void CombatSystem::hardpoint_damage(Unit& v, Hardpoint& h, float x, ObjectId source, DamageResult& out, uint32_t frame,
                                    bool owner_dying, int32_t sub_key) {
    // HP_Take_Damage (0x387f50): destroyable, health > 0, amount > 0; subtract with NO clamp.
    if (!(h.health > 0.0f) || !(x > 0.0f)) return;
    h.health = h.health - x;
    out.to_hardpoint = x;
    if (h.health > 0.0f) return;
    h.destroyed = 1;  // HP_Check_Destroyed; not reversible, never repaired
    const HardpointDesc& hd = data_->hardpoints[h.desc];
    bool last = true;  // no functioning hardpoint of the kind remains (destroyed / disabled do not count)
    for (uint32_t i = 0; i < v.hp_count; ++i) {
        const Hardpoint& o = hps_[v.hp_begin + i];
        if (!hp_out(o) && data_->hardpoints[o.desc].kind == hd.kind) last = false;
    }
    // Set_System_Online(hp, 0, destroyed = 1) (0x384d00): combat + ability side, special-weapon flag.
    const auto index = static_cast<uint32_t>(&h - &hps_[v.hp_begin]);
    set_system_online(v, index, false, true, frame);
    HardpointEvent ev;
    ev.owner = v.id;
    ev.index = index;
    ev.kind = hd.kind;
    ev.owner_dying = owner_dying;
    ev.last_of_kind = last;
    ev.explosion_type = hd.death_explosion_type;
    ev.breakoff_type = hd.death_breakoff_type;
    hp_events_.push_back(ev);
    if (!owner_dying && desc_of(v).destroy_when_all_hardpoints_destroyed && !(v.flags & kUnitDead)) {
        bool any_live = false;
        for (uint32_t i = 0; i < v.hp_count; ++i) {
            const Hardpoint& o = hps_[v.hp_begin + i];
            if (!o.destroyed && data_->hardpoints[o.desc].destroyable) any_live = true;
        }
        if (!any_live) hull_damage(v, v.health, source, out, frame, kKindNormal, sub_key);  // hull set to 0 -> the hull path kills [H]
    }
}

bool CombatSystem::damage_gates(Unit& v, const DamageRequest& r, uint32_t frame) {
    // s78n t12 (HERO_CLASH §8.4.1, Take_Damage 0x3a9e30 in code order). Steps 5 - 7 stop the hit before any clear:
    // 5. 0x3aa1be the +0x37d invulnerability (kinds other than 0 / 13; OI abilities: INVULNERABLE 0x4b is invuln_end);
    // 6. 0x3aa1db Immune_To_Damage; 7. 0x3aa1e8 a garrisoned / hidden victim (+0x3a7) - it takes no damage of any kind.
    const uint8_t k = r.kind;
    if ((v.flags & kUnitInvulnerable) && k != kKindDebug && k != kKindVehicleThief) return false;
    if (v.invuln_end > frame && k != kKindDebug && k != kKindVehicleThief) return false;
    if (desc_of(v).immune_to_damage) return false;
    if (v.flags & kUnitGarrisoned) return false;
    // 8. 0x3aa20e the victim's incognito bit (+0x3a1 & 0x10) goes when a source is given and that source is not a
    // projectile, or is a projectile whose target is this victim (a splash or stray hit keeps it; the projectile's own
    // target loses it even when the shooter is gone). No weapon / targeting test on the source.
    if ((v.flags & kUnitIncognito) && (r.source_is_projectile ? r.projectile_target == v.id : r.source != kNoObject))
        v.flags &= ~kUnitIncognito;
    return true;
}

DamageResult CombatSystem::take_damage_inner(const DamageRequest& r, uint32_t frame, int depth) {
    DamageResult out;
    Unit* vp = unit(r.victim);
    if (vp && (vp->flags & kUnitDead)) {
        // s78n t12 (HERO_CLASH §8.4.1): retail has no live test before step 15, so a hit on a dead victim that is still
        // in the table runs the gates and both incognito clears. [port] nothing else of the call runs for it (no
        // redirect, no multiplier, no damage).
        if (damage_gates(*vp, r, frame)) {
            Unit* s = unit(r.source);
            if (s && (r.source_is_projectile || (s->flags & kUnitIncognito))) s->flags &= ~kUnitIncognito;
        }
        return out;
    }
    if (!live(vp)) return out;
    Unit& v = *vp;
    const UnitDesc& vd = desc_of(v);
    // 1. parent container takes the damage when alive [M]
    if (v.damage_parent != kNoObject && depth < 4) {
        const Unit* pc = unit(v.damage_parent);
        if (live(pc)) {
            DamageRequest r2 = r;
            r2.victim = pc->id;
            r2.hardpoint = -1;
            return take_damage_inner(r2, frame, depth + 1);
        }
    }
    // 2. teams [H]. s78g t06: no mode gate in §3.6, so space squadrons count too (vanilla Luke_XWing / TIE_Prototype carry
    // Redirect_Damage_To_Teammates): damage_team = team, or a space squadron member's squadron.
    const uint32_t dteam = v.damage_team;
    if (!r.no_team && dteam != 0 && (vd.redirect_damage_to_teammates || vd.share_damage_with_teammates) && depth < 4) {
        uint32_t count = 0;
        for (const Unit& m : units_)
            if (m.damage_team == dteam && !(m.flags & kUnitDead)) ++count;
        if (vd.redirect_damage_to_teammates && count > 1) {
            const float each = r.amount / static_cast<float>(count - 1);
            for (size_t i = 0; i < units_.size(); ++i) {
                const Unit& m = units_[i];
                if (m.damage_team != dteam || (m.flags & kUnitDead) || m.id == v.id) continue;
                DamageRequest r2 = r;
                r2.victim = m.id;
                r2.amount = each;
                r2.hardpoint = -1;
                r2.no_team = true;
                r2.self_origin = r.self_origin || r.source == v.id;  // s78n t19: a self hit passed on stays a self hit
                take_damage_inner(r2, frame, depth + 1);
            }
            out.applied = true;
            return out;
        }
        if (vd.share_damage_with_teammates) {
            const float self_max = unit_max_hull(v);
            if (self_max > 0.0f) {
                for (size_t i = 0; i < units_.size(); ++i) {
                    const Unit& m = units_[i];
                    if (m.damage_team != dteam || (m.flags & kUnitDead)) continue;
                    DamageRequest r2 = r;
                    r2.victim = m.id;
                    r2.amount = unit_max_hull(m) * r.amount / self_max;
                    r2.no_team = true;
                    r2.self_origin = r.self_origin || r.source == v.id;  // s78n t19
                    if (m.id != v.id) r2.hardpoint = -1;
                    take_damage_inner(r2, frame, depth + 1);
                }
                out.applied = true;
                return out;
            }
        }
    }
    // 3. gates and the incognito clears. s78n t12 (HERO_CLASH §8.4.1, retail order): steps 5 - 7 (the invulnerability,
    // Immune_To_Damage and garrisoned gates: no clear, no damage) and step 8 (the victim's clear) are damage_gates.
    const uint8_t k = r.kind;
    if (!damage_gates(v, r, frame)) return out;
    float x = r.amount;
    x = x * mult(v, ModKind::TakeDamage);  // 4 (retail step 9)
    // 5 (retail step 10), the source: a projectile's shooter when it resolves (a dead-this-tick shooter still counts
    // until the flush [M]) loses its incognito bit and its CAUSE_DAMAGE applies whether or not it had the bit; a
    // non-projectile source only when it has the bit - cleared, then CAUSE_DAMAGE. The victim's clear ran first, so a
    // self hit (source == victim, not a projectile) gets no CAUSE_DAMAGE. kUnitSkipTargeting is not read here any more.
    // s78n t13 (§8.4.1 step 10, side finding): the same branch sets the diminishing-firepower allow flag := the
    // source's ALLOW_TARGET_DIMINISHING_FIREPOWER (0x397f60(S, 3), default true). Every other hit (no source, a shooter
    // that is gone, a plain non-projectile source) leaves it false.
    Unit* const src = unit(r.source);
    bool dim_allowed = false;
    if (src && (r.source_is_projectile || (src->flags & kUnitIncognito))) {
        src->flags &= ~kUnitIncognito;
        x = x * mult(*src, ModKind::CauseDamage);
        const UnitAbilityDesc* ab[2] = {ability(*src, 0), ability(*src, 1)};
        const bool act[2] = {src->ability_active[0] != 0, src->ability_active[1] != 0};
        dim_allowed = ability_flag(ab, act, kFlagAllowDiminishingFirepower);
    }
    if (v.flags & kUnitInBaseShield) x = x * cfg_.base_shield_vulnerability;  // 6
    // 7 diminishing firepower (retail step 11). s78n t13 (HERO_CLASH §8.4.1, TACTICAL_GAPS §3.3): only for kind 1 and only
    // when step 10 set the allow flag - a projectile with a found shooter, or an incognito non-projectile source. The
    // victim's last-hit stamp (+0x304) is written only then, and here: before the damage-enabled gate, the listeners and
    // the health test (the factor is read first, then the stamp).
    if (k == kKindNormal && dim_allowed) {
        x = x * diminishing(frame, v);
        v.ever_hit = 1;
        v.last_hit_frame = frame;
    }
    if (v.flags & kUnitUnderConstruction) x = x * cfg_.under_construction_damage_multiplier;  // 8
    if (v.flags & kUnitRetreating) x = x * player(v.owner).retreat_damage_mod;               // 9 (cu8 t03: per faction)
    // s78n t12 (retail step 12, 0x3aa3a8): the damage-enabled gate comes after the clears and the multipliers.
    if (!cfg_.damage_enabled && k != kKindDebug && k != 8 && k != kKindVehicleThief) return out;
    out.applied = true;
    // 10 hardpoint name = r.hardpoint
    int32_t hp_index = r.hardpoint;
    // 11 lucky shot (S9 hook): consumed on the hit, x its CAUSE_DAMAGE multiplier; a hull hit moves
    //    to the hardpoint closest to the impact.
    if (hooks_ && r.source != kNoObject) x = x * hooks_->lucky_shot(r.source, v.id, hp_index, r.impact, r.has_impact);
    {  // 12 Get_Damage_Multiplier
        const float a1 = src ? mod_r(*src, ModList::Damage) : 0.0f;
        const float ai = (src && player(src->owner).ai) ? player(src->owner).ai_damage_multiplier : 1.0f;
        x = x * damage_multiplier(a1, mod_r(v, ModList::Defense), ai, 1.0f);
    }
    // 13 damage listeners (style Take_Damage special abilities, e.g. Redirect_Blaster) may cancel.
    if (hooks_ && hooks_->damage_listener(v.id, r.source, r.source_is_projectile ? r.projectile_desc : -1, frame,
                                          listener_rng())) {
        out.cancelled = true;
        out.after_multipliers = 0.0f;
        return out;
    }
    out.after_multipliers = x;
    // 14-16 shields, armor, hull [H]
    uint16_t hull_armor = vd.armor_type;
    for (int s = 1; s >= 0; --s) {  // active ability armor override, slot 1 before slot 0 [M]
        const UnitAbilityDesc* ab = ability(v, s);
        if (ab && v.ability_active[static_cast<size_t>(s)] && ab->armor_override != kNoTypeId) {
            hull_armor = ab->armor_override;
            break;
        }
    }
    const float sf = data_->damage.factor(r.damage_type, vd.shield_armor_type);
    const float hf = data_->damage.factor(r.damage_type, hull_armor);
    out.hull_armor_factor = hf;
    const ProjectileDesc* pd = (r.source_is_projectile && r.projectile_desc >= 0 &&
                                static_cast<size_t>(r.projectile_desc) < data_->projectiles.size())
                                   ? &data_->projectiles[static_cast<size_t>(r.projectile_desc)]
                                   : nullptr;
    const float sx = x * sf;
    float absorbed = 0.0f;
    // s78n t26.1 (MOVEMENT §3.2.1 item 6): Shielded_Only_When_Deployed gates the absorb on D (0x39a390)
    if (k != kKindBypassShield && (!vd.shielded_only_when_deployed || deployed_or_deploying(v))) {
        const bool can = !r.source_is_projectile || (pd && pd->does_shield_damage);
        const bool lockout = (v.shield_depleted &&
                              shield_lockout_active(frame, v.shield_depleted_frame, cfg_.fps, cfg_.depleted_shield_disable_time)) ||
                             (hooks_ && hooks_->ion_storm_shields_down(v.id, frame));  // L1 systems: ion storm
        if (can && !lockout && v.shield > 0.0f && sx > 0.0f) {
            absorbed = v.shield < sx ? v.shield : sx;  // min(shield, x)
            v.shield = v.shield - absorbed;
            if (v.shield <= 0.0f) {  // depletion starts the lockout
                v.shield = 0.0f;
                v.shield_depleted = 1;
                v.shield_depleted_frame = frame;
            }
        }
    }
    out.absorbed = absorbed;
    // S9: energy pool absorption (EnergyPool::Absorb 0x56c230, Does_Energy_Damage only) [L: takes
    // min(energy, the unabsorbed rest) in shield-armor space].
    float energy_taken = 0.0f;
    if (pd && pd->does_energy_damage && has_energy_pool(v) && k != kKindBypassShield) {
        const float rest = sx - absorbed;
        if (rest > 0.0f && v.energy > 0.0f) {
            energy_taken = v.energy < rest ? v.energy : rest;
            v.energy = v.energy - energy_taken;
        }
    }
    out.energy_absorbed = energy_taken;
    // cu8f t02 (CU8F_T01_T02_T07_RE.md §2.4): any positive absorption ("drained" is not "reached 0") with
    // Projectile_Disables_Engines_When_Power_Drained disables the victim's engines for the projectile's duration.
    if (energy_taken > 0.0f && pd->disables_engines_when_drained && hooks_)
        hooks_->engines_drained(v.id, pd->disable_engines_duration, frame);
    float hx;
    if (sf != 0.0f) {
        hx = (sx - absorbed - energy_taken) / sf * hf;  // back to raw, then hull-armor space
    } else {
        hx = x * hf;  // [L] shield factor 0: nothing to convert back from
    }
    if (pd && !pd->does_hitpoint_damage) hx = 0.0f;
    // S9 t04 (§1.1): the death-clone sub-key = the source projectile's Damage_Type index (-1: not a projectile).
    const int32_t sub_key = pd && pd->damage_type != kNoTypeId ? static_cast<int32_t>(pd->damage_type) : -1;
    // s78n t12 review [port]: a self hit (source == victim: STIM_PACK's 0x3a9e30(V, 1, x, V)) runs every step above
    // with the unit as its own source, as retail does; what retail does with the kill credit and the damage record of
    // such a hit is not in the spec, so both stay as they were while the hit was sourceless - a self kill has no killer
    // and the damage event names no source (final-blow player, story "destroyed by", DAMAGE_TRACKING unchanged).
    // s78n t19: the same for the teammates a self hit is redirected to / shared with (DamageRequest::self_origin).
    const ObjectId credit = r.source == v.id || r.self_origin ? kNoObject : r.source;
    // 17 hardpoint routing: a named live destroyable hardpoint takes everything; a named dead destroyable one swallows the
    //    hit (refixf t06, TACTICAL_COMBAT §2.9 Q1 step 5: neither hardpoint nor hull; the shield part stays absorbed); a
    //    named non-destroyable one -> hull.
    if (hp_index >= 0 && static_cast<uint32_t>(hp_index) < v.hp_count) {
        Hardpoint& h = hps_[v.hp_begin + static_cast<uint32_t>(hp_index)];
        if (data_->hardpoints[h.desc].destroyable) {
            if (!hp_dead(h)) hardpoint_damage(v, h, hx, credit, out, frame, false, sub_key);
            track_damage(v, absorbed + out.to_hardpoint, frame);
            record_damage(r, v, src, hp_index, out, frame);
            return out;
        }
    }
    // 18 hull
    hull_damage(v, hx, credit, out, frame, k, sub_key);
    track_damage(v, absorbed + hx, frame);
    record_damage(r, v, src, -1, out, frame);
    return out;
}

void CombatSystem::record_damage(const DamageRequest& r, const Unit& v, const Unit* src, int32_t hp, const DamageResult& out,
                                 uint32_t frame) {
    DamageEvent e;
    e.victim = v.id;
    // s78n t12 review [port]: a self hit is recorded without a source (see above); s78n t19: also its team shares
    const bool self = r.source == v.id || r.self_origin;
    e.source = self ? kNoObject : r.source;
    e.source_owner = src && !self ? src->owner : -1;
    e.source_is_projectile = r.source_is_projectile;
    e.projectile_desc = r.projectile_desc;
    e.kind = r.kind;
    e.hardpoint = hp;
    e.after_multipliers = out.after_multipliers;
    e.absorbed = out.absorbed;
    e.energy_absorbed = out.energy_absorbed;
    e.to_hardpoint = out.to_hardpoint;
    e.to_hull = out.to_hull;
    e.hull_lost = out.hull_lost;
    e.killed = out.killed || (v.flags & kUnitDead) != 0;
    e.frame = frame;
    damage_events_.push_back(e);
}

// ================================================================================================
// upkeep (shields, hull <-> hardpoint coupling)
// ================================================================================================

void CombatSystem::upkeep_unit(Unit& u, uint32_t frame) {
    if (u.flags & kUnitDead) return;
    const UnitDesc& ud = desc_of(u);
    const float C = cfg_.hull_vs_hardpoints_constraint;
    if (u.hp_count) {
        const float maxh = unit_max_hull(u);
        // Hull cap (0x3a6b80, types with +0xa8 only): hull fraction <= min(1, hardpoint fraction + C).
        if (ud.destroy_when_all_hardpoints_destroyed && maxh > 0.0f) {
            float cap = hp_fraction(u) + C;
            if (cap > 1.0f) cap = 1.0f;
            if (cap > 0.0f && u.health / maxh > cap) u.health = cap * maxh;
        }
        // Hardpoint cap (0x3a76b0): excess = (hp fraction - min(1, hull fraction + C)) x T, T = sum
        // of difficulty-scaled base health (no modifiers); each live hardpoint loses health_i / T x excess.
        float T = 0.0f;
        for (uint32_t i = 0; i < u.hp_count; ++i) {
            const HardpointDesc& hd = data_->hardpoints[hps_[u.hp_begin + i].desc];
            if (hd.destroyable) T = T + hd.health * cfg_.hardpoint_health_multiplier;
        }
        if (T > 0.0f && maxh > 0.0f) {
            float hull_cap = u.health / maxh + C;
            if (hull_cap > 1.0f) hull_cap = 1.0f;
            const float excess = (hp_fraction(u) - hull_cap) * T;
            if (excess > 0.0f) {
                for (uint32_t i = 0; i < u.hp_count; ++i) {
                    Hardpoint& h = hps_[u.hp_begin + i];
                    if (h.destroyed || !data_->hardpoints[h.desc].destroyable) continue;
                    h.health = h.health - h.health / T * excess;
                }
            }
        }
    }
    // Shield regeneration (0x5494b0 / 0x549250).
    if (ud.shield_points > 0.0f) {
        u.shield_regen_countdown -= 1;
        if (u.shield_regen_countdown <= 0) {
            u.shield_regen_countdown = std::max<int32_t>(
                1, shield_regen_interval_frames(cfg_.fps, mult(u, ModKind::ShieldRegenInterval), cfg_.shield_recharge_interval_secs));
            uint32_t gens = 0, live_gens = 0;
            for (uint32_t i = 0; i < u.hp_count; ++i) {
                const Hardpoint& h = hps_[u.hp_begin + i];
                if (data_->hardpoints[h.desc].kind != HardpointKind::ShieldGenerator) continue;
                ++gens;
                if (!h.destroyed) ++live_gens;
            }
            const float frac = gens ? static_cast<float>(live_gens) / static_cast<float>(gens) : 1.0f;
            const float amount = ud.shield_refresh_rate * mult(u, ModKind::ShieldRegen) * frac;
            const bool lockout = u.shield_depleted &&
                                 shield_lockout_active(frame, u.shield_depleted_frame, cfg_.fps, cfg_.depleted_shield_disable_time);
            // OI abilities: Shield_Requires_Base_Power (0x56bb00, TACTICAL_GAPS §7.3) - no regeneration without power [M].
            const bool unpowered = ud.shield_requires_base_power && !has_base_power(u.owner);
            if (!u.regen_blocked && !unpowered) {
                const float next = shield_regen_step(u.shield, amount, unit_max_shield(u), lockout, cfg_.depleted_shield_regen_cap);
                if (has_energy_pool(u) && next > u.shield) {
                    // S9 shield exchange (0x56be20): x points cost x * EnergyToShieldExchangeRate, all or nothing.
                    const float cost = (next - u.shield) * cfg_.energy_to_shield_exchange_rate;
                    if (u.energy >= cost) {
                        u.energy = u.energy - cost;
                        u.shield = next;
                    }
                } else {
                    u.shield = next;
                }
            }
        }
    }
    // S9 energy pool regeneration (EnergyPoolBehavior 0x56bfd0 / 0x56c030) [H].
    if (has_energy_pool(u)) {
        u.energy_countdown -= 1;
        if (u.energy_countdown <= 0) {
            u.energy_countdown = std::max<int32_t>(
                1, shield_regen_interval_frames(cfg_.fps, mult(u, ModKind::EnergyRegenInterval), cfg_.energy_recharge_interval_secs));
            if (ud.requires_base_power && !has_base_power(u.owner)) {  // OI abilities: the base-power list (§7.3)
                u.energy = 0.0f;
            } else if (!u.regen_blocked) {
                float e = u.energy + ud.energy_refresh_rate * mult(u, ModKind::EnergyRegen);
                const float mx = unit_max_energy(u);
                if (e < 0.0f) e = 0.0f;
                if (e > mx) e = mx;
                u.energy = e;
            }
        }
    }
}

float CombatSystem::unit_max_energy(const Unit& u) const {
    return apply_bonus(desc_of(u).energy_capacity, mod_r(u, ModList::MaxEnergy));  // 0x372320
}

void CombatSystem::burning_service(Unit& u, uint32_t frame) {
    // BurningBehavior (0x6241b0): Take_Damage(kind 7, Burning_Damage_Per_Second) [H] once per service; OI abilities:
    // serviced every 30 frames (vtable +0x28 = 0x1e, TACTICAL_GAPS §1.8 [H]); burns until death [M].
    if (!u.burning || (u.flags & kUnitDead)) return;
    if (static_cast<int32_t>(frame) < u.burn_countdown) return;  // burn_countdown = due frame
    u.burn_countdown = static_cast<int32_t>(frame) + kBurningInterval;
    DamageRequest r;
    r.victim = u.id;
    r.amount = desc_of(u).burning_damage_per_second;
    r.kind = kKindFire;
    take_damage(r, frame);
}

void CombatSystem::infect(Unit& v, int32_t desc, int32_t player, ObjectId source, uint32_t frame) {
    // cu8f t06 (CU8F_T04_T06_RE §3.5 [H]; projectile hit 0x4cb280 / spread 0x636ae0): attach INFECTION (behaviour factory
    // 0x4c3520) to a victim without it, or resume it (vfunc +0xb8: counter - 1, floored at 0); then the victim's pack
    // (obj +0x218) takes the infector's data: frames left = int(FPS x Duration_Of_Infection) (full), its parameters (the
    // infector type's InfectionDesc: mask, distance, DPS, Seconds_To_Infect), the infecting player and source.
    (void)frame;
    if (v.flags & kUnitDead) return;
    if (!v.infect_attached) {
        v.infect_attached = 1;
        v.infect_suspend = 0;
    } else if (v.infect_suspend != 0) {
        --v.infect_suspend;
    }
    v.infect_desc = desc >= 0 && static_cast<size_t>(desc) < data_->units.size() ? desc : -1;
    v.infect_player = player;
    v.infect_source = source;
    v.infect_left = 0;
    if (v.infect_desc >= 0)
        v.infect_left = static_cast<int32_t>(static_cast<float>(cfg_.fps) *
                                             data_->units[static_cast<size_t>(v.infect_desc)].infection.duration_secs);
    if (v.infect_suspend == 0) v.flags |= kUnitInfected;
}

void CombatSystem::infection_service(Unit& u, uint32_t frame, SimRng& rng) {
    // INFECTION service (vf6 0x636e80, interval 1), cu8f t06 (CU8F_T04_T06_RE §3.5 [H]). cu8g t05: a parameterless
    // behaviour (flag only: spawned / set infected, no infector pack) is serviced too, as the pack it has: frames left 0
    // (no Duration_Of_Infection written; the pack ctor's value [L] taken as 0), so its first service makes the frame % 60
    // draw like any other and then suspends it (expiry) before any damage or spread.
    if (!u.infect_attached || u.infect_suspend != 0 || (u.flags & kUnitDead)) return;
    if (u.flags & kUnitGarrisoned) return;  // nothing while garrisoned (+0x3a7)
    // Coughing SFX: on frames with frame % 60 == 0 one synced Get_Int(0, 4), the sound on 0 (presentation).
    if (frame % 60u == 0u) (void)rng.range_int(0, 4);
    // Frames left -= 1; at <= 0 the behaviour suspends itself (the cure: expiry) and its particle goes.
    if (--u.infect_left <= 0 || u.infect_desc < 0) {  // an empty pack: no DPS, no mask (nothing to damage or spread)
        if (u.infect_left <= 0) suspend_infection(u.id);
        return;
    }
    const InfectionDesc& in = data_->units[static_cast<size_t>(u.infect_desc)].infection;
    DamageRequest r;  // Take_Damage(kind 0x14, DPS / FPS) every frame
    r.victim = u.id;
    r.amount = in.damage_per_second / static_cast<float>(cfg_.fps);
    r.kind = kKindInfection;
    r.source = u.infect_source;
    take_damage(r, frame);
    // Spread 0x636ae0 (the mask's only reader): every player in index order (allies and the infector's own included), that
    // player's objects within Distance_To_Infect (XY) of the infected unit ([port] ascending id for the radius query).
    // A candidate counts when its TD +0x114 != NOW and it passes 0x6cc790: not this unit, not dying / removed, not a
    // projectile, has a locomotor, has a model (obj +0x2a0 non-null; cu8g t05: UnitDesc::has_model), not garrisoned, and
    // its type CategoryMask & Infection_CategoryMask != 0. Actively infected: its counter := 0. Else the consecutive-frame
    // counter (+0x118 += 1 when +0x114 = NOW - 1, else 0; +0x114 := NOW); at count >= FPS x Seconds_To_Infect it is infected with a copy
    // of this pack (duration full) and its counter := 0.
    const float r2 = in.distance_to_infect * in.distance_to_infect;
    const float need = static_cast<float>(cfg_.fps) * in.seconds_to_infect;
    const auto np = static_cast<int32_t>(players_.size());
    for (int32_t p = 0; p < np; ++p) {
        for (Unit& c : units_) {
            if (c.owner != p || &c == &u || c.infect_seen == frame) continue;
            if (c.flags & (kUnitDead | kUnitGarrisoned)) continue;
            const UnitDesc& cd = desc_of(c);
            if (!cd.has_locomotor || !cd.has_model || (cd.category_mask & in.category_mask) == 0) continue;
            if (distance2_xy(u.pos, c.pos) > r2) continue;
            if (c.infect_attached && c.infect_suspend == 0) {
                c.infect_count = 0;
                continue;
            }
            c.infect_count = c.infect_seen != 0xFFFFFFFFu && c.infect_seen + 1u == frame ? c.infect_count + 1 : 0;
            c.infect_seen = frame;
            if (static_cast<float>(c.infect_count) < need) continue;
            infect(c, u.infect_desc, u.infect_player, u.infect_source, frame);
            c.infect_count = 0;
        }
    }
}

void CombatSystem::converted_decay(Unit& u, uint32_t frame, SimRng& rng) {
    // GameObject::Update (0x3a6b80) step 7 (4.10) [H]; draw bounds +-d/10 [M].
    if (!(u.flags & kUnitConverted) || (u.flags & kUnitDead)) return;
    const float t = desc_of(u).converted_die_time;
    if (!(t > 0.0f)) return;
    const float d = unit_max_hull(u) / t / static_cast<float>(cfg_.fps);
    const float draw = rng.range_float(-(d / 10.0f), d / 10.0f);  // one synced Get_Float per frame
    if (d + draw > 0.0f) {
        DamageResult tmp;
        hull_damage(u, d + draw, kNoObject, tmp, frame, kKindConvertedDecay);
    }
}

float CombatSystem::projectile_turn_rate(const ProjectileDesc& pd) const {
    // 0x372560 (rate <= 0 path of Turn_Toward): Max_Rate_Of_Turn x Object_Max_Speed_Multiplier_{Space,Land} x 1.0.
    // [port] the type flag (+0x790 byte 1 bit 0) that skips the multiplier is not read.
    return pd.max_rate_of_turn * (cfg_.mode == GameMode::Space ? cfg_.turn_multiplier_space : cfg_.turn_multiplier_land);
}

bool CombatSystem::shield_away(const Projectile& p, Vec3& away) const {
    // Missile_Shield_Deflect_Test (0x4c6e30), S9_DEATH_CLONE_SHIELDS.md §2.3 [H]: emitters not allied with the
    // projectile's owner, radius = the ability system's qualifying radius (passive / ability 32 / jamming 63); the first
    // (ascending id) with |p - e|^2 < r^2 gives the outward direction (+x when p = e).
    for (const uint32_t idx : emitters_) {
        const Unit& e = units_[idx];
        if (e.flags & kUnitDead) continue;
        if (!is_enemy(e.owner, p.owner)) continue;
        const float r = e.emitter_radius;
        if (!(r > 0.0f)) continue;
        const Vec3 d = p.pos - e.pos;
        if (dot(d, d) < r * r) {
            away = dot(d, d) > 0.0f ? d : Vec3{1.0f, 0.0f, 0.0f};
            return true;
        }
    }
    return false;
}

// ---- refixf t01: spline rockets (S9_DEATH_CLONE_SHIELDS.md §2.4) -------------------------------------------------------

bool CombatSystem::spline_rocket(const ProjectileDesc& pd) const {
    return pd.category == ProjectileCategory::Rocket || pd.category == ProjectileCategory::MptlRocket;
}

void CombatSystem::add_rocket(const Projectile& p, Vec3 fire_at, Vec3 launch) {
    // Spawn (§2.4.10 step 1): FireAtPosition, launch point, s = 0, no path yet; the budget is set by the initial plan.
    Rocket rk;
    rk.serial = p.serial;
    rk.fire_at = fire_at;
    rk.launch = launch;
    rk.budget = data_->projectiles[p.desc].max_flight_distance;
    rockets_.push_back(std::move(rk));  // serials only grow: rockets_ stays in serial order
}

CombatSystem::Rocket* CombatSystem::find_rocket(uint32_t serial) {
    const auto it = std::lower_bound(rockets_.begin(), rockets_.end(), serial,
                                     [](const Rocket& r, uint32_t v) { return r.serial < v; });
    return (it == rockets_.end() || it->serial != serial) ? nullptr : &*it;
}

const CombatSystem::Rocket* CombatSystem::find_rocket(uint32_t serial) const {
    const auto it = std::lower_bound(rockets_.begin(), rockets_.end(), serial,
                                     [](const Rocket& r, uint32_t v) { return r.serial < v; });
    return (it == rockets_.end() || it->serial != serial) ? nullptr : &*it;
}

bool CombatSystem::rocket(size_t i, RocketView& out) const {
    if (i >= projectiles_.size() || !projectiles_[i].rocket) return false;
    const Rocket* rk = find_rocket(projectiles_[i].serial);
    if (!rk) return false;
    out.planned = rk->planned != 0;
    out.s = rk->s;
    out.budget = rk->budget;
    out.fire_at = rk->fire_at;
    out.launch = rk->launch;
    out.last_velocity = rk->last_vel;
    out.path = &rk->path;
    return true;
}

void CombatSystem::expire_projectile(uint32_t i, uint32_t frame) {
    Projectile& p = projectiles_[i];
    const ProjectileDesc& pd = data_->projectiles[p.desc];
    ++stats_.expired;
    p.alive = 0;
    const Projectile pc = p;  // copy: the detonation may spawn projectiles
    HitEvent he;  // S10-int (R4): lifetime detonation particle
    he.shooter = pc.shooter;
    he.owner = pc.owner;
    he.projectile_desc = pc.desc;
    he.position = pc.pos;
    hit_events_.push_back(he);
    if (pd.blast_area_damage > 0.0f && pd.blast_area_range > 0.0f) area_damage(pc, pc.pos, kNoObject, frame);
    detonation_effects(pc, pc.pos, kNoObject, frame);
}

void CombatSystem::rocket_step(uint32_t i, uint32_t frame, SimRng& rng) {
    // One ROCKET / MPTL_ROCKET flight step (0x4cfc40, S9_DEATH_CLONE_SHIELDS.md §2.4.2) [H unless tagged]. Serial commit
    // phase in projectile serial order, so the plan / re-plan draws come at this projectile's place in the stream, before
    // its hit draws (the original re-plans before the sweep). No reference into rockets_ / projectiles_ is used after a
    // call that can spawn (hit processing, expiry).
    Rocket* rk = find_rocket(projectiles_[i].serial);
    if (!rk) {  // [port] cannot happen (spawn / read keep rockets_ in step with the projectiles)
        projectiles_[i].alive = 0;
        return;
    }
    const ProjectileDesc& pd = data_->projectiles[projectiles_[i].desc];
    RocketParams rp;
    rp.curve_distance = pd.rocket_curve_distance;
    rp.curve_offset = pd.rocket_curve_offset;
    rp.straight_distance = pd.rocket_straight_distance;
    rp.max_flight_distance = pd.max_flight_distance;
    rp.target_on_terrain = pd.target_point_on_terrain;
    rp.explode_at_target = pd.explode_when_reached_target_radius;
    RocketContext ctx;
    ctx.params = &rp;
    ctx.terrain = &rocket_terrain_;
    ctx.rng = &rng;
    ctx.launch = rk->launch;
    const bool land = cfg_.mode == GameMode::Land;  // mode 1 = land builders, mode 2 = space
    const Vec3 P = projectiles_[i].pos;
    // 1. No path yet: the initial plan (first flight step, before the facing turns). [port] FireAtPosition is always set.
    const bool fresh = rk->planned == 0;
    if (fresh) {
        if (land) rocket_plan_land(rk->path, P, rk->fire_at, projectiles_[i].dir, rk->budget, ctx);
        else rocket_plan_space(rk->path, P, rk->fire_at, projectiles_[i].dir, rk->budget, ctx);
        rk->planned = 1;
        rk->s = 0.0f;
    }
    // 2. s += speed, N = sample(s) (phase E sampled the same s + speed for planned rockets). Past the path's end the
    //    rocket expires where it is (forced expiry: detonation at P).
    rk->s = rk->s + projectiles_[i].speed;
    Vec3 N;
    bool ok = false;
    if (fresh) {
        ok = rk->path.sample(rk->s, N);
    } else {
        ok = rocket_next_[i].ok != 0;
        N = rocket_next_[i].n;
    }
    if (!ok) {
        expire_projectile(i, frame);
        return;
    }
    Projectile& p = projectiles_[i];
    // 3. Turn_Toward P -> N at the type turn rate: only the facing turns (the position comes from the spline) [M].
    p.dir = turn_toward(p.dir, N - P, projectile_turn_rate(pd));
    // 4. Every qualifying emitter (§2.3 rules, no jamming skip), in list order, with the same P and N: a crossing (N inside,
    //    P outside; squares summed y, x, z) re-plans. A second crossing in the same step re-plans again from s = 0.
    for (const uint32_t idx : emitters_) {
        const Unit& e = units_[idx];
        if (e.flags & kUnitDead) continue;
        if (!is_enemy(e.owner, p.owner)) continue;
        const float R = e.emitter_radius;
        if (!(R > 0.0f)) continue;
        const Vec3 dn = N - e.pos, dp = P - e.pos;
        const float n2 = (dn.y * dn.y + dn.x * dn.x) + dn.z * dn.z;
        const float p2 = (dp.y * dp.y + dp.x * dp.x) + dp.z * dp.z;
        const float R2 = R * R;
        if (n2 <= R2 && R2 <= p2) {
            if (land) rocket_replan_land(rk->path, P, e.pos, rk->last_vel, rk->s, rk->budget, ctx);
            else rocket_replan_space(rk->path, P, e.pos, rk->last_vel, rk->s, rk->budget, ctx);
        }
    }
    // 5. Sweep P -> N (on a re-plan step still the OLD path's N), hit processing, else move to N.
    const Vec3 seg = N - P;
    SweepHit hit = fresh ? sweep_segment(p, seg, serial_ctx_) : sweep_[i];
    if (hit.id != kNoObject && !live(unit(hit.id))) {  // victim died earlier in this phase
        ++stats_.resweeps;
        hit = sweep_segment(p, seg, serial_ctx_);
    }
    if (hit.id != kNoObject) {
        Unit* v = unit(hit.id);
        p.alive = 0;
        Projectile pc = p;
        process_hit(pc, *v, hit.t, frame, rng, &seg, hit.mesh_hp);
        return;
    }
    // LastKnownVelocity (+0x98) = the move's displacement x the frame-time float 0x140b0a344 [M: 1 / FPS].
    rk->last_vel = seg * (1.0f / static_cast<float>(cfg_.fps));
    p.pos = N;
    p.travelled = p.travelled + p.speed;  // DistanceTravelled += speed
    // 6. End of flight (0x4c7050): Explode_When_Reached_Target_Radius and s >= budget, or the travel / lifetime rule of
    //    every projectile.
    bool expire = pd.explode_when_reached_target_radius && rk->s >= rk->budget;
    if (!expire)
        expire = p.max_travel > 0.0f
                     ? p.travelled >= p.max_travel
                     : static_cast<float>(frame - p.spawn_frame) / static_cast<float>(cfg_.fps) > pd.max_lifetime;
    if (expire) expire_projectile(i, frame);
}

// ---- cu8k t01: the thrown saber (CU8F_T01_T02_T07_RE.md §2.7 item 3) ----

uint32_t CombatSystem::launch_saber(const SaberLaunch& l, uint32_t frame) {
    if (l.projectile >= data_->projectiles.size()) return 0;
    const ProjectileDesc& pd = data_->projectiles[l.projectile];
    Projectile p;
    p.serial = next_serial_++;
    p.desc = l.projectile;
    p.shooter = l.thrower;
    p.owner = l.owner;
    p.target = l.target;
    p.pos = l.from;
    const Vec3 d = l.fire_at - l.from;
    const float dist = dmath::sqrt(dot(d, d));
    p.dir = dist > 0.0f ? d * (1.0f / dist) : Vec3{1.0f, 0.0f, 0.0f};
    p.speed = pd.max_speed;  // 0x370f00(type, 0): Max_Speed, no mode multiplier for category 8
    p.spawn_frame = frame;
    p.damage = pd.damage;
    p.damage_type = pd.damage_type;
    p.kind = pd.damage_kind;
    // Pack +0x6c: the max-travel override (Projectile_Max_Flight_Distance) + the target's radius (0x397780) + |dz|.
    const Unit* t = unit(l.target);
    const float radius = t ? desc_of(*t).radius : 0.0f;
    p.max_travel = pd.max_flight_distance + radius + abs_f(l.fire_at.z - l.from.z);
    p.rocket = 2;
    Rocket rk;
    rk.serial = p.serial;
    rk.fire_at = l.fire_at;
    rk.launch = l.from;
    rk.budget = pd.max_flight_distance;  // pack +0x54 := 250 (read only with Explode_When_Reached_Target_Radius)
    rk.thrower = l.thrower;
    rockets_.push_back(std::move(rk));  // serials only grow: rockets_ stays in serial order
    projectiles_.push_back(p);
    ++stats_.shots;
    return p.serial;
}

bool CombatSystem::saber_position(uint32_t serial, Vec3& out) const {
    const auto it = std::lower_bound(projectiles_.begin(), projectiles_.end(), serial,
                                     [](const Projectile& p, uint32_t v) { return p.serial < v; });
    if (it == projectiles_.end() || it->serial != serial || !it->alive || it->rocket != 2) return false;
    out = it->pos;
    return true;
}

void CombatSystem::saber_step(uint32_t i, uint32_t frame, SimRng& rng) {
    // LIGHT_SABER step 0x4cf460 (§2.7 item 3) [H unless tagged]. Serial commit phase, projectile serial order: the plan's
    // jitter draws come at this projectile's place in the stream on its first step.
    Rocket* rk = find_rocket(projectiles_[i].serial);
    if (!rk) {  // [port] cannot happen (spawn / read keep rockets_ in step with the projectiles)
        projectiles_[i].alive = 0;
        return;
    }
    const ProjectileDesc& pd = data_->projectiles[projectiles_[i].desc];
    RocketParams rp;
    rp.curve_distance = pd.rocket_curve_distance;
    rp.curve_offset = pd.rocket_curve_offset;
    rp.max_flight_distance = pd.max_flight_distance;
    RocketContext ctx;
    ctx.params = &rp;
    ctx.terrain = &rocket_terrain_;
    ctx.rng = &rng;
    ctx.launch = rk->launch;
    const Vec3 P = projectiles_[i].pos;
    if (rk->planned == 0) {
        // Path plan 0x4c8910 (first step): P0 = the spawn position; leg 1 P0 -> T (amp 1.0, length scale 1.1, land mode,
        // no ground end, plain end), leg 2 T -> L (amp 0.3, 1.1, land mode, ground end, plain end) with L = the thrower's
        // position + Ranged_Target_Z_Adjust at this step [port: the spawn point when the thrower is gone]; Build(10).
        const Unit* th = unit(rk->thrower);
        const Vec3 L = live(th) ? lob_target_point(*th) : rk->launch;
        rk->path.clear();
        rk->path.append(P);
        rocket_leg(rk->path, P, rk->fire_at, 1.0f, 1.1f, 1, false, true, ctx);
        rocket_leg(rk->path, rk->fire_at, L, 0.3f, 1.1f, 1, true, true, ctx);
        rk->path.build(10);
        rk->planned = 1;
        rk->s = 0.0f;
    }
    rk->s = rk->s + projectiles_[i].speed;
    Vec3 N;
    if (!rk->path.sample(rk->s, N)) {  // past the path's end: expire where it is
        expire_projectile(i, frame);
        return;
    }
    Projectile& p = projectiles_[i];
    p.dir = turn_toward(p.dir, N - P, projectile_turn_rate(pd));
    rk->last_vel = (N - P) * (1.0f / static_cast<float>(cfg_.fps));
    p.pos = N;
    p.travelled = p.travelled + p.speed;
    const bool detonate = p.max_travel > 0.0f && p.travelled >= p.max_travel;
    // Area damage every step: every live unit of a player hostile to the saber's owner within Projectile_Stun_Radius (3D)
    // takes Projectile_Damage of the saber's damage type, the saber's thrower as the source; no once-per-victim memory.
    // [port] ascending unit id (the original's 0x2a9ff0 query order is not read); [M] also on the detonation step.
    const float R = pd.stun_radius;
    if (R > 0.0f && p.damage > 0.0f) {
        const Projectile pc = p;  // damage may kill: no reference into projectiles_ / units_ is held across it
        const float R2 = R * R;
        std::vector<ObjectId> victims;
        for (const Unit& v : units_) {
            if (!live(&v) || !is_enemy(pc.owner, v.owner)) continue;
            const Vec3 dv = v.pos - pc.pos;
            if (dot(dv, dv) <= R2) victims.push_back(v.id);
        }
        for (const ObjectId vid : victims) {
            if (!live(unit(vid))) continue;
            DamageRequest r;
            r.victim = vid;
            r.amount = pc.damage;
            r.damage_type = pc.damage_type;
            r.kind = pc.kind;
            r.source = pc.shooter;
            r.source_is_projectile = true;
            r.projectile_target = pc.target;  // s78n t12
            r.projectile_desc = static_cast<int32_t>(pc.desc);
            apply_or_queue(r, 0.0f, frame);
        }
    }
    if (detonate) expire_projectile(i, frame);
}

// ---- S8 t04: grenade flight (docs/re/LOB_SOLVER.md §6, ProjectileBehavior category 3 0x4ceea0) ----

Vec3 CombatSystem::lob_next(const Projectile& p, uint32_t frame) const {
    // §6.1: evaluated from scratch each step; t = frames since the creation frame (the first step, the frame after the
    // spawn, has t = 1).
    return lob_position(p.launch, p.lob_dir, p.speed, p.gravity, static_cast<float>(frame - p.spawn_frame));
}

bool CombatSystem::ground_hit(Vec3 a, Vec3 b, GroundHit& out) const {
    // Ground test 0x4d0630 (§6.2 step 3) -> 0x135840 (§6.6, S8 t06): the segment a -> b against the heightmap cells'
    // triangles and the water plane, two-sided, nearest t in (0, 1) (rocket.hpp ground_segment). [port] No grid from
    // the host: a plane at z 0.
    return ground_segment(rocket_terrain_.grid, a, b, out);
}

bool CombatSystem::straight_ground_test(const Projectile& p, Vec3 delta) const {
    // A1j t03 (TACTICAL_COMBAT §3.4 step 1, 0x4d0630) [H]: land mode only. For Projectile_Category DEFAULT (and an unknown
    // category, -1; the port parses those as DEFAULT) the test is skipped while the target point is still ahead: the sign
    // of (target point - position) equals the sign of the step on all three axes (sign(0) = +). Target point = the
    // targeted hardpoint's position (0x396ce0) with a hardpoint index, else position + Ranged_Target_Z_Adjust (0x398440).
    // No target: tested. Every other category (LASER 7 included) is tested on every step.
    if (cfg_.mode != GameMode::Land) return false;
    if (data_->projectiles[p.desc].category != ProjectileCategory::Default) return true;
    const Unit* t = unit(p.target);
    if (!live(t)) return true;  // [port] a dead target stands for the cleared pack reference
    Vec3 tp;
    if (p.target_hp >= 0 && static_cast<uint32_t>(p.target_hp) < t->hp_count)
        tp = hp_world(*t, data_->hardpoints[hps_[t->hp_begin + static_cast<uint32_t>(p.target_hp)].desc]);
    else
        tp = lob_target_point(*t);
    auto sgn = [](float v) { return v >= 0.0f; };
    return !(sgn(tp.x - p.pos.x) == sgn(delta.x) && sgn(tp.y - p.pos.y) == sgn(delta.y) && sgn(tp.z - p.pos.z) == sgn(delta.z));
}

Vec3 CombatSystem::stuck_local(const Unit& v, Vec3 world) const {
    // S8 t06 (§6.4, LOB_SOLVER §8 item 18): the stuck point in the parent's frame (yaw of its facing, as local_to_world),
    // so it turns with the parent. [port] Retail carries it on the hit bone (bone animation and pitch / roll ignored).
    float fx = v.facing.x, fy = v.facing.y;
    const float len2 = fx * fx + fy * fy;
    if (len2 > 0.0f) {
        const float inv = 1.0f / dmath::sqrt(len2);
        fx = fx * inv;
        fy = fy * inv;
    } else {
        fx = 1.0f;
        fy = 0.0f;
    }
    const Vec3 d = world - v.pos;
    return Vec3{d.x * fx + d.y * fy, d.y * fx - d.x * fy, d.z};
}

uint32_t CombatSystem::bomb_frame(float seconds, uint32_t armed_frame) const {
    // BOMB arm 0x58f030 (§6.4): detonation frame = int((float)FPS x seconds + (float)frame + 0.5); it fires on the first
    // service with frame >= it. [port] Negative / huge results clamp to 0 / UINT32_MAX.
    const float f = static_cast<float>(cfg_.fps) * seconds + static_cast<float>(armed_frame) + 0.5f;
    if (!(f > 0.0f)) return 0;
    if (!(f < 4294967040.0f)) return 0xFFFFFFFFu;
    return static_cast<uint32_t>(static_cast<int64_t>(f));
}

void CombatSystem::lob_step(uint32_t i, uint32_t frame, SimRng& rng) {
    // §6.2 in the serial commit phase (projectile serial order). No reference into projectiles_ is used after a call that
    // can spawn (hit processing, detonation).
    Projectile& p = projectiles_[i];
    const ProjectileDesc& pd = data_->projectiles[p.desc];
    const float age = static_cast<float>(frame - p.spawn_frame) / static_cast<float>(cfg_.fps);
    // BOMB timer (Grenade_Explode_Timer, armed at the throw = the spawn frame; S8 t06, §6.4): fires on the first frame >=
    // int(FPS x secs + spawn frame + 0.5), wherever the grenade is (in flight, stuck or resting). Remote bombs carry none.
    const bool fuse_out = p.fuse >= 0.0f && frame >= bomb_frame(p.fuse, p.spawn_frame);
    if (p.stuck) {
        // §6.4: flight stopped (vfunc +0xc0): no lifetime any more, only the BOMB timer or the remote detonation. An
        // attached grenade rides on its object: S8 t06 the BOMB service puts it at the stuck point carried by the
        // parent, here the offset kept in the parent's frame turned with its facing ([port] no bone animation).
        if (p.stuck_to != kNoObject)
            if (const Unit* v = unit(p.stuck_to); live(v)) p.pos = local_to_world(*v, p.stuck_offset);
        if (fuse_out) expire_projectile(i, frame);
        return;
    }
    // 1. N = N(t); 2. facing := the chord P -> N.
    const Vec3 P = p.pos;
    const Vec3 N = lob_next(p, frame);
    const Vec3 seg = N - P;
    const float l2 = dot(seg, seg);
    if (l2 > 0.0f) p.dir = seg * (1.0f / dmath::sqrt(l2));
    // 3. sweep: the object hit of phase E (re-swept when its victim died earlier in this phase) and the land-only ground
    //    test; the object hit wins when its fraction <= the ground fraction.
    SweepHit hit = sweep_[i];
    if (hit.id != kNoObject && !live(unit(hit.id))) {
        ++stats_.resweeps;
        hit = sweep_segment(p, seg, serial_ctx_);
    }
    GroundHit gh;
    const bool ground = cfg_.mode == GameMode::Land && ground_hit(P, N, gh);
    const float tg = gh.t;
    const bool object = hit.id != kNoObject && (!ground || hit.t <= tg);
    if (pd.sticks_on_collision && (object || ground)) {
        // 5. §6.4: no damage; attach to the struck object or rest on the ground, then wait (BOMB timer / remote).
        p.stuck = 1;
        p.speed = 0.0f;
        if (object) {
            p.pos = P + seg * hit.t;
            p.stuck_to = hit.id;
            p.stuck_offset = stuck_local(*unit(hit.id), p.pos);  // S8 t06: in the parent's frame
        } else {
            p.pos = gh.point;
            p.stuck_to = kNoObject;
        }
        if (fuse_out) expire_projectile(i, frame);
        return;
    }
    if (object) {  // 4. Process_Hit (direct + area damage, effects)
        Unit* v = unit(hit.id);
        p.alive = 0;
        Projectile pc = p;
        process_hit(pc, *v, hit.t, frame, rng, &seg, hit.mesh_hp);
        return;
    }
    if (ground) {  // 4. On_Detonate 0x4cb070 at the ground hit point: no direct damage, area damage + effects there
        p.pos = gh.point;
        expire_projectile(i, frame);
        return;
    }
    // Move_And_Expire 0x4c7050 (§6.5): DistanceTravelled += s; the lifetime rule replaces the travel budget when the
    // budget is 0 or the type has a Max_Lifetime > 0 (every grenade is category 3).
    p.pos = N;
    p.travelled = p.travelled + p.speed;
    const bool lifetime_rule = !(p.max_travel > 0.0f) || pd.max_lifetime > 0.0f;
    const bool expire = fuse_out || (lifetime_rule ? age > pd.max_lifetime : p.travelled >= p.max_travel);
    if (expire) expire_projectile(i, frame);
}

// ================================================================================================
// tick
// ================================================================================================

QueryContext* CombatSystem::acquire_ctx() {
    std::lock_guard<std::mutex> lock(ctx_mutex_);
    if (ctx_free_.empty()) {
        ctx_all_.push_back(std::make_unique<QueryContext>());
        ctx_all_.back()->reserve(std::max<size_t>(cand_reserve_, 64));
        ctx_free_.reserve(ctx_all_.size());
        return ctx_all_.back().get();
    }
    QueryContext* c = ctx_free_.back();
    ctx_free_.pop_back();
    return c;
}

void CombatSystem::release_ctx(QueryContext* c) {
    std::lock_guard<std::mutex> lock(ctx_mutex_);
    ctx_free_.push_back(c);
}

void CombatSystem::tick(uint32_t frame, const TreeSet& trees, SimRng& rng, const ParallelFor& pf) {
    frame_ = frame;
    trees_ = &trees;
    last_trees_ = &trees;
    tick_rng_ = &rng;
    deaths_.clear();
    hp_events_.clear();
    smoke_events_.clear();
    damage_events_.clear();
    hit_events_.clear();  // S10-int
    shot_events_.clear();  // OI fog
    lob_events_.clear();   // S8 t30
    attack_events_.clear();  // s78f t08 (signal 0x1E)
    survivor_events_.clear();  // OI abilities
    const auto n = static_cast<uint32_t>(units_.size());
    profile::Steps steps;  // SP5: the phases' times (cmb.*), timing only

    // A. modifiers
    for (ModifierSet& ps : player_mods_) ps.purge_expired(frame);  // player-level purge [M: cadence]
    {
        const RangeFn fn = [this](uint32_t b, uint32_t e) {
            for (uint32_t i = b; i < e; ++i) {
                Unit& u = units_[i];
                if (u.flags & kUnitDead) continue;
                u.mods.purge_expired(frame_);  // once per object update
                ensure_cache(u);
                const UnitAbilityDesc* ab[2] = {ability(u, 0), ability(u, 1)};
                const bool act[2] = {u.ability_active[0] != 0, u.ability_active[1] != 0};
                for (uint32_t k = 0; k < kModKindCount; ++k)
                    u.own_mult[k] = ability_product(ab, act, static_cast<ModKind>(k + 1));
                u.mult = u.own_mult;  // s78h t03: the team_leader too (the squad value is team_mod_multiplier)
            }
        };
        pf(n, cfg_.grain, fn);
    }
    ALAMO_PROFILE_STEP(steps, "cmb.a_mods");
    // Team heads (serial). s78h t03: no squad rule here any more; a member (the team_leader stand-in included) keeps its own
    // Mod_Multiplier, and the container's squad value is team_mod_multiplier (TACTICAL_COMBAT §4.4).
    leaders_.clear();
    for (uint32_t i = 0; i < n; ++i)
        if (units_[i].team != 0 && units_[i].team_leader && !(units_[i].flags & kUnitDead)) leaders_.emplace_back(units_[i].team, i);
    // OI tactical team targeting: (team, team_leader id) for team_head() (read-only in the parallel gather phase).
    team_heads_.clear();
    for (const auto& [team, index] : leaders_) team_heads_.emplace_back(team, units_[index].id);
    std::sort(team_heads_.begin(), team_heads_.end());
    rebuild_team_members();  // cu8f t04a: team owners / container targets of the stickiness test
    // s78f t03: the heads of ground teams (TeamTargeting services in phase D), ascending id. A member keeps no target of
    // its own (TD +8 null, TACTICAL_COMBAT §3.1.1 "Team targeting copies"): one it picked before it became a member (its
    // head appeared later) is dropped here, since its scan no longer runs to replace it (s78f review).
    land_heads_.clear();
    if (cfg_.mode == GameMode::Land)
        for (uint32_t i = 0; i < n; ++i)
            if (!(units_[i].flags & kUnitDead))
                if (const Unit* h = land_team_lead(units_[i])) {  // s78i t03: a head with no member left too
                    land_heads_.push_back(h->id);
                    Unit& m = units_[i];
                    if (h != &m && m.auto_target != kNoObject) {
                        m.auto_target = kNoObject;
                        m.auto_hp = -1;
                        m.target_score = 0.0f;
                    }
                }
    std::sort(land_heads_.begin(), land_heads_.end());
    land_heads_.erase(std::unique(land_heads_.begin(), land_heads_.end()), land_heads_.end());
    // s78h t06 (TACTICAL_COMBAT §3.1.1 "When the container is serviced"): each land team's container slot = its highest
    // live unit index (members and head) in phase A; phase D's descending walk reaches it before any of the team's units.
    land_teams_.clear();
    if (cfg_.mode == GameMode::Land) {
        // SP5: (team, top index) per live unit, then the highest per team (was a linear search of land_teams_ per unit);
        // the same slots, ordered below.
        std::vector<std::pair<uint32_t, uint32_t>> tops;
        for (uint32_t i = 0; i < n; ++i) {
            const Unit& m = units_[i];
            if (m.flags & kUnitDead) continue;
            const Unit* h = land_team_lead(m);  // s78i t03
            if (!h) continue;
            const auto hi = static_cast<uint32_t>(h - units_.data());
            tops.emplace_back(m.team, hi > i ? hi : i);
        }
        std::sort(tops.begin(), tops.end());
        for (size_t i = 0; i < tops.size(); ++i)
            if (i + 1 == tops.size() || tops[i + 1].first != tops[i].first)  // the team's last = highest top
                land_teams_.push_back(LandTeamSlot{tops[i].second, tops[i].first, 0});
    }
    std::sort(land_teams_.begin(), land_teams_.end(), [](const LandTeamSlot& a, const LandTeamSlot& b) {
        return a.slot != b.slot ? a.slot > b.slot : a.team < b.team;
    });
    if (hooks_) hooks_->begin_tick(frame);
    ALAMO_PROFILE_STEP(steps, "cmb.a_teams");

    // B1. delayed damage (queue order)
    if (!delayed_.empty()) {
        size_t w = 0;
        const size_t count = delayed_.size();
        for (size_t i = 0; i < count; ++i) {
            Delayed d = delayed_[i];
            d.frames -= 1;
            if (d.frames > 0) {
                delayed_[w++] = d;
                continue;
            }
            DamageRequest r;
            r.victim = d.victim;
            r.amount = d.amount;
            r.damage_type = d.damage_type;
            r.kind = d.kind;
            r.source = d.source;
            r.source_is_projectile = d.source_is_projectile != 0;
            r.projectile_target = d.projectile_target;  // s78n t12
            r.projectile_desc = d.projectile_desc;
            r.hardpoint = d.hardpoint;
            take_damage(r, frame);
        }
        delayed_.resize(w);
    }
    ALAMO_PROFILE_STEP(steps, "cmb.b1_delayed");
    // B2. shields and hull/hardpoint coupling (per unit)
    {
        const RangeFn fn = [this](uint32_t b, uint32_t e) {
            for (uint32_t i = b; i < e; ++i) upkeep_unit(units_[i], frame_);
        };
        pf(n, cfg_.grain, fn);
    }
    ALAMO_PROFILE_STEP(steps, "cmb.b2_upkeep");
    // C. target candidates (parallel, no draws)
    {
        const RangeFn fn = [this](uint32_t b, uint32_t e) {
            QueryContext* c = acquire_ctx();
            for (uint32_t i = b; i < e; ++i) gather(i, *c);
            release_ctx(c);
        };
        pf(n, cfg_.grain, fn);
    }
    ALAMO_PROFILE_STEP(steps, "cmb.c_gather");
    // D. decide + fire, newest unit first (serial: every synced draw of the tick happens here or in F).
    //    S9 per-unit order (OBJECT_RUNTIME 4.4 behaviours, ABILITIES_HARDPOINTS.md 6.2): ability
    //    countdown (hook) -> burning -> targeting -> pending ability + special abilities (hook) ->
    //    hardpoints / weapons -> converted-unit decay.
    if (hooks_) hooks_->before_units(frame, rng);
    drop_clash_targets();  // s78n t06
    size_t next_team = 0;  // s78h t06: land_teams_ cursor (slot descending)
    for (uint32_t k = n; k-- > 0;) {
        // s78h t06: a land team's TeamTargeting runs once, at its container slot just before its highest-index unit, even
        // when that unit died earlier in the walk, while the team has a live head (after an in-tick hand-over: the heir).
        for (; next_team < land_teams_.size() && land_teams_[next_team].slot >= k; ++next_team) {
            LandTeamSlot& ts = land_teams_[next_team];
            if (ts.serviced) continue;
            ts.serviced = 1;
            Unit* h = team_head_of(ts.team);
            if (!h) continue;
            const auto hi = static_cast<uint32_t>(h - units_.data());
            if (hi >= n) continue;  // spawned this tick: phase C has no candidates for it [port]
            h->acquired = 0;
            if (!targeting_idle(*h, frame)) {
                decide(*h, cands_[hi], frame, rng);
                team_targeting_service(*h, frame, rng);
            }
        }
        Unit& u = units_[k];
        if (u.flags & kUnitDead) continue;
        if (hooks_) {
            hooks_->unit_begin(u.id, frame, rng);
            if (u.flags & kUnitDead) continue;
        }
        burning_service(u, frame);
        if (u.flags & kUnitDead) continue;
        infection_service(u, frame, rng);  // refix t02 [M: slot next to the other damage-over-time behaviour]
        if (u.flags & kUnitDead) continue;
        // OI abilities: a stunned unit's targeting / turret behaviours are suspended (0x647290); its STUNNABLE service
        // at the end frame releases it [inferred]. Requires_Base_Power units do not act without base power (0x632270).
        if (u.stun_end != 0 && frame >= u.stun_end) {
            u.stun_end = 0;
            if (hooks_) hooks_->unit_effect(u.id, UnitEffect::StunEnded, u.owner, u.pos, frame);
        }
        const bool idle = u.stun_end > frame || (desc_of(u).requires_base_power && !has_base_power(u.owner));
        // S7 t14: on the jetpack (MOVEMENT §3.1.1) TARGETING and WEAPON are suspended; S8 t12 (MOVEMENT §3.2): the
        // hardpoints are not (update_weapons' `suspended`).
        const bool suspended = (u.flags & kUnitWeaponsSuspended) != 0;
        // S7 t15 A2: an idle unit's weapons are not serviced; the WeaponBehavior recharge still elapses (fire_unit_weapon
        // subtracts the frames since its last service when it runs again).
        // s78h t06: a land team head's TARGETING (TeamTargeting) ran at its team's container slot above (its acquired flag
        // was reset there); a member has none of its own (its container's service writes its state).
        const bool land_head = std::binary_search(land_heads_.begin(), land_heads_.end(), u.id);
        if (!land_head) u.acquired = 0;
        if (!idle && !suspended && !(u.flags & (kUnitTargetingSuspended | kUnitHeroClash))) {  // S8 t25; s78n t06
            // s78f t03: TARGETING = the scan, then the state service (TACTICAL_COMBAT §3.1.1 "Targeting states").
            if (!land_head && !land_team_head(u)) {
                decide(u, cands_[k], frame, rng);
                targeting_service(u, frame, rng);
            }
        }
        if (hooks_) {
            hooks_->unit_after_targeting(u.id, frame, rng);
            if (u.flags & kUnitDead) continue;
        }
        service_repairs(u, frame);  // OI abilities: hardpoint update 0x387010 -> repair 0x387170, before the weapons
        if (!idle) update_weapons(u, frame, rng, suspended);
        converted_decay(u, frame, rng);
    }
    ALAMO_PROFILE_STEP(steps, "cmb.d_units");
    // E. projectile sweeps (parallel)
    const auto np = static_cast<uint32_t>(projectiles_.size());
    sweep_.resize(np);
    rocket_next_.resize(np);  // refixf t01
    {
        const RangeFn fn = [this](uint32_t b, uint32_t e) {
            QueryContext* c = acquire_ctx();
            for (uint32_t i = b; i < e; ++i) sweep(i, *c);
            release_ctx(c);
        };
        pf(np, cfg_.grain, fn);
    }
    ALAMO_PROFILE_STEP(steps, "cmb.e_sweep");
    // F. commit in serial order. Hit processing can spawn projectiles (death projectiles), so no
    //    reference into projectiles_ is held across it: work on a copy, write back by index.
    emitters_.clear();  // missile-shield emitters, ascending id (S9 port choice, 6.3)
    for (uint32_t i = 0; i < n; ++i)  // OI abilities: registered (Add_Emitter) and qualifying (radius) emitters
        if (!(units_[i].flags & kUnitDead) && units_[i].emitter_radius > 0.0f && units_[i].emitter_entries > 0) emitters_.push_back(i);
    for (uint32_t i = 0; i < np; ++i) {
        if (!projectiles_[i].alive || projectiles_[i].spawn_frame >= frame) continue;
        if (projectiles_[i].rocket == 2) {  // cu8k t01: thrown saber (category 8 step 0x4cf460)
            saber_step(i, frame, rng);
            continue;
        }
        if (projectiles_[i].rocket) {  // refixf t01: spline rocket (0x4cfc40)
            rocket_step(i, frame, rng);
            continue;
        }
        if (projectiles_[i].lob) {  // S8 t04: grenade (0x4ceea0)
            lob_step(i, frame, rng);
            continue;
        }
        SweepHit hit = sweep_[i];
        if (hit.id != kNoObject && !live(unit(hit.id))) {  // victim died earlier in this phase
            ++stats_.resweeps;
            sweep(i, serial_ctx_);
            hit = sweep_[i];
        }
        // A1j t03: the object hit wins when its segment fraction <= the ground fraction; a ground hit detonates there
        // (On_Detonate 0x4cb070: no direct damage, area damage and effects at the ground point), as lob_step.
        if (hit.id != kNoObject && (!hit.ground || hit.t <= hit.gh.t)) {
            Unit* v = unit(hit.id);
            projectiles_[i].alive = 0;
            Projectile pc = projectiles_[i];
            process_hit(pc, *v, hit.t, frame, rng, nullptr, hit.mesh_hp);
            continue;
        }
        if (hit.ground) {
            projectiles_[i].pos = hit.gh.point;
            expire_projectile(i, frame);
            continue;
        }
        Projectile& p = projectiles_[i];
        const ProjectileDesc& pd = data_->projectiles[p.desc];
        // OI abilities (TACTICAL_GAPS §1.5 [H], disassembly 0x1404ce768): the MISSILE branch tests Is_Jammed on the
        // projectile's TARGET - no target or a jammed target -> straight flight.
        // S9 t05 (S9_DEATH_CLONE_SHIELDS.md §2.2 / §2.3): the flight modes steer by Turn_Toward (0x3abe40) limited to
        // Max_Rate_Of_Turn x the mode multiplier per step - homing toward the target, and in the straight / homing modes
        // away from a missile-shield emitter holding the projectile (that goal replaces the target for the step).
        // Rockets fly rocket_step (spline, re-plan on crossing); bombs, grenades and sabers are never shield-tested.
        using PC = ProjectileCategory;
        const PC cat = pd.category;
        const bool jammed = is_jammed(p.target, frame);
        const Unit* homing_target = nullptr;
        if ((pd.homing || cat == PC::Missile) && !jammed) {
            const Unit* t = unit(p.target);
            if (live(t)) homing_target = t;
        }
        bool steer = false;
        Vec3 goal;
        // ROCKET / MPTL_ROCKET never get here (rocket_step above, refixf t01).
        if (cat != PC::Bomb && cat != PC::Grenade && cat != PC::StaticBomb && cat != PC::LightSaber &&
                   cat != PC::ClusterBomb && !emitters_.empty()) {
            steer = shield_away(p, goal);
        }
        if (!steer && homing_target) {
            goal = homing_target->pos - p.pos;
            steer = true;
        }
        if (steer) p.dir = turn_toward(p.dir, goal, projectile_turn_rate(pd));
        p.pos = p.pos + p.dir * p.speed;
        p.travelled = p.travelled + p.speed;
        bool expire;
        if (p.fuse >= 0.0f) {  // OI abilities: armed bomb timer (S8 t06: the BOMB arm's frame rounding, §6.4)
            expire = frame >= bomb_frame(p.fuse, p.spawn_frame);
        } else {
            expire = p.max_travel > 0.0f
                         ? p.travelled >= p.max_travel
                         : static_cast<float>(frame - p.spawn_frame) / static_cast<float>(cfg_.fps) > pd.max_lifetime;
        }
        if (expire) expire_projectile(i, frame);
    }
    ALAMO_PROFILE_STEP(steps, "cmb.f_projectiles");
    // G. velocity history (OI tactical: two steps for the circular-lead fit), flush
    for (Unit& u : units_) {
        u.prev_pos = u.last_pos;
        u.last_pos = u.pos;
    }
    flush();
    // s78n review: the team member lists as a load rebuilds them (combat_save), so readers between ticks (the movement
    // tick's fighter-formation in_range hook -> inside_min_range -> nearest_team_member) agree live and after a load.
    rebuild_team_members();
    ALAMO_PROFILE_STEP(steps, "cmb.g_flush");
    trees_ = nullptr;
    tick_rng_ = nullptr;
}
}  // namespace alamo::tactical::combat
