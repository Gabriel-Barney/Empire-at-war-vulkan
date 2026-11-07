// N1 stage 2 - save / load during a tactical battle (contract: alamo/core/save_file.hpp) for AbilitySystem,
// SpecialWeaponSystem and SpaceFieldIndex. Little-endian, field by field, counts before variable-length data, containers
// in their own deterministic order (objects / packs ascending id, lists in insertion order). Floats as bit patterns.
// Cross-references are object ids, player indices and descriptor indices, never pointers.
//
// AbilitySystem layout (version kAbilitySaveVersion):
//   u32 version; definition counts (checked): AbilityData objects, specials, players
//   u32 spawn serial, u8 direct-order context
//   u32 + objects (ascending id): ids / desc / owner / container / flags, members, the 77 slot records (i16) and
//         active / autofire / behaviour bytes, the countdown (77 x counter, maximum, phase + last update), pending
//         ability, host-fed stamps, state bytes, the ion / lucky shot slots, special-ability states (with their victim
//         lists), OI behaviour state, the autofire map and the trooper list;
//         then the animation record (i32 type, u32 start, f32 rate, f32 end, u8 loop / then) and the u8 lock pairs
//   (only kAbilitySaveVersion reads; the v1 layout without that record is no longer accepted)
//   u32 + spawn requests, mines, Battlefield_Modifier player entries, interdict / retreat / weatherproof entries,
//   remote bombs, events, player overrides, type overrides; u32 + per-player black-market lists (not hashed, but read
//   by the gates)
// Rebuilt / not saved: the query scratch lists, the host query table (AbilityHost), the bound RNG, the fog callback.
//
// SpecialWeaponSystem layout (version kSpecialWeaponSaveVersion): u32 + descriptors (every field), u32 + packs, u32 +
// shots, u32 + events. SpaceFieldIndex (version kSpaceFieldSaveVersion): u32 + fields.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "alamo/core/bytes.hpp"
#include "alamo/tactical/abilities/ability_system.hpp"
#include "alamo/tactical/abilities/space_fields.hpp"
#include "alamo/tactical/abilities/special_weapons.hpp"
#include "alamo/tactical/save_io.hpp"

namespace alamo::tactical::abilities {

using save_io::get_vec3;
using save_io::put_vec3;

namespace {

constexpr uint32_t kAbilitySaveVersion = 3;  // 2: OI follow-up animation record + lock pairs; 3: cu8 t02 stealth bonus gone
constexpr uint32_t kSpecialWeaponSaveVersion = 1;
constexpr uint32_t kSpaceFieldSaveVersion = 1;

void put_ids(ByteWriter& w, const std::vector<ObjectId>& v) {
    w.u32(static_cast<uint32_t>(v.size()));
    for (const ObjectId id : v) w.i32(id);
}

bool get_ids(ByteReader& r, std::vector<ObjectId>& v) {
    uint32_t n = 0;
    if (!r.count(n, 4)) return false;
    v.assign(n, kNoObject);
    for (ObjectId& id : v) id = r.i32();
    return r.ok();
}

}  // namespace

// ---- AbilitySystem --------------------------------------------------------------------------------------------------

void AbilitySystem::write(ByteWriter& w) const {
    w.u32(kAbilitySaveVersion);
    w.u32(static_cast<uint32_t>(data_->objects.size()));
    w.u32(static_cast<uint32_t>(data_->specials.size()));
    w.u32(combat_->player_count());
    w.u32(spawn_serial_).u8(direct_ctx_);

    w.u32(static_cast<uint32_t>(objs_.size()));
    for (const Obj& o : objs_) {
        w.i32(o.id).u32(o.desc).i32(o.owner).i32(o.container).u8(o.is_container).u8(o.combat_unit).u8(o.dead);
        put_ids(w, o.members);
        for (const int16_t rec : o.record) w.u16(static_cast<uint16_t>(rec));
        for (const uint8_t v : o.active) w.u8(v);
        for (const uint8_t v : o.autofire) w.u8(v);
        for (const uint8_t v : o.behavior) w.u8(v);
        for (const int32_t v : o.cd.counter) w.i32(v);
        for (const int32_t v : o.cd.maximum) w.i32(v);
        for (const uint8_t v : o.cd.phase) w.u8(v);
        w.u32(o.cd.last_update);
        w.i32(o.pending_target).i32(o.pending_hp).u32(o.pending_crc).i32(o.pending_type);
        w.u32(o.nebula_frame).u32(o.force_frame).u32(o.ion_storm_frame);
        w.u8(o.locomotor_busy).u8(o.sap_disabled).u8(o.corrupted).u8(o.weapons_disabled).i32(o.garrison_count);
        w.u8(o.gravity_well_destroyed).u8(o.tractored).u8(o.interdicting).i32(o.immobilize);
        w.u8(o.stealthed).i32(o.stealth_frames).u32(o.last_detected);
        for (size_t k = 0; k < 2; ++k) w.i32(o.shot_target[k]).i32(o.shot_hp[k]).i32(o.shot_proj[k]);
        w.u8(o.lucky_fired);
        w.u32(static_cast<uint32_t>(o.specials.size()));
        for (const SpecialState& sp : o.specials) {
            w.u32(sp.desc).u32(sp.due).u8(sp.applied).u8(sp.fired).u8(sp.trigger).i32(sp.target).i32(sp.hp);
            w.i32(sp.frames).i32(sp.cooldown).i32(sp.player).u32(sp.next).i32(sp.counter).u8(sp.phase).u8(sp.direct);
            w.u8(sp.aura).f32(sp.radius).f32(sp.value);
            put_vec3(w, sp.point);
            put_ids(w, sp.victims);
        }
        w.u32(o.heal_due);
        w.u32(static_cast<uint32_t>(o.autofire_map.size()));
        for (const auto& [crc, on] : o.autofire_map) w.u32(crc).u8(on);
        w.u32(o.hunt_due);
        w.u8(o.deployed).u8(o.spread).u8(o.self_destruct).u8(o.confused).u8(o.barrage).u8(o.stunned);
        w.u32(o.confuse_end).i32(o.barrage_dummy).u8(o.pending_direct).i32(o.troopers_deployed);
        put_ids(w, o.troopers);
        w.i32(o.anim_type).u32(o.anim_start).f32(o.anim_rate).f32(o.anim_end).u8(o.anim_loop).u8(o.anim_then).u8(o.lock_flags);
    }

    w.u32(static_cast<uint32_t>(spawns_.size()));
    for (const SpawnRequest& s : spawns_) {
        w.u32(s.serial).u8(static_cast<uint8_t>(s.purpose)).i32(s.object_type);
        put_vec3(w, s.position);
        put_vec3(w, s.facing);
        w.i32(s.owner).i32(s.source).i32(s.target).u8(s.ability_type).f32(s.fuse_seconds).i32(s.bone);
    }
    w.u32(static_cast<uint32_t>(mines_.size()));
    for (const Mine& m : mines_) {
        w.u32(m.serial).i32(m.id).i32(m.owner);
        put_vec3(w, m.pos);
        w.f32(m.trigger).f32(m.detect).u32(m.arm).u8(m.detected).u8(m.dead);
    }
    w.u32(static_cast<uint32_t>(player_entries_.size()));
    for (const PlayerEntry& e : player_entries_) w.i32(e.player).u8(e.list).i32(e.category).f32(e.value).i32(e.source).u32(e.crc);
    w.u32(static_cast<uint32_t>(source_entries_.size()));
    for (const SourceEntry& e : source_entries_) w.i32(e.player).i32(e.source).u32(e.crc).u8(e.kind);
    w.u32(static_cast<uint32_t>(remote_bombs_.size()));
    for (const RemoteBomb& b : remote_bombs_) w.i32(b.player).u32(b.serial);
    w.u32(static_cast<uint32_t>(events_.size()));
    for (const AbilityEvent& e : events_) {
        w.u8(static_cast<uint8_t>(e.kind)).u8(e.type).i32(e.id).i32(e.target).u32(e.frame);
        put_vec3(w, e.point);
        w.i32(e.value);
    }
    w.u32(static_cast<uint32_t>(overrides_.size()));
    for (const PlayerOverride& p : overrides_) w.i32(p.player).u32(p.crc).u8(p.enabled);
    w.u32(static_cast<uint32_t>(type_overrides_.size()));
    for (const TypeOverride& t : type_overrides_) w.i32(t.player).u32(t.desc).u32(t.crc).u8(t.enabled);
    w.u32(static_cast<uint32_t>(bm_blocked_.size()));
    for (const std::vector<uint32_t>& list : bm_blocked_) {
        w.u32(static_cast<uint32_t>(list.size()));
        for (const uint32_t crc : list) w.u32(crc);
    }
}

bool AbilitySystem::read(ByteReader& r) {
    const uint32_t version = r.u32();
    if (!r.ok() || version != kAbilitySaveVersion) return false;
    const uint32_t c_objects = r.u32(), c_specials = r.u32(), c_players = r.u32();
    if (!r.ok() || c_objects != data_->objects.size() || c_specials != data_->specials.size() ||
        c_players != combat_->player_count())
        return false;
    const uint32_t spawn_serial = r.u32();
    const uint8_t direct_ctx = r.u8();
    if (!r.ok()) return false;

    uint32_t n = 0;
    if (!r.count(n, 1000)) return false;
    std::vector<Obj> objs(n);
    bool first = true;
    ObjectId prev = 0;
    for (Obj& o : objs) {
        o.id = r.i32();
        o.desc = r.u32();
        o.owner = r.i32();
        o.container = r.i32();
        o.is_container = r.u8();
        o.combat_unit = r.u8();
        o.dead = r.u8();
        if (!r.ok() || !get_ids(r, o.members)) return false;
        if (o.id == kNoObject || o.desc >= c_objects || (!first && o.id <= prev)) return false;
        first = false;
        prev = o.id;
        const size_t records = data_->objects[o.desc].unit_abilities.size();
        for (int16_t& rec : o.record) {
            rec = static_cast<int16_t>(r.u16());
            if (rec < -1 || (rec >= 0 && static_cast<size_t>(rec) >= records)) return false;
        }
        for (uint8_t& v : o.active) v = r.u8();
        for (uint8_t& v : o.autofire) v = r.u8();
        for (uint8_t& v : o.behavior) v = r.u8();
        for (int32_t& v : o.cd.counter) v = r.i32();
        for (int32_t& v : o.cd.maximum) v = r.i32();
        for (uint8_t& v : o.cd.phase) v = r.u8();
        o.cd.last_update = r.u32();
        o.pending_target = r.i32();
        o.pending_hp = r.i32();
        o.pending_crc = r.u32();
        o.pending_type = r.i32();
        o.nebula_frame = r.u32();
        o.force_frame = r.u32();
        o.ion_storm_frame = r.u32();
        o.locomotor_busy = r.u8();
        o.sap_disabled = r.u8();
        o.corrupted = r.u8();
        o.weapons_disabled = r.u8();
        o.garrison_count = r.i32();
        o.gravity_well_destroyed = r.u8();
        o.tractored = r.u8();
        o.interdicting = r.u8();
        o.immobilize = r.i32();
        o.stealthed = r.u8();
        o.stealth_frames = r.i32();
        o.last_detected = r.u32();
        for (size_t k = 0; k < 2; ++k) {
            o.shot_target[k] = r.i32();
            o.shot_hp[k] = r.i32();
            o.shot_proj[k] = r.i32();
        }
        o.lucky_fired = r.u8();
        if (!r.ok() || o.pending_type < -1 || o.pending_type >= static_cast<int32_t>(kAbilitySlots)) return false;
        uint32_t ns = 0;
        if (!r.count(ns, 64)) return false;
        o.specials.resize(ns);
        for (SpecialState& sp : o.specials) {
            sp.desc = r.u32();
            sp.due = r.u32();
            sp.applied = r.u8();
            sp.fired = r.u8();
            sp.trigger = r.u8();
            sp.target = r.i32();
            sp.hp = r.i32();
            sp.frames = r.i32();
            sp.cooldown = r.i32();
            sp.player = r.i32();
            sp.next = r.u32();
            sp.counter = r.i32();
            sp.phase = r.u8();
            sp.direct = r.u8();
            sp.aura = r.u8();
            sp.radius = r.f32();
            sp.value = r.f32();
            sp.point = get_vec3<Vec3>(r);
            if (!r.ok() || sp.desc >= c_specials || !get_ids(r, sp.victims)) return false;
        }
        o.heal_due = r.u32();
        uint32_t na = 0;
        if (!r.count(na, 5)) return false;
        o.autofire_map.resize(na);
        for (auto& [crc, on] : o.autofire_map) {
            crc = r.u32();
            on = r.u8();
        }
        o.hunt_due = r.u32();
        o.deployed = r.u8();
        o.spread = r.u8();
        o.self_destruct = r.u8();
        o.confused = r.u8();
        o.barrage = r.u8();
        o.stunned = r.u8();
        o.confuse_end = r.u32();
        o.barrage_dummy = r.i32();
        o.pending_direct = r.u8();
        o.troopers_deployed = r.i32();
        if (!r.ok() || !get_ids(r, o.troopers)) return false;
        o.anim_type = r.i32();
        o.anim_start = r.u32();
        o.anim_rate = r.f32();
        o.anim_end = r.f32();
        o.anim_loop = r.u8();
        o.anim_then = r.u8();
        o.lock_flags = r.u8();
    }

    if (!r.count(n, 51)) return false;
    std::vector<SpawnRequest> spawns(n);
    for (SpawnRequest& s : spawns) {
        s.serial = r.u32();
        const uint8_t purpose = r.u8();
        s.object_type = r.i32();
        s.position = get_vec3<Vec3>(r);
        s.facing = get_vec3<Vec3>(r);
        s.owner = r.i32();
        s.source = r.i32();
        s.target = r.i32();
        s.ability_type = r.u8();
        s.fuse_seconds = r.f32();
        s.bone = r.i32();
        if (!r.ok() || purpose > static_cast<uint8_t>(SpawnPurpose::StationUpgrade)) return false;
        s.purpose = static_cast<SpawnPurpose>(purpose);
    }
    if (!r.count(n, 38)) return false;
    std::vector<Mine> mines(n);
    for (Mine& m : mines) {
        m.serial = r.u32();
        m.id = r.i32();
        m.owner = r.i32();
        m.pos = get_vec3<Vec3>(r);
        m.trigger = r.f32();
        m.detect = r.f32();
        m.arm = r.u32();
        m.detected = r.u8();
        m.dead = r.u8();
    }
    if (!r.count(n, 21)) return false;
    std::vector<PlayerEntry> player_entries(n);
    for (PlayerEntry& e : player_entries) {
        e.player = r.i32();
        e.list = r.u8();
        e.category = r.i32();
        e.value = r.f32();
        e.source = r.i32();
        e.crc = r.u32();
    }
    if (!r.count(n, 13)) return false;
    std::vector<SourceEntry> source_entries(n);
    for (SourceEntry& e : source_entries) {
        e.player = r.i32();
        e.source = r.i32();
        e.crc = r.u32();
        e.kind = r.u8();
    }
    if (!r.count(n, 8)) return false;
    std::vector<RemoteBomb> remote_bombs(n);
    for (RemoteBomb& b : remote_bombs) {
        b.player = r.i32();
        b.serial = r.u32();
    }
    if (!r.count(n, 30)) return false;
    std::vector<AbilityEvent> events(n);
    for (AbilityEvent& e : events) {
        const uint8_t kind = r.u8();
        e.type = r.u8();
        e.id = r.i32();
        e.target = r.i32();
        e.frame = r.u32();
        e.point = get_vec3<Vec3>(r);
        e.value = r.i32();
        if (!r.ok() || kind > static_cast<uint8_t>(AbilityEventKind::BribeRefused)) return false;
        e.kind = static_cast<AbilityEventKind>(kind);
    }
    if (!r.count(n, 9)) return false;
    std::vector<PlayerOverride> overrides(n);
    for (PlayerOverride& p : overrides) {
        p.player = r.i32();
        p.crc = r.u32();
        p.enabled = r.u8();
    }
    if (!r.count(n, 13)) return false;
    std::vector<TypeOverride> type_overrides(n);
    for (TypeOverride& t : type_overrides) {
        t.player = r.i32();
        t.desc = r.u32();
        t.crc = r.u32();
        t.enabled = r.u8();
    }
    if (!r.count(n, 4)) return false;
    std::vector<std::vector<uint32_t>> bm_blocked(n);
    for (std::vector<uint32_t>& list : bm_blocked) {
        uint32_t nl = 0;
        if (!r.count(nl, 4)) return false;
        list.resize(nl);
        for (uint32_t& crc : list) crc = r.u32();
    }
    if (!r.ok() || !r.done()) return false;

    spawn_serial_ = spawn_serial;
    direct_ctx_ = direct_ctx;
    objs_ = std::move(objs);
    spawns_ = std::move(spawns);
    mines_ = std::move(mines);
    player_entries_ = std::move(player_entries);
    source_entries_ = std::move(source_entries);
    remote_bombs_ = std::move(remote_bombs);
    events_ = std::move(events);
    overrides_ = std::move(overrides);
    type_overrides_ = std::move(type_overrides);
    bm_blocked_ = std::move(bm_blocked);
    scratch_.clear();
    member_scratch_.clear();
    return true;
}

// ---- SpecialWeaponSystem --------------------------------------------------------------------------------------------

void SpecialWeaponSystem::write(ByteWriter& w) const {
    w.u32(kSpecialWeaponSaveVersion);
    w.u32(static_cast<uint32_t>(descs_.size()));
    for (const SpecialWeaponDesc& d : descs_) {
        w.u8(d.kinds).f32(d.recharge_seconds).i32(d.pulse_count).f32(d.pulse_delay_seconds).i32(d.projectile);
        w.u64(d.valid_targets).boolean(d.requires_base_power).i32(d.target_action_index).f32(d.max_speed);
        w.f32(d.tsw_start_distance).f32(d.tsw_attack_distance).boolean(d.tsw_match_target_z).f32(d.tsw_z_adjust);
        w.f32(d.tsw_power_up_seconds).f32(d.tsw_attack_anim_seconds).i32(d.tsw_debris_delay_frames);
        w.i32(d.tsw_post_destruction_frames);
    }
    w.u32(static_cast<uint32_t>(packs_.size()));
    for (const Pack& p : packs_) {
        w.i32(p.id).u32(p.desc).i32(p.owner).u32(p.start).u32(p.ready).u8(p.targeting_flag).u8(p.ready_signalled);
        w.i32(p.pulses_left).u32(p.pulse_index).u32(p.next_pulse).i32(p.shot_target);
        put_vec3(w, p.shot_point);
        w.u8(p.at_point).i32(p.tsw).u8(p.tsw_init).i32(p.tsw_target);
        put_vec3(w, p.tsw_target_pos);
        w.i32(p.countdown).i32(p.debris_pick).u32(p.debris_frame).u8(p.debris_pending);
    }
    w.u32(static_cast<uint32_t>(shots_.size()));
    for (const SpecialWeaponShot& s : shots_) {
        w.i32(s.weapon).i32(s.owner).i32(s.projectile).i32(s.target);
        put_vec3(w, s.point);
        w.boolean(s.at_point).u32(s.pulse);
    }
    w.u32(static_cast<uint32_t>(events_.size()));
    for (const SpecialWeaponEvent& e : events_) {
        w.u8(static_cast<uint8_t>(e.kind)).i32(e.id).i32(e.target).i32(e.value).u32(e.frame);
        put_vec3(w, e.point);
    }
}

bool SpecialWeaponSystem::read(ByteReader& r) {
    if (r.u32() != kSpecialWeaponSaveVersion || !r.ok()) return false;
    uint32_t n = 0;
    if (!r.count(n, 63)) return false;
    std::vector<SpecialWeaponDesc> descs(n);
    for (SpecialWeaponDesc& d : descs) {
        d.kinds = r.u8();
        d.recharge_seconds = r.f32();
        d.pulse_count = r.i32();
        d.pulse_delay_seconds = r.f32();
        d.projectile = r.i32();
        d.valid_targets = r.u64();
        d.requires_base_power = r.boolean();
        d.target_action_index = r.i32();
        d.max_speed = r.f32();
        d.tsw_start_distance = r.f32();
        d.tsw_attack_distance = r.f32();
        d.tsw_match_target_z = r.boolean();
        d.tsw_z_adjust = r.f32();
        d.tsw_power_up_seconds = r.f32();
        d.tsw_attack_anim_seconds = r.f32();
        d.tsw_debris_delay_frames = r.i32();
        d.tsw_post_destruction_frames = r.i32();
        if (!r.ok()) return false;
    }
    const auto nd = static_cast<uint32_t>(descs.size());
    if (!r.count(n, 85)) return false;
    std::vector<Pack> packs(n);
    bool first = true;
    ObjectId prev = 0;
    for (Pack& p : packs) {
        p.id = r.i32();
        p.desc = r.u32();
        p.owner = r.i32();
        p.start = r.u32();
        p.ready = r.u32();
        p.targeting_flag = r.u8();
        p.ready_signalled = r.u8();
        p.pulses_left = r.i32();
        p.pulse_index = r.u32();
        p.next_pulse = r.u32();
        p.shot_target = r.i32();
        p.shot_point = get_vec3<Vec3>(r);
        p.at_point = r.u8();
        p.tsw = r.i32();
        p.tsw_init = r.u8();
        p.tsw_target = r.i32();
        p.tsw_target_pos = get_vec3<Vec3>(r);
        p.countdown = r.i32();
        p.debris_pick = r.i32();
        p.debris_frame = r.u32();
        p.debris_pending = r.u8();
        if (!r.ok() || p.desc >= nd || (!first && p.id <= prev) || p.tsw < -1 || p.tsw > static_cast<int32_t>(kTswRelease) ||
            p.tsw == 11 || p.tsw_init > 2)  // no state 11 (TSW_PLANET_DESTROY §3)
            return false;
        first = false;
        prev = p.id;
    }
    if (!r.count(n, 33)) return false;
    std::vector<SpecialWeaponShot> shots(n);
    for (SpecialWeaponShot& s : shots) {
        s.weapon = r.i32();
        s.owner = r.i32();
        s.projectile = r.i32();
        s.target = r.i32();
        s.point = get_vec3<Vec3>(r);
        s.at_point = r.boolean();
        s.pulse = r.u32();
    }
    if (!r.count(n, 29)) return false;
    std::vector<SpecialWeaponEvent> events(n);
    for (SpecialWeaponEvent& e : events) {
        const uint8_t kind = r.u8();
        e.id = r.i32();
        e.target = r.i32();
        e.value = r.i32();
        e.frame = r.u32();
        e.point = get_vec3<Vec3>(r);
        if (!r.ok() || kind > static_cast<uint8_t>(SwEventKind::TswDone)) return false;
        e.kind = static_cast<SwEventKind>(kind);
    }
    if (!r.ok() || !r.done()) return false;
    descs_ = std::move(descs);
    packs_ = std::move(packs);
    shots_ = std::move(shots);
    events_ = std::move(events);
    return true;
}

// ---- SpaceFieldIndex ------------------------------------------------------------------------------------------------

void SpaceFieldIndex::write(ByteWriter& w) const {
    w.u32(kSpaceFieldSaveVersion);
    w.u32(static_cast<uint32_t>(fields_.size()));
    for (const SpaceField& f : fields_) w.i32(f.id).u32(f.kinds).f32(f.x).f32(f.y).f32(f.radius);
}

bool SpaceFieldIndex::read(ByteReader& r) {
    if (r.u32() != kSpaceFieldSaveVersion || !r.ok()) return false;
    uint32_t n = 0;
    if (!r.count(n, 20)) return false;
    std::vector<SpaceField> fields(n);
    for (SpaceField& f : fields) {
        f.id = r.i32();
        f.kinds = r.u32();
        f.x = r.f32();
        f.y = r.f32();
        f.radius = r.f32();
    }
    if (!r.ok() || !r.done()) return false;
    fields_ = std::move(fields);
    return true;
}

}  // namespace alamo::tactical::abilities
