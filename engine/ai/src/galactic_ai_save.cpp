// N1 save / load of the galactic AI (GalacticAiWorld::write / read; contract alamo/core/save_file.hpp).
//
// LAYOUT (u32 kWorldVersion first; every count before its items; maps in key order, lists in their own order):
//   shape        AI player pattern, plan library size, planet count (validated against the rebuilt world)
//   counters     now, perception frame, market frame, current plan, the six id counters, orders / rejected
//   gate         AiEnableGate
//   perception   GalacticSnapshot of the last update, GalacticPerception, the token resolver (length-prefixed:
//                stamps, last-seen values, leaf cache), Apply_Markup accumulators
//   lua world    the AI's own LuaPlayerWorld (Suspend_AI flag, per-player script state, pending requests) and the
//                world's GlobalValue map
//   players      per AI player (index order): AiContext (goals, templates, learning, budget, clocks), known objects,
//                pending desire bonuses, reachability caches (per frame / 10 s source lists), AiPathFinder, free-store VM
//   plans        PlanInstance + its Lua VM; task forces; blocking statuses; PotentialPlans; build tasks, the pending
//                list, the planet build queues, the market's per-frame delivery floods, free-store reservations and
//                moves, PAYG reclaims
//   lua runtime  pooled plan VMs per plan (pool order) + the created counter; Script_ evaluator VMs + missing names.
//                A pooled VM still pristine (PlanScriptRuntime::pristine: never acquired since load(), so every session
//                rebuilt from the same data has it too) is saved as a reference - its image digest - not as an image:
//                the reader keeps its own pristine VM after checking the digest (most of a session's VMs: the HotM save
//                at 1 min references 164 of 183, 13.4 of 15.0 MB of images). A reader whose own VM of that plan is no
//                longer pristine (acquired since its build) refuses the save: load into a freshly rebuilt session.
// Objects are saved as (handle value, id): the id is looked up again in the restored ObjectStore (so the AI chunk
// must be read after "objects"); a handle that was already stale stays stale.
// Lua VMs are length-prefixed LuaVm::write images, restored with LuaVm::read on a blank VM of the same host / script
// name, then the AI's host hooks (players world, AI host, markup host) are re-pointed at this world.
//
// NOT SAVED (rebuilt or diagnostic): definitions, plan library, equations, type info / masks, the equation-name and
// team-type lookup caches, planet Layer_Z_Adjust values (static data), the per-player perception memories handed to the
// S2 resolver and the resolver itself (copies of GalacticPerception's, rebuilt), the token resolver's neighbour lists,
// the path finders' matrices (rebuilt from their saved inputs and checked against the saved digest), the decision log,
// Lua error lists and the proposal statistics (diagnostics), host attachments (order sink, story worlds, the
// session GlobalValue store, sim hooks registered by start()).
//
// read(): all-or-nothing. Every section is parsed into staged copies (fresh VMs included); the token resolver, the
// only part read in place, is read after the whole stream validated; then everything is committed.
#include <algorithm>
#include <array>
#include <span>

#include "ai_save_io.hpp"
#include "galactic_ai_impl.hpp"

namespace alamo::ai {

using save::get_vec;
using save::put_vec;

namespace {
// 2: pristine pooled VMs by reference; 3: AI player types (q6 t07); 4 (cu12 t14): the status records' orders_out byte
// removed (never set or read).
constexpr uint32_t kWorldVersion = 4;
// 2: logic frame, player conflict stamps, conflict observer (q6 t03); 3: leaf cache keyed by (leaf, instance, system player,
// parameter set) with each entry's parameters, the 0xE visible flags and the last seen surface units (q6 t18)
constexpr uint32_t kResolverVersion = 3;

// ---- perception parameters (a leaf cache entry's bindings copy) ----
void put_param(ByteWriter& w, const Param& p) {
    w.str(p.name);
    w.u8(static_cast<uint8_t>(p.value.kind));
    w.f64(p.value.number);
    w.str(p.value.text);
    w.u32(static_cast<uint32_t>(p.value.enum_values.size()));
    for (const std::string& e : p.value.enum_values) w.str(e);
}
bool get_param(ByteReader& r, Param& p) {
    p.name = r.str();
    const uint8_t kind = r.u8();
    if (kind > static_cast<uint8_t>(ParamValue::Kind::Identifier)) r.fail();
    p.value.kind = static_cast<ParamValue::Kind>(kind);
    p.value.number = r.f64();
    p.value.text = r.str();
    p.value.enum_values.resize(save::count(r, 4));
    for (std::string& e : p.value.enum_values) e = r.str();
    return r.ok();
}
constexpr int kMaxValueDepth = 32;

// ---- objects ----
void put_handle(ByteWriter& w, const objects::ObjectStore& st, ObjectHandle h) {
    w.u64(h.value);
    w.i32(st.exists(h) ? st.id(h) : objects::kNoObjectId);
}

ObjectHandle get_handle(ByteReader& r, const objects::ObjectStore& st) {
    const ObjectHandle raw{r.u64()};
    const objects::ObjectId id = r.i32();
    if (!r.ok()) return {};
    if (id != objects::kNoObjectId) {
        const ObjectHandle h = st.find(id);
        if (!h) r.fail();  // the restored store does not know the object
        return h;
    }
    // Stale (or null) when saved: it stays stale; a slot the restored store reuses differently becomes null.
    return st.exists(raw) ? ObjectHandle{} : raw;
}

void put_handles(ByteWriter& w, const objects::ObjectStore& st, const std::vector<ObjectHandle>& v) {
    w.u32(static_cast<uint32_t>(v.size()));
    for (const ObjectHandle h : v) put_handle(w, st, h);
}

bool get_handles(ByteReader& r, const objects::ObjectStore& st, std::vector<ObjectHandle>& out) {
    std::vector<ObjectHandle> v(save::count(r, 12));
    for (ObjectHandle& h : v) h = get_handle(r, st);
    if (!r.ok()) return false;
    out = std::move(v);
    return true;
}

// ---- Lua VMs ----
// The VM's LuaVm::write image, length-prefixed; an empty image without Lua state (set_save_lua_state(false), tests).
void put_vm(ByteWriter& w, bool lua_state, const lua::LuaVm& vm) {
    ByteWriter img;
    if (lua_state) vm.write(img);
    w.u32(static_cast<uint32_t>(img.size()));
    w.raw(img.bytes());
}

std::span<const uint8_t> get_vm_image(ByteReader& r) {
    const uint32_t n = r.u32();
    if (!r.ok() || n > r.remaining()) {
        r.fail();
        return {};
    }
    return r.raw(n);
}

bool read_vm_image(std::span<const uint8_t> img, lua::LuaVm& vm) {
    ByteReader sub(img);
    return vm.read(sub) && sub.ok() && sub.done();
}

// ---- GlobalValue ----
void put_value(ByteWriter& w, const lua::AiGlobalValue& v) {
    w.u8(static_cast<uint8_t>(v.kind));
    w.f64(v.number);
    w.str(v.text);
    if (v.kind == lua::AiGlobalValue::Table) {
        w.u32(static_cast<uint32_t>(v.keys.size()));
        for (size_t i = 0; i < v.keys.size(); ++i) {
            put_value(w, v.keys[i]);
            put_value(w, i < v.values.size() ? v.values[i] : lua::AiGlobalValue{});
        }
    }
}

bool get_value(ByteReader& r, lua::AiGlobalValue& v, int depth) {
    const uint8_t kind = r.u8();
    if (kind > lua::AiGlobalValue::Table || depth > kMaxValueDepth) r.fail();
    v.kind = static_cast<lua::AiGlobalValue::Kind>(kind);
    v.number = r.f64();
    v.text = r.str();
    v.keys.clear();
    v.values.clear();
    if (r.ok() && v.kind == lua::AiGlobalValue::Table) {
        const uint32_t n = save::count(r, 26);
        v.keys.resize(n);
        v.values.resize(n);
        for (uint32_t i = 0; i < n && r.ok(); ++i)
            if (!get_value(r, v.keys[i], depth + 1) || !get_value(r, v.values[i], depth + 1)) return false;
    }
    return r.ok();
}

// ---- the AI's LuaPlayerWorld ----
struct WorldState {
    bool ai_suspended = false;
    std::vector<lua::LuaPlayerState> states;
    std::vector<int32_t> purge;
    std::vector<lua::DesireBonusRequest> bonuses;
    std::vector<lua::TacticalCreditRelease> releases;
    std::vector<lua::BudgetFlushRequest> flushes;
};

void put_world(ByteWriter& w, const lua::LuaPlayerWorld& world) {
    w.boolean(world.ai_suspended());
    w.u32(static_cast<uint32_t>(world.states().size()));
    for (const lua::LuaPlayerState& s : world.states()) {
        const lua::LuaPlayerSetup& u = s.setup;
        w.str(u.faction);
        w.boolean(u.human);
        w.boolean(u.ai);
        w.boolean(u.neutral);
        w.str(u.difficulty);
        w.f32(u.credit_multiplier);
        w.f32(u.credits);
        w.f32(u.credit_cap);
        w.i32(u.tech_level);
        w.i32(u.team);
        w.u32(static_cast<uint32_t>(s.tech_override.size()));
        for (const auto& [t, locked] : s.tech_override) {
            w.u32(t);
            w.boolean(locked);
        }
        w.boolean(s.actor);
        w.i32(s.selected);
        w.i32(s.retreat_requests);
        w.boolean(s.bombing_run_disabled);
        w.boolean(s.orbital_bombardment_disabled);
        w.boolean(s.orbital_bombardment_removed);
        w.boolean(s.black_market_tutorial);
        w.boolean(s.sabotage_tutorial);
        w.u32(static_cast<uint32_t>(s.advisor_hints.size()));
        for (const auto& [name, on] : s.advisor_hints) {
            w.str(name);
            w.boolean(on);
        }
    }
    put_vec(w, world.purge_goal_requests);
    w.u32(static_cast<uint32_t>(world.desire_bonuses.size()));
    for (const lua::DesireBonusRequest& b : world.desire_bonuses) {
        w.i32(b.player);
        w.str(b.goal);
        w.i32(b.target);
        w.f64(b.bonus);
        w.f64(b.duration);
    }
    w.u32(static_cast<uint32_t>(world.tactical_releases.size()));
    for (const lua::TacticalCreditRelease& t : world.tactical_releases) {
        w.i32(t.player);
        w.f64(t.amount);
    }
    w.u32(static_cast<uint32_t>(world.budget_flushes.size()));
    for (const lua::BudgetFlushRequest& f : world.budget_flushes) {
        w.i32(f.player);
        w.str(f.category);
    }
}

bool get_world(ByteReader& r, WorldState& out) {
    WorldState s;
    s.ai_suspended = r.boolean();
    s.states.resize(save::count(r, 40));
    for (lua::LuaPlayerState& p : s.states) {
        lua::LuaPlayerSetup& u = p.setup;
        u.faction = r.str();
        u.human = r.boolean();
        u.ai = r.boolean();
        u.neutral = r.boolean();
        u.difficulty = r.str();
        u.credit_multiplier = r.f32();
        u.credits = r.f32();
        u.credit_cap = r.f32();
        u.tech_level = r.i32();
        u.team = r.i32();
        const uint32_t nt = save::count(r, 5);
        for (uint32_t i = 0; i < nt && r.ok(); ++i) {
            const uint32_t t = r.u32();
            const bool locked = r.boolean();
            if (!p.tech_override.empty() && p.tech_override.rbegin()->first >= t) r.fail();
            p.tech_override.emplace_hint(p.tech_override.end(), t, locked);
        }
        p.actor = r.boolean();
        p.selected = r.i32();
        p.retreat_requests = r.i32();
        p.bombing_run_disabled = r.boolean();
        p.orbital_bombardment_disabled = r.boolean();
        p.orbital_bombardment_removed = r.boolean();
        p.black_market_tutorial = r.boolean();
        p.sabotage_tutorial = r.boolean();
        const uint32_t nh = save::count(r, 5);
        for (uint32_t i = 0; i < nh && r.ok(); ++i) {
            std::string name = r.str();
            const bool on = r.boolean();
            if (!p.advisor_hints.empty() && !(p.advisor_hints.rbegin()->first < name)) r.fail();
            p.advisor_hints.emplace_hint(p.advisor_hints.end(), std::move(name), on);
        }
    }
    if (!r.ok() || !get_vec(r, s.purge)) return false;
    s.bonuses.resize(save::count(r, 28));
    for (lua::DesireBonusRequest& b : s.bonuses) {
        b.player = r.i32();
        b.goal = r.str();
        b.target = r.i32();
        b.bonus = r.f64();
        b.duration = r.f64();
    }
    s.releases.resize(save::count(r, 12));
    for (lua::TacticalCreditRelease& t : s.releases) {
        t.player = r.i32();
        t.amount = r.f64();
    }
    s.flushes.resize(save::count(r, 8));
    for (lua::BudgetFlushRequest& f : s.flushes) {
        f.player = r.i32();
        f.category = r.str();
    }
    if (!r.ok()) return false;
    out = std::move(s);
    return true;
}

// ---- per AI player ----
struct StagedPlayer {
    AiContext ctx{AiMode::Galactic};
    std::set<objects::ObjectId> known;
    std::vector<PendingBonus> bonuses;
    int64_t reach_frame = -1;
    std::vector<std::vector<uint8_t>> reach;
    AiPathFinder paths;
    std::array<PlayerAi::SourceList, 6> sources;
    bool has_fs = false;
    std::unique_ptr<lua::LuaVm> fs_vm;  // null with has_fs: keep the current VM (tests without Lua state)
};

bool valid_index(int32_t v, size_t n) { return v == -1 || (v >= 0 && static_cast<size_t>(v) < n); }
bool valid_planet_or_none(PlanetIndex p, size_t planets) { return p == kInvalidPlanet || p < planets; }
}  // namespace

// ================================================================ GalacticSimTokenResolver

void GalacticSimTokenResolver::write(ByteWriter& w) const {
    w.u32(kResolverVersion);
    w.i32(frame_);
    w.u32(static_cast<uint32_t>(stamp_space_.size()));
    for (const auto* v : {&stamp_space_, &stamp_ground_, &stamp_conversion_, &stamp_corruption_, &last_owner_})
        for (const int32_t x : *v) w.i32(x);
    for (const int8_t x : last_corruption_) w.u8(static_cast<uint8_t>(x));
    for (const uint8_t x : last_battle_) w.u8(x);
    std::lock_guard<std::mutex> lock(mu_);
    w.u32(static_cast<uint32_t>(cache_.size()));
    for (const auto& [k, e] : cache_) {  // version 3 entry layout
        w.str(k.leaf);
        w.i32(k.inst);
        w.i32(k.self);
        w.str(k.params);
        w.f64(e.value);
        w.i32(e.stamp);
        w.u32(static_cast<uint32_t>(e.params.size()));
        for (const Param& p : e.params) put_param(w, p);
    }
    w.u64(hits_);
    // version 2 (q6 t03)
    w.i32(logic_frame_);
    for (const std::vector<int32_t>& s : player_stamps_) {
        w.u32(static_cast<uint32_t>(s.size()));
        for (const int32_t x : s) w.i32(x);
    }
    w.u32(conflict_log_seen_);
    w.boolean(active_.valid);
    w.u8(active_.kind);
    w.u32(active_.planet);
    w.i32(active_.attacker);
    w.i32(active_.defender);
    w.u8(static_cast<uint8_t>((active_.linked ? 1u : 0u) | (active_.space_stamped ? 2u : 0u) | (active_.ground_stamped ? 4u : 0u) |
                              (active_.in_battle ? 8u : 0u)));
    // version 3 (q6 t18)
    w.u32(static_cast<uint32_t>(visible_.size()));
    for (const std::vector<uint32_t>& row : visible_) {
        w.u32(static_cast<uint32_t>(row.size()));
        for (const uint32_t x : row) w.u32(x);
    }
    w.u32(static_cast<uint32_t>(last_surface_.size()));
    for (const std::vector<int32_t>& ids : last_surface_) {
        w.u32(static_cast<uint32_t>(ids.size()));
        for (const int32_t id : ids) w.i32(id);
    }
}

bool GalacticSimTokenResolver::read(ByteReader& r) {
    const uint32_t version = r.u32();
    if (!r.ok() || version != kResolverVersion) return false;
    const int32_t frame = r.i32();
    const uint32_t n = save::count(r, 22);
    if (!r.ok() || (n != 0 && n != sim_->planets().size())) return false;
    std::array<std::vector<int32_t>, 5> v;
    for (std::vector<int32_t>& x : v) {
        x.resize(n);
        for (int32_t& e : x) e = r.i32();
    }
    std::vector<int8_t> corruption(n);
    for (int8_t& e : corruption) e = static_cast<int8_t>(r.u8());
    std::vector<uint8_t> battle(n);
    for (uint8_t& e : battle) {
        e = r.u8();
        if (e > 1) r.fail();
    }
    std::map<CacheKey, Entry> cache;
    const uint32_t nc = save::count(r, 32);
    for (uint32_t i = 0; i < nc && r.ok(); ++i) {
        CacheKey k;
        k.leaf = r.str();
        k.inst = r.i32();
        k.self = r.i32();
        k.params = r.str();
        Entry e;
        e.value = r.f64();
        e.stamp = r.i32();
        e.params.resize(save::count(r, 18));
        for (Param& p : e.params)
            if (!get_param(r, p)) break;
        if (!cache.empty() && !(cache.rbegin()->first < k)) r.fail();
        cache.emplace_hint(cache.end(), std::move(k), std::move(e));
    }
    const uint64_t hits = r.u64();
    const int32_t logic_frame = r.i32();
    std::array<std::vector<int32_t>, 4> stamps;
    for (std::vector<int32_t>& s : stamps) {
        s.resize(save::count(r, 4));
        for (int32_t& x : s) {
            x = r.i32();
            if (x < -1) r.fail();
        }
    }
    const uint32_t log_seen = r.u32();
    ObservedConflict active;
    active.valid = r.boolean();
    active.kind = r.u8();
    active.planet = r.u32();
    active.attacker = r.i32();
    active.defender = r.i32();
    const uint8_t flags = r.u8();
    if (flags > 15) r.fail();
    active.linked = (flags & 1u) != 0;
    active.space_stamped = (flags & 2u) != 0;
    active.ground_stamped = (flags & 4u) != 0;
    active.in_battle = (flags & 8u) != 0;
    std::vector<std::vector<uint32_t>> visible(save::count(r, 4));
    for (std::vector<uint32_t>& row : visible) {
        row.resize(save::count(r, 4));
        for (uint32_t& x : row) x = r.u32();
    }
    std::vector<std::vector<int32_t>> surface(save::count(r, 4));
    if (!surface.empty() && surface.size() != n) r.fail();
    for (std::vector<int32_t>& ids : surface) {
        ids.resize(save::count(r, 4));
        for (int32_t& id : ids) id = r.i32();
        if (!std::is_sorted(ids.begin(), ids.end())) r.fail();
    }
    if (!r.ok() || !r.done()) return false;
    visible_ = std::move(visible);
    last_surface_ = std::move(surface);
    frame_ = frame;
    logic_frame_ = logic_frame;
    player_stamps_ = std::move(stamps);
    conflict_log_seen_ = log_seen;
    active_ = active;
    stamp_space_ = std::move(v[0]);
    stamp_ground_ = std::move(v[1]);
    stamp_conversion_ = std::move(v[2]);
    stamp_corruption_ = std::move(v[3]);
    last_owner_ = std::move(v[4]);
    last_corruption_ = std::move(corruption);
    last_battle_ = std::move(battle);
    if (n == 0) neighbours_.clear();  // never refreshed: the next refresh initialises everything
    else build_neighbours();
    std::lock_guard<std::mutex> lock(mu_);
    cache_ = std::move(cache);
    hits_ = hits;
    return true;
}

// ================================================================ GalacticAiWorld

void GalacticAiWorld::Impl::rebind_vm(lua::LuaVm& vm, int32_t script_id, bool with_markup) {
    // Every AI VM is prepared with this world's three host pointers (registry light userdata, not Lua state). A
    // LuaVm::read that keeps the blank VM's attachments needs nothing here; one that drops them gets them back. Only
    // pointers that differ are written: attach_ai_host also re-installs the GlobalValue object and rewrites the
    // script id, which a restored VM already has.
    if (lua::player_world(vm) != &world) lua::attach_player_world(vm, &world);
    if (lua::ai_host(vm) != static_cast<lua::AiScriptHost*>(this)) lua::attach_ai_host(vm, this, script_id);
    lua::ScriptMarkupHost* const markup_want = with_markup ? static_cast<lua::ScriptMarkupHost*>(this) : nullptr;
    if (lua::markup_host(vm) != markup_want) lua::attach_markup_host(vm, markup_want);
}

void GalacticAiWorld::set_save_lua_state(bool on) { impl_->lua_state = on; }

void GalacticAiWorld::write(ByteWriter& w) const {
    const Impl& m = *impl_;
    const objects::ObjectStore& st = m.store;
    w.u32(kWorldVersion);
    // ---- shape ----
    w.u32(static_cast<uint32_t>(m.ai.size()));
    for (const auto& a : m.ai) w.boolean(a != nullptr);
    for (const auto& a : m.ai)  // q6 t07: types may differ from the session's start (set_ai_type)
        if (a) {
            w.str(a->type->name);
            w.i32(a->difficulty);
        }
    w.u32(static_cast<uint32_t>(m.library.plans.size()));
    w.u32(static_cast<uint32_t>(m.sim.planets().size()));
    // ---- counters ----
    w.i64(m.now);
    w.i64(m.perception_frame);
    w.i32(m.market_sim_frame);
    w.i32(m.current_plan);
    for (const int32_t c : {m.next_plan, m.next_tf, m.next_status, m.next_pp, m.next_build, m.next_queue_id}) w.i32(c);
    w.u64(m.orders);
    w.u64(m.rejected);
    m.gate.write(w);
    // ---- perception ----
    m.snap.write(w);
    m.perc.write(w);
    {
        ByteWriter rw;
        m.res.write(rw);
        w.u32(static_cast<uint32_t>(rw.size()));
        w.raw(rw.bytes());
    }
    w.u32(static_cast<uint32_t>(m.markup.size()));
    for (const auto& [k, v] : m.markup) {
        w.i32(k.first);
        w.i32(k.second);
        w.f64(v);
    }
    // ---- the AI's Lua world + GlobalValue map ----
    put_world(w, m.world);
    w.u32(static_cast<uint32_t>(m.globals.size()));
    for (const auto& [k, v] : m.globals) {
        w.str(k);
        put_value(w, v);
    }
    // ---- AI players ----
    for (const auto& a : m.ai) {
        if (!a) continue;
        const PlayerAi& p = *a;
        p.ctx.write(w);
        save::put_set(w, p.known);
        w.u32(static_cast<uint32_t>(p.bonuses.size()));
        for (const PendingBonus& b : p.bonuses) {
            w.u32(b.goal_type);
            w.i32(b.target);
            w.f32(b.bonus);
            w.i64(b.deadline);
        }
        w.i64(p.reach_frame);
        w.u32(static_cast<uint32_t>(p.reach.size()));
        for (const std::vector<uint8_t>& v : p.reach) put_vec(w, v);
        p.paths.write(w);
        for (const PlayerAi::SourceList& s : p.sources) {
            w.i64(s.frame);
            put_vec(w, s.planets);
        }
        w.boolean(p.fs_vm != nullptr);
        if (p.fs_vm) put_vm(w, m.lua_state, *p.fs_vm);
    }
    // ---- plans ----
    w.u32(static_cast<uint32_t>(m.plans.size()));
    for (const auto& [id, pi] : m.plans) {
        w.i32(id);
        w.i32(pi.player);
        w.u32(pi.plan);
        w.u32(pi.goal_type);
        w.u32(pi.goal_key);
        w.i32(pi.category);
        w.i32(pi.target);
        w.i32(pi.target_owner);
        w.i32(pi.pp);
        put_vec(w, pi.tfs);
        w.boolean(pi.result);
        w.boolean(pi.non_removable);
        w.boolean(pi.test_contrast);
        w.boolean(pi.blocking);
        w.boolean(pi.ended);
        w.u32(pi.live_threads);
        w.boolean(pi.vm != nullptr);
        if (pi.vm) put_vm(w, m.lua_state, *pi.vm);
    }
    // ---- task forces ----
    w.u32(static_cast<uint32_t>(m.tfs.size()));
    for (const auto& [id, t] : m.tfs) {
        w.i32(id);
        w.i32(t.plan);
        w.i32(t.player);
        w.u32(t.def);
        w.str(t.name);
        put_handles(w, st, t.units);
        put_vec(w, t.wanted);
        put_vec(w, t.wanted_source);
        put_vec(w, t.builds);
        w.i32(t.stage);
        w.boolean(t.had_units);
        w.boolean(t.no_units_sent);
        w.boolean(t.alive);
    }
    // ---- blocking statuses ----
    w.u32(static_cast<uint32_t>(m.statuses.size()));
    for (const auto& [id, s] : m.statuses) {
        w.i32(id);
        w.i32(s.plan);
        w.i32(s.tf);
        w.u8(static_cast<uint8_t>(s.kind));
        w.boolean(s.finished);
        w.u8(static_cast<uint8_t>(s.result));
        w.u32(s.dest);
        w.u8(s.mode);
        w.i64(s.started);
        w.u8(static_cast<uint8_t>(s.op));
        w.f64(s.amount);
    }
    // ---- PotentialPlans ----
    w.u32(static_cast<uint32_t>(m.pps.size()));
    for (const auto& [id, pp] : m.pps) {
        w.i32(id);
        w.i32(pp.player);
        w.u32(pp.plan);
        w.u32(pp.goal_type);
        w.i32(pp.target);
        w.u32(static_cast<uint32_t>(pp.sel.size()));
        for (const PpSelection& s : pp.sel) {
            w.u32(s.type);
            put_handle(w, st, s.unit);
            w.i32(s.tf);
            w.u32(s.source);
            w.u8(s.queue);
            w.f32(s.btime);
        }
        w.f32(pp.gross);
        w.f32(pp.net);
        w.f32(pp.eta);
        w.boolean(pp.contrast_ok);
        w.boolean(pp.queues_reserved);
        w.f64(pp.pop);
        put_vec(w, pp.reserved_units);
    }
    // ---- build tasks + market ----
    w.u32(static_cast<uint32_t>(m.builds.size()));
    for (const auto& [id, b] : m.builds) {
        w.i32(id);
        w.i32(b.plan);
        w.i32(b.tf);
        w.i32(b.player);
        w.u32(b.type);
        w.u32(b.dest);
        w.u8(b.queue);
        w.u8(static_cast<uint8_t>(b.state));
        w.boolean(b.bound);
        w.u32(b.bid_planet);
        w.f64(b.bid);
        w.i64(b.created);
        w.i64(b.next_poll);
        w.boolean(b.charged);
        w.f32(b.charge);
        w.i32(b.queue_id);
        w.u32(b.producing_at);
        w.i32(b.queue_entries_before);
        w.u32(b.goal_key);
        w.i32(b.category);
        w.boolean(b.listed);
        w.boolean(b.reclaimed);
    }
    put_vec(w, m.pending_builds);
    w.u32(static_cast<uint32_t>(m.planet_queues.size()));
    for (const auto& pq : m.planet_queues)
        for (const PlanetBuildQueue& q : pq) {
            w.i64(q.last_run);
            put_vec(w, q.tasks);
            w.i32(q.current);
            w.i32(q.current_queue_id);
            w.u32(q.current_type);
            w.u32(static_cast<uint32_t>(q.reservations.size()));
            for (const QueueReservation& res : q.reservations) {
                w.i32(res.pp);
                w.u32(res.type);
                w.f32(res.time);
            }
        }
    w.i64(m.delivery_frame);
    w.u32(static_cast<uint32_t>(m.delivery_reach.size()));
    for (const auto& [k, v] : m.delivery_reach) {
        w.i32(std::get<0>(k));
        w.u32(std::get<1>(k));
        w.i32(static_cast<int32_t>(std::get<2>(k)));
        put_vec(w, v);
    }
    w.u32(static_cast<uint32_t>(m.unit_reserved.size()));
    for (const auto& [u, pp] : m.unit_reserved) {
        w.i32(u);
        w.i32(pp);
    }
    w.u32(static_cast<uint32_t>(m.fs_moves.size()));
    for (const FreeStoreMove& mv : m.fs_moves) {
        put_handle(w, st, mv.unit);
        w.i32(mv.player);
        w.u32(mv.dest);
        w.boolean(mv.done);
    }
    w.u32(static_cast<uint32_t>(m.reclaim.size()));
    for (const auto& [serial, amount] : m.reclaim) {
        w.u32(serial);
        w.f32(amount);
    }
    // ---- Lua runtime: pooled plan VMs, evaluators ----
    const PlanScriptRuntime& rt = *m.runtime;
    w.u32(static_cast<uint32_t>(rt.plan_count()));
    for (uint32_t p = 0; p < rt.plan_count(); ++p) {
        const auto& pool = rt.pool(p);
        w.u32(static_cast<uint32_t>(pool.size()));
        for (const auto& vm : pool) {
            const bool pristine = m.lua_state && vm.get() == rt.pristine(p);
            w.boolean(pristine);
            if (pristine) {
                w.u64(rt.pristine_digest(p));
                continue;
            }
            w.boolean(lua::markup_host(*vm) != nullptr);
            put_vm(w, m.lua_state, *vm);
        }
    }
    w.u64(static_cast<uint64_t>(rt.vms_created()));
    const auto& evals = m.scripts.loaded_vms();
    w.u32(static_cast<uint32_t>(evals.size()));
    for (const auto& [key, e] : evals) {
        w.str(key);
        w.str(e.name);
        put_vm(w, m.lua_state, *e.vm);
    }
    save::put_set(w, m.scripts.missing());
}

bool GalacticAiWorld::read(ByteReader& r) {
    Impl& m = *impl_;
    const objects::ObjectStore& st = m.store;
    const size_t planets = m.sim.planets().size();
    const size_t goal_types = m.defs.goals.size();
    const size_t categories = m.defs.categories.size();
    const size_t lib = m.library.plans.size();
    // VMs staged with fresh() / make_vm() (no Lua state restore) count as created; a failed read undoes that.
    struct CreatedGuard {
        PlanScriptRuntime& rt;
        size_t before;
        bool committed = false;
        ~CreatedGuard() {
            if (!committed) rt.set_vms_created(before);
        }
    } guard{*m.runtime, m.runtime->vms_created()};
    if (r.u32() != kWorldVersion || !r.ok()) return false;
    // ---- shape: the loader rebuilt the same session ----
    if (r.u32() != m.ai.size() || !r.ok()) return false;
    std::vector<uint8_t> present(m.ai.size());
    for (uint8_t& b : present) b = r.boolean() ? 1 : 0;
    if (!r.ok()) return false;
    // q6 t07: the saved AI players' types. A player whose type / difficulty differs from this world's (set_ai_type ran
    // in the saved session) gets a fresh staged AI player of the saved type, built as set_ai_type builds it.
    std::vector<GalacticAiPlayerConfig> saved_cfg;
    for (size_t i = 0; i < present.size(); ++i) {
        if (!present[i]) continue;
        GalacticAiPlayerConfig pc;
        pc.player = static_cast<int32_t>(i);
        pc.ai_type = r.str();
        pc.difficulty = r.i32();
        if (!r.ok() || !m.defs.find_player(pc.ai_type) || pc.difficulty < 0 || pc.difficulty > 2) return false;
        saved_cfg.push_back(std::move(pc));
    }
    if (r.u32() != lib || r.u32() != planets || !r.ok()) return false;
    std::vector<std::unique_ptr<PlayerAi>> fresh(m.ai.size());
    {
        std::vector<GalacticAiPlayerConfig> live_cfg = std::move(m.cfg.players);
        m.cfg.players = saved_cfg;  // make_player's stagger ordinal reads the saved player list
        for (const GalacticAiPlayerConfig& pc : saved_cfg) {
            const PlayerAi* cur = m.player_ai(pc.player);
            const AiPlayerTypeDef* type = m.defs.find_player(pc.ai_type);
            if (!cur || cur->type != type || cur->difficulty != pc.difficulty)
                fresh[static_cast<size_t>(pc.player)] = m.make_player(pc.player, type, pc.difficulty, nullptr);
        }
        m.cfg.players = std::move(live_cfg);
    }
    auto staged_ai = [&m, &present, &fresh](size_t i) -> PlayerAi* {
        if (i >= present.size() || !present[i]) return nullptr;
        return fresh[i] ? fresh[i].get() : m.ai[i].get();
    };
    auto valid_ai = [&staged_ai](int32_t p) { return p >= 0 && staged_ai(static_cast<size_t>(p)) != nullptr; };
    // ---- counters ----
    const int64_t now = r.i64(), perception_frame = r.i64();
    const int32_t market_sim_frame = r.i32(), current_plan = r.i32();
    std::array<int32_t, 6> next{};
    for (int32_t& c : next) c = r.i32();
    const uint64_t orders = r.u64(), rejected = r.u64();
    AiEnableGate gate = m.gate;
    if (!r.ok() || !gate.read(r)) return false;
    // ---- perception ----
    GalacticSnapshot snap;
    GalacticPerception perc;
    if (!snap.read(r) || !perc.read(r)) return false;
    if (perception_frame >= 0 && (snap.planet_count != planets || perc.memory_count() < snap.player_count())) return false;
    const std::span<const uint8_t> res_image = get_vm_image(r);  // length-prefixed; read in place after validation
    std::map<std::pair<int32_t, int32_t>, double> markup;
    {
        const uint32_t n = save::count(r, 16);
        for (uint32_t i = 0; i < n && r.ok(); ++i) {
            std::pair<int32_t, int32_t> k;
            k.first = r.i32();
            k.second = r.i32();
            const double v = r.f64();
            if (!markup.empty() && !(markup.rbegin()->first < k)) r.fail();
            markup.emplace_hint(markup.end(), k, v);
        }
    }
    // ---- Lua world + GlobalValue map ----
    WorldState world;
    if (!r.ok() || !get_world(r, world) || world.states.size() != m.world.states().size()) return false;
    std::map<std::string, lua::AiGlobalValue> globals;
    {
        const uint32_t n = save::count(r, 17);
        for (uint32_t i = 0; i < n && r.ok(); ++i) {
            std::string k = r.str();
            lua::AiGlobalValue v;
            if (!get_value(r, v, 0)) return false;
            if (!globals.empty() && !(globals.rbegin()->first < k)) r.fail();
            globals.emplace_hint(globals.end(), std::move(k), std::move(v));
        }
    }
    if (!r.ok()) return false;
    // ---- AI players ----
    std::vector<StagedPlayer> players(m.ai.size());
    for (size_t i = 0; i < m.ai.size(); ++i) {
        if (!staged_ai(i)) continue;
        PlayerAi& pa = *staged_ai(i);
        StagedPlayer& sp = players[i];
        sp.ctx = pa.ctx;
        if (!sp.ctx.read(r)) return false;
        for (const auto* v : {&sp.ctx.goals.active(), &sp.ctx.goals.proposed()})
            for (const auto& cat : *v)
                for (const Goal& g : cat)
                    if (!valid_index(g.target, planets) || (g.plan != kAiNone && g.plan >= lib)) return false;
        if (!save::get_set(r, sp.known)) return false;
        sp.bonuses.resize(save::count(r, 20));
        for (PendingBonus& b : sp.bonuses) {
            b.goal_type = r.u32();
            b.target = r.i32();
            b.bonus = r.f32();
            b.deadline = r.i64();
            if (b.goal_type >= goal_types) r.fail();
        }
        sp.reach_frame = r.i64();
        sp.reach.resize(save::count(r, 4));
        if (!sp.reach.empty() && sp.reach.size() != 6) r.fail();
        for (std::vector<uint8_t>& v : sp.reach)
            if (!get_vec(r, v)) return false;
        if (!r.ok() || !sp.paths.read(r, m.sim.graph(), m.cfg.path_pf)) return false;
        for (PlayerAi::SourceList& s : sp.sources) {
            s.frame = r.i64();
            if (!get_vec(r, s.planets)) return false;
        }
        sp.has_fs = r.boolean();
        if (!r.ok()) return false;
        if (sp.has_fs) {
            const std::span<const uint8_t> img = get_vm_image(r);
            if (!r.ok()) return false;
            if (m.lua_state) {
                const std::string& fs = pa.type->free_store_script[static_cast<size_t>(AiMode::Galactic)];
                sp.fs_vm = m.runtime->blank_vm(fs);
                if (!read_vm_image(img, *sp.fs_vm)) return false;
                m.rebind_vm(*sp.fs_vm, kFreeStoreScriptBase + pa.player, true);
            } else if (!pa.fs_vm) {
                return false;  // tests without Lua state keep the VM start() made
            }
        }
    }
    // ---- plans ----
    std::map<int32_t, PlanInstance> plans;
    {
        const uint32_t n = save::count(r, 50);
        for (uint32_t i = 0; i < n && r.ok(); ++i) {
            PlanInstance pi;
            pi.id = r.i32();
            pi.player = r.i32();
            pi.plan = r.u32();
            pi.goal_type = r.u32();
            pi.goal_key = r.u32();
            pi.category = r.i32();
            pi.target = r.i32();
            pi.target_owner = r.i32();
            pi.pp = r.i32();
            if (!get_vec(r, pi.tfs)) return false;
            pi.result = r.boolean();
            pi.non_removable = r.boolean();
            pi.test_contrast = r.boolean();
            pi.blocking = r.boolean();
            pi.ended = r.boolean();
            pi.live_threads = r.u32();
            const bool has_vm = r.boolean();
            if (!r.ok() || !valid_ai(pi.player) || pi.plan >= lib || pi.goal_type >= goal_types ||
                !valid_index(pi.category, categories) || !valid_index(pi.target, planets) ||
                (!plans.empty() && plans.rbegin()->first >= pi.id))
                return false;
            if (has_vm) {
                const std::span<const uint8_t> img = get_vm_image(r);
                if (!r.ok()) return false;
                if (m.lua_state) {
                    pi.vm = m.runtime->blank_vm(m.runtime->plan_name(pi.plan));
                    if (!read_vm_image(img, *pi.vm)) return false;
                } else {  // tests without Lua state: a new instance named as instantiate() names it
                    pi.vm = m.runtime->fresh(pi.plan);
                    if (!pi.vm) return false;
                    lua::attach_ai_host(*pi.vm, &m, pi.id);
                }
                m.rebind_vm(*pi.vm, pi.id, true);
            }
            const int32_t id = pi.id;
            plans.emplace_hint(plans.end(), id, std::move(pi));
        }
    }
    // ---- task forces ----
    std::map<int32_t, TaskForceRec> tfs;
    {
        const uint32_t n = save::count(r, 40);
        for (uint32_t i = 0; i < n && r.ok(); ++i) {
            TaskForceRec t;
            t.id = r.i32();
            t.plan = r.i32();
            t.player = r.i32();
            t.def = r.u32();
            t.name = r.str();
            if (!get_handles(r, st, t.units) || !get_vec(r, t.wanted) || !get_vec(r, t.wanted_source) || !get_vec(r, t.builds))
                return false;
            t.stage = r.i32();
            t.had_units = r.boolean();
            t.no_units_sent = r.boolean();
            t.alive = r.boolean();
            if (!r.ok() || !valid_ai(t.player) || (!tfs.empty() && tfs.rbegin()->first >= t.id)) return false;
            if (const auto pit = plans.find(t.plan); pit != plans.end() && t.def >= m.library.plans[pit->second.plan].task_forces.size())
                return false;
            const int32_t id = t.id;
            tfs.emplace_hint(tfs.end(), id, std::move(t));
        }
    }
    // ---- blocking statuses ----
    std::map<int32_t, StatusRec> statuses;
    {
        const uint32_t n = save::count(r, 36);
        for (uint32_t i = 0; i < n && r.ok(); ++i) {
            StatusRec s;
            s.id = r.i32();
            s.plan = r.i32();
            s.tf = r.i32();
            const uint8_t kind = r.u8();
            s.finished = r.boolean();
            const uint8_t result = r.u8();
            s.dest = r.u32();
            s.mode = r.u8();
            s.started = r.i64();
            const uint8_t op = r.u8();
            s.amount = r.f64();
            if (kind > static_cast<uint8_t>(StatusKind::BudgetWait) || result > static_cast<uint8_t>(lua::AiStatusResult::False) ||
                op > static_cast<uint8_t>(lua::AiBudgetOp::WaitUnallocated) || s.mode > 5 || !valid_planet_or_none(s.dest, planets))
                r.fail();
            s.kind = static_cast<StatusKind>(kind);
            s.result = static_cast<lua::AiStatusResult>(result);
            s.op = static_cast<lua::AiBudgetOp>(op);
            if (!r.ok() || (s.kind == StatusKind::MoveTo && s.dest >= planets) || (!statuses.empty() && statuses.rbegin()->first >= s.id))
                return false;
            const int32_t id = s.id;
            statuses.emplace_hint(statuses.end(), id, s);
        }
    }
    // ---- PotentialPlans ----
    std::map<int32_t, PotentialPlanRec> pps;
    {
        const uint32_t n = save::count(r, 48);
        for (uint32_t i = 0; i < n && r.ok(); ++i) {
            const int32_t id = r.i32();
            PotentialPlanRec pp;
            pp.player = r.i32();
            pp.plan = r.u32();
            pp.goal_type = r.u32();
            pp.target = r.i32();
            pp.sel.resize(save::count(r, 29));
            for (PpSelection& s : pp.sel) {
                s.type = r.u32();
                s.unit = get_handle(r, st);
                s.tf = r.i32();
                s.source = r.u32();
                s.queue = r.u8();
                s.btime = r.f32();
                if (s.queue > 1 || !valid_planet_or_none(s.source, planets)) r.fail();
            }
            pp.gross = r.f32();
            pp.net = r.f32();
            pp.eta = r.f32();
            pp.contrast_ok = r.boolean();
            pp.queues_reserved = r.boolean();
            pp.pop = r.f64();
            if (!get_vec(r, pp.reserved_units)) return false;
            if (!r.ok() || (pp.plan != kAiNone && pp.plan >= lib) || (pp.goal_type != kAiNone && pp.goal_type >= goal_types) ||
                !valid_index(pp.target, planets) || (!pps.empty() && pps.rbegin()->first >= id))
                return false;
            pps.emplace_hint(pps.end(), id, std::move(pp));
        }
    }
    // ---- build tasks + market ----
    std::map<int32_t, BuildTask> builds;
    {
        const uint32_t n = save::count(r, 71);
        for (uint32_t i = 0; i < n && r.ok(); ++i) {
            BuildTask b;
            b.id = r.i32();
            b.plan = r.i32();
            b.tf = r.i32();
            b.player = r.i32();
            b.type = r.u32();
            b.dest = r.u32();
            b.queue = r.u8();
            const uint8_t state = r.u8();
            b.bound = r.boolean();
            b.bid_planet = r.u32();
            b.bid = r.f64();
            b.created = r.i64();
            b.next_poll = r.i64();
            b.charged = r.boolean();
            b.charge = r.f32();
            b.queue_id = r.i32();
            b.producing_at = r.u32();
            b.queue_entries_before = r.i32();
            b.goal_key = r.u32();
            b.category = r.i32();
            b.listed = r.boolean();
            b.reclaimed = r.boolean();
            if (state > static_cast<uint8_t>(BuildState::Failed)) r.fail();
            b.state = static_cast<BuildState>(state);
            if (!r.ok() || !valid_ai(b.player) || b.queue > 1 || !valid_planet_or_none(b.dest, planets) ||
                !valid_planet_or_none(b.bid_planet, planets) || !valid_planet_or_none(b.producing_at, planets) ||
                (b.state == BuildState::Producing && b.producing_at >= planets) || !valid_index(b.category, categories) ||
                (!builds.empty() && builds.rbegin()->first >= b.id))
                return false;
            const int32_t id = b.id;
            builds.emplace_hint(builds.end(), id, b);
        }
    }
    std::vector<int32_t> pending_builds;
    if (!get_vec(r, pending_builds)) return false;
    std::vector<std::array<PlanetBuildQueue, 2>> planet_queues(save::count(r, 2 * 28));
    if (!r.ok() || planet_queues.size() != planets) return false;
    for (auto& pq : planet_queues)
        for (PlanetBuildQueue& q : pq) {
            q.last_run = r.i64();
            if (!get_vec(r, q.tasks)) return false;
            q.current = r.i32();
            q.current_queue_id = r.i32();
            q.current_type = r.u32();
            q.reservations.resize(save::count(r, 12));
            for (QueueReservation& res : q.reservations) {
                res.pp = r.i32();
                res.type = r.u32();
                res.time = r.f32();
            }
        }
    const int64_t delivery_frame = r.i64();
    std::map<std::tuple<int32_t, PlanetIndex, int>, std::vector<uint8_t>> delivery_reach;
    {
        const uint32_t n = save::count(r, 16);
        for (uint32_t i = 0; i < n && r.ok(); ++i) {
            const int32_t owner = r.i32();
            const PlanetIndex source = r.u32();
            const int mode = r.i32();
            std::vector<uint8_t> v;
            if (!get_vec(r, v)) return false;
            const auto k = std::make_tuple(owner, source, mode);
            if (!delivery_reach.empty() && !(delivery_reach.rbegin()->first < k)) r.fail();
            delivery_reach.emplace_hint(delivery_reach.end(), k, std::move(v));
        }
    }
    std::map<objects::ObjectId, int32_t> unit_reserved;
    {
        const uint32_t n = save::count(r, 8);
        for (uint32_t i = 0; i < n && r.ok(); ++i) {
            const objects::ObjectId u = r.i32();
            const int32_t pp = r.i32();
            if (!unit_reserved.empty() && unit_reserved.rbegin()->first >= u) r.fail();
            unit_reserved.emplace_hint(unit_reserved.end(), u, pp);
        }
    }
    std::vector<FreeStoreMove> fs_moves(save::count(r, 21));
    for (FreeStoreMove& mv : fs_moves) {
        mv.unit = get_handle(r, st);
        mv.player = r.i32();
        mv.dest = r.u32();
        mv.done = r.boolean();
        if (!valid_ai(mv.player) || mv.dest >= planets) r.fail();
    }
    std::map<uint32_t, float> reclaim;
    {
        const uint32_t n = save::count(r, 8);
        for (uint32_t i = 0; i < n && r.ok(); ++i) {
            const uint32_t serial = r.u32();
            const float amount = r.f32();
            if (!reclaim.empty() && reclaim.rbegin()->first >= serial) r.fail();
            reclaim.emplace_hint(reclaim.end(), serial, amount);
        }
    }
    if (!r.ok()) return false;
    // ---- Lua runtime ----
    std::vector<std::vector<std::unique_ptr<lua::LuaVm>>> pools;
    if (r.u32() != m.runtime->plan_count() || !r.ok()) return false;
    pools.resize(m.runtime->plan_count());
    for (uint32_t p = 0; p < pools.size(); ++p) {
        const uint32_t n = save::count(r, 6);
        bool pristine_named = false;
        for (uint32_t k = 0; k < n && r.ok(); ++k) {
            if (r.boolean()) {  // this session's own pristine VM of the plan (restore_pools keeps it at this slot)
                const uint64_t digest = r.u64();
                if (!r.ok() || pristine_named || !m.runtime->pristine(p) || m.runtime->pristine_digest(p) != digest) return false;
                pristine_named = true;
                pools[p].push_back(nullptr);
                continue;
            }
            const bool with_markup = r.boolean();
            const std::span<const uint8_t> img = get_vm_image(r);
            if (!r.ok()) return false;
            std::unique_ptr<lua::LuaVm> vm;
            if (m.lua_state) {
                vm = m.runtime->blank_vm(m.runtime->plan_name(p));
                if (!read_vm_image(img, *vm)) return false;
            } else {
                vm = m.runtime->fresh(p);
                if (!vm) return false;
            }
            m.rebind_vm(*vm, -1, with_markup);
            pools[p].push_back(std::move(vm));
        }
    }
    const uint64_t created = r.u64();
    std::map<std::string, ScriptEvaluators::Loaded> evals;
    {
        const uint32_t n = save::count(r, 12);
        for (uint32_t i = 0; i < n && r.ok(); ++i) {
            std::string key = r.str();
            ScriptEvaluators::Loaded e;
            e.name = r.str();
            const std::span<const uint8_t> img = get_vm_image(r);
            if (!r.ok() || key != ai_upper_copy(e.name) || (!evals.empty() && !(evals.rbegin()->first < key))) return false;
            if (m.lua_state) {
                e.vm = m.runtime->blank_vm(e.name);
                if (!read_vm_image(img, *e.vm)) return false;
            } else {  // tests without Lua state: the evaluator loaded fresh as on first use
                if (!m.files) return false;
                std::string err;
                e.vm = m.runtime->make_vm_from_file(*m.files, e.name, "Data\\Scripts\\Evaluators\\" + e.name + ".lua", &err);
                if (!e.vm) return false;
            }
            m.rebind_vm(*e.vm, -1, true);
            evals.emplace_hint(evals.end(), std::move(key), std::move(e));
        }
    }
    std::set<std::string> missing;
    if (!r.ok() || !save::get_set(r, missing) || !r.done()) return false;

    // ---- the token resolver: the last fallible step (read in place, all-or-nothing itself) ----
    {
        ByteReader rr(res_image);
        if (!m.res.read(rr)) return false;
    }

    // ---- commit ----
    m.now = now;
    m.perception_frame = perception_frame;
    m.market_sim_frame = market_sim_frame;
    m.current_plan = current_plan;
    m.next_plan = next[0];
    m.next_tf = next[1];
    m.next_status = next[2];
    m.next_pp = next[3];
    m.next_build = next[4];
    m.next_queue_id = next[5];
    m.orders = orders;
    m.rejected = rejected;
    m.gate = gate;
    m.snap = std::move(snap);
    m.perc = std::move(perc);
    // The S2 resolver over the last update's snapshot + memories (as update_player builds it).
    m.mem.clear();
    m.s2.reset();
    if (m.perception_frame >= 0) {
        for (uint32_t p = 0; p < m.snap.player_count(); ++p) m.mem.push_back(m.perc.memory(static_cast<int32_t>(p)));
        m.s2 = std::make_unique<GalacticTokenResolver>(m.snap, m.mem);
    }
    m.res.set_s2(m.s2.get());
    m.markup = std::move(markup);
    m.world.set_ai_suspended(world.ai_suspended);
    m.world.states() = std::move(world.states);
    m.world.purge_goal_requests = std::move(world.purge);
    m.world.desire_bonuses = std::move(world.bonuses);
    m.world.tactical_releases = std::move(world.releases);
    m.world.budget_flushes = std::move(world.flushes);
    m.globals = std::move(globals);
    for (size_t i = 0; i < m.ai.size(); ++i) {  // q6 t07: the saved AI player set (fresh players replace changed ones)
        if (!present[i]) m.ai[i].reset();
        else if (fresh[i]) m.ai[i] = std::move(fresh[i]);
    }
    m.cfg.players = std::move(saved_cfg);
    if (!m.cfg.players.empty()) m.install_sim_hooks();
    for (size_t i = 0; i < m.ai.size(); ++i) {
        if (!m.ai[i]) continue;
        PlayerAi& pa = *m.ai[i];
        StagedPlayer& sp = players[i];
        pa.ctx = std::move(sp.ctx);
        pa.known = std::move(sp.known);
        pa.bonuses = std::move(sp.bonuses);
        pa.reach_frame = sp.reach_frame;
        pa.reach = std::move(sp.reach);
        pa.paths = std::move(sp.paths);
        pa.paths.set_background(m.cfg.path_latency, m.cfg.background);  // OPT2 t02: this world's settings
        pa.sources = std::move(sp.sources);
        if (!sp.has_fs) pa.fs_vm.reset();
        else if (sp.fs_vm) pa.fs_vm = std::move(sp.fs_vm);
    }
    m.plans = std::move(plans);  // the replaced plan VMs are destroyed (not pooled)
    m.tfs = std::move(tfs);
    m.statuses = std::move(statuses);
    m.pps = std::move(pps);
    m.builds = std::move(builds);
    m.pending_builds = std::move(pending_builds);
    m.planet_queues = std::move(planet_queues);
    m.delivery_frame = delivery_frame;
    m.delivery_reach = std::move(delivery_reach);
    m.unit_reserved = std::move(unit_reserved);
    m.fs_moves = std::move(fs_moves);
    m.reclaim = std::move(reclaim);
    m.runtime->restore_pools(std::move(pools), static_cast<size_t>(created));
    guard.committed = true;
    m.scripts.restore(std::move(evals), std::move(missing));
    return true;
}

}  // namespace alamo::ai
