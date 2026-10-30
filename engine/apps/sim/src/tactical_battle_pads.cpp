// Int-final: build pads of a tactical battle (L1_S4_REMAINING §2.2 / §3.6; tactical_battle.hpp "build pads").
// Serial code (Lua service / tick), no RNG draws; pad state in ascending pad id.
#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

#include "alamo/core/dmath.hpp"
#include "alamo/galactic/campaign.hpp"
#include "alamo/objects/behaviors.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/sim/tactical_battle.hpp"

namespace alamo::sim {

namespace {

bool same_name(std::string_view a, std::string_view b) {
    while (!a.empty() && std::isspace(static_cast<unsigned char>(a.front()))) a.remove_prefix(1);
    while (!a.empty() && std::isspace(static_cast<unsigned char>(a.back()))) a.remove_suffix(1);
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::toupper(static_cast<unsigned char>(a[i])) != std::toupper(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

std::string_view trim_ws(std::string_view a) {
    while (!a.empty() && std::isspace(static_cast<unsigned char>(a.front()))) a.remove_prefix(1);
    while (!a.empty() && std::isspace(static_cast<unsigned char>(a.back()))) a.remove_suffix(1);
    return a;
}

}  // namespace

void TacticalBattle::pad_persist(const objects::ObjectStore& st, objects::ObjectHandle pad, objects::TypeIndex type, int32_t value, bool add) {
    // §6.5 0x42f160 gate: tactical mode with a galactic-campaign parent, a battle planet [port: the host passes the list
    // only then; the GOM state +0x618 != 9 test is not modelled]; the pad type's Tactically_Built_Child_Object_Persists.
    if (!pad_records_ || !st.alive(pad) || type == objects::kNone) return;
    const objects::GameObjectTypes& types = catalog_->types();
    const objects::TypeIndex pt = st.type(pad);
    if (pt >= types.size() || !types.get_bool(pt, "Tactically_Built_Child_Object_Persists")) return;
    const Vec3 p = st.position(pad);
    std::vector<PadRecord>& v = *pad_records_;
    if (add) {  // 0x4b6c70: refused when the position is already present (or the small int > 5)
        for (const PadRecord& r : v)
            if (r.x == p.x && r.y == p.y && r.z == p.z) return;
        v.push_back({static_cast<uint32_t>(type), p.x, p.y, p.z, 0, value});
        return;
    }
    for (size_t i = 0; i < v.size(); ++i)  // 0x4bb3a0: position and type must both match
        if (v[i].x == p.x && v[i].y == p.y && v[i].z == p.z && v[i].type == static_cast<uint32_t>(type)) {
            v.erase(v.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
}

TacticalBattle::PadState* TacticalBattle::pad_state(objects::ObjectId pad, bool create) {
    auto it = std::lower_bound(pads_.begin(), pads_.end(), pad, [](const PadState& p, objects::ObjectId id) { return p.pad < id; });
    if (it != pads_.end() && it->pad == pad) return &*it;
    if (!create) return nullptr;
    PadState s;
    s.pad = pad;
    pads_used_ = true;
    return &*pads_.insert(it, s);
}

const TacticalBattle::PadState* TacticalBattle::pad_state(objects::ObjectId pad) const {
    auto it = std::lower_bound(pads_.begin(), pads_.end(), pad, [](const PadState& p, objects::ObjectId id) { return p.pad < id; });
    return (it != pads_.end() && it->pad == pad) ? &*it : nullptr;
}

objects::ObjectId TacticalBattle::pad_contents(objects::ObjectId pad) const {
    const PadState* s = pad_state(pad);
    if (!s) return objects::kNoObjectId;
    return s->built != objects::kNoObjectId ? s->built : s->uc;
}

bool TacticalBattle::pad_locked(objects::ObjectId pad) const {
    const PadState* s = pad_state(pad);
    return s && s->locked;
}

void TacticalBattle::pad_lock(objects::ObjectId pad, bool lock) {
    PadState* s = pad_state(pad, false);
    if (s && (s->uc != objects::kNoObjectId || s->built != objects::kNoObjectId)) s->locked = lock;  // empty pad: warning only (§2.2)
}

int32_t TacticalBattle::pad_build(TacticalMode& mode, objects::ObjectId pad, objects::TypeIndex type, bool check_rules) {
    if (finished_) return -1;
    objects::ObjectStore& st = mode.primary();
    const objects::GameObjectTypes& types = catalog_->types();
    static const objects::BehaviorTypeId kPad = objects::behavior_type_id("TACTICAL_BUILD_OBJECTS");
    const objects::ObjectHandle ph = st.find(pad);
    if (!st.alive(ph) || kPad == objects::kNoBehavior || !st.has_behavior(ph, kPad) || type >= types.size()) return -1;
    const PadState* cur = pad_state(pad);
    const bool empty = !cur || (cur->uc == objects::kNoObjectId && cur->built == objects::kNoObjectId);
    const int32_t owner = st.owner(ph);
    if (check_rules) {
        // Buildable list (0x372c10): the pad owner's faction row of Tactical_Buildable_Objects_Campaign.
        std::string faction;
        if (owner >= 0 && static_cast<size_t>(owner) < galaxy_->players().size()) {
            const uint32_t f = galaxy_->players()[static_cast<size_t>(owner)].faction;
            if (f < galaxy_->rules().factions.size()) faction = galaxy_->rules().factions[f].name;
        }
        const std::string_view name = types.raw().types().name[type];
        bool listed = false, in_row = false;
        for (const std::string_view tok : types.get_list(st.type(ph), "Tactical_Buildable_Objects_Campaign")) {
            if (types.raw().find(tok) == objects::kNone) {
                in_row = same_name(tok, faction);
                continue;
            }
            listed = listed || (in_row && same_name(tok, name));
        }
        // Rule 0x42e400: empty, no blocking (alive, Influences_Capture_Point, not allied) object within the radius.
        bool blocked = !empty;
        const objects::TypeIndex pt = st.type(ph);
        const double r = pt < types.size() ? static_cast<double>(types.get_float(pt, "Capture_Point_Radius")) : 0.0;
        const Vec3 pp = st.position(ph);
        for (const objects::ObjectHandle h : st.all_objects()) {
            if (blocked) break;
            if (!st.alive(h) || h == ph) continue;
            const objects::TypeIndex t = st.type(h);
            if (t >= types.size() || !types.get_bool(t, "Influences_Capture_Point")) continue;
            const int32_t o = st.owner(h);
            const bool allied = o == owner || (combat_ && !combat_->is_enemy(owner, o));
            if (allied) continue;
            const Vec3 q = st.position(h);
            const double dx = static_cast<double>(q.x) - pp.x, dy = static_cast<double>(q.y) - pp.y, dz = static_cast<double>(q.z) - pp.z;
            blocked = dx * dx + dy * dy + dz * dz <= r * r;
        }
        if (!listed || blocked) return 0;
    }
    // Start build 0x42dbd0 (charge 0, sfx 0): empty pad, cooldown elapsed, the builder exists.
    if (!empty || (cur && cur->cooldown_end > frame_) || owner < 0) return -1;
    objects::SpawnParams sp;
    sp.type = type;
    sp.owner = owner;
    sp.position = bt_pad_attach_point(mode, ph);  // REMAINING_GAPS §6.5: the attachment bone 0x42e280
    sp.facing = st.facing(ph);
    const objects::ObjectHandle uc = mode.spawn(sp);
    if (!st.alive(uc)) return -1;
    PadState* s = pad_state(pad, true);
    pads_mode_ = &mode;
    s->uc = st.id(uc);
    s->builder = owner;
    s->cooldown_start = s->cooldown_end = 0;
    const int32_t secs = std::max(0, types.get_int(type, "Tactical_Build_Time_Seconds"));  // int field (+0xa00)
    const uint32_t frames = static_cast<uint32_t>(secs) * 30u;  // [port] per-mode player multiplier 1
    s->uc_end = frame_ + frames;
    // battle host: the UC joins the battle as a combat unit of the builder; health ramp vf9 / vf6 (rate = max / frames,
    // starting at one service's worth [inferred]).
    const float start = frames > 0 ? 1.0f / static_cast<float>(frames) : 1.0f;
    if (bt_adopt_built(mode, uc, owner, start, bt_rng_) != 0xFFFFFFFFu && frames > 0) {
        const objects::ObjectId id = st.id(uc);
        const float rate = combat_->max_health(id) / static_cast<float>(frames);
        const auto it = std::lower_bound(bt_ramps_.begin(), bt_ramps_.end(), id, [](const UcRamp& r, objects::ObjectId v) { return r.id < v; });
        bt_ramps_.insert(it, UcRamp{id, rate});
    }
    return 1;
}

void TacticalBattle::pads_tick(TacticalMode& mode) {
    if (!pads_used_) return;
    bt_pad_health();  // battle host: UC health ramp (TacticalUnderConstructionBehavior vf6)
    objects::ObjectStore& st = mode.primary();
    const objects::GameObjectTypes& types = catalog_->types();
    for (size_t i = 0; i < pads_.size(); ++i) {
        PadState& s = pads_[i];
        const objects::ObjectHandle ph = st.find(s.pad);
        const bool pad_alive = st.alive(ph);
        auto child_gone = [&](bool was_built, objects::TypeIndex child_type) {  // 0x42e6d0
            if (was_built) s.locked = false;
            const objects::TypeIndex pt = pad_alive ? st.type(ph) : objects::kNone;
            if (pad_alive && child_type != objects::kNone) pad_persist(st, ph, child_type, -1, false);  // §6.5 remove
            if (pt < types.size() && types.get_bool(pt, "Destroy_When_Child_Dies")) {
                mode.destroy(ph);
                return;
            }
            if (pad_alive) s4_capture_reset(mode, s.pad);  // §6.5: a CAPTURE_POINT pad goes back to the neutral player
            const float wait = pt < types.size() ? types.get_float(pt, "Minimum_Time_Before_Pad_Can_Build_Again") : 0.0f;
            if (wait > 0.0f) {
                s.cooldown_start = frame_;
                s.cooldown_end = frame_ + static_cast<uint32_t>(wait * 30.0f + 0.5f);
            }
        };
        if (s.uc != objects::kNoObjectId) {
            const objects::ObjectHandle uh = st.find(s.uc);
            if (!st.alive(uh)) {
                s.uc = objects::kNoObjectId;
                s.builder = -1;
                child_gone(false, objects::kNone);
            } else if (frame_ >= s.uc_end) {
                // Completion 0x42e890: the final type for the builder at the UC (+0.1 z), then the UC goes.
                const objects::TypeIndex ut = st.type(uh);
                const objects::TypeIndex ft = ut < types.size() ? types.resolve_name(types.get_string(ut, "Tactical_Buildable_Constructed"))
                                                                : objects::kNone;
                // battle host: the UC's health fraction carries over; the UC leaves the battle (replacement, not a death).
                float frac = 1.0f;
                Unit* ucu = unit_by_id(s.uc);
                if (ucu && ucu->alive && combat_->has_unit(s.uc) && combat_->max_health(s.uc) > 0.0f)
                    frac = combat_->health(s.uc) / combat_->max_health(s.uc);
                if (ucu && ucu->alive) {
                    ucu->alive = false;
                    --parts_[ucu->participant].alive;
                    abilities_->remove(ucu->id);
                    unit_gone(mode, *ucu);
                }
                ucu = nullptr;  // units_ may grow below
                objects::ObjectHandle bh{};
                if (ft != objects::kNone && ft < types.size()) {
                    objects::SpawnParams sp;
                    sp.type = ft;
                    sp.owner = s.builder >= 0 ? s.builder : st.owner(uh);
                    sp.position = st.position(uh);
                    sp.position.z += 0.1f;
                    sp.facing = st.facing(uh);
                    bh = mode.spawn(sp);
                    if (st.alive(bh)) bt_adopt_built(mode, bh, sp.owner, frac, bt_rng_);
                }
                mode.destroy(uh);
                const int32_t builder = s.builder;
                s.uc = objects::kNoObjectId;
                s.builder = -1;
                if (st.alive(bh)) {
                    s.built = st.id(bh);
                    s.built_type = ft;
                    // §6.5 kill list (+0xa60): every object of exactly Previous_Upgrade_Level_Type (the built type's, else
                    // the UC type's [M: which type carries the field]) owned by a player allied with the pad owner
                    // (0x39a230) is killed silently (0x391310 flag 1), store order.
                    std::string_view prev = trim_ws(types.get_string(ft, "Previous_Upgrade_Level_Type"));
                    if (prev.empty() && ut < types.size()) prev = trim_ws(types.get_string(ut, "Previous_Upgrade_Level_Type"));
                    const objects::TypeIndex pv = prev.empty() ? objects::kNone : types.resolve_name(prev);
                    if (pv != objects::kNone && pad_alive) {
                        const int32_t po = st.owner(ph);
                        std::vector<objects::ObjectHandle> victims;
                        for (const objects::ObjectHandle h : st.all_objects()) {
                            if (!st.alive(h) || h == bh || st.type(h) != pv) continue;
                            const int32_t o = st.owner(h);
                            if (o == po || (combat_ && o >= 0 && po >= 0 && !combat_->is_enemy(o, po))) victims.push_back(h);
                        }
                        for (const objects::ObjectHandle h : victims) bt_kill_silently(mode, h);
                    }
                    if (pad_alive) pad_persist(st, ph, ft, builder, true);  // §6.5 add
                } else {
                    child_gone(false, objects::kNone);
                }
            }
        } else if (s.built != objects::kNoObjectId && !st.alive(st.find(s.built))) {
            s.built = objects::kNoObjectId;
            const objects::TypeIndex bt = s.built_type;
            s.built_type = objects::kNone;
            child_gone(true, bt);
        }
    }
}

void TacticalBattle::pads_hash(StateHasher& h) const {
    h.u32(static_cast<uint32_t>(pads_.size()));
    for (const PadState& s : pads_) {
        h.i32(s.pad);
        h.i32(s.uc);
        h.i32(s.built);
        h.i32(s.builder);
        h.u32(s.uc_end);
        h.u32(s.cooldown_start);
        h.u32(s.cooldown_end);
        h.boolean(s.locked);
        h.i32(static_cast<int32_t>(s.built_type));
    }
}

}  // namespace alamo::sim
