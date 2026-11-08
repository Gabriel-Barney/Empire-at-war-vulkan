// N1 stage 2 - save / load during a space battle (contract: alamo/core/save_file.hpp) for HangarSystem, ContentsSystem,
// TacticalProduction and EngineLoops. Little-endian, field by field, counts first; carriers / lists / objects ascending
// id (their storage order), everything else in insertion order. Floats as bit patterns; references are object ids,
// player indices and descriptor / entry / hardpoint indices.
//
// HangarSystem (version kHangarSaveVersion): u32 descriptor count (checked); u32 request serial; u32 + carriers (id,
// desc, owner, spawn player, due, next, state, enabled, deploy latch, bays-gone, u32 + bays, u32 + entries, u32 + spawned);
// u32 + launch requests; u32 + events.
// ContentsSystem (kContentsSaveVersion): u32 + lists (container, u32 + objects).
// TacticalProduction (kProductionSaveVersion): u32 next order id; u32 + players (u32 + orders); u32 + completed orders.
// EngineLoops (kEngineLoopsSaveVersion): u32 + objects (id, idle, moving, suppressed, holster, drawn, transition start /
// end, last engaged, version 4: the thrown saber's throw / catch / end frames); u32 + requests (id, loop, start, frame, op,
// fade). Versions 2 (no saber state) and 3 (the cu8i catch frame) are still read.
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "alamo/core/bytes.hpp"
#include "alamo/tactical/hangar/contents.hpp"
#include "alamo/tactical/hangar/engine_loops.hpp"
#include "alamo/tactical/hangar/hangar_system.hpp"
#include "alamo/tactical/save_io.hpp"

namespace alamo::tactical::hangar {

using save_io::get_vec3;
using save_io::put_vec3;

namespace {

constexpr uint32_t kHangarSaveVersion = 1;
constexpr uint32_t kContentsSaveVersion = 1;
constexpr uint32_t kProductionSaveVersion = 1;
constexpr uint32_t kEngineLoopsSaveVersion = 4;  // 2: cu8g t01 holster state per object, request op + fade; 3: cu8i t01 catch frame; 4: cu8j t03 throw / catch / end

void put_order(ByteWriter& w, const BuildOrder& o) {
    w.u32(o.id).i32(o.player).i32(o.builder).i32(o.type).u32(o.total_frames).u32(o.frames_left).u32(o.issued);
}

BuildOrder get_order(ByteReader& r) {
    BuildOrder o;
    o.id = r.u32();
    o.player = r.i32();
    o.builder = r.i32();
    o.type = r.i32();
    o.total_frames = r.u32();
    o.frames_left = r.u32();
    o.issued = r.u32();
    return o;
}

}  // namespace

// ---- HangarSystem ---------------------------------------------------------------------------------------------------

void HangarSystem::write(ByteWriter& w) const {
    w.u32(kHangarSaveVersion);
    w.u32(static_cast<uint32_t>(descs_.size()));
    w.u32(serial_);
    w.u32(static_cast<uint32_t>(carriers_.size()));
    for (const Carrier& c : carriers_) {
        w.i32(c.id).u32(c.desc).i32(c.owner).i32(c.spawn_player).u32(c.due).u32(c.next);
        w.u8(c.state).u8(c.enabled).u8(c.deploy_latch).u8(c.bays_gone);
        w.u32(static_cast<uint32_t>(c.bays.size()));
        for (const uint32_t b : c.bays) w.u32(b);
        w.u32(static_cast<uint32_t>(c.entries.size()));
        for (const Entry& e : c.entries) w.i32(e.type).i32(e.live).i32(e.max).i32(e.remaining);
        w.u32(static_cast<uint32_t>(c.spawned.size()));
        for (const Spawned& s : c.spawned) w.i32(s.id).u32(s.serial).i32(s.type).i32(s.entry).u8(s.spent);
    }
    w.u32(static_cast<uint32_t>(requests_.size()));
    for (const LaunchRequest& q : requests_) {
        w.u32(q.serial).i32(q.carrier).i32(q.owner).i32(q.type).i32(q.entry).boolean(q.from_pending).i32(q.bay);
        put_vec3(w, q.origin);
        put_vec3(w, q.flyout);
        put_vec3(w, q.direction);
        put_vec3(w, q.facing);
        w.boolean(q.escort);
    }
    w.u32(static_cast<uint32_t>(events_.size()));
    for (const HangarEvent& e : events_) w.u8(static_cast<uint8_t>(e.kind)).i32(e.id).i32(e.target).i32(e.value).u32(e.frame);
}

bool HangarSystem::read(ByteReader& r) {
    if (r.u32() != kHangarSaveVersion || !r.ok()) return false;
    const uint32_t ndesc = r.u32(), serial = r.u32();
    if (!r.ok() || ndesc != descs_.size()) return false;
    uint32_t n = 0;
    if (!r.count(n, 40)) return false;
    std::vector<Carrier> carriers(n);
    for (size_t i = 0; i < carriers.size(); ++i) {
        Carrier& c = carriers[i];
        c.id = r.i32();
        c.desc = r.u32();
        c.owner = r.i32();
        c.spawn_player = r.i32();
        c.due = r.u32();
        c.next = r.u32();
        c.state = r.u8();
        c.enabled = r.u8();
        c.deploy_latch = r.u8();
        c.bays_gone = r.u8();
        if (!r.ok() || c.desc >= ndesc || (i > 0 && c.id <= carriers[i - 1].id)) return false;
        uint32_t m = 0;
        if (!r.count(m, 4)) return false;
        c.bays.resize(m);
        for (uint32_t& b : c.bays) b = r.u32();
        if (!r.count(m, 16)) return false;
        c.entries.resize(m);
        for (Entry& e : c.entries) {
            e.type = r.i32();
            e.live = r.i32();
            e.max = r.i32();
            e.remaining = r.i32();
        }
        if (!r.count(m, 17)) return false;
        c.spawned.resize(m);
        const auto entries = static_cast<int32_t>(c.entries.size());
        for (Spawned& s : c.spawned) {
            s.id = r.i32();
            s.serial = r.u32();
            s.type = r.i32();
            s.entry = r.i32();
            s.spent = r.u8();
            if (!r.ok() || s.entry < -1 || s.entry >= entries) return false;
        }
        if (!r.ok()) return false;
    }
    if (!r.count(n, 74)) return false;
    std::vector<LaunchRequest> requests(n);
    for (LaunchRequest& q : requests) {
        q.serial = r.u32();
        q.carrier = r.i32();
        q.owner = r.i32();
        q.type = r.i32();
        q.entry = r.i32();
        q.from_pending = r.boolean();
        q.bay = r.i32();
        q.origin = get_vec3<Vec3>(r);
        q.flyout = get_vec3<Vec3>(r);
        q.direction = get_vec3<Vec3>(r);
        q.facing = get_vec3<Vec3>(r);
        q.escort = r.boolean();
    }
    if (!r.count(n, 17)) return false;
    std::vector<HangarEvent> events(n);
    for (HangarEvent& e : events) {
        const uint8_t kind = r.u8();
        e.id = r.i32();
        e.target = r.i32();
        e.value = r.i32();
        e.frame = r.u32();
        if (!r.ok() || kind > static_cast<uint8_t>(HangarEventKind::CarrierLost)) return false;
        e.kind = static_cast<HangarEventKind>(kind);
    }
    if (!r.ok() || !r.done()) return false;
    serial_ = serial;
    carriers_ = std::move(carriers);
    requests_ = std::move(requests);
    events_ = std::move(events);
    return true;
}

// ---- ContentsSystem -------------------------------------------------------------------------------------------------

void ContentsSystem::write(ByteWriter& w) const {
    w.u32(kContentsSaveVersion);
    w.u32(static_cast<uint32_t>(lists_.size()));
    for (const List& l : lists_) {
        w.i32(l.container).u32(static_cast<uint32_t>(l.objects.size()));
        for (const ObjectId id : l.objects) w.i32(id);
    }
}

bool ContentsSystem::read(ByteReader& r) {
    if (r.u32() != kContentsSaveVersion || !r.ok()) return false;
    uint32_t n = 0;
    if (!r.count(n, 8)) return false;
    std::vector<List> lists(n);
    for (size_t i = 0; i < lists.size(); ++i) {
        List& l = lists[i];
        l.container = r.i32();
        uint32_t m = 0;
        if (!r.count(m, 4) || (i > 0 && l.container <= lists[i - 1].container)) return false;
        l.objects.resize(m);
        for (ObjectId& id : l.objects) id = r.i32();
        if (!r.ok()) return false;
    }
    if (!r.done()) return false;
    lists_ = std::move(lists);
    return true;
}

// ---- TacticalProduction ---------------------------------------------------------------------------------------------

void TacticalProduction::write(ByteWriter& w) const {
    w.u32(kProductionSaveVersion);
    w.u32(next_id_);
    w.u32(static_cast<uint32_t>(players_.size()));
    for (const std::vector<BuildOrder>& list : players_) {
        w.u32(static_cast<uint32_t>(list.size()));
        for (const BuildOrder& o : list) put_order(w, o);
    }
    w.u32(static_cast<uint32_t>(done_.size()));
    for (const BuildOrder& o : done_) put_order(w, o);
}

bool TacticalProduction::read(ByteReader& r) {
    if (r.u32() != kProductionSaveVersion || !r.ok()) return false;
    const uint32_t next_id = r.u32();
    uint32_t n = 0;
    if (!r.count(n, 4)) return false;
    std::vector<std::vector<BuildOrder>> players(n);
    for (std::vector<BuildOrder>& list : players) {
        uint32_t m = 0;
        if (!r.count(m, 28)) return false;
        list.resize(m);
        for (BuildOrder& o : list) o = get_order(r);
        if (!r.ok()) return false;
    }
    if (!r.count(n, 28)) return false;
    std::vector<BuildOrder> done(n);
    for (BuildOrder& o : done) o = get_order(r);
    if (!r.ok() || !r.done()) return false;
    next_id_ = next_id;
    players_ = std::move(players);
    done_ = std::move(done);
    return true;
}

// ---- EngineLoops ----------------------------------------------------------------------------------------------------

void EngineLoops::write(ByteWriter& w) const {
    w.u32(kEngineLoopsSaveVersion);
    w.u32(static_cast<uint32_t>(objs_.size()));
    for (const Obj& o : objs_) {
        w.i32(o.id).u8(o.idle).u8(o.moving).u8(o.suppressed);
        w.u8(o.holster).u8(o.drawn).u32(o.t_start).u32(o.t_end).u32(o.last_engaged);  // version 2: cu8g t01 holster
        w.u32(o.throw_frame).u32(o.catch_frame).u32(o.end_frame);                     // version 4: cu8j t03
    }
    w.u32(static_cast<uint32_t>(requests_.size()));
    for (const EngineLoopRequest& q : requests_)
        w.i32(q.id).u8(static_cast<uint8_t>(q.loop)).boolean(q.start).u32(q.frame).u8(static_cast<uint8_t>(q.op)).f32(q.fade);
}

bool EngineLoops::read(ByteReader& r) {
    const uint32_t version = r.u32();
    // 2: no saber state (none in flight); 3: the cu8i catch frame (in flight up to it, then caught and ended).
    if (version < 2 || version > kEngineLoopsSaveVersion || !r.ok()) return false;
    uint32_t n = 0;
    if (!r.count(n, version >= 4 ? 33u : version == 3 ? 25u : 21u)) return false;
    std::vector<Obj> objs(n);
    for (size_t i = 0; i < objs.size(); ++i) {
        Obj& o = objs[i];
        o.id = r.i32();
        o.idle = r.u8();
        o.moving = r.u8();
        o.suppressed = r.u8();
        o.holster = r.u8();
        o.drawn = r.u8();
        o.t_start = r.u32();
        o.t_end = r.u32();
        o.last_engaged = r.u32();
        if (version >= 4) {
            o.throw_frame = r.u32();
            o.catch_frame = r.u32();
            o.end_frame = r.u32();
            if (o.end_frame == 0 ? (o.throw_frame | o.catch_frame) != 0 : o.end_frame < o.throw_frame) return false;
        } else if (version == 3) {
            o.catch_frame = o.end_frame = r.u32();
        }
        if (!r.ok() || (i > 0 && o.id <= objs[i - 1].id)) return false;
    }
    if (!r.count(n, 15)) return false;
    std::vector<EngineLoopRequest> requests(n);
    for (EngineLoopRequest& q : requests) {
        q.id = r.i32();
        const uint8_t loop = r.u8();
        q.start = r.boolean();
        q.frame = r.u32();
        const uint8_t op = r.u8();
        q.fade = r.f32();
        if (!r.ok() || loop > static_cast<uint8_t>(EngineLoop::Moving) || op > static_cast<uint8_t>(EngineLoopOp::HolsterWeapon))
            return false;
        q.loop = static_cast<EngineLoop>(loop);
        q.op = static_cast<EngineLoopOp>(op);
    }
    if (!r.ok() || !r.done()) return false;
    objs_ = std::move(objs);
    requests_ = std::move(requests);
    return true;
}

}  // namespace alamo::tactical::hangar
