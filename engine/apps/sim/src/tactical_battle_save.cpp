// N1 stage 2 save / load of a running battle (owner: n2-host): tactical_battle.hpp "N1 stage 2 save / load".
#include <utility>

#include "alamo/sim/save_session.hpp"
#include "alamo/sim/tactical_battle.hpp"
#include "save_io.hpp"
#ifdef ALAMO_SIM_HAVE_GARRISON
#include "alamo/tactical/garrison/capture.hpp"
#include "alamo/tactical/garrison/garrison.hpp"
#endif

namespace alamo::sim {

namespace {

constexpr uint32_t kBattleStateVersion = 2;  // 2: a1i t04 landing objects
constexpr uint32_t kBattlePartVersion = 1;

// One module system in a part: presence flag + a section with its own write (nothing when it has no write / read).
template <class T>
void put_system(ByteWriter& w, const T* s) {
    w.boolean(s != nullptr);
    if (s) write_section(w, [s](ByteWriter& sub) { write_if_saveable(*s, sub); });
}

template <class T>
bool get_system(ByteReader& r, T* s) {
    const bool has = r.boolean();
    if (!r.ok() || has != (s != nullptr)) return false;
    return !s || read_section(r, [s](ByteReader& sub) { return read_if_saveable(*s, sub); });
}

// A part loads only when every system in it has write / read (a pending one fails the part before anything is read).
template <class... T>
constexpr bool all_saveable(const T*...) {
    return (SaveableSystem<T> && ...);
}

}  // namespace

const char* TacticalBattle::save_part_name(SavePart p) {
    switch (p) {
        case SavePart::State: return "battle.state";
        case SavePart::Movement: return "battle.movement";
        case SavePart::Combat: return "battle.combat";
        case SavePart::Abilities: return "battle.abilities";
        case SavePart::Garrison: return "battle.garrison";
        case SavePart::Hangar: return "battle.hangar";
        case SavePart::Fog: return "battle.fog";
        case SavePart::Damage: return "battle.damage";
        case SavePart::Count: break;
    }
    return "battle.?";
}

// The host state of the battle, one field list for both directions (save_io.hpp). Self = TacticalBattle or const.
template <class Ar, class Self>
void TacticalBattle::state_io(Ar& a, Self& b) {
    a(b.types_used_, b.descs_);  // checked against the rebuilt battle by read_part
    a.seq(b.parts_, [](auto& x, auto& p) {
        x(p.galactic, p.galactic_id, p.owner, p.type, p.side, p.reserve, p.starbase, p.structure, p.deployed, p.built);
        x(p.pop, p.power, p.spawned, p.alive, p.map, p.third, p.unplaced, p.other_type, p.spots);
    });
    a.seq(b.units_, [](auto& x, auto& u) {
        x(u.id, u.h, u.participant, u.type, u.side, u.movable, u.alive, u.retreated, u.tracked, u.engines_offline);
        x(u.dt_next, u.container, u.power, u.range, u.attached_to, u.attach_bone, u.flags_3a2, u.fighter_seen);
    });
    a.seq(b.containers_, [](auto& x, auto& c) { x(c.id, c.h, c.alive, c.removed, c.team, c.type, c.owner, c.squad_type); });
    auto timed = [](auto& x, auto& t) { x(t.id, t.h, t.frame); };
    a.seq(b.particles_, timed);
    a.seq(b.dummies_, timed);
    a.seq(b.death_clones_, timed);  // refixf t04
    a.seq(b.effects_, [](auto& x, auto& e) { x(e.id, e.asset, e.percent, e.frame); });
    a(b.anchor_[0], b.anchor_[1], b.facing_[0], b.facing_[1], b.placed_[0], b.placed_[1]);
    a(b.drop_facing_[0], b.drop_facing_[1]);  // a1h t06
    a.seq(b.landings_, [](auto& x, auto& l) {  // a1i t04: landing objects (state version 2)
        x(l.id, l.h, l.participant, l.state, l.timed, l.timer_start, l.timer_len, l.unloaded, l.next_offset, l.remove_frame);
        x(l.targets, l.units);
    });
    a(b.frame_, b.retreat_allowed_frame_, b.retreat_side_, b.retreat_done_frame_, b.retreat_complete_, b.finished_);
    a(b.end_hold_until_, b.end_winner_, b.end_victory_, b.end_retreated_);
    a(b.result_.winner, b.result_.victory, b.result_.retreated, b.result_.destroyed, b.result_.starbase_destroyed);
    {
        auto& s = b.stats_;
        a(s.frames, s.spawned, s.reinforcements, s.deaths, s.orders, s.shots, s.hits, s.containers, s.ability_events);
        a(s.activations, s.ability_commands, s.hardpoints_destroyed, s.particles, s.props, s.death_clones, s.smoke, s.dummies,
          s.anim_plays, s.death_clones_removed);
    }
    // [port: Gabriel] order queues
    a.seq(b.queues_, [](auto& x, auto& q) {
        x(q.id);
        x.seq(q.orders, [](auto& y, auto& o) {
            y(o.kind, o.player, o.group, o.target, o.hardpoint, o.x, o.y, o.facing, o.attack_move, o.flags, o.ability);
        });
        x(q.has_current, q.current);
    });
    a(b.queue_group_, b.queue_used_);
#ifdef ALAMO_SIM_HAVE_GARRISON
    a(b.s4_used_);
#endif
#ifdef ALAMO_SIM_HAVE_FOG
    a(b.fog_pings_, b.fog_on_, b.fog_global_active_, b.fog_global_render_);
#endif
    // L1 systems
    a(b.l1_desc_cache_, b.l1_in_asteroid_, b.l1_weather_started_, b.fog_weather_dirty_);
    a.seq(b.l1_trackers_, [](auto& x, auto& t) { x(t.id, t.acc, t.rate, t.next); });
    a(b.l1_final_blow_);
    a.seq(b.l1_bombard_, [](auto& x, auto& s) {
        x(s.player, s.faction, s.enabled, s.available, s.in_progress, s.timer_only, s.disabled);
        x(s.ready, s.interval, s.start, s.next_shot, s.cursor, s.deferred, s.shot, s.salvo, s.target);
    });
    a.seq(b.l1_runs_, [](auto& x, auto& s) {
        x(s.player, s.enabled, s.available, s.in_progress, s.disabled, s.ready, s.interval, s.last_start, s.bomber_type);
        x.seq(s.bombers, [](auto& y, auto& m) {
            y(m.id, m.marker, m.pos, m.dir, m.exit, m.speed, m.next_fire, m.projectile, m.range, m.recharge);
        });
        x(s.total);
    });
    a(b.l1_used_, b.l1_planet_destroyed_, b.l1_tsw_story_winner_);
    a.seq(b.l1_tsw_debris_, [](auto& x, auto& d) { x(d.tsw, d.type, d.pos, d.facing); });
    // Int-final: build pads
    a.seq(b.pads_, [](auto& x, auto& p) {
        x(p.pad, p.uc, p.built, p.builder, p.uc_end, p.cooldown_start, p.cooldown_end, p.locked, p.built_type);
    });
    a(b.pads_used_);
    // battle host (BT pass)
    a.seq(b.bt_markers_, [](auto& x, auto& m) { x(m.id, m.type, m.kind, m.owner, m.pos, m.facing, m.specific, m.used); });
    a.seq(b.bt_removals_, [](auto& x, auto& r) { x(r.owner, r.object_type, r.original_type, r.survived); });
    a(b.bt_landing_, b.bt_shields_, b.bt_attrition_ids_, b.bt_attrition_kills_, b.bt_lead_, b.bt_lead_participant_);
    a(b.bt_raid_, b.bt_sw_killed_, b.bt_used_, b.bt_have_ds_, b.bt_ds_pos_);
    a.seq(b.bt_ramps_, [](auto& x, auto& r) { x(r.id, r.rate); });
    // OI abilities
    a.seq(b.oi_respawns_, [](auto& x, auto& r) { x(r.due, r.type, r.owner, r.pos, r.facing); });
    a(b.oi_global_frame_);
    // hangars / station upgrade / production
    a.seq(b.hg_order_cost_, [](auto& x, auto& c) { x(c.order, c.player, c.cost); });
    a.seq(b.hg_upgrade_objs_, [](auto& x, auto& u) { x(u.id, u.h); });
    a(b.hg_orphans_, b.prod_used_, b.hg_tech_bonus_);
    a.seq(b.hg_upgrades_, [](auto& x, auto& u) { x(u.old_station, u.new_station, u.owner); });
    a(b.hg_launches_, b.hg_used_);
}

void TacticalBattle::write_part(SavePart p, ByteWriter& w) const {
    w.u32(p == SavePart::State ? kBattleStateVersion : kBattlePartVersion);
    switch (p) {
        case SavePart::State: {
            save_io::Out o(w);
            state_io(o, *this);
            break;
        }
        case SavePart::Movement:
            put_system(w, movement_.get());
            put_system(w, &l1_jumps_);
            put_system(w, &l1_engines_);
            break;
        case SavePart::Combat:
            put_system(w, combat_.get());
            put_system(w, &l1_weather_);
            break;
        case SavePart::Abilities:
            put_system(w, abilities_.get());
            put_system(w, &l1_sw_);
            put_system(w, &l1_fields_);
            break;
        case SavePart::Garrison:
#ifdef ALAMO_SIM_HAVE_GARRISON
            w.boolean(true);
            put_system(w, &garrison_);
            put_system(w, captures_.get());
#else
            w.boolean(false);
#endif
            break;
        case SavePart::Hangar:
            put_system(w, hangars_.get());
            put_system(w, contents_.get());
            put_system(w, production_.get());
            put_system(w, &engine_loops_);
            break;
        case SavePart::Fog:
#ifdef ALAMO_SIM_HAVE_FOG
            w.boolean(true);
            put_system(w, &fog_);
#else
            w.boolean(false);
#endif
            break;
        case SavePart::Damage:
#ifdef ALAMO_SIM_HAVE_AI_TACTICAL
            w.boolean(true);
            put_system(w, &damage_tracking_);
#else
            w.boolean(false);
#endif
            break;
        case SavePart::Count: break;
    }
}

bool TacticalBattle::read_part(SavePart p, ByteReader& r) {
    if (r.u32() != (p == SavePart::State ? kBattleStateVersion : kBattlePartVersion) || !r.ok()) return false;
    switch (p) {
        case SavePart::State: {
            // Parsed into a scratch copy of the record lists via a backup: the current state is written first and read
            // back when the saved bytes do not fit (the read is all-or-nothing).
            ByteWriter backup;
            {
                save_io::Out o(backup);
                state_io(o, *this);
            }
            const std::vector<uint32_t> types_used = types_used_, descs = descs_;
            save_io::In in(r);
            state_io(in, *this);
            bool ok = r.ok() && r.done() && types_used_ == types_used && descs_ == descs;
            for (const Unit& u : units_)
                if (u.participant >= parts_.size() || (u.container != 0xFFFFFFFFu && u.container >= containers_.size())) ok = false;
            if (!ok) {
                ByteReader br(backup.bytes());
                save_io::In restore(br);
                state_io(restore, *this);
                return false;
            }
            // The combat hooks' tick context (the synced RNG + the global frame of the last battle tick) is used by
            // command-phase hooks before the next battle tick: as in the saving run.
            resume_tick_context();
            return true;
        }
        case SavePart::Movement:
            if (!all_saveable(movement_.get(), &l1_jumps_, &l1_engines_)) return false;
            return get_system(r, movement_.get()) && get_system(r, &l1_jumps_) && get_system(r, &l1_engines_) && r.done();
        case SavePart::Combat:
            if (!all_saveable(combat_.get(), &l1_weather_)) return false;
            if (!(get_system(r, combat_.get()) && get_system(r, &l1_weather_) && r.done())) return false;
            attach_rocket_terrain();  // refixf t01: the host terrain is not part of the save
            return true;
        case SavePart::Abilities:
            if (!all_saveable(abilities_.get(), &l1_sw_, &l1_fields_)) return false;
            return get_system(r, abilities_.get()) && get_system(r, &l1_sw_) && get_system(r, &l1_fields_) && r.done();
        case SavePart::Garrison: {
            const bool has = r.boolean();
#ifdef ALAMO_SIM_HAVE_GARRISON
            if (!has || !all_saveable(&garrison_, captures_.get())) return false;
            return get_system(r, &garrison_) && get_system(r, captures_.get()) && r.done();
#else
            return r.ok() && !has && r.done();
#endif
        }
        case SavePart::Hangar:
            if (!all_saveable(hangars_.get(), contents_.get(), production_.get(), &engine_loops_)) return false;
            return get_system(r, hangars_.get()) && get_system(r, contents_.get()) && get_system(r, production_.get()) &&
                   get_system(r, &engine_loops_) && r.done();
        case SavePart::Fog: {
            const bool has = r.boolean();
#ifdef ALAMO_SIM_HAVE_FOG
            if (!has || !all_saveable(&fog_)) return false;
            return get_system(r, &fog_) && r.done();
#else
            return r.ok() && !has && r.done();
#endif
        }
        case SavePart::Damage: {
            const bool has = r.boolean();
#ifdef ALAMO_SIM_HAVE_AI_TACTICAL
            if (!has || !all_saveable(&damage_tracking_)) return false;
            return get_system(r, &damage_tracking_) && r.done();
#else
            return r.ok() && !has && r.done();
#endif
        }
        case SavePart::Count: break;
    }
    return false;
}

}  // namespace alamo::sim
