// Tactical AI world (A1 stage 2): contexts, goal-system hooks, target slots, free stores, plans on the Lua
// bridge, task forces, blocking statuses (see tactical_ai.hpp). [port] / [L] / [M] mark behaviour the RE docs
// do not fix.
#include "alamo/ai/tactical/tactical_ai.hpp"
#include "alamo/core/profile.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <climits>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <span>

#include "alamo/ai/ai_context.hpp"
#include "alamo/ai/ai_lua_runtime.hpp"
#include "alamo/ai/perception_equations.hpp"
#include "alamo/ai/tactical/find_target.hpp"
#include "alamo/ai/tactical/perception_lua.hpp"
#include "alamo/ai/tactical/tactical_perception.hpp"
#include "alamo/ai/tactical/tactical_pp.hpp"  // A2 tactical
#include "alamo/ai/tactical/target_list.hpp"
#include "alamo/ai/tactical/zone_reachability.hpp"
#include "alamo/commands/pipeline.hpp"
#include "alamo/core/dmath.hpp"  // refixf t06: space reinforcement ring directions
#include "alamo/core/file_provider.hpp"
#include "alamo/core/hash.hpp"  // A2 tactical: crc32 (perception '#' reseed)
#include "alamo/core/profile.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/core/sim_rng.hpp"
#include "alamo/data/xml.hpp"
#include "alamo/galactic/auto_resolve.hpp"
#include "alamo/galactic/cadence.hpp"  // PHASE2_ANSWERS Q11 all-pairs cadence (land path finder)
#include "alamo/lua/ai_bridge.hpp"
#include "alamo/lua/ai_tactical_bridge.hpp"
#include "alamo/lua/gc_schedule.hpp"  // OPT2 t04
#include "alamo/lua/lua_vm.hpp"
#include "alamo/lua/object_bridge.hpp"
#include "alamo/lua/script_remaining.hpp"  // L1 final
#include "alamo/objects/behaviors.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "tactical_save_io.hpp"  // N1 save / load

namespace alamo::ai::tactical {

namespace cmd = alamo::commands;
using lua::AiTacticalOrder;
using objects::ObjectHandle;

namespace {

constexpr int32_t kFreeStoreScriptBase = 1000000;  // free-store script ids: base + player (A2's convention)
constexpr int32_t kSlotBits = 20;                    // AITarget id = (player << 20) | slot [port]
constexpr int32_t kSlotMask = (1 << kSlotBits) - 1;
constexpr uint32_t kNoWireObject = 0x3FFFFFu;        // commands::kNoObject
constexpr size_t kTfGroupOrderMaxUnits = 4096;       // [port] ids in one group order (payload < 16 KiB, header)

int32_t target_id(int32_t player, int32_t slot) { return (player << kSlotBits) | slot; }

// OPT2 t10: the seed of a slot-10 candidate's own RNG stream, a function of (session seed, frame, player, goal source,
// target) only (splitmix64 finaliser chain).
uint64_t pp_mix(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
uint32_t pp_stream_seed(uint32_t game_seed, int64_t frame, int32_t player, uint32_t source, int32_t target) {
    uint64_t h = pp_mix(game_seed);
    h = pp_mix(h ^ static_cast<uint64_t>(frame));
    h = pp_mix(h ^ static_cast<uint64_t>(static_cast<uint32_t>(player)));
    h = pp_mix(h ^ source);
    h = pp_mix(h ^ static_cast<uint64_t>(static_cast<uint32_t>(target)));
    return static_cast<uint32_t>(h >> 32);
}

// EnumConversionClass<HardPointType> (TACTICAL_COMBAT §2.4, refixf t06 [H]): the short names only, value = index; 11 has
// no name. Lookups trim and upper-case the token (case-insensitive); a HARD_POINT_* token does not resolve.
const char* kHardPointTypeNames[] = {
    "ENGINE",         "SHIELD_GENERATOR", "GRAVITY_WELL",      "FIGHTER_BAY",        "TRACTOR_BEAM",
    "WEAPON_LASER",   "WEAPON_MISSILE",   "WEAPON_TORPEDO",    "WEAPON_ION_CANNON",  "WEAPON_MASS_DRIVER",
    "WEAPON_SPECIAL", "",                 "DUMMY_ART",
};

void put_u32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

// The L1 object-order event (object_bridge.hpp: PortExtension, subtype kLuaOrderSubtype + order) for the task force's
// movable units as ONE group order (header: the 24-byte block for units[0], then one u32 per further unit).
cmd::EventData lua_order_event(lua::LuaOrder order, std::span<const uint32_t> units, int32_t target, const Vec3f& p, uint32_t arg = 0) {
    cmd::EventData ev(cmd::EventType::PortExtension);
    ev.set("subtype", static_cast<uint32_t>(lua::kLuaOrderSubtype + static_cast<uint32_t>(order)));
    const size_t n = std::min(units.size(), kTfGroupOrderMaxUnits);
    std::vector<uint8_t> b(lua::kLuaOrderPayloadBytes + 4 * (n > 0 ? n - 1 : 0), 0);
    put_u32(b.data() + 0, n > 0 ? units[0] : kNoWireObject);
    put_u32(b.data() + 4, target < 0 ? kNoWireObject : static_cast<uint32_t>(target));
    put_u32(b.data() + 8, std::bit_cast<uint32_t>(p.x));
    put_u32(b.data() + 12, std::bit_cast<uint32_t>(p.y));
    put_u32(b.data() + 16, std::bit_cast<uint32_t>(p.z));
    put_u32(b.data() + 20, arg);
    for (size_t i = 1; i < n; ++i) put_u32(b.data() + lua::kLuaOrderPayloadBytes + 4 * (i - 1), units[i]);
    ev.set_bytes("data", std::span<const uint8_t>(b.data(), b.size()));
    return ev;
}

// S7 t27: a threat tolerance as the Lua MoveTo order's argument word (float bits; BattleAi reads 0 as "no threshold", so a
// tolerance of +0.0 is sent as the smallest positive float [port]) and its decision-log suffix.
uint32_t threat_bits(float t) { return t == 0.0f ? 1u : std::bit_cast<uint32_t>(t); }
std::string threat_text(float t) {
    char b[48];
    std::snprintf(b, sizeof b, " threat %g", static_cast<double>(t));
    return b;
}

AiMode ai_mode_of(TacticalMode m) { return m == TacticalMode::Land ? AiMode::Land : AiMode::Space; }

double dist3(const Vec3f& a, const Vec3f& b) {
    const double dx = static_cast<double>(a.x) - b.x, dy = static_cast<double>(a.y) - b.y, dz = static_cast<double>(a.z) - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

}  // namespace

// ================================================================ state records

struct Slot {
    TacticalTarget t;
    int32_t handle = -1;  // TacticalPerception handle
    bool alive = true;
};

struct TfRec {
    int32_t id = -1;
    int32_t plan = -1;
    int32_t player = -1;
    uint32_t def = 0;
    std::string name;
    lua::AiTaskForceKind kind = lua::AiTaskForceKind::Land;
    std::vector<ObjectHandle> units;
    std::vector<TacticalReinforcement> pending;              // claimed pool entries (Reinforce draws them)
    std::vector<std::pair<int32_t, int32_t>> reservations;   // (pad object id, type) for Build / Build_All
    std::vector<std::pair<int32_t, int32_t>> producing;      // (type, builder object id): tactical builds in progress
    std::string priorities;                                  // Set_Targeting_Priorities
};

// Ambush: S7 t21 SpaceAmbushBlockStatus (AI_TACTICAL §7.3.1); Ability: S7 t15 A1 AbilityBlockStatus of the task-force
// Activate_Ability (MOVEMENT §3.1.1 step 6). Reinforce: a1j t01 land ReinforceBlockStatus with landings in flight.
enum class StatusKind : uint8_t { Done, Failed, ProduceForce, Move, AttackTarget, Guard, SpaceReinforce, Ambush, Ability, Reinforce };

struct StatusRec {
    int32_t id = -1;
    int32_t plan = -1;
    int32_t tf = -1;
    StatusKind kind = StatusKind::Done;
    bool finished = false;
    lua::AiStatusResult result = lua::AiStatusResult::Nil;
    int64_t started = 0, last_order = 0;
    ObjectHandle unit{};  // L1 final: a one-unit Move status (GameObjectWrapper.Explore_Area), tf = -1
    int32_t target = -1;  // object id (attack / guard)
    Vec3f dest;           // SpaceReinforce: the ring centre T (+0x68, z = 0)
    // refixf t06 - SpaceReinforceBlockStatus (AI_TACTICAL §7.4): timeout frame (+0x74, -1 none), ring radius R (+0x7c),
    // T inside a non-allied prevention radius at creation (R resets to 1000, else 0).
    int64_t timeout = -1;
    float ring = 0.0f;
    bool prevent = false;
    // S7 t21 - SpaceAmbushBlockStatus (§7.3.1): direction (+0x50: 0 front, 1 left, 2 right, 3 back), minDist (+0x54),
    // threat tolerance (+0x58), mode frame of the last plan (+0x40), lists created (first plan); `members` (+0x68) and
    // `moving` (+0x60) as object ids in list order; `target` = the target object, -1 once it died; last_order = the AI
    // frame of the last group move. S7 t15 A1 - AbilityBlockStatus: `members` = the activated units still pending.
    int8_t direction = -1;
    float min_dist = 0.0f, threat = 1.0e18f;
    uint32_t plan_frame = 0;
    bool lists = false;
    std::vector<int32_t> members, moving;
    // a1j t01 - land ReinforceBlockStatus (A1_READINGS §36): the landings in flight as (pool key, company type), order
    // issued (in-flight count +0x40 = its size); the plan's removable flag saved at the order (+0x44), restored when the
    // last landing reports; Result = `result` set at the order (+0x45: every pending unit placed).
    std::vector<std::pair<int32_t, int32_t>> landings;
    bool saved_removable = true;
};

struct PlanInstance {
    int32_t id = -1;
    int32_t player = -1;
    uint32_t plan = kAiNone;
    uint32_t goal_type = kAiNone;
    int32_t slot = -1;          // target slot (-1 global)
    int32_t target_owner = -1;  // unit targets: owner for Original_Target_Owner_Changed
    std::unique_ptr<lua::LuaVm> vm;
    std::vector<int32_t> tfs;
    bool result = false, non_removable = false, ended = false;
    uint32_t live_threads = 0;
    int32_t pp = -1;  // A2 tactical: the goal's PotentialPlan handle (Produce_Force reads its selection)
};

// --- A2 tactical: a solved tactical PotentialPlan (tactical_pp.hpp). ---
struct TacPpRec {
    int32_t player = -1;
    uint32_t plan = kAiNone;
    std::vector<TacticalPpPick> picks;  // FreeStore picks = PP+0x108 (0x6c27d0 removes a unit that left the plan)
    bool reserved = false;              // PP+0x100: the first 0x6c2d60 ran (pool / pad slots reserved then)
};
// --- end A2 tactical ---

struct PendingBonus {
    uint32_t goal_type = kAiNone;
    int32_t target = -1;
    float bonus = 0.0f;
    int64_t deadline = 0;
};

struct PlayerAi;

// ================================================================ Impl

struct TacticalAiWorld::Impl final : lua::AiScriptHost, lua::AiTacticalScriptHost {
    Impl(objects::ObjectStore& st, const objects::GameObjectTypes* ty, const FileProvider* fp, GameRandom& r, const AiDefinitions& d,
         const EquationSet& e, TacticalAiHost& h, TacticalAiConfig c)
        : store(st), types(ty), files(fp), rng(r), defs(d), eqs(e), host(h), cfg(std::move(c)) {}

    objects::ObjectStore& store;
    const objects::GameObjectTypes* types;
    const FileProvider* files;
    GameRandom& rng;
    const AiDefinitions& defs;
    const EquationSet& eqs;
    TacticalAiHost& host;
    TacticalAiConfig cfg;

    // perception
    PerceptionWorld w;
    std::vector<TacticalAiObject> facts;
    std::vector<std::pair<int32_t, int32_t>> prev;  // (id, owner) of the last snapshot, ascending id
    TacticalPerception perc;
    ZoneGraph zones;
    std::map<std::string, uint32_t> eq_index;
    std::vector<uint8_t> eq_script_free;  // OPT2 t03: per equation, 0 not known yet, 1 being checked, 2 Script_-free, 3 not

    // Lua
    lua::LuaPlayerWorld world;
    lua::ObjectBridge bridge;
    cmd::CommandPipeline pipeline;
    PlanLibrary library;
    std::unique_ptr<PlanScriptRuntime> runtime;
    std::map<std::string, lua::AiGlobalValue> globals;
    ScriptEvaluators scripts;  // Script_<File> perception evaluators (AI_GALACTIC_2 §4.4), one shared VM per file
    lua::GlobalValueStore* gv_store = nullptr;  // Int-final: attached session-wide GlobalValue table (globals unused then)

    AiEnableGate gate;
    std::vector<std::unique_ptr<PlayerAi>> ai;  // by player index

    std::map<int32_t, PlanInstance> plans;
    std::map<int32_t, TfRec> tfs;
    std::map<int32_t, StatusRec> statuses;
    int32_t next_plan = 1, next_tf = 0, next_status = 0;
    int32_t current_plan = -1;
    int64_t now = 0;
    uint32_t mode_frame = 0;

    std::vector<TacticalAiDecision> log;
    std::vector<std::string> errors;
    std::map<std::string, uint32_t> stats;
    uint64_t orders = 0, rejected = 0, eval_count = 0;

    // --- A2 tactical: PotentialPlans (solved at proposal, slot 10) and their reservations. The free-store entry +0x30
    // lives in the player's TacticalFreeStore (A1_READINGS §28). ---
    std::map<int32_t, TacPpRec> pps;
    int32_t next_pp = 0;
    std::map<int32_t, int32_t> pool_reserved;  // reinforcement pool key -> PP handle
    std::map<int32_t, int32_t> pad_reserved;   // pad id -> PP handle
    std::map<int32_t, uint32_t> handle_name_crc;  // perception handle -> CRC-32 of the target's name ('#' reseed) [M]
    // N1: read() kept a PerceptionWorld without types; relink() checks the host's against the saved count / digest.
    bool relink_types = false;
    uint32_t saved_type_count = 0;
    uint64_t saved_types_digest = 0;
    // first_solve: Init at proposal (PP+0x104 == 0, FreeStore Setup A ignores +0x30); else the re-solve at accept.
    bool solve_pp(PlayerAi& p, uint32_t plan, const Goal& g, TacticalPpResult& out, bool first_solve);
    // The solve's input (no RNG set). snapshot (OPT2 t10): the host / world callbacks are replaced by their answers
    // (can_build_now for the plan's team types, enemy_strength for the masks the Contrast marshaller asks), so the solve
    // reads nothing that changes after this call and may run on any thread, now or frames later.
    void pp_input(PlayerAi& p, uint32_t plan, const Goal& g, bool first_solve, bool snapshot, TacticalPpInput& in);
    bool resolve_pp(PlayerAi& p, const Goal& g);          // 0x6c4a50: re-solve at accept (A1_READINGS §28.6 step 3)
    void reserve_pp(int32_t handle);                      // 0x6c2d60 (+ 0x6c2b20)
    void suspend_pp(int32_t handle);                      // 0x6c0be0's +0x30 part (Commit step 4)
    bool pp_stolen(int32_t handle);                       // 0x6c45e0
    void pp_unit_left(int32_t handle, int32_t unit);      // 0x6c27d0 (the unit's re-admission is the caller's)
    void release_pp(int32_t handle, int32_t tf_def = -1);
    TacticalFreeStore* pp_store(int32_t handle);
    void reinforce_joined(const TfRec& t, int32_t unit, int32_t type);  // 0x6bf3d0 for a land TF.Reinforce delivery
    double enemy_strength_at(int32_t player, const Vec3f& at, int32_t zone, uint64_t mask) const;
    // OPT2 t10: enemy_strength_at for several masks in one pass (each sum in the same object order: identical values).
    std::vector<double> enemy_strengths_at(int32_t player, const Vec3f& at, int32_t zone, std::span<const uint64_t> masks) const;
    // OPT2 t10: the host answers every first solve of one player's proposal slice reads (build pads, reinforcement pool,
    // population). They only change in the battle tick or through an executed order, and no order runs between a
    // slice's candidates (an order a Script_ evaluator might issue clears the cache), so the slice asks the host once
    // per answer instead of once per candidate (BattleAi::build_pads scans every object per pad). player < 0: no slice
    // open, solves ask the host.
    struct PpSliceHost {
        int32_t player = -1;
        bool pads_ok = false, pool_ok = false, pop_ok = false;
        std::vector<TacticalPad> pads;
        std::vector<TacticalReinforcement> pool;
        float pop_cap = 0.0f, pop_used = 0.0f;
        std::vector<int8_t> can_build;  // per type: -1 not asked, 0 / 1 (snapshot inputs)
        void open(int32_t p) {
            player = p;
            clear();
        }
        void clear() {
            pads_ok = pool_ok = pop_ok = false;
            can_build.clear();
        }
    } pp_slice;
    // --- end A2 tactical ---

    // ---- helpers ----
    void decide(int32_t player, std::string text) {
        if (cfg.log_decisions) log.push_back({now, player, std::move(text)});
    }
    PlayerAi* player_ai(int32_t p) {
        return p >= 0 && static_cast<size_t>(p) < ai.size() ? ai[static_cast<size_t>(p)].get() : nullptr;
    }
    const PlayerAi* player_ai(int32_t p) const {
        return p >= 0 && static_cast<size_t>(p) < ai.size() ? ai[static_cast<size_t>(p)].get() : nullptr;
    }
    uint32_t equation(const std::string& name) {
        const std::string key = ai_upper_copy(ai_trim(name));
        const auto it = eq_index.find(key);
        if (it != eq_index.end()) return it->second;
        const uint32_t i = eqs.find(key);
        eq_index.emplace(key, i);
        return i;
    }
    int32_t first_human() const {
        for (size_t p = 0; p < w.players.size(); ++p)
            if (w.players[p].is_human) return static_cast<int32_t>(p);
        return -1;
    }
    int32_t opponent_of(int32_t player) const;
    EvalContext eval_context(int32_t self, int32_t handle) const {
        EvalContext c;
        c.self = self;
        c.target = handle;
        c.location = handle;
        c.enemy = opponent_of(self);
        c.human = first_human();
        // A2 tactical: the '#' reseed inputs (AI_GALACTIC_3 §7.3): session seed + the target's name CRC.
        c.game_seed = cfg.game_seed;
        if (const auto it = handle_name_crc.find(handle); it != handle_name_crc.end()) c.target_name_crc = it->second;
        return c;
    }
    bool evaluate(int32_t self, const std::string& fn, int32_t handle, double& out) {
        const uint32_t e = equation(fn);
        if (e == kUnresolved) {
            ++stats["unknown equation " + fn];
            return false;
        }
        EvalContext c = eval_context(self, handle);
        c.rng = &rng.synced();  // Script_ evaluators only
        const EvalResult r = perc.evaluate(eqs, e, c, w);
        ++eval_count;
        if (!r.ok) return false;
        out = r.value;
        return true;
    }
    // OPT2 t03: an equation with no Script_ reference (itself or through its Function_ references) evaluates const and
    // re-entrantly (perception_equations.hpp, tactical_perception.hpp), so goal candidates may evaluate it on any thread.
    // Serial (sim thread): resolves `fn` and memoises the answer per equation.
    bool concurrent_equation(const std::string& fn) {
        const uint32_t e = equation(fn);
        if (e == kUnresolved) return false;  // evaluate() counts it
        if (eq_script_free.size() != eqs.size()) eq_script_free.assign(eqs.size(), 0);
        return script_free(e);
    }
    bool script_free(uint32_t e) {
        uint8_t& state = eq_script_free[e];
        if (state != 0) return state != 3;  // 1: a Function_ cycle (fails at evaluation, no Script_ on this path)
        state = 1;
        bool pure = true;
        for (const ExprNode& n : eqs.equation(e).expr.nodes) {
            if (n.op == Op::Script) pure = false;
            else if (n.op == Op::Function && n.ref != kUnresolved && n.ref < eqs.size() && !script_free(n.ref)) pure = false;
            if (!pure) break;
        }
        eq_script_free[e] = pure ? 2 : 3;
        return pure;
    }
    // The evaluate() of a function concurrent_equation() accepted, without its bookkeeping (counted serially).
    bool evaluate_concurrent(int32_t self, const std::string& fn, int32_t handle, double& out) const {
        const auto it = eq_index.find(ai_upper_copy(ai_trim(fn)));
        if (it == eq_index.end() || it->second == kUnresolved) return false;
        const LeafCacheUncounted uncounted;
        const EvalResult r = perc.evaluate(eqs, it->second, eval_context(self, handle), w);  // rng unset: no Script_
        if (!r.ok) return false;
        out = r.value;
        return true;
    }
    bool script_evaluate(const EvalContext& c, std::string_view name, std::span<const Param> params, double& out);
    int32_t script_target(int32_t self, int32_t handle) const;
    const PerceptionObject* object(int32_t id) const { return w.object(id); }
    const TacticalAiObject* fact(int32_t id) const {
        const PerceptionObject* o = w.object(id);
        return o ? &facts[static_cast<size_t>(o - w.objects.data())] : nullptr;
    }
    std::string type_name(int32_t t) const {
        return t >= 0 && static_cast<size_t>(t) < w.types.size() ? w.types[static_cast<size_t>(t)].name : std::string("?");
    }
    bool unit_ok(ObjectHandle h, int32_t player) const { return store.alive(h) && store.owner(h) == player; }
    int32_t id_of(ObjectHandle h) const { return store.id(h); }
    Vec3f pos_of(ObjectHandle h) const {
        const Vec3 p = store.position(h);
        return {p.x, p.y, p.z};
    }
    bool movable(ObjectHandle h) const {
        const PerceptionObject* o = object(id_of(h));
        return o && o->mobile;
    }
    // slot helpers
    Slot* slot_of(int32_t tid);
    Vec3f slot_position(const Slot& s) const {
        if (s.t.is_unit())
            if (const PerceptionObject* o = object(s.t.object_id)) return o->position;
        return s.t.position;
    }
    int32_t zone_at(const Vec3f& p) const {
        if (cfg.mode != TacticalMode::Land || zones.size() == 0) return -1;
        return zones.nearest_zone(p.x, p.y);
    }
    // Position of an argument (object / AI target / task force / point). False when none (never for a live task force record).
    bool arg_position(const lua::AiArg& a, const lua::AiPoint& p, Vec3f& out);
    Vec3f tf_center(const TfRec& t) const {
        Vec3f c;
        uint32_t n = 0;
        for (const ObjectHandle u : t.units)
            if (store.alive(u)) {
                const Vec3f q = pos_of(u);
                c.x += q.x;
                c.y += q.y;
                c.z += q.z;
                ++n;
            }
        if (n > 0) {  // S7 t20 (AI_TACTICAL §7.3.2): a reciprocal multiply (vf46 0x5fcc60), not a divide; no member -> (0, 0, 0)
            const float f = 1.0f / static_cast<float>(n);
            c.x *= f;
            c.y *= f;
            c.z *= f;
        }
        return c;
    }
    TfRec* tf_rec(int32_t id) {
        const auto it = tfs.find(id);
        return it == tfs.end() ? nullptr : &it->second;
    }
    void prune_tf(TfRec& t) {
        auto& u = t.units;
        u.erase(std::remove_if(u.begin(), u.end(), [this, &t](ObjectHandle h) { return !unit_ok(h, t.player); }), u.end());
    }
    bool tf_owns(ObjectHandle u) const {
        for (const auto& [id, t] : tfs)
            if (std::find(t.units.begin(), t.units.end(), u) != t.units.end()) return true;
        return false;
    }
    // a1h t02 (A1_READINGS §33.3): a newer task-force order collects every TF unit and raises signal 0xb on it, so each
    // older movement / attack / guard status of that TF loses all its units and finishes (Result nil); it never acts
    // again. [port] unit-level Lua orders, abilities and garrison orders on a TF member do not supersede yet.
    void supersede(int32_t tf) {
        for (auto& [id, s] : statuses)
            if (s.tf == tf && !s.finished && (s.kind == StatusKind::Move || s.kind == StatusKind::Guard || s.kind == StatusKind::AttackTarget)) {
                s.finished = true;
                s.result = lua::AiStatusResult::Nil;
            }
    }
    int32_t new_status(int32_t plan, int32_t tf, StatusKind kind) {
        StatusRec s;
        s.id = next_status++;
        s.plan = plan;
        s.tf = tf;
        s.kind = kind;
        s.started = s.last_order = now;
        if (kind == StatusKind::Done || kind == StatusKind::Failed) {
            s.finished = true;
            s.result = kind == StatusKind::Done ? lua::AiStatusResult::True : lua::AiStatusResult::False;
        }
        statuses.emplace(s.id, s);
        return s.id;
    }
    int32_t script_player(int32_t script) const {
        if (script >= kFreeStoreScriptBase) return script - kFreeStoreScriptBase;
        const auto it = plans.find(script);
        return it == plans.end() ? -1 : it->second.player;
    }
    cmd::RejectReason order(int32_t player, const cmd::EventData& ev, const std::string& what) {
        const cmd::RejectReason r = pipeline.execute_direct(static_cast<uint64_t>(now), static_cast<uint8_t>(player),
                                                            cmd::CommandSource::AI, ev);
        decide(player, "order " + what + (r == cmd::RejectReason::None ? "" : " rejected " + std::string(cmd::reject_reason_name(r))));
        return r;
    }

    // ---- object signals / free store ----
    void refresh();
    void admit(PlayerAi& p, size_t index, bool call_lua);
    // A task force releases `u` (TaskForce vf49 0x5ff610): 0x6c27d0 with the plan's PP `pp` (-1 none), re-admission.
    void release_to_free_store(int32_t player, ObjectHandle u, int32_t pp);
    void service_free_store(PlayerAi& p);
    int32_t pp_of_tf(const TfRec& t) const {
        const auto it = plans.find(t.plan);
        return it == plans.end() ? -1 : it->second.pp;
    }

    // ---- plans ----
    int32_t instantiate(PlayerAi& p, Goal& g);
    void end_plan(int32_t id, bool by_goal_system);
    void service_plans(int32_t player);
    void drain_world_requests(int32_t player, int32_t plan);
    void tf_event(const TfRec& tf, std::string_view event, const std::vector<lua::AiEventArg>& args, bool always_queue = false);
    // S7 t20 (AI_TACTICAL §7.3.2): a member left the task force (died / removed / owner changed / released) or a production
    // entry failed. When nothing is left (no unit, no pending reinforcement, no build in progress [M: the TF +0x60 entry
    // vector's type-flag test]) plan event 7 No_Units_Remaining is queued to the plan (dispatched by its next PumpEvents).
    void units_lost(TfRec& t);
    bool counted_entry_type(int32_t type) const;  // S7 t26: an entry type that keeps the task force (not hero / SW)
    // S7 t15 A1 / S7 t21: per-frame object signals of the ambush / ability statuses (deaths, target death, movement
    // finished) and the host's "unit ability finished" signals (0x29).
    void status_signals();
    void ability_signals(const std::vector<int32_t>& ids);
    // S7 t26: signal 0xb on a unit (and its team container): the blocks waiting on it end (no plan event).
    void order_signal(int32_t oid);
    // S7 t26: the ability blocks waiting on `oid` (or its team `team_id`) lose that unit (any of 0x29 / 0xb / 0x10).
    void ability_block_signal(int32_t oid, int32_t team_id);
    int32_t tf_member_of(int32_t object_id) const;  // the task force holding the object or its team container, -1 none
    bool ambush_point(const StatusRec& s, int32_t player, Vec3f& out, bool* no_walk = nullptr) const;
    void ambush_plan(StatusRec& s, bool first);
    void opportunity_events(const std::vector<OpportunityTargetEvent>& events);  // STRATEGIC_GAPS §5.3
    void service_statuses(int32_t player);
    bool step_status(StatusRec& s);
    void space_reinforce_poll(StatusRec& s);  // refixf t06: SpaceReinforceBlockStatus IsFinished (AI_TACTICAL §7.4)
    void reinforcements_unloaded(const std::vector<ReinforcementsUnloaded>& recs);  // a1j t01: signal 0x19 (0x6a1440)
    void claim_force(TfRec& t);
    // skai: the TF's pad picks (PpSource::Build) of its plan's PotentialPlan as reservations, for a Build / Build_All
    // that runs without a Produce_Force first (BuildEconStructure.lua) [M: retail's TF holds the reservations from plan
    // start]. Only when the TF has none yet.
    void claim_pads(TfRec& t) {
        const auto pit = plans.find(t.plan);
        if (pit == plans.end()) return;
        const auto ppit = pps.find(pit->second.pp);
        if (ppit == pps.end()) return;
        for (const TacticalPpPick& k : ppit->second.picks)
            if (k.tf == static_cast<int32_t>(t.def) && k.source == PpSource::Build) t.reservations.emplace_back(k.ref, k.type);
    }
    // A new object of a type a task force of its owner is producing (TfRec::producing, task forces in id order, first
    // entry of the type) joins that task force instead of the free store (AI_GALACTIC_3 §1.11: the build task registers
    // the unit with the plan). True when claimed.
    bool claim_produced(const PerceptionObject& o, ObjectHandle h) {
        if (!h || o.owner < 0) return false;
        for (auto& [id, t] : tfs) {
            if (t.player != o.owner) continue;
            const int32_t ty = o.rep_type >= 0 ? o.rep_type : o.type;
            for (size_t k = 0; k < t.producing.size(); ++k)
                if (t.producing[k].first == o.type || t.producing[k].first == ty) {
                    t.producing.erase(t.producing.begin() + static_cast<std::ptrdiff_t>(k));
                    t.units.push_back(h);
                    decide(t.player, "tf " + t.name + " produced " + type_name(o.type) + " " + std::to_string(o.id));
                    return true;
                }
        }
        return false;
    }

    // ---- tactical orders ----
    // S7 t27: `threat` (MoveTo only) = the group move's threat tolerance (MOVEMENT REMAINING_GAPS §4.2 threshold T), sent
    // as the Lua MoveTo group order; without one the MoveToPositionFacing group move.
    int32_t issue_move(TfRec& t, AiTacticalOrder order, const Vec3f& dest, int32_t target_object,
                       std::optional<float> threat = std::nullopt);
    std::vector<uint32_t> movable_ids(const TfRec& t);  // the task force's movable units, list order

    // ================= lua::AiScriptHost (A2 family) =================
    bool tf_valid(int32_t tf) override { return tf_rec(tf) != nullptr; }
    int32_t tf_force_count(int32_t tf) override {
        TfRec* t = tf_rec(tf);
        if (!t) return 0;
        prune_tf(*t);
        return static_cast<int32_t>(t->units.size());
    }
    void tf_set_flag(int32_t tf, lua::AiTfFlag flag, bool value) override;
    int32_t tf_produce_force(int32_t tf, const lua::AiArg&, bool) override {
        TfRec* t = tf_rec(tf);
        if (!t) return -1;
        claim_force(*t);
        return new_status(t->plan, tf, StatusKind::ProduceForce);
    }
    int32_t tf_get_stage(int32_t) override { return -1; }
    bool tf_all_on_free_store(int32_t tf) override {
        TfRec* t = tf_rec(tf);
        return t && t->pending.empty();
    }
    int32_t tf_order(int32_t tf, lua::AiTfOrder order, const lua::AiArg& arg) override {
        TfRec* t = tf_rec(tf);
        if (!t) return -1;
        if (order == lua::AiTfOrder::MoveTo) return tf_tactical_order(tf, lua::AiTacticalOrder::MoveTo, arg, {}, -1.0, false);
        // Form_Units / Activate_Ability: A2's binding drops the ability name [port: no effect]; galactic
        // orders do not exist in tactical.
        if (order == lua::AiTfOrder::FormUnits || order == lua::AiTfOrder::ActivateAbility) return new_status(t->plan, tf, StatusKind::Done);
        return -1;
    }
    bool tf_is_raid_capable(int32_t) override { return false; }
    void tf_release_forces(int32_t tf, double fraction) override {
        TfRec* t = tf_rec(tf);
        if (!t) return;
        const auto n = std::min(static_cast<size_t>(std::floor(fraction * static_cast<double>(t->units.size()) + 1e-9)), t->units.size());
        const std::vector<ObjectHandle> gone(t->units.begin(), t->units.begin() + static_cast<std::ptrdiff_t>(n));
        t->units.erase(t->units.begin(), t->units.begin() + static_cast<std::ptrdiff_t>(n));
        for (const ObjectHandle u : gone) release_to_free_store(t->player, u, pp_of_tf(*t));
        if (n > 0) units_lost(*t);  // S7 t20: Release_Unit path (0x5ff2d0)
    }
    void tf_release_unit(int32_t tf, ObjectHandle unit) override {
        TfRec* t = tf_rec(tf);
        if (!t) return;
        auto& u = t->units;
        const auto it = std::find(u.begin(), u.end(), unit);
        if (it == u.end()) return;
        u.erase(it);
        release_to_free_store(t->player, unit, pp_of_tf(*t));
        units_lost(*t);  // S7 t20: Release_Unit (0x5ff2d0)
    }
    std::vector<ObjectHandle> tf_units(int32_t tf) override {
        TfRec* t = tf_rec(tf);
        if (!t) return {};
        prune_tf(*t);
        return t->units;
    }
    std::string tf_goal_type_name(int32_t tf) override {
        TfRec* t = tf_rec(tf);
        const auto pit = t ? plans.find(t->plan) : plans.end();
        if (pit == plans.end() || pit->second.goal_type >= defs.goals.size()) return {};
        return defs.goals[pit->second.goal_type].name;
    }
    bool tf_collect_free_units(int32_t tf) override;
    bool status_finished(int32_t status) override {
        const auto it = statuses.find(status);
        if (it == statuses.end()) return true;
        // refixf t06 (AI_TACTICAL §7.4): the space reinforcement places its units inside IsFinished, once per Lua poll.
        if (it->second.kind == StatusKind::SpaceReinforce && !it->second.finished) space_reinforce_poll(it->second);
        // S7 t21 (§7.3.1, IsFinished 0x73a8d0): an unfinished ambush re-plans when Lua polls it more than 150 mode frames
        // after its last plan (new point from the target's current position and facing; every member re-ordered).
        if (it->second.kind == StatusKind::Ambush && !it->second.finished && mode_frame - it->second.plan_frame > 150)
            ambush_plan(it->second, false);
        return it->second.finished;
    }
    lua::AiStatusResult status_result(int32_t status) override {
        const auto it = statuses.find(status);
        return it == statuses.end() ? lua::AiStatusResult::Nil : it->second.result;
    }
    bool target_valid(int32_t target) override {
        const Slot* s = slot_of(target);
        return s && s->alive;
    }
    ObjectHandle target_object(int32_t target) override {
        const Slot* s = slot_of(target);
        if (!s || !s->alive || !s->t.is_unit()) return {};
        const ObjectHandle h = store.find(s->t.object_id);
        return store.alive(h) ? h : ObjectHandle{};
    }
    ObjectHandle find_target_object(int32_t target) override { return target_object(target); }
    bool target_position(int32_t target, Vec3& out) override {
        const Slot* s = slot_of(target);
        if (!s || !s->alive) return false;
        const Vec3f p = slot_position(*s);
        out = {p.x, p.y, p.z};
        return true;
    }
    double target_distance(int32_t target, const lua::AiArg& other) override {
        const Slot* s = slot_of(target);
        Vec3f b;
        if (!s || !arg_position(other, {}, b)) return -1.0;
        return dist3(slot_position(*s), b);
    }
    double budget_op(int32_t, lua::AiBudgetOp, double, std::string_view) override { return 0.0; }  // §4.1: no tactical budget
    int32_t budget_wait(int32_t script, lua::AiBudgetOp, double) override { return new_status(script, -1, StatusKind::Done); }
    int32_t free_store_count(int32_t script, int which) override;
    bool free_store_has(int32_t script, ObjectHandle unit) override;
    bool unit_in_transit(ObjectHandle) override { return false; }
    bool unit_safe(ObjectHandle) override { return true; }
    bool free_store_move(int32_t, ObjectHandle, const lua::AiArg&) override { return false; }
    int32_t find_target(int32_t script, const lua::AiFindTargetRequest& request) override;
    int32_t find_stage_area(int32_t, const lua::AiArg&, const lua::AiArg&, const lua::AiArg&) override { return -1; }
    lua::AiGlobalValue global_value(std::string_view name) override {
        if (gv_store) return gv_store->get(name);  // Int-final: the session-wide table
        const auto it = globals.find(std::string(name));
        return it == globals.end() ? lua::AiGlobalValue{} : it->second;
    }
    void set_global_value(std::string_view name, const lua::AiGlobalValue& v) override {
        if (gv_store) return gv_store->set(name, v);  // Int-final
        if (v.kind == lua::AiGlobalValue::Nil) globals.erase(std::string(name));
        else globals[std::string(name)] = v;
    }

    // ================= lua::AiTacticalScriptHost =================
    int32_t tf_tactical_order(int32_t tf, lua::AiTacticalOrder order, const lua::AiArg& dest, const lua::AiPoint& point,
                              double number, bool flag) override;
    int32_t tf_attack_target(int32_t tf, const lua::AiArg& target, std::string_view hardpoint) override;
    int32_t tf_reinforce(int32_t tf, const lua::AiArg& target, const lua::AiPoint& point, double max_wait) override;
    int32_t tf_build_all(int32_t tf) override;
    int32_t tf_build(int32_t tf, std::string_view type_name, const lua::AiArg& pad) override;
    std::vector<ObjectHandle> tf_reserved_pads(int32_t tf) override {
        std::vector<ObjectHandle> out;
        if (TfRec* t = tf_rec(tf))
            for (const auto& [pad, type] : t->reservations) {
                const ObjectHandle h = store.find(pad);
                if (store.alive(h)) out.push_back(h);
            }
        return out;
    }
    bool tf_withdraw(int32_t tf) override {
        TfRec* t = tf_rec(tf);
        if (!t) return false;
        cmd::EventData ev(cmd::EventType::Retreat);
        ev.set("request", 1);
        return order(t->player, ev, "withdraw (tf " + t->name + ")") == cmd::RejectReason::None;
    }
    double tf_self_threat(int32_t tf, bool sum) override {
        TfRec* t = tf_rec(tf);
        if (!t) return 0.0;
        std::vector<SelfThreatUnit> u;
        for (const ObjectHandle h : t->units) {
            const PerceptionObject* o = store.alive(h) ? object(id_of(h)) : nullptr;
            SelfThreatUnit s;
            s.alive = o != nullptr;
            s.has_type = o && o->type >= 0;
            s.combat_power = o ? o->combat_power : 0.0f;
            u.push_back(s);
        }
        return static_cast<double>(sum ? self_threat_sum(u) : self_threat_max(u));
    }
    double tf_distance(int32_t tf, const lua::AiArg& other, const lua::AiPoint& point) override {
        TfRec* t = tf_rec(tf);
        Vec3f b;
        if (!t || !arg_position(other, point, b)) return -1.0;
        prune_tf(*t);
        // S7 t27 (AI_TACTICAL §7.3.2): Get_Distance takes the task-force position (vf46), so an empty task force measures
        // from (0, 0, 0) like every other position-convertible use (S7 t20), not nil.
        return dist3(tf_center(*t), b);
    }
    void tf_set_targeting_priorities(int32_t tf, std::string_view set) override {
        if (TfRec* t = tf_rec(tf)) t->priorities = std::string(set);  // [port] recorded; combat priority sets not switched
    }
    // S7 t21 Prepare_Ambush (AI_TACTICAL §7.3.1), after the binding's argument checks.
    int32_t tf_prepare_ambush(int32_t tf, const lua::AiArg& target, int direction, double min_dist, double threat, bool zone_path,
                              std::string* name) override;
    // S7 t15 A1: the task-force Activate_Ability's AbilityBlockStatus (MOVEMENT §3.1.1 step 6).
    int32_t tf_activate_ability_units(int32_t tf, std::string_view ability, bool on, const lua::AiArg& target,
                                      const std::vector<std::pair<ObjectHandle, bool>>& units) override;
    std::string tf_name(int32_t tf) override {
        const TfRec* t = tf_rec(tf);
        return t ? t->name : std::string();
    }
    // --- L1 final (script_remaining.hpp) ---
    // Add_Force(unit): a live unit of the task force's player held by no task force joins it [port].
    bool tf_add_unit(int32_t tf, ObjectHandle unit) override {
        TfRec* t = tf_rec(tf);
        if (!t || !unit_ok(unit, t->player) || tf_owns(unit)) return false;
        t->units.push_back(unit);
        return true;
    }
    // TaskForceClass.Get_AI_Power_Vs_Unit (L1_S4_REMAINING §2.12, tactical only): sum over the TF's units of the
    // perception combat power x the power-versus weight (1 [port]: the TF context's WeightedTypeList table is not
    // modelled) minus the target's power. The object form is answered by the binding (health terms).
    double ai_power_vs(const lua::AiArg& self, const lua::AiArg& other) override {
        TfRec* t = tf_rec(self.task_force);
        if (!t) return -1.0;
        double sum = 0.0;
        for (const ObjectHandle h : t->units) {
            const PerceptionObject* o = store.alive(h) ? object(id_of(h)) : nullptr;
            if (o) sum += static_cast<double>(o->combat_power);
        }
        ObjectHandle target = other.object;
        if (!target && other.target >= 0) target = target_object(other.target);
        if (const PerceptionObject* o = target && store.alive(target) ? object(id_of(target)) : nullptr)
            sum -= static_cast<double>(o->combat_power);
        return sum;
    }
    // GameObjectWrapper.Explore_Area(target) (§2.3): an ExploreAreaBlockStatus over the one unit: a move to the
    // target's position [port: the exploration helper's least-recently-seen point is not modelled]; finished when the
    // unit stops (or dies).
    int32_t unit_explore_area(int32_t script, ObjectHandle unit, int32_t target) override;
    // --- end L1 final ---
    ObjectHandle find_deadly_enemy(int32_t script, const lua::AiArg& source, double radius, int32_t player) override;
    bool most_defended_position(int32_t script, const lua::AiArg& source, const lua::AiPoint& point, int32_t player,
                                lua::AiPoint& out) override;
    bool project_by_unit_range(ObjectHandle unit, const lua::AiArg& other, const lua::AiPoint& point, lua::AiPoint& out) override {
        if (!store.alive(unit)) return false;
        Vec3f p;
        if (!arg_position(other, point, p)) return false;
        // t48: [port] a live object outside the snapshot (a battle unit already dead or retreated, an object newer than the last
        // service) has no weapon data: range 0. Before, the nil result made the scripts' Move_To fail on a target TestValid accepted.
        const PerceptionObject* u = object(id_of(unit));
        const std::optional<Vec3f> r = tactical::project_by_unit_range(w, u, pos_of(unit), p);
        if (!r) return false;
        out.present = true;
        out.x = r->x;
        out.y = r->y;
        out.z = r->z;
        return true;
    }
    int is_good_against(ObjectHandle unit, ObjectHandle other) override {
        const galactic::ContrastTable* ct = host.contrast();
        const PerceptionObject* a = store.alive(unit) ? object(id_of(unit)) : nullptr;
        const PerceptionObject* b = store.alive(other) ? object(id_of(other)) : nullptr;
        if (!ct || !a || !b) return -1;
        const PerceptionType* ta = w.type_of(*a);
        const PerceptionType* tb = w.type_of(*b);
        if (!ta || !tb) return -1;
        return tactical::is_good_against(*ct, ta->name, ta->category, tb->category) ? 1 : 0;
    }
    bool evaluate_perception(int32_t script, std::string_view name, int32_t player, const lua::AiArg& target, double& out) override {
        (void)script;
        if (!w.is_player(player)) return false;
        int32_t handle = -1;
        if (target.target >= 0) {
            const Slot* s = slot_of(target.target);
            if (!s) return false;
            handle = s->handle;
        } else if (target.object) {
            const int32_t id = id_of(target.object);
            handle = perc.handle_of_object(id);
            if (handle < 0) handle = perc.register_unit(w, id);
            if (handle < 0) return false;
        }
        return evaluate(player, std::string(name), handle, out);
    }
};

// ================================================================ per-player AI (goal hooks + services)

struct PlayerAi final : GoalModeHooks, ContextModeServices {
    TacticalAiWorld::Impl* w;
    int32_t player;
    const AiPlayerTypeDef* type;
    int32_t difficulty;
    AiContext ctx;
    TacticalTargetList list;
    std::vector<Slot> slots;
    TacticalFreeStore fs;
    FreeStoreSchedule sched;
    std::unique_ptr<lua::LuaVm> fs_vm;
    std::vector<PendingBonus> bonuses;
    // A1_READINGS §25.2: flood sets per (target zone, level), BFS order; rebuilt when used after 10 s (mode frames).
    struct FloodSet {
        uint32_t stamp = 0;
        std::vector<uint32_t> zones;
    };
    std::map<std::pair<int32_t, uint8_t>, FloodSet> floods;
    float contrast_mult = 1.0f;  // A2 tactical: Land_ / Space_AI_Contrast_Multiplier of the difficulty
    // Land execution path finder (TACTICAL_GAPS §6.1): the player's own zone graph (node costs from its threat grid),
    // created at Execution slot 6 (first service) and recomputed on the PHASE2_ANSWERS Q11 cadence.
    ZoneGraph paths;
    bool paths_built = false;
    int32_t next_recompute = 0;
    const ZoneGraph& zone_paths() const { return paths_built ? paths : w->zones; }
    void recompute_paths() {
        paths.recompute_with_threat(w->perc.grid().valid() ? w->perc.zone_threat_fn(w->w, player) : ZoneThreatFn{});
        ++w->stats["land path finder recomputes"];
    }
    // OPT2 t02: a cadence recompute in the background (TacticalAiConfig::path_latency): the node costs are taken from the
    // threat grid on the kick frame, the all-pairs runs on the host's background hook (ZoneGraph::compute reads only
    // the static zones and links) and is applied on kick + latency (publish_paths, start of the update). Declared after
    // `paths`, so it is destroyed (waited for) first; `paths` is only replaced with no job in flight.
    LatencyJob<ZoneGraphResult> path_job;
    std::vector<uint32_t> path_pend;  // the in-flight recompute's node costs (N1 / OPT3 t04)
    void kick_paths(int32_t frame) {
        if (path_job.in_flight()) publish_paths(path_job.ready_frame());
        path_pend = paths.threat_node_costs(w->perc.grid().valid() ? w->perc.zone_threat_fn(w->w, player) : ZoneThreatFn{});
        start_paths_job(frame, frame + static_cast<int64_t>(w->cfg.path_latency), w->cfg.background);
        ++w->stats["land path finder recomputes"];
    }
    void start_paths_job(int64_t kick, int64_t ready, const BackgroundStart& bg) {
        path_job.kick(kick, ready, bg, [g = &paths, cost = path_pend](ZoneGraphResult& out) { g->compute(cost, out); });
    }
    bool publish_paths(int64_t frame) {
        if (!path_job.due(frame)) return false;
        paths.adopt(path_job.finish());
        path_pend.clear();
        return true;
    }

    PlayerAi(TacticalAiWorld::Impl* world, int32_t p, const AiPlayerTypeDef* t, int32_t d)
        : w(world), player(p), type(t), difficulty(d), ctx(ai_mode_of(world->cfg.mode)), list(world->cfg.mode) {}

    // Mirrors list entries appended since `from` into slots + perception handles.
    void mirror(size_t from) {
        for (size_t i = from; i < list.size(); ++i) {
            Slot s;
            s.t = list[i];
            if (s.t.is_unit()) s.handle = w->perc.register_unit(w->w, s.t.object_id);
            else if (s.t.zone >= 0) s.handle = w->perc.register_zone(s.t.zone);
            else s.handle = w->perc.register_cell(s.t.cell_x, s.t.cell_y, s.t.eval_rect);
            if (s.handle >= 0 && !s.t.name.empty())  // A2 tactical: target vf+0x50 name [M: = the target name]
                w->handle_name_crc[s.handle] = crc32(s.t.name.data(), s.t.name.size());
            slots.push_back(std::move(s));
        }
    }
    void on_added(const TacticalAiObject& f) {
        const size_t before = list.size();
        if (list.on_object_added(f.target)) mirror(before);
    }
    void on_removed(int32_t id) {
        if (!list.on_object_destroyed(id)) return;
        for (Slot& s : slots)
            if (s.alive && s.t.is_unit() && s.t.object_id == id) {
                s.alive = false;
                break;
            }
    }
    // The flood set around `zone` at `level` (ZoneGraph::flood; zone danger for this player [M which player field]).
    // Levels outside 6..10 and non-land modes give an empty set.
    const std::vector<uint32_t>& flood_set(int32_t zone, Reachability level) {
        static const std::vector<uint32_t> kNone;
        const double tol = reachability_tolerance(level, w->cfg.constants);
        if (tol < 0.0 || w->cfg.mode != TacticalMode::Land || zone < 0) return kNone;
        const std::pair<int32_t, uint8_t> key{zone, static_cast<uint8_t>(level)};
        if (const auto it = floods.find(key);
            it != floods.end() && w->mode_frame - it->second.stamp <= static_cast<uint32_t>(10 * kFramesPerSecond))
            return it->second.zones;
        FloodSet& f = floods[key];
        f.stamp = w->mode_frame;
        const ZoneThreatFn danger = w->perc.grid().valid() ? w->perc.zone_threat_fn(w->w, player) : ZoneThreatFn{};
        w->zones.flood(zone, [&](uint32_t z) {
            const double d = danger ? danger(z) : 0.0;
            return d == 0.0 || d <= tol;
        }, f.zones);
        return f.zones;
    }

    // Flood slot 6 (A1_READINGS §25.2): the first zone of `zone`'s flood set at `level`; with a link mask other than
    // -1 or a list, the first one `zone` links with (flags & mask) != 0, not in `exclude`, in `include` when given.
    // -1 = null (empty set, level outside 6..10, no match).
    int32_t flood_pick(int32_t zone, Reachability level, uint32_t mask, const std::vector<int32_t>* exclude,
                       const std::vector<int32_t>* include) {
        const std::vector<uint32_t>& set = flood_set(zone, level);
        if (set.empty()) return -1;
        if (mask == 0xFFFFFFFFu && !exclude && !include) return static_cast<int32_t>(set.front());
        for (const uint32_t z : set) {
            const auto zi = static_cast<int32_t>(z);
            if (!w->zones.links_to(static_cast<uint32_t>(zone), z, mask)) continue;
            if (exclude && std::find(exclude->begin(), exclude->end(), zi) != exclude->end()) continue;
            if (include && std::find(include->begin(), include->end(), zi) == include->end()) continue;
            return zi;
        }
        return -1;
    }

    // ---- GoalModeHooks ----
    uint32_t target_count() override { return static_cast<uint32_t>(slots.size()); }
    bool applies(int32_t target, uint32_t flags) override { return slot_applies(target, flags); }
    bool slot_applies(int32_t target, uint32_t flags) const {
        if (target < 0 || static_cast<size_t>(target) >= slots.size()) return false;
        const Slot& s = slots[static_cast<size_t>(target)];
        if (!s.alive) return false;
        if (!s.t.is_unit()) return region_applies(flags);
        const TacticalAiObject* f = w->fact(s.t.object_id);
        const PerceptionObject* o = w->object(s.t.object_id);
        if (!f || !o) return false;
        UnitAppliesInfo u = f->applies;
        u.object_id = o->id;
        u.has_owner = o->owner >= 0;
        u.owner_allied = u.has_owner && w->w.allied(o->owner, player);
        const PerceptionWorld& pw = w->w;
        const int32_t self = player;
        return unit_applies(flags, u, [&pw, self](int32_t id) { return pw.object_fogged && pw.object_fogged(self, id); });
    }
    bool blocked(uint32_t goal_type, int32_t target) override {
        // Land slot 9 (0x6b8710, A1_READINGS §25.3) [H]: the target's flood slot 6 (mask -1, no lists): skipped when the
        // region's flood set at the goal type's Reachability is empty (its zone and every neighbour above the tolerance).
        // No path from any unit is measured. A target without a reachability object is not skipped; a region target
        // with a level outside 6..10 always is.
        if (w->cfg.mode != TacticalMode::Land || target < 0 || static_cast<size_t>(target) >= slots.size()) return false;
        const Slot& s = slots[static_cast<size_t>(target)];
        if (!s.t.has_reachability()) return false;
        return flood_set(s.t.zone, w->defs.goals[goal_type].reachability).empty();
    }
    int32_t target_handle(int32_t target) const {
        return target >= 0 && static_cast<size_t>(target) < slots.size() ? slots[static_cast<size_t>(target)].handle : -1;
    }
    bool evaluate(const std::string& function, int32_t target, double& out) override {
        return w->evaluate(player, function, target_handle(target), out);
    }
    // OPT2 t03: batched goal values (ai_goals.hpp) on the host's job system.
    const ParallelFor* candidate_pf() override { return &w->cfg.pf; }
    bool prepare_evaluation(const std::string& function) override { return w->concurrent_equation(function); }
    bool evaluate_prepared(const std::string& function, int32_t target, double& out) const override {
        return w->evaluate_concurrent(player, function, target_handle(target), out);
    }
    void evaluated(const std::string&) override { ++w->eval_count; }
    void slice_begin() override { w->pp_slice.open(player); }  // OPT2 t10 (PpSliceHost)
    void slice_end() override { w->pp_slice.player = -1; }
    bool accept(Goal& g) override {
        // Slot 10 (0x6b8850, land and space): Find_Plan_For_Goal only - one synced draw (§4.4).
        ALAMO_PROFILE_SCOPE("tac.ai.accept");  // OPT2 t10: slot 10 (plan pick + first PotentialPlan solve)
        std::vector<double> ratio;
        if (!plan_candidates(g, ratio)) return false;
        const uint32_t plan = pp_cand[pick_weighted(ratio, w->rng.synced())];
        // --- A2 tactical: the PotentialPlan solve inside Find_Plan_For_Goal (land / space marshaller lists);
        // a failed solve rejects the candidate (AI_GALACTIC_2 §1.5.4, as galactic). Init's first solve: it ignores
        // the free-store reservations and reserves nothing (A1_READINGS §28.6 step 1). ---
        TacticalPpResult pp;
        w->solve_pp(*this, plan, g, pp, true);
        return accept_result(g, plan, pp);
    }
    // Slot 10's plan list: the plans that serve the goal type and whose template value is positive (pp_cand), with
    // their success ratios. False (counted) when there is none.
    std::vector<uint32_t> pp_cand;
    bool plan_candidates(const Goal& g, std::vector<double>& ratio) {
        if (g.source < ctx.goals.sources().size()) ++w->stats["~ proposed " + ctx.goals.sources()[g.source].function];
        pp_cand.clear();
        for (size_t i = 0; i < w->library.plans.size(); ++i)
            if (ctx.templates.plan_value(static_cast<uint32_t>(i)) > 0 && w->library.plans[i].serves(g.type)) {
                pp_cand.push_back(static_cast<uint32_t>(i));
                ratio.push_back(ctx.learning.plan_success_ratio(static_cast<uint32_t>(i)));
            }
        if (!pp_cand.empty()) return true;
        ++w->stats[w->defs.goals[g.type].name + " no plan"];
        return false;
    }
    // The end of slot 10 once the solve is known: a failed solve rejects g, else the PotentialPlan record is created.
    bool accept_result(Goal& g, uint32_t plan, TacticalPpResult& pp) {
        const std::string& gname = w->defs.goals[g.type].name;
        if (!pp.ok) {
            ++w->stats[gname + " no potential plan (" + w->library.plans[plan].name + ", failed " + pp.last_veto + ")"];
            return false;
        }
        ++w->stats[gname + " accepted"];
        const int32_t h = w->next_pp++;
        TacPpRec rec;
        rec.player = player;
        rec.plan = plan;
        rec.picks = std::move(pp.picks);
        w->pps.emplace(h, std::move(rec));
        g.pp_handle = h;
        // --- end A2 tactical ---
        g.plan = plan;
        g.pp_valid = true;
        g.cost = 0.0f;
        g.eta = 0.0f;
        return true;
    }
    // ---- OPT2 t10: batched accept (ai_goals.hpp; TacticalAiConfig::pp_streams / pp_latency) ----
    // pp_solves: the slice's prepared solves (inputs with their own RNG stream); pp_plans: their plans (kept for the
    // apply); pp_out: their results once known; pp_job: the slice's solves in the background (pp_latency > 0). A save
    // waits for pp_job and writes pp_out (fetch_pp), so a loaded run applies the same results on the same frame.
    struct PpSolve {
        TacticalPpInput in;
        SimRng rng;
    };
    std::vector<PpSolve> pp_solves;
    std::vector<uint32_t> pp_plans;
    mutable std::vector<TacticalPpResult> pp_out;
    mutable bool pp_out_ready = false;
    mutable LatencyJob<std::vector<TacticalPpResult>> pp_job;
    static void run_pp_solves(std::vector<PpSolve>& s, std::vector<TacticalPpResult>& out, const ParallelFor& pf) {
        out.assign(s.size(), TacticalPpResult{});
        const auto body = [&s, &out](uint32_t b, uint32_t e) {
            for (uint32_t i = b; i < e; ++i) {
                ALAMO_PROFILE_SCOPE("tac.ai.pp");  // OPT2 t10: one solve (any thread)
                s[i].in.rng = &s[i].rng;
                solve_tactical_pp(s[i].in, out[i]);
            }
        };
        const auto n = static_cast<uint32_t>(s.size());
        if (n >= 2) pf(n, 1, body);
        else body(0, n);
    }
    void fetch_pp() const {
        if (pp_out_ready) return;
        pp_out = std::move(pp_job.finish());
        pp_out_ready = true;
    }
    bool accept_batched() override { return w->cfg.pp_streams; }
    uint32_t accept_latency() override { return w->cfg.pp_latency; }
    bool accept_prepare(Goal& g, uint32_t i) override {
        ALAMO_PROFILE_SCOPE("tac.ai.accept");
        if (i == 0) {
            pp_solves.clear();
            pp_plans.clear();
        }
        std::vector<double> ratio;
        if (!plan_candidates(g, ratio)) return false;
        PpSolve s;
        s.rng.seed(pp_stream_seed(w->cfg.game_seed, g.created, player, g.source, g.target));
        const uint32_t plan = pp_cand[pick_weighted(ratio, s.rng)];
        w->pp_input(*this, plan, g, true, true, s.in);
        pp_solves.push_back(std::move(s));
        pp_plans.push_back(plan);
        return true;
    }
    void accept_start(uint32_t n, int64_t now, int64_t ready) override {
        ALAMO_PROFILE_SCOPE("tac.ai.accept");
        (void)n;
        pp_job.cancel();
        pp_out.clear();
        if (ready <= now) {
            run_pp_solves(pp_solves, pp_out, w->cfg.pf);
            pp_out_ready = true;
        } else {
            pp_out_ready = false;
            pp_job.kick(now, ready, w->cfg.background,
                        [solves = std::move(pp_solves), fan = w->cfg.pf](std::vector<TacticalPpResult>& out) mutable {
                            run_pp_solves(solves, out, fan);
                        });
        }
        pp_solves.clear();
    }
    bool accept_finish(Goal& g, uint32_t i) override {
        ALAMO_PROFILE_SCOPE("tac.ai.accept");
        fetch_pp();
        if (i >= pp_out.size() || i >= pp_plans.size()) return false;
        return accept_result(g, pp_plans[i], pp_out[i]);
    }
    void accept_cancel() override {
        pp_job.cancel();
        pp_solves.clear();
        pp_plans.clear();
        pp_out.clear();
        pp_out_ready = false;
    }
    bool plan_activatable(const Goal& g) override {
        return g.pp_valid && g.plan != kAiNone && ctx.templates.plan_value(g.plan) > 0 && w->resolve_pp(*this, g);
    }
    bool plan_invalid(const Goal& g) override {
        if (g.running < 0) return true;
        const auto it = w->plans.find(g.running);
        return it == w->plans.end() || it->second.ended || w->pp_stolen(g.pp_handle);
    }
    bool replaceable(const Goal& g) override {
        if (g.running < 0) return true;
        const auto it = w->plans.find(g.running);
        return it == w->plans.end() || !it->second.non_removable;
    }
    void refresh_plan(Goal& g) override { w->reserve_pp(g.pp_handle); }
    void suspend_reservations(Goal& g) override { w->suspend_pp(g.pp_handle); }
    bool uses_budget() override { return false; }  // the AIBudget is galactic-only (§4.1)
    bool can_fund(const Goal&) override { return true; }
    float category_credits(int32_t) override { return 1.0f; }
    void budget_add(Goal&) override {}
    void budget_release(Goal&) override {}
    void budget_weights() override {}
    std::vector<int32_t> category_order() override {
        std::vector<int32_t> order;
        for (size_t c = 0; c < w->defs.categories.size(); ++c) order.push_back(static_cast<int32_t>(c));
        return order;
    }
    void queue_production(Goal&) override {}
    void instantiate(Goal& g) override { g.running = w->instantiate(*this, g); }
    void end_plan(Goal& g, bool by_goal_system) override {
        if (g.running >= 0) w->end_plan(g.running, by_goal_system);
    }
    void drop_potential_plan(Goal& g) override {  // A2 tactical: release the PP's reservations
        if (g.pp_handle >= 0) {
            w->release_pp(g.pp_handle);
            w->pps.erase(g.pp_handle);
        }
        g.pp_handle = -1;
    }
    float elapsed_eta(const Goal&) override { return 0.0f; }
    int32_t quota_players() override {
        int32_t n = 0, humans = 0;
        for (const PerceptionPlayerInfo& p : w->w.players) {
            ++n;
            humans += p.is_human ? 1 : 0;
        }
        return n - humans;
    }

    // ---- ContextModeServices ----
    void first_service(int64_t) override;
    void template_init(int64_t) override {
        std::string names;
        for (const uint32_t t : ctx.templates.active()) names += " " + w->defs.templates[t].name;
        w->decide(player, "templates" + names);
    }
    void perception_service(int64_t) override {}  // §2: Service only reschedules
    void planning_service(int64_t) override { w->service_plans(player); }
    void execution_service(int64_t) override {
        // Execution service (0x526590): all-pairs recompute when the mode frame reached next, then next = frame + 90.
        if (paths_built) {
            const auto f = static_cast<int32_t>(w->mode_frame);
            if (galactic::recompute_due(f, next_recompute)) {
                next_recompute = galactic::next_after_service(f);
                if (w->cfg.path_latency == 0) recompute_paths();
                else kick_paths(f);
            }
        }
        w->service_statuses(player);
        w->service_free_store(*this);
        for (size_t i = 0; i < bonuses.size();) {
            if (ctx.goals.give_desire_bonus(bonuses[i].goal_type, bonuses[i].target, bonuses[i].bonus) || bonuses[i].deadline <= w->now)
                bonuses.erase(bonuses.begin() + static_cast<std::ptrdiff_t>(i));
            else
                ++i;
        }
    }
    void templates_changed(int64_t now) override { template_init(now); }
};

int32_t TacticalAiWorld::Impl::opponent_of(int32_t player) const {
    const PlayerAi* p = player_ai(player);
    if (p && p->ctx.initialised()) return p->ctx.opponent;
    for (size_t q = 0; q < w.players.size(); ++q)
        if (static_cast<int32_t>(q) != player && w.rel(player, static_cast<int32_t>(q)) == 1) return static_cast<int32_t>(q);
    return -1;
}

Slot* TacticalAiWorld::Impl::slot_of(int32_t tid) {
    if (tid < 0) return nullptr;
    PlayerAi* p = player_ai(tid >> kSlotBits);
    const int32_t s = tid & kSlotMask;
    if (!p || static_cast<size_t>(s) >= p->slots.size()) return nullptr;
    return &p->slots[static_cast<size_t>(s)];
}

bool TacticalAiWorld::Impl::arg_position(const lua::AiArg& a, const lua::AiPoint& p, Vec3f& out) {
    if (p.present) {
        out = {p.x, p.y, p.z};
        return true;
    }
    if (a.object && store.alive(a.object)) {
        out = pos_of(a.object);
        return true;
    }
    if (a.target >= 0) {
        const Slot* s = slot_of(a.target);
        if (!s) return false;
        out = slot_position(*s);
        return true;
    }
    if (a.task_force >= 0) {
        // S7 t20 (AI_TACTICAL §7.3.2): the extractor never fails for a task force; no live member -> (0, 0, 0).
        TfRec* t = tf_rec(a.task_force);
        if (!t) return false;
        prune_tf(*t);
        out = tf_center(*t);
        return true;
    }
    return false;
}

// ---- first service (§1.2): opponent, slot 6 of Perception, Goal, Planning, Execution, Learning ----

void PlayerAi::first_service(int64_t) {
    const PerceptionWorld& pw = w->w;
    // 1. Opponent: hostile players with an object in the mode, faction not neutral; the first is taken, later ones
    //    overwrite it only when they are the battle's attacker / defender.
    int32_t pick = -1;
    for (uint32_t q = 0; q < pw.player_count(); ++q) {
        const auto qi = static_cast<int32_t>(q);
        if (qi == player || pw.rel(player, qi) != 1 || pw.players[q].neutral_faction) continue;
        bool owns = false;
        for (const PerceptionObject& o : pw.objects) owns = owns || (o.alive && o.owner == qi);
        if (!owns) continue;
        if (pick < 0 || qi == w->cfg.attacker || qi == w->cfg.defender) pick = qi;
    }
    ctx.opponent = pick;
    w->decide(player, "opponent " + std::to_string(pick));
    // 2. Perception slot 6: every existing object (GOM order), then the regions.
    std::vector<TargetObjectInfo> infos;
    for (const TacticalAiObject& f : w->facts) infos.push_back(f.target);
    size_t before = list.size();
    list.add_existing_objects(infos);
    mirror(before);
    before = list.size();
    if (w->cfg.mode == TacticalMode::Land)
        list.add_land_regions(w->cfg.zones);
    else
        list.build_space_grid(w->cfg.region_map == Rect2{} ? w->cfg.map : w->cfg.region_map, w->cfg.constants.space_region_size,
                              w->perc.grid().valid());
    mirror(before);
    // Goal slot 6: base init (the land campaign credit transfer is the battle host's, CampaignBattles).
    // Execution slot 6 (0x526120): the land path finder (the player's zone graph, TACTICAL_GAPS §6.1) with one
    // Recompute_All_Pairs, next recompute staggered (PHASE2_ANSWERS Q11: slot = player index [M: PlayerClass +0x48],
    // K = player slots - humans); free store start (script thread created at start) + the existing owned objects [M].
    if (w->cfg.mode == TacticalMode::Land && w->zones.size() > 0) {
        paths = w->zones;
        recompute_paths();
        paths_built = true;
        next_recompute = galactic::first_recompute_due(static_cast<int32_t>(w->mode_frame), player, quota_players());
    }
    for (size_t i = 0; i < w->facts.size(); ++i) w->admit(*this, i, false);
    w->decide(player, "targets " + std::to_string(slots.size()) + " free store " + std::to_string(fs.size()));
}

// ================================================================ object signals / free store

void TacticalAiWorld::Impl::admit(PlayerAi& p, size_t index, bool call_lua) {
    const PerceptionObject& o = w.objects[index];
    const TacticalAiObject& f = facts[index];
    if (!o.alive || o.owner != p.player || p.fs.contains(o.id) || tf_owns(f.handle)) return;
    FreeStoreObjectInfo info = f.free_store;
    info.owned_by_player = true;
    const bool ok = cfg.mode == TacticalMode::Land ? land_free_store_admits(info) : space_free_store_admits(info);
    if (!ok || !p.fs.admit(o.id, static_cast<uint32_t>(std::max(0, o.type)))) return;
    if (call_lua && p.fs_vm) {  // On_Unit_Added (context mode = current mode)
        lua::AiEventArg a;
        a.kind = lua::AiEventArg::Object;
        a.object = f.handle;
        std::string err;
        if (!lua::call_global_function(*p.fs_vm, "On_Unit_Added", {a}, &err) && !err.empty())
            errors.push_back("free store " + std::to_string(p.player) + " On_Unit_Added: " + err);
    }
}

void TacticalAiWorld::Impl::release_to_free_store(int32_t player, ObjectHandle u, int32_t pp) {
    if (pp >= 0 && store.alive(u)) pp_unit_left(pp, id_of(u));  // 0x6c27d0: out of PP+0x108, +0x30 = 0 if it is pp
    PlayerAi* p = player_ai(player);
    if (!p || !p->ctx.initialised() || !store.alive(u) || tf_owns(u)) return;
    const PerceptionObject* o = object(id_of(u));
    if (!o) return;
    admit(*p, static_cast<size_t>(o - w.objects.data()), false);
    // Active_By_Default after the free store took the unit back or refused it (0x5ff610, A1_READINGS §28.8).
    if (o->alive && o->owner == player) host.unit_released(player, o->id, TacticalAiHost::UnitRelease::TaskForce);
}

void TacticalAiWorld::Impl::refresh() {
    host.fill(w, facts);
    if (facts.size() != w.objects.size()) facts.resize(w.objects.size());
    // refix t01: dying / removing objects (death clones: +0x3a0 |= 0xc0, S9_DEATH_CLONE_SHIELDS.md §1.6) are not
    // perceived - the perception adds (AI_TACTICAL §2 slot 11), the FreeStore base test and the target lists all skip
    // +0x3a0 bits 0x40 / 0x80. A host whose store walk lists them is filtered here (order of the rest kept).
    {
        size_t keep = 0;
        for (size_t i = 0; i < w.objects.size(); ++i) {
            if (store.dying(facts[i].handle)) continue;
            if (keep != i) {
                w.objects[keep] = std::move(w.objects[i]);
                facts[keep] = std::move(facts[i]);
            }
            ++keep;
        }
        w.objects.resize(keep);
        facts.resize(keep);
    }
    w.frame = mode_frame;
    w.index();
    gate.suspend(world.ai_suspended());
    // Object signals by snapshot comparison (header comment).
    std::vector<std::pair<int32_t, int32_t>> cur;
    cur.reserve(w.objects.size());
    for (const PerceptionObject& o : w.objects)
        if (o.alive) cur.emplace_back(o.id, o.owner);
    std::sort(cur.begin(), cur.end());
    auto in = [](const std::vector<std::pair<int32_t, int32_t>>& v, int32_t id) -> const std::pair<int32_t, int32_t>* {
        const auto it = std::lower_bound(v.begin(), v.end(), std::make_pair(id, INT32_MIN));
        return (it != v.end() && it->first == id) ? &*it : nullptr;
    };
    // removed (ascending id)
    for (const auto& [id, owner] : prev) {
        if (in(cur, id)) continue;
        perc.grid().on_object_destroyed(id);
        perc.on_object_destroyed(id);
        for (auto& pa : ai)
            if (pa && pa->ctx.initialised()) {
                pa->on_removed(id);
                pa->fs.remove(id);
            }
    }
    // added (GOM order) and owner changes
    for (size_t i = 0; i < w.objects.size(); ++i) {
        const PerceptionObject& o = w.objects[i];
        if (!o.alive) continue;
        const auto* old = in(prev, o.id);
        if (!old) {
            if (o.has_unit_ai) {  // first check at the stamp the host drew at creation (A1_READINGS §14)
                const int32_t stamp = facts[i].reprojection_stamp;
                if (stamp != TacticalAiObject::kNoReprojectionStamp) perc.grid().register_object(w, o.id, mode_frame, stamp);
                else perc.grid().register_object(w, o.id, mode_frame);
            }
            const bool produced = claim_produced(o, facts[i].handle);
            for (auto& pa : ai)
                if (pa && pa->ctx.initialised()) {
                    pa->on_added(facts[i]);
                    if (!produced) admit(*pa, i, true);
                }
        } else if (old->second != o.owner) {
            perc.grid().on_owner_changed(w, o.id);
            for (auto& pa : ai)
                if (pa && pa->ctx.initialised()) {
                    if (o.owner != pa->player) pa->fs.remove(o.id);
                    else admit(*pa, i, true);
                }
        }
    }
    prev = std::move(cur);
    // UNIT_AI service (every frame, registration order).
    perc.grid().service_all(w, mode_frame);
    // Order completion for the AI VMs' object states [port]: idle units lose Has_Active_Orders and the order lock;
    // Get_Attack_Target reads the combat target.
    for (size_t i = 0; i < w.objects.size(); ++i) {
        const PerceptionObject& o = w.objects[i];
        if (!o.alive || !player_ai(o.owner)) continue;
        lua::ScriptObjectState& s = bridge.state(o.id);
        s.attack_target = facts[i].attack_target;
        if (!facts[i].moving && facts[i].attack_target < 0) {
            s.has_orders = false;
            // A script lock's order finished (free store signal 10/11, 0x4d4aa0; A1_READINGS §28.8): release the
            // lock (0x3a8e50(obj, 0) on the script's share), then the host re-applies Active_By_Default.
            if (s.orders_locked) {
                s.orders_locked = false;
                host.unit_released(o.owner, o.id, TacticalAiHost::UnitRelease::OrderLockDone);
            }
        }
    }
    // S7 t20 (AI_TACTICAL §7.3.2): the task forces' death / removal signals (0x10 / 0x1a) - members gone this frame leave
    // at once (ascending TF id) and No_Units_Remaining is queued when nothing is left. Before, a per-service poll.
    for (auto& [id, t] : tfs) {
        const size_t before = t.units.size();
        prune_tf(t);
        if (t.units.size() < before) units_lost(t);
    }
}

// S7 t26 (AI_TACTICAL §7.3.3 item 4, TF signal handler 0x5fe5d0): on every member loss (0x10 / 0x1a, Release_Unit, a failed
// production entry) event 7 is queued unless a remaining entry of the TF +0x60 vector - members (by company type),
// claimed reinforcements not yet arrived, builds in progress [port: the entry vector] - has a type that is not
// Is_Named_Hero, not Is_Generic_Hero, not Is_Special_Weapon_In_Space and has no land LOBBING_SUPERWEAPON (0x37). So a
// task force left with only heroes / special weapons gets No_Units_Remaining, again on each further loss.
bool TacticalAiWorld::Impl::counted_entry_type(int32_t type) const {
    if (!types || type < 0 || static_cast<size_t>(type) >= types->size()) return true;
    const auto ti = static_cast<objects::TypeIndex>(type);
    constexpr objects::BehaviorTypeId kLobbingSuperweapon = 0x37;
    return !types->get_bool(ti, "Is_Named_Hero") && !types->get_bool(ti, "Is_Generic_Hero") &&
           !types->get_bool(ti, "Is_Special_Weapon_In_Space") && !types->has_behavior(ti, objects::BehaviorSet::Land, kLobbingSuperweapon);
}

void TacticalAiWorld::Impl::units_lost(TfRec& t) {
    for (const ObjectHandle u : t.units) {
        if (!store.exists(u)) continue;
        const ObjectHandle team = store.container(u);
        const objects::TypeIndex ty = team && store.exists(team) ? store.type(team) : store.type(u);  // company type 0x3973b0
        if (ty == objects::kNone || counted_entry_type(static_cast<int32_t>(ty))) return;
    }
    for (const TacticalReinforcement& e : t.pending)
        if (counted_entry_type(e.type)) return;
    for (const auto& [type, builder] : t.producing)
        if (counted_entry_type(type)) return;
    // a1j t01: a company still in its landing (0x6ee010 put its type entries on the TF +0x60 vector at the order, 0x5f9fd0).
    for (const auto& [id, s] : statuses)
        if (s.kind == StatusKind::Reinforce && !s.finished && s.tf == t.id)
            for (const auto& [key, type] : s.landings)
                if (counted_entry_type(type)) return;
    decide(t.player, "tf " + t.name + " no units remaining");
    const TfRec copy = t;
    tf_event(copy, "No_Units_Remaining", {}, true);
}

void TacticalAiWorld::Impl::service_free_store(PlayerAi& p) {
    if (!p.fs_vm) return;
    lua::LuaVm& vm = *p.fs_vm;
    vm.set_time(static_cast<double>(free_store_mode_time(mode_frame)));
    vm.set_frame(mode_frame);
    FreeStoreServiceStep step;
    free_store_service(p.sched, p.fs, mode_frame, step);
    if (step.run_service) {
        vm.service_threads();
        lua::set_number_global(vm, "LastService", p.sched.last_service);
    }
    if (step.run_unit_service) {
        for (const int32_t id : step.units) {
            // Entry +0x30 set = reserved by a PotentialPlan: not serviced. 0x4d5a70 tests nothing else (no order-lock
            // test, A1_READINGS §28.2 / §28.5). Re-checked per unit: a callback may have released or reserved it.
            if (!p.fs.contains(id) || p.fs.reservation(id) >= 0) continue;
            const ObjectHandle h = store.find(id);
            if (!store.alive(h)) continue;
            lua::AiEventArg a;
            a.kind = lua::AiEventArg::Object;
            a.object = h;
            std::string err;
            if (!lua::call_global_function(vm, "On_Unit_Service", {a}, &err) && !err.empty())
                errors.push_back("free store " + std::to_string(p.player) + " On_Unit_Service: " + err);
        }
        lua::set_number_global(vm, "LastUnitService", p.sched.last_unit_service);
    }
    drain_world_requests(p.player, -1);
    append_script_errors(vm, "free store " + std::to_string(p.player) + ": ", errors);  // S7 t27: with SCRIPT ERROR lines
    lua::clear_script_logs(vm);
}

int32_t TacticalAiWorld::Impl::free_store_count(int32_t script, int) {
    PlayerAi* p = player_ai(script_player(script));
    return p ? static_cast<int32_t>(p->fs.size()) : 0;
}

bool TacticalAiWorld::Impl::free_store_has(int32_t script, ObjectHandle unit) {
    PlayerAi* p = player_ai(script_player(script));
    if (!p || !store.alive(unit)) return false;
    return p->fs.contains(id_of(unit));
}

bool TacticalAiWorld::Impl::tf_collect_free_units(int32_t tf) {
    TfRec* t = tf_rec(tf);
    PlayerAi* p = t ? player_ai(t->player) : nullptr;
    if (!p) return false;
    std::vector<int32_t> ids;
    p->fs.units(ids, false);
    for (const int32_t id : ids) {
        const ObjectHandle h = store.find(id);
        if (!unit_ok(h, t->player)) continue;
        p->fs.remove(id);
        t->units.push_back(h);
    }
    return true;
}

// ---- Produce_Force (A2 tactical: the PotentialPlan's selection for this task force) ----

// What the solve picked for this TF: free-store units (still free, alive, owned) join at once; reinforcement-pool
// entries become pending reinforcements (Reinforce drops them); UC_ picks become pad reservations (Build / Build_All).
// "To be produced" picks (no source) are not bought [port: no tactical AIBuildTask purchase path in the host; the
// Reinforcement marshaller vetoes types without a pool entry or pad, so only units lost since the solve are missing].
void TacticalAiWorld::Impl::claim_force(TfRec& t) {
    PlayerAi* p = player_ai(t.player);
    const auto pit = plans.find(t.plan);
    if (!p || pit == plans.end()) return;
    const auto ppit = pps.find(pit->second.pp);
    uint32_t missing = 0;
    std::vector<lua::AiEventArg> failed;  // HIJACK failures (Production_Failed type names)
    if (ppit != pps.end()) {
        const std::vector<TacticalReinforcement> pool = host.reinforcement_pool(t.player);
        for (const TacticalPpPick& k : ppit->second.picks) {
            if (k.tf != static_cast<int32_t>(t.def)) continue;
            switch (k.source) {
                case PpSource::FreeStore: {
                    const ObjectHandle h = store.find(k.ref);
                    if (!p->fs.contains(k.ref) || !unit_ok(h, t.player) || tf_owns(h)) {
                        ++missing;
                        break;
                    }
                    p->fs.remove(k.ref);
                    t.units.push_back(h);
                    break;
                }
                case PpSource::Reinforcement: {
                    // One pool entry can serve several selections of this TF (a company, §1.5): the TF's distinct pool
                    // indices in first-occurrence order (0x6c0520).
                    if (std::any_of(t.pending.begin(), t.pending.end(), [&k](const TacticalReinforcement& r) { return r.key == k.ref; }))
                        break;
                    const auto it = std::find_if(pool.begin(), pool.end(), [&k](const TacticalReinforcement& r) { return r.key == k.ref; });
                    bool held = false;
                    for (const auto& [id, o] : tfs)
                        for (const TacticalReinforcement& r : o.pending) held = held || r.key == k.ref;
                    if (it == pool.end() || held) {
                        ++missing;
                        break;
                    }
                    t.pending.push_back(*it);
                    break;
                }
                case PpSource::Build:  // skai: claim_pads may have taken it already
                    if (std::find(t.reservations.begin(), t.reservations.end(), std::make_pair(k.ref, k.type)) == t.reservations.end())
                        t.reservations.emplace_back(k.ref, k.type);
                    break;
                case PpSource::Produce: {
                    const PlanDefinition& def = library.plans[pit->second.plan];
                    if (!def.magic_plan) {
                        // AI_GALACTIC_3 §1.11 [H branch]: only a skirmish build plan (AllowFreeStoreUnits = false) buys:
                        // Production {T, builder} for each builder of the free store's list in order until one starts;
                        // none -> FAILED (Production_Failed). Campaign battles never buy through the AI.
                        const bool uc = k.type >= 0 && static_cast<size_t>(k.type) < w.types.size() &&
                                        w.types[static_cast<size_t>(k.type)].under_construction;
                        if (cfg.campaign || def.allow_free_store_units || uc || k.type < 0) {
                            ++missing;
                            break;
                        }
                        int32_t builder = -1;
                        for (const int32_t b : host.tactical_builders(t.player, k.type)) {
                            cmd::EventData ev(cmd::EventType::Production);
                            ev.set("type_index", static_cast<uint32_t>(k.type)).set("location", static_cast<uint32_t>(b));
                            if (order(t.player, ev, "produce " + type_name(k.type) + " at " + std::to_string(b) + " (tf " + t.name + ")") ==
                                cmd::RejectReason::None) {
                                builder = b;
                                break;
                            }
                        }
                        if (builder < 0) {
                            ++missing;
                            lua::AiEventArg a;
                            a.kind = lua::AiEventArg::String;
                            a.text = type_name(k.type);
                            failed.push_back(a);
                            break;
                        }
                        t.producing.emplace_back(k.type, builder);
                        break;
                    }
                    // MagicPlan HIJACK_UNIT, tactical with parent (AI_GALACTIC_3 §4.3): the host spawns the unit in the
                    // parent galactic mode and appends it to the battle's reinforcement record; the new pool entry is
                    // registered with this TF (TF.Reinforce drops it). FAILED -> Production_Failed(tf, type).
                    const bool steal = def.magic_plan_stealing || (k.type >= 0 && static_cast<size_t>(k.type) < w.types.size() &&
                                                                   w.types[static_cast<size_t>(k.type)].is_hero);
                    const TacticalReinforcement r = host.hijack_unit(t.player, k.type, steal);
                    decide(t.player, "hijack " + type_name(k.type) + (r.key >= 0 ? " -> pool " + std::to_string(r.key) : " failed"));
                    if (r.key < 0) {
                        ++missing;
                        lua::AiEventArg a;
                        a.kind = lua::AiEventArg::String;
                        a.text = type_name(k.type);
                        failed.push_back(a);
                        break;
                    }
                    t.pending.push_back(r);
                    break;
                }
            }
        }
        release_pp(pit->second.pp, static_cast<int32_t>(t.def));
    }
    decide(t.player, "tf " + t.name + " produce: " + std::to_string(t.units.size()) + " units, " + std::to_string(t.pending.size()) +
                         " pending reinforcements, " + std::to_string(t.reservations.size()) + " pads" +
                         (t.producing.empty() ? std::string() : ", " + std::to_string(t.producing.size()) + " in production") +
                         (missing ? ", " + std::to_string(missing) + " unavailable" : std::string()));
    for (const lua::AiEventArg& a : failed) {
        const TfRec copy = t;
        tf_event(copy, "Production_Failed", {a});
    }
    if (!failed.empty()) units_lost(t);  // S7 t20: the production-failed path (§7.3.2)
}

// ---- A2 tactical: the PotentialPlan solve (slot 10) and its reservations ----

bool TacticalAiWorld::Impl::solve_pp(PlayerAi& p, uint32_t plan, const Goal& g, TacticalPpResult& out, bool first_solve) {
    TacticalPpInput in;
    pp_input(p, plan, g, first_solve, false, in);
    in.rng = &rng.synced();
    return solve_tactical_pp(in, out);
}

void TacticalAiWorld::Impl::pp_input(PlayerAi& p, uint32_t plan, const Goal& g, bool first_solve, bool snapshot,
                                     TacticalPpInput& in) {
    in.mode = cfg.mode;
    in.def = &library.plans[plan];
    in.plan = plan;
    in.goal_type = g.type;
    in.player = p.player;
    if (w.is_player(p.player)) {
        in.faction = w.players[static_cast<size_t>(p.player)].faction;
        in.credits = w.players[static_cast<size_t>(p.player)].credits;
        in.player_tech = static_cast<int32_t>(w.players[static_cast<size_t>(p.player)].tech_level);
    }
    in.time_limit = g.type < defs.goals.size() ? defs.goals[g.type].time_limit : -1.0f;
    if (g.target >= 0 && static_cast<size_t>(g.target) < p.slots.size()) {
        const Slot& s = p.slots[static_cast<size_t>(g.target)];
        in.has_target = s.alive;
        in.target_position = slot_position(s);
        if (s.t.is_unit()) {
            in.target_object = s.t.object_id;
            const PerceptionObject* o = object(s.t.object_id);
            in.target_zone = o && o->zone >= 0 ? o->zone : zone_at(in.target_position);
        } else {
            in.target_zone = s.t.zone;
        }
    }
    in.types = &w.types;
    in.category_mask = perc.config().category_mask;
    // FreeStore Setup A stages (A1_READINGS §25.3), land with a target: per TF the flood slot 6 pick around the goal
    // target's zone at the goal type's Reachability (link mask: TF +0x94 is not parsed [port: -1]; RequireUniqueStage
    // excludes / RequireMatchingStage restricts to the stages so far), else the goal target itself.
    struct Stage {
        int32_t zone = -1;
        Vec3f position;
    };
    std::vector<Stage> stages;
    if (cfg.mode == TacticalMode::Land && in.has_target && zones.size() > 0) {
        const Reachability lvl = g.type < defs.goals.size() ? defs.goals[g.type].reachability : Reachability::Any;
        std::vector<int32_t> so_far;
        for (const TaskForceDef& d : in.def->task_forces) {
            const int32_t z = in.target_zone < 0 ? -1
                                                 : p.flood_pick(in.target_zone, lvl, 0xFFFFFFFFu, d.require_unique_stage ? &so_far : nullptr,
                                                                d.require_matching_stage ? &so_far : nullptr);
            Stage st;
            st.zone = z >= 0 ? z : in.target_zone;
            if (z >= 0) {
                const HintZone& hz = zones.zone(static_cast<uint32_t>(z));
                st.position = {hz.center[0], hz.center[1], hz.center[2]};
            } else {
                st.position = in.target_position;
            }
            so_far.push_back(z);  // [port] a null stage (the goal target) enters the lists as -1
            stages.push_back(st);
        }
    }
    std::vector<int32_t> ids;
    p.fs.units(ids, false);
    for (const int32_t id : ids) {
        const PerceptionObject* o = object(id);
        if (!o || !o->alive) continue;
        PpFreeUnit u;
        u.id = id;
        u.type = o->type;
        u.position = o->position;
        u.health = o->health;
        // FreeStore Setup A 0x7287a0: a candidate needs PP+0x104 == 0 (the first solve) or +0x30 == 0 (A1_READINGS §28.5).
        u.reserved = !first_solve && p.fs.reservation(id) >= 0;
        if (!stages.empty()) {
            // AI_GALACTIC_3 §1.2: Σ (Low_Threat slot-7 cost from the unit's zone)² over the finite stages + Σ 2-D
            // distance² to each stage, divided by the stage count when a cost was finite.
            const int32_t uz = o->zone >= 0 ? o->zone : zone_at(o->position);
            double sum = 0.0;
            bool finite = false;
            for (const Stage& st : stages) {
                const double c = zone_reachability_cost(p.zone_paths(), uz, st.zone, Reachability::LowThreat, cfg.constants);
                if (c < kUnreachable) {
                    sum += c * c;
                    finite = true;
                }
                const double dx = static_cast<double>(o->position.x) - st.position.x;
                const double dy = static_cast<double>(o->position.y) - st.position.y;
                sum += dx * dx + dy * dy;
            }
            if (finite) u.stage_key = sum / static_cast<double>(stages.size());
        }
        in.free_units.push_back(u);
    }
    // OPT2 t10: inside a proposal slice the first solves share the host's answers (PpSliceHost).
    PpSliceHost& sh = pp_slice;
    const bool shared = first_solve && sh.player == p.player;
    if (shared && !sh.pool_ok) {
        sh.pool = host.reinforcement_pool(p.player);
        sh.pool_ok = true;
    }
    std::vector<TacticalReinforcement> own_pool;
    if (!shared) own_pool = host.reinforcement_pool(p.player);
    std::set<int32_t> held;
    for (const auto& [id, t] : tfs)
        if (t.player == p.player)
            for (const TacticalReinforcement& r : t.pending) held.insert(r.key);
    for (const TacticalReinforcement& r : shared ? sh.pool : own_pool) {
        PpPoolEntry e;
        e.key = r.key;
        e.type = r.type;
        e.population = r.population;
        e.units = r.units;
        e.member_type = r.member_type;
        e.member_create_team = r.member_create_team;
        e.count = r.count;
        e.reserved = held.count(r.key) != 0 || pool_reserved.count(r.key) != 0;
        in.pool.push_back(e);
    }
    std::set<int32_t> pads_held;
    for (const auto& [id, t] : tfs)
        for (const auto& rv : t.reservations) pads_held.insert(rv.first);
    if (shared && !sh.pads_ok) {
        sh.pads = host.build_pads(p.player);
        sh.pads_ok = true;
    }
    std::vector<TacticalPad> own_pads;
    if (!shared) own_pads = host.build_pads(p.player);
    for (const TacticalPad& pd : shared ? sh.pads : own_pads) {
        PpPad x;
        x.id = pd.id;
        x.position = pd.position;
        x.accepts = pd.accepts;
        x.reserved = pads_held.count(pd.id) != 0 || pad_reserved.count(pd.id) != 0;
        in.pads.push_back(std::move(x));
    }
    if (shared && !sh.pop_ok) {
        sh.pop_cap = host.population_cap(p.player);
        sh.pop_used = host.population_used(p.player);
        sh.pop_ok = true;
    }
    in.pop_cap = shared ? sh.pop_cap : host.population_cap(p.player);
    in.pop_used = shared ? sh.pop_used : host.population_used(p.player);
    in.campaign = cfg.campaign;
    in.health_threshold = cfg.constants.health_low_percent_threshold;
    const int32_t builder = p.player;
    in.contrast = host.contrast();
    in.contrast_multiplier = p.contrast_mult;
    in.recent_failures = in.has_target ? p.ctx.learning.recent_plan_failures(plan, g.target, now) : 0;
    const int32_t self = p.player;
    const Vec3f at = in.target_position;
    const int32_t zone = in.target_zone;
    if (!snapshot) {
        in.can_build_now = [this, builder](int32_t type) { return host.can_build_now(builder, type); };
        in.enemy_strength = [this, self, at, zone](uint64_t mask) { return enemy_strength_at(self, at, zone, mask); };
        return;
    }
    // OPT2 t10 snapshot. TacticalBuild asks can_build_now only for team types of a plan without free-store units / magic
    // (tactical_pp.cpp TacticalBuildM::score); the answers are shared by the slice (PpSliceHost, cleared by any order).
    if (!(in.def->allow_free_store_units || in.def->magic_plan)) {
        std::vector<int32_t> ok;
        for (const TaskForceDef& d : in.def->task_forces)
            for (const TeamDef& t : d.teams)
                for (const int32_t ty : pp_team_types(d, t, w.types, in.category_mask)) {
                    if (std::find(ok.begin(), ok.end(), ty) != ok.end()) continue;
                    bool yes = false;
                    if (shared) {  // the pool / pads cache's condition
                        std::vector<int8_t>& c = pp_slice.can_build;
                        if (c.size() < w.types.size()) c.resize(w.types.size(), int8_t{-1});
                        int8_t& v = c[static_cast<size_t>(ty)];
                        if (v < 0) v = static_cast<int8_t>(host.can_build_now(builder, ty) ? 1 : 0);
                        yes = v == 1;
                    } else {
                        yes = host.can_build_now(builder, ty);
                    }
                    if (yes) ok.push_back(ty);
                }
        in.can_build_now = [types_ok = std::move(ok)](int32_t type) {
            return std::find(types_ok.begin(), types_ok.end(), type) != types_ok.end();
        };
    }
    // Contrast Setup A asks the total (all bits) and every contrast row's enemy mask, only with a target and no IgnoreTarget.
    if (in.has_target && !in.def->ignore_target) {
        std::vector<uint64_t> masks{~uint64_t{0}};
        if (in.contrast)
            for (size_t j = 0; j < in.contrast->size(); ++j) masks.push_back(in.contrast->rows[j].enemy_mask);
        std::vector<double> v = enemy_strengths_at(self, at, zone, masks);
        in.enemy_strength = [mk = std::move(masks), sums = std::move(v)](uint64_t mask) {
            for (size_t k = 0; k < mk.size(); ++k)
                if (mk[k] == mask) return sums[k];
            return 0.0;
        };
    }
}

// Enemy strength at a tactical target for a category mask (the Contrast requirement 0x6c5de0 / 0x5f2430 in tactical)
// [M]: the combat power of live objects hostile to `player` - land: in the target's hint zone (every hostile object
// when the map has no zones); space: inside the AI_SpaceEvaluatorRegionSize square centred on the target.
double TacticalAiWorld::Impl::enemy_strength_at(int32_t player, const Vec3f& at, int32_t zone, uint64_t mask) const {
    double s = 0.0;
    const float half = cfg.constants.space_region_size * 0.5f;
    for (const PerceptionObject& o : w.objects) {
        if (!o.alive || o.owner < 0 || o.owner == player || w.rel(player, o.owner) != 1) continue;
        const PerceptionType* t = w.type_of(o);
        if (!t || (t->category & mask) == 0) continue;
        if (cfg.mode == TacticalMode::Land) {
            if (zone >= 0 && zones.size() > 0 && (o.zone >= 0 ? o.zone : zone_at(o.position)) != zone) continue;
        } else if (std::fabs(o.position.x - at.x) > half || std::fabs(o.position.y - at.y) > half) {
            continue;
        }
        s += static_cast<double>(o.combat_power);
    }
    return s;
}

std::vector<double> TacticalAiWorld::Impl::enemy_strengths_at(int32_t player, const Vec3f& at, int32_t zone,
                                                               std::span<const uint64_t> masks) const {
    std::vector<double> s(masks.size(), 0.0);
    const float half = cfg.constants.space_region_size * 0.5f;
    for (const PerceptionObject& o : w.objects) {
        if (!o.alive || o.owner < 0 || o.owner == player || w.rel(player, o.owner) != 1) continue;
        const PerceptionType* t = w.type_of(o);
        if (!t) continue;
        if (cfg.mode == TacticalMode::Land) {
            if (zone >= 0 && zones.size() > 0 && (o.zone >= 0 ? o.zone : zone_at(o.position)) != zone) continue;
        } else if (std::fabs(o.position.x - at.x) > half || std::fabs(o.position.y - at.y) > half) {
            continue;
        }
        for (size_t k = 0; k < masks.size(); ++k)
            if ((t->category & masks[k]) != 0) s[k] += static_cast<double>(o.combat_power);
    }
    return s;
}

TacticalFreeStore* TacticalAiWorld::Impl::pp_store(int32_t handle) {
    const auto it = pps.find(handle);
    PlayerAi* p = it == pps.end() ? nullptr : player_ai(it->second.player);
    return p ? &p->fs : nullptr;
}

// 0x6c4a50 (A1_READINGS §28.6 step 3): the new proposal's PP is solved again (PP+0x104 >= 1: units another PP reserved
// are skipped) and its selection replaced; a failed solve = the plan cannot be activated. [M] 0x6c4a50 runs the same
// solver 0x6c3da0, so the re-solve draws its own synced numbers (Plan Setup C start team, IgnoreTarget contrast) in
// Resolve_Category order. [port] The goal keeps Init's cost / ETA.
bool TacticalAiWorld::Impl::resolve_pp(PlayerAi& p, const Goal& g) {
    const auto it = pps.find(g.pp_handle);
    if (it == pps.end()) return false;
    TacticalPpResult out;
    if (!solve_pp(p, it->second.plan, g, out, false)) {
        ++stats[defs.goals[g.type].name + " re-solve failed (" + library.plans[it->second.plan].name + ", failed " + out.last_veto + ")"];
        return false;
    }
    it->second.picks = std::move(out.picks);
    return true;
}

// 0x6c2d60 (A1_READINGS §28.4): +0x30 = PP (0x6c2b20, overwriting any owner) for every PP+0x108 unit still on the
// free store; the first call (PP+0x100) also reserves the reinforcement-pool and build-pad slots [port: the tactical
// credits reservation is not modelled].
void TacticalAiWorld::Impl::reserve_pp(int32_t handle) {
    const auto it = pps.find(handle);
    TacticalFreeStore* fs = pp_store(handle);
    if (it == pps.end() || !fs) return;
    const bool first = !it->second.reserved;
    it->second.reserved = true;
    for (const TacticalPpPick& k : it->second.picks) {
        if (k.source == PpSource::FreeStore) fs->reserve(k.ref, handle);
        if (first && k.source == PpSource::Reinforcement) pool_reserved[k.ref] = handle;
        if (first && k.source == PpSource::Build) pad_reserved[k.ref] = handle;
    }
}

// 0x6c0be0 at Commit start: the PP's +0x30 entries are cleared until 0x6c2d60 re-asserts them (§28.6 step 2).
void TacticalAiWorld::Impl::suspend_pp(int32_t handle) {
    const auto it = pps.find(handle);
    TacticalFreeStore* fs = pp_store(handle);
    if (it == pps.end() || !fs) return;
    for (const TacticalPpPick& k : it->second.picks)
        if (k.source == PpSource::FreeStore) fs->unreserve(k.ref, handle);
}

// 0x6c45e0 (from 0x6c47c0, reserved PP): invalid when a PP+0x108 unit still on the free store is reserved by another PP.
bool TacticalAiWorld::Impl::pp_stolen(int32_t handle) {
    const auto it = pps.find(handle);
    if (it == pps.end() || !it->second.reserved) return false;
    const PlayerAi* p = player_ai(it->second.player);
    if (!p) return false;
    for (const TacticalPpPick& k : it->second.picks) {
        if (k.source != PpSource::FreeStore) continue;
        const int32_t owner = p->fs.reservation(k.ref);
        if (owner >= 0 && owner != handle) {
            decide(it->second.player, "pp " + std::to_string(handle) + " " + library.plans[it->second.plan].name + ": unit " +
                                          std::to_string(k.ref) + " reserved by pp " + std::to_string(owner) + ", plan invalid");
            return true;
        }
    }
    return false;
}

// 0x6c27d0 (PP, unit): +0x30 = 0 if it is this PP, and the unit leaves PP+0x108 (later re-asserts skip it).
void TacticalAiWorld::Impl::pp_unit_left(int32_t handle, int32_t unit) {
    const auto it = pps.find(handle);
    if (it == pps.end()) return;
    if (TacticalFreeStore* fs = pp_store(handle)) fs->unreserve(unit, handle);
    auto& v = it->second.picks;
    v.erase(std::remove_if(v.begin(), v.end(), [unit](const TacticalPpPick& k) { return k.source == PpSource::FreeStore && k.ref == unit; }),
            v.end());
}

// Releases the reservations of a PP (all: 0x6c2320 from the destructor; or only those of one task force once its
// Produce_Force has gathered them).
void TacticalAiWorld::Impl::release_pp(int32_t handle, int32_t tf_def) {
    const auto it = pps.find(handle);
    if (it == pps.end()) return;
    TacticalFreeStore* fs = pp_store(handle);
    for (const TacticalPpPick& k : it->second.picks) {
        if (tf_def >= 0 && k.tf != tf_def) continue;
        if (k.source == PpSource::FreeStore) {
            if (fs) fs->unreserve(k.ref, handle);
            continue;
        }
        std::map<int32_t, int32_t>* m = k.source == PpSource::Reinforcement ? &pool_reserved
                                        : k.source == PpSource::Build ? &pad_reserved : nullptr;
        if (!m) continue;
        const auto r = m->find(k.ref);
        if (r != m->end() && r->second == handle) m->erase(r);
    }
}

// a1g t04 + t01 - 0x6bf3d0 (PP, unit) from ReinforceBlockStatus (A1_READINGS §28.4): a unit a land TF.Reinforce
// delivered joins its plan's PP+0x108 (reinforce_join_pp) and is reserved by that PP (+0x30 = PP). [port] The port keeps
// task-force units off the free store (0x4d5000 erases the node when a task force takes a unit), so the delivered unit is
// not admitted: +0x30 = PP is set only if it is on the store. The new object is not perceived until the next refresh, so
// the delivered entry's type stands for it. When it fills a null slot that held a pool / pad reservation of this PP, the
// reservation is released here (the port ties those tables to the slot's pick).
void TacticalAiWorld::Impl::reinforce_joined(const TfRec& t, int32_t unit, int32_t type) {
    const auto pi = plans.find(t.plan);
    if (pi == plans.end() || pi->second.pp < 0) return;
    const int32_t handle = pi->second.pp;
    const auto it = pps.find(handle);
    if (it == pps.end()) return;
    if (TacticalFreeStore* fs = pp_store(handle); fs && fs->contains(unit)) fs->reserve(unit, handle);
    TacticalPpPick filled;
    const bool slot = reinforce_join_pp(it->second.picks, unit, type, static_cast<int32_t>(t.def), &filled);
    if (slot) {
        std::map<int32_t, int32_t>* m = filled.source == PpSource::Reinforcement ? &pool_reserved
                                        : filled.source == PpSource::Build ? &pad_reserved : nullptr;
        if (m) {
            const auto r = m->find(filled.ref);
            if (r != m->end() && r->second == handle) m->erase(r);
        }
        decide(t.player, "pp " + std::to_string(handle) + ": reinforce unit " + std::to_string(unit) + " " + type_name(type) +
                             " fills a null slot");
    }
}

// ================================================================ plans

int32_t TacticalAiWorld::Impl::instantiate(PlayerAi& p, Goal& g) {
    if (g.plan == kAiNone) return -1;
    std::string err;
    std::unique_ptr<lua::LuaVm> vm = runtime->acquire(g.plan, &err);
    if (!vm) {
        errors.push_back(err);
        return -1;
    }
    const PlanDefinition& def = library.plans[g.plan];
    PlanInstance pi;
    pi.id = next_plan++;
    pi.player = p.player;
    pi.plan = g.plan;
    pi.goal_type = g.type;
    pi.slot = g.target;
    pi.pp = g.pp_handle;  // A2 tactical
    lua::attach_ai_host(*vm, this, pi.id);
    lua::attach_ai_tactical_host(*vm, this);
    vm->set_time(static_cast<double>(free_store_mode_time(mode_frame)));
    vm->set_frame(mode_frame);
    const Slot* s = g.target >= 0 ? &p.slots[static_cast<size_t>(g.target)] : nullptr;
    if (s) {
        lua::set_ai_target_global(*vm, "AITarget", target_id(p.player, g.target));
        ObjectHandle obj;
        if (s->t.is_unit()) {
            obj = store.find(s->t.object_id);
            if (const PerceptionObject* o = object(s->t.object_id)) pi.target_owner = o->owner;
        }
        vm->set_object_global("Target", store.alive(obj) ? obj : ObjectHandle{});
    } else {
        lua::set_nil_global(*vm, "AITarget");
        lua::set_nil_global(*vm, "Target");
    }
    lua::set_budget_global(*vm, pi.id);
    lua::set_player_global(*vm, "PlayerObject", p.player);
    const lua::AiTaskForceKind kind = cfg.mode == TacticalMode::Land ? lua::AiTaskForceKind::Land : lua::AiTaskForceKind::Space;
    std::string names;
    for (size_t i = 0; i < def.task_forces.size(); ++i) {
        TfRec tf;
        tf.id = next_tf++;
        tf.plan = pi.id;
        tf.player = p.player;
        tf.def = static_cast<uint32_t>(i);
        tf.name = def.task_forces[i].name;
        tf.kind = kind;
        lua::set_task_force_global(*vm, tf.name, tf.id, kind);
        if (vm->create_thread(tf.name + "_Thread") < 0) continue;  // a missing thread removes the TF
        names += " " + tf.name;
        pi.tfs.push_back(tf.id);
        tfs.emplace(tf.id, std::move(tf));
    }
    pi.vm = std::move(vm);
    pi.live_threads = static_cast<uint32_t>(pi.tfs.size());
    std::string where = "global";
    if (s) where = s->t.name;
    decide(p.player, "plan " + std::to_string(pi.id) + " " + def.name + " for " + defs.goals[g.type].name + " @" + where + names);
    const int32_t id = pi.id;
    plans.emplace(id, std::move(pi));
    return id;
}

void TacticalAiWorld::Impl::end_plan(int32_t id, bool by_goal_system) {
    const auto it = plans.find(id);
    if (it == plans.end() || it->second.ended) return;
    PlanInstance& pi = it->second;
    pi.ended = true;
    for (auto s = statuses.begin(); s != statuses.end();) s = s->second.plan == id ? statuses.erase(s) : std::next(s);
    for (const int32_t t : pi.tfs) {
        const auto tit = tfs.find(t);
        if (tit == tfs.end()) continue;
        const std::vector<ObjectHandle> units = tit->second.units;
        const int32_t player = tit->second.player;
        tfs.erase(tit);
        // AI_GALACTIC §1.9.4: back to the free store (plan end 0x5f6a50 step 3 -> 0x6c27d0 with the plan's PP).
        for (const ObjectHandle u : units) release_to_free_store(player, u, pi.pp);
    }
    if (PlayerAi* pa = player_ai(pi.player)) {
        const float dur = pi.goal_type < defs.goals.size() ? defs.goals[pi.goal_type].tracking_duration : 0.0f;
        pa->ctx.learning.record_goal_outcome(pi.goal_type, pi.slot, pi.plan, pi.result, dur, now);
        if (!by_goal_system) pa->ctx.goals.plan_ended(id);
    }
    decide(pi.player, "plan " + std::to_string(id) + " " + library.plans[pi.plan].name + " ended " + (pi.result ? "success" : "failure") +
                          (by_goal_system ? " (released)" : ""));
    if (pi.vm) {
        append_script_errors(*pi.vm, library.plans[pi.plan].name + ": ", errors);  // S7 t27: with SCRIPT ERROR lines
        runtime->release(pi.plan, std::move(pi.vm));
    }
    plans.erase(it);
}

void TacticalAiWorld::Impl::tf_event(const TfRec& tf, std::string_view event, const std::vector<lua::AiEventArg>& args,
                                    bool always_queue) {
    const auto it = plans.find(tf.plan);
    if (it == plans.end() || !it->second.vm) return;
    std::string err;
    const bool queued = always_queue || current_plan == tf.plan;
    if (!lua::dispatch_task_force_event(*it->second.vm, tf.id, tf.kind, tf.name, event, args, queued, &err) && !err.empty())
        errors.push_back(library.plans[it->second.plan].name + " " + std::string(event) + ": " + err);
}

// STRATEGIC_GAPS §5.3 [H]: the task-force signal handler (case 0x21) checks the hard point's type against the TF's
// subscription list (Add/Remove/Clear_Opportunity_Fire_Event_Subscription(s)) and raises plan event 17
// Hardpoint_Opportunity_Target_Acquired (unit wrapper, target wrapper). The owner is matched as a TF unit or through its
// team container [M: the TF member that receives a team member's signal].
void TacticalAiWorld::Impl::opportunity_events(const std::vector<OpportunityTargetEvent>& events) {
    for (const OpportunityTargetEvent& e : events) {
        const ObjectHandle unit = store.find(e.owner);
        const ObjectHandle target = store.find(e.target);
        if (!store.alive(unit) || !store.alive(target)) continue;
        const ObjectHandle team = store.container(unit);
        int32_t hit = -1;
        for (const auto& [id, t] : tfs) {  // ascending TF id; a unit is held by at most one TF
            for (const ObjectHandle u : t.units)
                if (u == unit || (team && u == team)) hit = id;
            if (hit >= 0) break;
        }
        const TfRec* t = hit >= 0 ? tf_rec(hit) : nullptr;
        if (!t) continue;
        const std::vector<int32_t>* subs = opportunity_fire_subscriptions(t->id);
        if (!subs || std::find(subs->begin(), subs->end(), e.hardpoint_type) == subs->end()) continue;
        lua::AiEventArg a, b;
        a.kind = b.kind = lua::AiEventArg::Object;
        a.object = unit;
        b.object = target;
        decide(t->player, "event Hardpoint_Opportunity_Target_Acquired (tf " + t->name + ")");
        const TfRec copy = *t;
        tf_event(copy, "Hardpoint_Opportunity_Target_Acquired", {a, b});
    }
}

void TacticalAiWorld::Impl::drain_world_requests(int32_t player, int32_t plan) {
    for (const lua::DesireBonusRequest& r : world.desire_bonuses) {
        PlayerAi* t = player_ai(r.player);
        const uint32_t g = defs.find_goal(r.goal);
        if (!t || g == kAiNone) continue;
        int32_t slot = -1;
        for (size_t i = 0; i < t->slots.size() && slot < 0; ++i)
            if (t->slots[i].alive && t->slots[i].t.is_unit() && t->slots[i].t.object_id == r.target) slot = static_cast<int32_t>(i);
        if (slot < 0) continue;
        PendingBonus b;
        b.goal_type = g;
        b.target = slot;
        b.bonus = static_cast<float>(r.bonus);
        b.deadline = now + static_cast<int32_t>(static_cast<float>(kAiFps) * static_cast<float>(r.duration < 0.0 ? 0.0 : r.duration));
        t->bonuses.push_back(b);
    }
    world.desire_bonuses.clear();
    for (const int32_t p : world.purge_goal_requests)
        if (PlayerAi* t = player_ai(p)) t->ctx.goals.purge(plan);
    world.purge_goal_requests.clear();
    world.budget_flushes.clear();     // no tactical budget (§4.1)
    world.tactical_releases.clear();  // [port] Release_Credits_For_Tactical: the S10 host's credit transfer has no release yet
    (void)player;
}

void TacticalAiWorld::Impl::service_plans(int32_t player) {
    std::vector<int32_t> ids;
    for (const auto& [id, pi] : plans)
        if (pi.player == player && !pi.ended) ids.push_back(id);
    for (const int32_t id : ids) {
        auto it = plans.find(id);
        if (it == plans.end() || it->second.ended) continue;
        PlanInstance& pi = it->second;
        // Original_Target_Owner_Changed for unit targets (old, new owner).
        if (pi.slot >= 0 && pi.target_owner >= 0) {
            const Slot* s = slot_of(target_id(player, pi.slot));
            const PerceptionObject* o = s && s->alive ? object(s->t.object_id) : nullptr;
            if (o && o->owner != pi.target_owner) {
                lua::AiEventArg a, b;
                a.kind = b.kind = lua::AiEventArg::Player;
                a.id = pi.target_owner;
                b.id = o->owner;
                pi.target_owner = o->owner;
                for (const int32_t t : std::vector<int32_t>(pi.tfs))
                    if (const TfRec* rec = tf_rec(t)) tf_event(*rec, "Original_Target_Owner_Changed", {a, b});
            }
        }
        it = plans.find(id);
        if (it == plans.end()) continue;
        lua::LuaVm& vm = *it->second.vm;
        vm.set_time(static_cast<double>(free_store_mode_time(mode_frame)));
        vm.set_frame(mode_frame);
        current_plan = id;
        vm.service_threads();
        current_plan = -1;
        drain_world_requests(player, id);
        it = plans.find(id);
        if (it == plans.end()) continue;
        it->second.live_threads = static_cast<uint32_t>(it->second.vm->thread_count());
        if (it->second.vm->exit_requested() || it->second.live_threads == 0) end_plan(id, false);
    }
}

// ================================================================ statuses

void TacticalAiWorld::Impl::service_statuses(int32_t player) {
    std::vector<int32_t> ids;
    for (const auto& [id, s] : statuses)
        if (!s.finished && script_player(s.plan) == player) ids.push_back(id);
    for (const int32_t id : ids) {
        const auto it = statuses.find(id);
        if (it == statuses.end() || it->second.finished) continue;
        for (int k = 0; k < 50 && !it->second.finished; ++k)
            if (!step_status(it->second)) break;
    }
    for (auto s = statuses.begin(); s != statuses.end();)
        s = (s->second.finished && plans.find(s->second.plan) == plans.end()) ? statuses.erase(s) : std::next(s);
    // S7 t20: No_Units_Remaining is raised from the removal signals (refresh, release, production failure), not polled here.
}

bool TacticalAiWorld::Impl::step_status(StatusRec& s) {
    auto finish = [&s](bool ok) {
        s.finished = true;
        s.result = ok ? lua::AiStatusResult::True : lua::AiStatusResult::False;
        return false;
    };
    TfRec* t = tf_rec(s.tf);
    switch (s.kind) {
        case StatusKind::Done: return finish(true);
        case StatusKind::Failed: return finish(false);
        case StatusKind::ProduceForce:
            if (!t) return finish(false);
            prune_tf(*t);
            // AI_GALACTIC_3 §1.11: a started tactical build registers the unit with the plan and waits for it. An entry
            // whose builder is gone is dropped [port: the build signals 0x22 / 3 are not modelled].
            if (const size_t before = t->producing.size(); before > 0) {
                t->producing.erase(std::remove_if(t->producing.begin(), t->producing.end(),
                                                  [this](const std::pair<int32_t, int32_t>& e) { return !store.alive(store.find(e.second)); }),
                                   t->producing.end());
                if (t->producing.size() < before) units_lost(*t);  // S7 t20: a build entry failed
            }
            if (!t->producing.empty()) return false;
            return finish(!t->units.empty() || !t->pending.empty() || !t->reservations.empty());
        case StatusKind::Move: {
            if (!t && s.unit) {  // L1 final: one-unit move (Explore_Area on an object)
                if (!store.alive(s.unit)) return finish(false);
                if (now <= s.started) return false;
                const TacticalAiObject* f = fact(id_of(s.unit));
                return (f && f->moving) ? false : finish(true);
            }
            if (!t) return finish(false);
            prune_tf(*t);
            if (t->units.empty()) return finish(false);
            if (s.target >= 0) {
                // An Attack_Move / Move_To to an OBJECT (0x73c4f0) stays open after arrival while the object lives
                // (A1_READINGS §33.2, modes 0 / 1) -> finished (true) when it dies. a1h t02: the status is passive; the
                // battle host's movement follows the object (§33.4), the status no longer re-orders the units.
                const ObjectHandle h = store.find(s.target);
                if (!store.alive(h)) return finish(true);
                return false;
            }
            if (now <= s.started) return false;
            for (const ObjectHandle u : t->units) {
                const TacticalAiObject* f = fact(id_of(u));
                if (f && f->moving) return false;
            }
            return finish(true);
        }
        case StatusKind::SpaceReinforce: return false;  // refixf t06: advanced only by the Lua poll (status_finished)
        case StatusKind::Reinforce: return false;       // a1j t01: advanced by the signal 0x19 records (reinforcements_unloaded)
        case StatusKind::Ambush:
        case StatusKind::Ability: return false;  // S7 t21 / S7 t15 A1: advanced by the object signals (status_signals)
        case StatusKind::AttackTarget: {
            const ObjectHandle h = store.find(s.target);
            if (!store.alive(h)) return finish(true);
            if (!t) return finish(false);
            prune_tf(*t);
            if (t->units.empty()) return finish(false);
            return false;
        }
        case StatusKind::Guard: {
            // a1h t02 (A1_READINGS §33.2): passive, Result = the BlockingStatus default (nil). An object guard finishes when
            // the object dies or the TF is empty, and stays open after arrival (the battle host's movement follows the
            // object, §33.4); a position guard finishes when every unit has arrived. A newer order of the TF finishes it
            // too (supersede). [port] the object's new-order (0xb) and owner-change (0x1a) signals are not modelled.
            auto end = [&s] {
                s.finished = true;
                s.result = lua::AiStatusResult::Nil;
                return false;
            };
            if (!t) return end();
            prune_tf(*t);
            if (t->units.empty() || (s.target >= 0 && !store.alive(store.find(s.target)))) return end();
            if (s.target >= 0 || now <= s.started) return false;
            for (const ObjectHandle u : t->units) {
                const TacticalAiObject* f = fact(id_of(u));
                if (f && f->moving) return false;
            }
            return end();
        }
    }
    return false;
}

// ================================================================ tactical orders (§7.3)

std::vector<uint32_t> TacticalAiWorld::Impl::movable_ids(const TfRec& t) {
    std::vector<uint32_t> ids;
    for (const ObjectHandle u : t.units)
        if (movable(u)) ids.push_back(static_cast<uint32_t>(id_of(u)));
    return ids;
}

int32_t TacticalAiWorld::Impl::issue_move(TfRec& t, AiTacticalOrder kind, const Vec3f& dest, int32_t target_object,
                                          std::optional<float> threat) {
    const std::vector<uint32_t> ids = movable_ids(t);
    const char* what = kind == AiTacticalOrder::AttackMove ? "attack-move" : kind == AiTacticalOrder::GuardTarget ? "guard" : "move";
    char buf[96];
    std::snprintf(buf, sizeof buf, " %zu units -> (%.0f, %.0f)", ids.size(), static_cast<double>(dest.x), static_cast<double>(dest.y));
    if (ids.empty()) return new_status(t.plan, t.id, StatusKind::Failed);
    bool any = false;
    if (kind == AiTacticalOrder::MoveTo && threat) {
        any = order(t.player, lua_order_event(lua::LuaOrder::MoveTo, ids, -1, dest, threat_bits(*threat)),
                    std::string("tf ") + t.name + " " + what + buf + threat_text(*threat)) == cmd::RejectReason::None;
    } else if (kind == AiTacticalOrder::MoveTo) {
        cmd::EventData ev(cmd::EventType::MoveToPositionFacing);
        ev.set_f("x", dest.x).set_f("y", dest.y).set_f("z", dest.z).set_ids("units", ids);
        any = order(t.player, ev, std::string("tf ") + t.name + " " + what + buf) == cmd::RejectReason::None;
    } else {
        // refixf t06 (MOVEMENT §6.3 [H], AI_TACTICAL §7.3): one group order for the whole task force (header). Before, one
        // order per unit, which the battle host merged again.
        const lua::LuaOrder lo = kind == AiTacticalOrder::GuardTarget ? lua::LuaOrder::Guard : lua::LuaOrder::AttackMove;
        // refixf t06 (TACTICAL_COMBAT §2.8 A): a task-force Attack_Move on an object issues Order_Attack to every unit at
        // order time (the host drops refusing units). The follow re-orders below carry arg 0 [port: re-aim not repeated].
        const uint32_t arg = lo == lua::LuaOrder::AttackMove && target_object >= 0 ? kTfAttackMoveIssueAttack : 0u;
        any = order(t.player, lua_order_event(lo, ids, target_object, dest, arg), std::string("tf ") + t.name + " " + what + buf) ==
              cmd::RejectReason::None;
    }
    if (!any) return new_status(t.plan, t.id, StatusKind::Failed);
    supersede(t.id);
    const int32_t id = new_status(t.plan, t.id, kind == AiTacticalOrder::GuardTarget ? StatusKind::Guard : StatusKind::Move);
    statuses[id].dest = dest;
    statuses[id].target = target_object;
    return id;
}

// --- L1 final ---
int32_t TacticalAiWorld::Impl::unit_explore_area(int32_t script, ObjectHandle unit, int32_t target) {
    const int32_t player = store.alive(unit) ? store.owner(unit) : -1;
    lua::AiArg a;
    a.present = true;
    a.target = target;
    Vec3f p;
    if (player < 0 || !movable(unit) || !arg_position(a, lua::AiPoint{}, p)) return -1;
    const std::vector<uint32_t> ids{static_cast<uint32_t>(id_of(unit))};
    cmd::EventData ev(cmd::EventType::MoveToPositionFacing);
    ev.set_f("x", p.x).set_f("y", p.y).set_f("z", p.z).set_ids("units", ids);
    if (order(player, ev, "explore area (unit)") != cmd::RejectReason::None) return new_status(script, -1, StatusKind::Failed);
    const int32_t id = new_status(script, -1, StatusKind::Move);
    statuses[id].dest = p;
    statuses[id].unit = unit;
    return id;
}
// --- end L1 final ---

int32_t TacticalAiWorld::Impl::tf_tactical_order(int32_t tf, lua::AiTacticalOrder order_kind, const lua::AiArg& dest,
                                                  const lua::AiPoint& point, double number, bool) {
    TfRec* t = tf_rec(tf);
    if (!t) return -1;
    prune_tf(*t);
    if (order_kind == AiTacticalOrder::FireSpecialWeapon) return new_status(t->plan, tf, StatusKind::Done);  // [port] no special weapons
    int32_t target_object = -1;
    Vec3f p;
    if (order_kind == AiTacticalOrder::GuardTarget && dest.task_force >= 0) {
        // A task-force target: guard ONE random unit of it, Get_Int(0, n - 1), no draw for n = 1 (§7.3).
        TfRec* g = tf_rec(dest.task_force);
        if (!g) return -1;
        prune_tf(*g);
        if (g->units.empty()) return -1;
        const auto n = static_cast<int32_t>(g->units.size());
        const int32_t k = n >= 2 ? rng.synced().range_int(0, n - 1) : 0;
        const ObjectHandle u = g->units[static_cast<size_t>(k)];
        target_object = id_of(u);
        p = pos_of(u);
    } else {
        if (!arg_position(dest, point, p)) return -1;
        if (dest.object) target_object = id_of(dest.object);
        else if (dest.target >= 0)
            if (const Slot* s = slot_of(dest.target); s && s->t.is_unit()) target_object = s->t.object_id;
    }
    const AiTacticalOrder k = order_kind == AiTacticalOrder::ExploreArea || order_kind == AiTacticalOrder::PrepareAmbush
                                  ? AiTacticalOrder::MoveTo
                                  : order_kind;
    if (k != AiTacticalOrder::GuardTarget && k != AiTacticalOrder::AttackMove) target_object = -1;
    // S7 t33 (AI_TACTICAL §7.3 table, Move_To(dest [, threatTolerance [, useZonePath]]) 0x6ec410 / §7.3.1): the tolerance goes
    // with the group move (Coordinator::Move_To threat tolerance, MOVEMENT REMAINING_GAPS §4.2). The binding passes -1 when
    // the argument is absent [port: a negative tolerance is read as absent]. Land only: the space Move_To (0x6e7940) reads an
    // optional speed / collision-avoidance argument instead.
    if (order_kind == AiTacticalOrder::MoveTo && cfg.mode == TacticalMode::Land && number >= 0.0)
        return issue_move(*t, k, p, target_object, static_cast<float>(number));
    return issue_move(*t, k, p, target_object);
}

// ---- S7 t21: Prepare_Ambush (AI_TACTICAL §7.3.1) ----

float land_ambush_step(float land_fow_reveal_range, float weather_sight_factor, float max_attack_range) {
    const float sight = land_fow_reveal_range * weather_sight_factor;  // 0x3722a0
    return sight > 0.0f ? sight : max_attack_range;                     // <= 0: 0x3749d0
}

// Ambush point (0x73a140): P = target + minDist x u on the chosen side, walked further out along u in steps of the target
// type's Space_FOW_Reveal_Range (<= 0: AI_SpaceEvaluatorRegionSize) until the enemy force of `player` in the step x step
// square around P (threat grid, all categories, x AI_SpaceAreaThreatScaleFactor) is <= the tolerance; no point once P
// leaves the map box on an axis. Land (0x6ec8b0, S7 t27 AI_TACTICAL §7.3.3 item 5): step = the land sight range 0x3722a0
// of the target's company type (0x3973b0: its team container's type, else its own) = Land_FOW_Reveal_Range x the weather
// Sight_Range_Modifier of its Weather_Category (host sight_weather_factor), else the target type's max attack range; both
// <= 0 -> the first point, no threat / box test (`*no_walk` set). The land threat is not scaled [M: the land call passes no
// AI_SpaceAreaThreatScaleFactor]. [port] The yaw's sin / cos are the target's facing vector normalised (XY); a zero facing
// counts as yaw 0. [port] At most 4096 steps.
bool TacticalAiWorld::Impl::ambush_point(const StatusRec& s, int32_t player, Vec3f& out, bool* no_walk) const {
    if (no_walk) *no_walk = false;
    const ObjectHandle h = store.find(s.target);
    if (!store.alive(h) || s.direction < 0 || s.direction > 3) return false;
    const Vec3 f = store.facing(h);
    float c = 1.0f, sn = 0.0f;
    if (const float len = std::sqrt(f.x * f.x + f.y * f.y); len > 0.0f) {
        c = f.x / len;
        sn = f.y / len;
    }
    Vec3f u;
    switch (s.direction) {
        case 0: u = {c, sn, 0.0f}; break;     // front
        case 1: u = {-sn, c, 0.0f}; break;    // left
        case 2: u = {sn, -c, 0.0f}; break;    // right
        default: u = {-c, -sn, -0.0f}; break;  // back
    }
    const bool land = cfg.mode == TacticalMode::Land;
    objects::TypeIndex ty = store.type(h);
    if (land)
        if (const ObjectHandle team = store.container(h); team && store.exists(team)) ty = store.type(team);  // company type
    float step = 0.0f;
    if (types && ty != objects::kNone) {
        const auto& col = land ? types->columns().land_fow_reveal_range : types->columns().space_fow_reveal_range;
        if (static_cast<size_t>(ty) < col.size()) step = col[static_cast<size_t>(ty)];
    }
    if (land) {
        const PerceptionObject* o = object(s.target);
        const float weather = ty != objects::kNone ? host.sight_weather_factor(static_cast<int32_t>(ty), s.target) : 1.0f;
        step = land_ambush_step(step, weather, o ? o->max_attack_range : 0.0f);
    } else if (step <= 0.0f) {
        step = cfg.constants.space_region_size;
    }
    const Vec3f t = pos_of(h);
    Vec3f p{t.x + s.min_dist * u.x, t.y + s.min_dist * u.y, t.z + s.min_dist * u.z};
    if (land && step <= 0.0f) {
        out = p;
        if (no_walk) *no_walk = true;
        return true;
    }
    const float scale = land ? 1.0f : cfg.constants.space_area_threat_scale;
    std::vector<uint32_t> cells;
    for (int k = 0; k < 4096; ++k) {
        if (p.x < cfg.map.min_x || p.x > cfg.map.max_x || p.y < cfg.map.min_y || p.y > cfg.map.max_y) return false;
        GridQuery q;
        q.self = player;
        q.ally = false;
        q.memory_counted_when_not_fogged = perc.config().memory_counted_when_not_fogged;
        const float half = step * 0.5f;
        perc.grid().rect_cells({p.x - half, p.y - half, p.x + half, p.y + half}, cells);
        const float threat = perc.grid().cells_force(w, cells, q) * scale;
        if (threat <= s.threat) {
            out = p;
            return true;
        }
        p.x += step * u.x;
        p.y += step * u.y;
        p.z += step * u.z;
    }
    return false;
}

// plan(first) (0x73b180): the point, then (first) the member list = the task force's units (member order) that exist, are
// not movement-locked and can move [port: the coordinator admission and type byte +0x1e]; the moving list = the member
// list's units that can still move; one plain group move to P with them. No point -> finished, result false. Nobody to
// move -> stays unfinished (the next re-plan tries again).
void TacticalAiWorld::Impl::ambush_plan(StatusRec& s, bool first) {
    s.plan_frame = mode_frame;
    TfRec* t = tf_rec(s.tf);
    Vec3f p;
    if (!t || !ambush_point(s, t->player, p)) {
        s.finished = true;
        s.result = lua::AiStatusResult::False;
        decide(t ? t->player : -1, "tf " + (t ? t->name : std::string("?")) + " ambush: no point");
        return;
    }
    s.dest = p;
    if (first) {
        s.lists = true;
        s.members.clear();
        for (const ObjectHandle u : t->units)
            if (unit_ok(u, t->player) && !store.dying(u) && movable(u) && !host.movement_locked(id_of(u))) s.members.push_back(id_of(u));
    }
    s.moving.clear();
    std::vector<uint32_t> ids;
    for (const int32_t id : s.members) {
        const ObjectHandle u = store.find(id);
        if (!unit_ok(u, t->player) || !movable(u)) continue;
        s.moving.push_back(id);
        ids.push_back(static_cast<uint32_t>(id));
    }
    if (ids.empty()) return;
    char buf[96];
    std::snprintf(buf, sizeof buf, " %zu units -> (%.0f, %.0f)", ids.size(), static_cast<double>(p.x), static_cast<double>(p.y));
    // Coordinator::Move_To {kind 1 = P, threat tolerance = +0x58}: S7 t27 the group move carries the tolerance (the Lua
    // MoveTo group order: one formation move with the movement threshold T).
    order(t->player, lua_order_event(lua::LuaOrder::MoveTo, ids, -1, p, threat_bits(s.threat)),
          "tf " + t->name + (first ? " ambush on " : " ambush re-plan on ") + std::to_string(s.target) + buf + threat_text(s.threat));
    s.last_order = now;
}

int32_t TacticalAiWorld::Impl::tf_prepare_ambush(int32_t tf, const lua::AiArg& target, int direction, double min_dist, double threat,
                                                 bool, std::string* name) {
    TfRec* t = tf_rec(tf);
    if (!t) return -1;
    prune_tf(*t);
    ObjectHandle h = target.object;
    if (!h && target.target >= 0) {
        const Slot* sl = slot_of(target.target);
        if (!sl || !sl->alive) return lua::AiTacticalScriptHost::kAmbushTargetDead;
        if (!sl->t.is_unit()) {
            if (name) *name = sl->t.name;
            return lua::AiTacticalScriptHost::kAmbushTargetRegion;
        }
        h = store.find(sl->t.object_id);
    }
    if (!store.alive(h) || store.dying(h)) return lua::AiTacticalScriptHost::kAmbushTargetDead;
    StatusRec probe;
    probe.target = id_of(h);
    probe.direction = static_cast<int8_t>(direction);
    probe.min_dist = static_cast<float>(min_dist);
    probe.threat = static_cast<float>(threat);
    if (cfg.mode == TacticalMode::Land) {
        // Land (0x6ec8b0): the walk once, then an ordinary movement block to P; nil without a point or when the block is
        // already finished (no unit to move) - S7 t27 (§7.3.3 item 5): without a walk (step <= 0) the block is returned
        // even when already finished. The move carries the tolerance (move parameter +0x54) in both cases.
        Vec3f p;
        bool no_walk = false;
        if (!ambush_point(probe, t->player, p, &no_walk)) return -1;
        const int32_t id = issue_move(*t, AiTacticalOrder::MoveTo, p, -1, probe.threat);
        if (statuses[id].finished && !no_walk) {
            statuses.erase(id);
            return -1;
        }
        return id;
    }
    const int32_t id = new_status(t->plan, tf, StatusKind::Ambush);
    StatusRec& s = statuses[id];
    s.target = probe.target;
    s.direction = probe.direction;
    s.min_dist = probe.min_dist;
    s.threat = probe.threat;
    ambush_plan(s, true);
    return id;
}

int32_t TacticalAiWorld::Impl::tf_member_of(int32_t object_id) const {
    const ObjectHandle unit = store.find(object_id);
    if (!store.exists(unit)) return -1;
    const ObjectHandle team = store.container(unit);
    for (const auto& [id, t] : tfs)  // ascending TF id; a unit is held by at most one TF
        for (const ObjectHandle u : t.units)
            if (u == unit || (team && u == team)) return id;
    return -1;
}

// S7 t15 A1 (MOVEMENT §3.1.1 step 6): the task-force Activate_Ability's AbilityBlockStatus - one count per unit that
// activated (members, task-force order), finished when each raised signal 0x29 or died / was removed; nobody activated ->
// finished at once. Units whose activation failed get Unit_Ability_Finished queued at once. Result nil [M].
int32_t TacticalAiWorld::Impl::tf_activate_ability_units(int32_t tf, std::string_view, bool, const lua::AiArg&,
                                                         const std::vector<std::pair<ObjectHandle, bool>>& units) {
    TfRec* t = tf_rec(tf);
    if (!t) return -1;
    const int32_t id = new_status(t->plan, tf, StatusKind::Ability);
    std::vector<lua::AiEventArg> failed;
    for (const auto& [u, ok] : units) {
        if (!store.alive(u)) continue;
        if (ok) {
            statuses[id].members.push_back(id_of(u));
        } else {
            lua::AiEventArg a;
            a.kind = lua::AiEventArg::Object;
            a.object = u;
            failed.push_back(a);
        }
    }
    StatusRec& s = statuses[id];
    if (s.members.empty()) s.finished = true;
    decide(t->player, "tf " + t->name + " activate ability: " + std::to_string(s.members.size()) + " activated, " +
                          std::to_string(failed.size()) + " failed");
    const TfRec copy = *t;
    for (const lua::AiEventArg& a : failed) tf_event(copy, "Unit_Ability_Finished", {a}, true);
    return id;
}

void TacticalAiWorld::Impl::ability_block_signal(int32_t oid, int32_t team_id) {
    for (auto& [sid, s] : statuses) {
        if (s.kind != StatusKind::Ability || s.finished) continue;
        auto it = std::find(s.members.begin(), s.members.end(), oid);
        if (it == s.members.end() && team_id >= 0) it = std::find(s.members.begin(), s.members.end(), team_id);
        if (it == s.members.end()) continue;
        s.members.erase(it);
        if (s.members.empty()) s.finished = true;
    }
}

void TacticalAiWorld::Impl::order_signal(int32_t oid) {
    const ObjectHandle h = store.find(oid);
    bridge.order_replaced(oid);
    const ObjectHandle team = store.exists(h) ? store.container(h) : ObjectHandle{};
    const int32_t team_id = team ? id_of(team) : -1;
    if (team_id >= 0 && team_id != oid) bridge.order_replaced(team_id);
    ability_block_signal(oid, team_id);
}

// S7 t15 A1: the host's signal 0x29 list (its order). Each id feeds the scripts' object bridge (blocking
// GameObjectWrapper.Activate_Ability, the team container too), decrements the ability statuses that wait on the unit (or
// its team) once, and - unless the unit is dying (obj +0x3a0 bit 0x02) - raises plan event 19 Unit_Ability_Finished(tf,
// member) on the task force holding it.
void TacticalAiWorld::Impl::ability_signals(const std::vector<int32_t>& ids) {
    for (const int32_t oid : ids) {
        const ObjectHandle h = store.find(oid);
        bridge.ability_finished(oid);
        const ObjectHandle team = store.exists(h) ? store.container(h) : ObjectHandle{};
        const int32_t team_id = team ? id_of(team) : -1;
        if (team_id >= 0 && team_id != oid) bridge.ability_finished(team_id);
        ability_block_signal(oid, team_id);
        if (!store.alive(h) || store.dying(h)) continue;
        const int32_t tf = tf_member_of(oid);
        const TfRec* t = tf >= 0 ? tf_rec(tf) : nullptr;
        if (!t) continue;
        lua::AiEventArg a;
        a.kind = lua::AiEventArg::Object;
        a.object = std::find(t->units.begin(), t->units.end(), h) != t->units.end() ? h : team;
        decide(t->player, "event Unit_Ability_Finished (tf " + t->name + ")");
        const TfRec copy = *t;
        tf_event(copy, "Unit_Ability_Finished", {a});
    }
}

// S7 t21 / S7 t15 A1 - the statuses' object signals, every AI frame after refresh (ascending status id):
//   * ability: a pending unit that died / was removed (0xb / 0x10) no longer counts;
//   * ambush (§7.3.1 signals 0x73ab80): the target dying -> result true, finished; a member dying / removed / leaving the
//     task force -> out of both lists, either list empty -> finished, result = member list not empty; a moving unit whose
//     movement finished (signal 10 [port: idle after the order frame]) -> out of the moving list, Unit_Move_Finished(unit)
//     on the task force, the list empty -> result true, finished. A status of an ended plan finishes.
void TacticalAiWorld::Impl::status_signals() {
    auto gone = [this](int32_t id, int32_t player) {
        const ObjectHandle h = store.find(id);
        return !unit_ok(h, player) || store.dying(h);
    };
    std::vector<std::pair<int32_t, ObjectHandle>> move_done;  // (tf, unit)
    for (auto& [sid, s] : statuses) {
        if (s.finished || (s.kind != StatusKind::Ambush && s.kind != StatusKind::Ability)) continue;
        if (plans.find(s.plan) == plans.end()) {
            s.finished = true;
            continue;
        }
        const TfRec* t = tf_rec(s.tf);
        const int32_t player = t ? t->player : -1;
        if (s.kind == StatusKind::Ability) {
            // S7 t26 (§7.3.3 item 2): signal 0x10 (death / removal) ends the unit's count; an owner change (0x1a) does not.
            s.members.erase(std::remove_if(s.members.begin(), s.members.end(),
                                           [&](int32_t id) {
                                               const ObjectHandle h = store.find(id);
                                               return !store.alive(h) || store.dying(h);
                                           }),
                            s.members.end());
            if (s.members.empty()) s.finished = true;
            continue;
        }
        if (!s.lists) continue;
        if (s.target >= 0 && (!store.alive(store.find(s.target)) || store.dying(store.find(s.target)))) {
            s.target = -1;
            s.finished = true;
            s.result = lua::AiStatusResult::True;
            continue;
        }
        bool lost = false;
        for (size_t i = 0; i < s.members.size();) {
            const int32_t id = s.members[i];
            if (!gone(id, player) && t && std::find(t->units.begin(), t->units.end(), store.find(id)) != t->units.end()) {
                ++i;
                continue;
            }
            s.members.erase(s.members.begin() + static_cast<std::ptrdiff_t>(i));
            s.moving.erase(std::remove(s.moving.begin(), s.moving.end(), id), s.moving.end());
            lost = true;
        }
        if (lost && (s.members.empty() || s.moving.empty())) {
            s.finished = true;
            s.result = s.members.empty() ? lua::AiStatusResult::False : lua::AiStatusResult::True;
            continue;
        }
        if (now <= s.last_order || s.moving.empty()) continue;
        for (size_t i = 0; i < s.moving.size();) {
            const TacticalAiObject* f = fact(s.moving[i]);
            if (f && f->moving) {
                ++i;
                continue;
            }
            move_done.emplace_back(s.tf, store.find(s.moving[i]));
            s.moving.erase(s.moving.begin() + static_cast<std::ptrdiff_t>(i));
            if (s.moving.empty()) {
                s.finished = true;
                s.result = lua::AiStatusResult::True;
            }
        }
    }
    for (const auto& [tf, u] : move_done)
        if (const TfRec* t = tf_rec(tf)) {
            lua::AiEventArg a;
            a.kind = lua::AiEventArg::Object;
            a.object = u;
            const TfRec copy = *t;
            tf_event(copy, "Unit_Move_Finished", {a});
        }
}

int32_t TacticalAiWorld::Impl::tf_attack_target(int32_t tf, const lua::AiArg& target, std::string_view hardpoint) {
    TfRec* t = tf_rec(tf);
    if (!t) return -1;
    prune_tf(*t);
    ObjectHandle h = target.object;
    if (!h && target.target >= 0) h = target_object(target.target);
    if (!store.alive(h)) return -1;
    uint32_t ref2 = 0;  // [port encoding] 1 + HardPointType, 0 = no ordered hard point
    if (cfg.mode == TacticalMode::Space && !hardpoint.empty()) {
        if (hardpoint == "Random") {  // TACTICAL_COMBAT §2.4 [H]: the exact-case literal, one synced draw
            const int32_t k = rng.synced().range_int(0, 13);  // 13 = any (§7.3, §10)
            ref2 = k >= 13 ? 0u : static_cast<uint32_t>(k + 1);
        } else {
            // refixf t06 (TACTICAL_COMBAT §2.4 [H]): the HardPointType converter, short names only, trimmed, case-insensitive.
            std::string_view n = hardpoint;
            while (!n.empty() && (n.front() == ' ' || n.front() == '\t')) n.remove_prefix(1);
            while (!n.empty() && (n.back() == ' ' || n.back() == '\t')) n.remove_suffix(1);
            for (uint32_t i = 0; i < std::size(kHardPointTypeNames); ++i)
                if (*kHardPointTypeNames[i] && ai_iequal(n, kHardPointTypeNames[i])) ref2 = i + 1;
            // The original raises "Unrecognized hard point type" (a 2-argument call parses the string as the avoidance
            // SpaceCollisionType instead) [port: counted and ignored; the order targets the object with no hard point].
            if (ref2 == 0) ++stats["Attack_Target: unrecognized hard point type"];
        }
    }
    std::vector<uint32_t> ids;
    for (const ObjectHandle u : t->units) ids.push_back(static_cast<uint32_t>(id_of(u)));
    if (ids.empty()) return new_status(t->plan, tf, StatusKind::Failed);
    cmd::EventData ev(cmd::EventType::Attack);
    ev.set("target", static_cast<uint32_t>(id_of(h))).set("ref2", ref2).set_ids("attackers", ids);
    if (order(t->player, ev, "tf " + t->name + " attack " + std::to_string(id_of(h)) + (ref2 ? " hp " + std::to_string(ref2 - 1) : "")) !=
        cmd::RejectReason::None)
        return new_status(t->plan, tf, StatusKind::Failed);
    supersede(tf);
    const int32_t id = new_status(t->plan, tf, StatusKind::AttackTarget);
    statuses[id].target = id_of(h);
    return id;
}

// a1g t04 - land TF.Reinforce placement (header comments of reinforce_points / reinforce_ring / reinforce_place).
std::vector<ReinforcePoint> reinforce_points(const PerceptionWorld& w, const std::vector<TacticalAiObject>& facts, int32_t player,
                                             const Vec3f& dest, const ReinforcePointRank& rank) {
    std::vector<ReinforcePoint> out;
    for (size_t i = 0; i < w.objects.size() && i < facts.size(); ++i) {
        const PerceptionObject& o = w.objects[i];
        const float r = facts[i].applies.reinforcement_enabling_radius;
        if (!o.alive || r <= 0.0f || o.owner < 0 || !(o.owner == player || w.allied(player, o.owner))) continue;
        float key = 0.0f;
        if (rank) {
            if (!rank(o.position, key)) continue;
        } else {
            const float dx = o.position.x - dest.x, dy = o.position.y - dest.y;
            key = std::sqrt(dx * dx + dy * dy);
        }
        out.push_back({o.position, r, key});
    }
    std::stable_sort(out.begin(), out.end(), [](const ReinforcePoint& a, const ReinforcePoint& b) { return a.key < b.key; });
    return out;
}

std::vector<Vec3f> reinforce_ring(const ReinforcePoint& point) {
    std::vector<Vec3f> out;
    const float half = point.radius * 0.5f;
    for (float theta = 0.0f; theta < 6.2831855f; theta += 0.7853982f) {
        float s = 0.0f, c = 1.0f;
        dmath::sincos(theta, s, c);
        out.push_back({point.position.x + half * c, point.position.y + half * s, point.position.z});
    }
    return out;
}

bool reinforce_place(const std::vector<ReinforcePoint>& points, std::vector<TacticalReinforcement>& pending, float remaining,
                     const ReinforceIsNull& is_null, const ReinforceLand& land) {
    // The working list is pending[0, top); popped entries are marked (erase: placed or null) and removed at the end.
    std::vector<uint8_t> erase(pending.size(), 0);
    size_t top = pending.size();
    bool all_placed = true;
    auto pop_trailing = [&] {
        while (top > 0) {
            const TacticalReinforcement& r = pending[top - 1];
            const bool null_unit = is_null && is_null(r);
            if (!null_unit && r.population <= remaining) break;
            erase[top - 1] = null_unit ? 1 : 0;
            --top;
            all_placed = false;
        }
    };
    pop_trailing();  // [port] (header comment)
    for (const ReinforcePoint& pt : points) {
        for (const Vec3f& spot : reinforce_ring(pt)) {
            if (top == 0 || remaining < 1.0f) break;
            const TacticalReinforcement r = pending[top - 1];  // a copy: `land` runs host code
            if (!land(r, spot)) continue;
            remaining -= r.population;
            erase[top - 1] = 1;
            --top;
            pop_trailing();
        }
        if (top == 0 || remaining < 1.0f) break;
    }
    const bool ok = all_placed && top == 0;
    size_t k = 0;
    for (size_t i = 0; i < pending.size(); ++i)
        if (!erase[i]) pending[k++] = pending[i];
    pending.resize(k);
    return ok;
}

int32_t TacticalAiWorld::Impl::tf_reinforce(int32_t tf, const lua::AiArg& target, const lua::AiPoint& point, double max_wait) {
    TfRec* t = tf_rec(tf);
    if (!t) return -1;
    if (t->pending.empty()) return -2;
    if (cfg.mode == TacticalMode::Space) {
        // refixf t06 - SpaceReinforceBlockStatus (0x6e8620 / init 0x7235c0, AI_TACTICAL §7.4 [H]): the target T is the centre
        // of a ring search (z := 0), not a drop point; the units are placed by the status's IsFinished, one per Lua poll
        // (space_reinforce_poll). Optional max wait in seconds -> timeout frame. R = 1000 when T lies inside a non-allied
        // Reinforcement_Prevention_Radius, else 0. [port: the campaign "no allied DUMMY_STAR_BASE" refusal is not modelled.]
        Vec3f p;
        if (!arg_position(target, point, p)) return -1;
        p.z = 0.0f;
        const int32_t id = new_status(t->plan, tf, StatusKind::SpaceReinforce);
        StatusRec& s = statuses[id];
        s.dest = p;
        s.timeout = max_wait >= 0.0 ? now + static_cast<int64_t>(static_cast<double>(kFramesPerSecond) * max_wait) : -1;
        s.prevent = host.space_prevention_hit(t->player, p);
        s.ring = s.prevent ? 1000.0f : 0.0f;
        return id;
    }
    // S10 AI land reinforcement: "can reinforce" 0x2b03e0 = the battle allows it (host) and room for one unit
    // (AI_GALACTIC_3 §3.3: (int)(1.0 + 0.5) <= cap - used); otherwise an immediately-failing ReinforceBlockStatus.
    const float cap = host.population_cap(t->player);
    const float remaining = cap - std::max(host.population_used(t->player), 0.0f);
    if (!host.can_reinforce(t->player) || (cap >= 0.0f && remaining < 1.0f)) return new_status(t->plan, tf, StatusKind::Failed);
    Vec3f p;
    if (!arg_position(target, point, p)) return -1;
    // a1g t04 - land TF.Reinforce placement (0x6eaf60, AI_TACTICAL §7.4, A1_READINGS §25.3 / §30.7 [H]). The destination
    // zone: a region target's zone, else the zone at the position. Points in a zone outside the destination's flood set
    // at the TF goal's Reachability are dropped; the rest are keyed (float) by the slot-7 path cost from their zone at
    // that level (a failed path = 1e18, sorted last); a point without a zone keys by its 2-D distance (the original's
    // mixed keys). A destination without a zone keeps the 2-D keys. The units land on the 8-spot rings at r/2 around the
    // points in key order (reinforce_place), not moved toward the target.
    ReinforcePointRank rank;
    if (cfg.mode == TacticalMode::Land && zones.size() > 0) {
        int32_t dest = -1;
        if (!point.present && target.target >= 0)
            if (const Slot* s = slot_of(target.target); s && !s->t.is_unit() && s->t.has_reachability()) dest = s->t.zone;
        if (dest < 0) dest = zone_at(p);
        const auto pi = plans.find(t->plan);
        PlayerAi* pa = player_ai(t->player);
        if (dest >= 0 && pi != plans.end() && pi->second.goal_type < defs.goals.size() && pa) {
            const Reachability lvl = defs.goals[pi->second.goal_type].reachability;
            const std::vector<uint32_t>& set = pa->flood_set(dest, lvl);
            const Vec3f to = p;
            rank = [this, pa, dest, lvl, &set, to](const Vec3f& at, float& key) {
                const int32_t z = zone_at(at);
                if (z < 0) {
                    const float dx = at.x - to.x, dy = at.y - to.y;
                    key = std::sqrt(dx * dx + dy * dy);
                    return true;
                }
                if (std::find(set.begin(), set.end(), static_cast<uint32_t>(z)) == set.end()) return false;
                key = static_cast<float>(zone_reachability_cost(pa->zone_paths(), z, dest, lvl, cfg.constants));
                return true;
            };
        }
    }
    std::vector<ReinforcePoint> points;
    if (cfg.constants.use_reinforcement_points) {
        points = reinforce_points(w, facts, t->player, p, rank);
        if (points.empty()) {
            ++stats["Reinforce: no allied reinforcement point"];
            return new_status(t->plan, tf, StatusKind::Failed);
        }
    } else {
        points.push_back({p, 0.0f, 0.0f});  // [port] without Use_Reinforcement_Points: every spot is the target itself
    }
    // Campaign (galactic parent): the galactic pool (units in orbit); skirmish: the tactical pool - both come from
    // the host's reinforcement_pool. A2 tactical (AI_GALACTIC_3 §3.4): the remaining cap (cap - max(used, 0)) is read
    // ONCE; each drop the host accepts takes its population off that one reading (§30.7 step 4), so the loop never
    // issues a drop the host's per-order cap rule refuses. A pending entry is null when it has no type or left the
    // host's pool (it can never land: dropped from the TF, like the space poll).
    const std::vector<TacticalReinforcement> pool = host.reinforcement_pool(t->player);
    auto is_null = [&pool](const TacticalReinforcement& r) {
        return r.type < 0 || std::none_of(pool.begin(), pool.end(), [&r](const TacticalReinforcement& e) { return e.key == r.key; });
    };
    uint32_t spawned = 0;
    std::vector<std::pair<int32_t, int32_t>> in_flight;  // a1j t01: (key, type) of the timed landings this call issued
    auto land = [&](const TacticalReinforcement& r, const Vec3f& spot) {
        // a1g t04 follow-up: the mode's drop validity (TacticalAiHost::land_drop_ok); a refused spot is not ordered.
        if (!host.land_drop_ok(t->player, r.key, spot)) {
            ++stats["Reinforce: land spot refused"];
            return false;
        }
        const size_t before = store.owned_by(t->player).size();
        cmd::EventData ev(cmd::EventType::Reinforce);
        ev.set("unit", static_cast<uint32_t>(r.key)).set_f("x", spot.x).set_f("y", spot.y).set_f("z", spot.z);
        char at[48];
        std::snprintf(at, sizeof at, " at (%.0f, %.0f)", static_cast<double>(spot.x), static_cast<double>(spot.y));
        if (order(t->player, ev, std::string(cfg.campaign ? "campaign" : "skirmish") + " reinforce " + type_name(r.type) + at + " (tf " + t->name + ")") !=
            cmd::RejectReason::None)
            return false;
        // a1j t01 (A1_READINGS §36): the company is inside a landing object that unloads it over the next seconds; the
        // status listens for its signal 0x19 (in-flight count + 1) and the units join the TF then (0x6a1440).
        if (host.reinforce_in_flight(t->player, r.key)) {
            in_flight.emplace_back(r.key, r.type);
            return true;
        }
        const auto owned = store.owned_by(t->player);
        for (size_t k = before; k < owned.size(); ++k)
            if (store.active(owned[k]) && store.has_behavior(owned[k], objects::beh::Locomotor)) {
                t->units.push_back(owned[k]);
                ++spawned;
                reinforce_joined(*t, id_of(owned[k]), r.type);
            }
        return true;
    };
    const bool placed_all = reinforce_place(points, t->pending, cap >= 0.0f ? remaining : HUGE_VALF, is_null, land);
    if (in_flight.empty()) return new_status(t->plan, tf, placed_all ? StatusKind::Done : StatusKind::Failed);
    // a1j t01 - ReinforceBlockStatus init 0x6a1120 / IsFinished 0x6a11d0 / Result 0x6a1a50 (A1_READINGS §36): unfinished
    // while a landing is in flight; Result = every pending unit placed (0x6eaf60's return, kept at the order). While it
    // waits the plan is not goal-system removable (+0x44 = the plan's flag, then cleared); the last landing restores it.
    const int32_t id = new_status(t->plan, tf, StatusKind::Reinforce);
    StatusRec& s = statuses[id];
    s.landings = std::move(in_flight);
    s.result = placed_all ? lua::AiStatusResult::True : lua::AiStatusResult::False;
    if (const auto pit = plans.find(t->plan); pit != plans.end()) {
        s.saved_removable = !pit->second.non_removable;
        pit->second.non_removable = true;
    }
    return id;
}

// a1j t01 - the ReinforceBlockStatus signal handler 0x6a1440 (A1_READINGS §32.1 / §36) for each signal 0x19 record of the
// tick, in order. The status that issued the landing of `key` for `player` drops it from its in-flight list; when its owner
// is a live task force each unloaded unit in list order joins it (Add_Force, land 0x6e9240: off the free store) and its
// plan's PP (0x6bf3d0). When the list empties the status finishes with the Result set at the order and the plan's
// removable flag is restored (IsFinished 0x6a11d0). [port] The members stand for their team: the port's task forces hold
// members (the original adds the TEAM container once); only units the untimed scan would take join (alive, the player's,
// with a locomotor), and a unit another task force already holds stays there. A record no status waits for is ignored.
void TacticalAiWorld::Impl::reinforcements_unloaded(const std::vector<ReinforcementsUnloaded>& recs) {
    for (const ReinforcementsUnloaded& rec : recs) {
        StatusRec* s = nullptr;
        size_t at = 0;
        for (auto& [id, st] : statuses) {
            if (st.kind != StatusKind::Reinforce || st.finished || script_player(st.plan) != rec.player) continue;
            const auto it = std::find_if(st.landings.begin(), st.landings.end(), [&rec](const std::pair<int32_t, int32_t>& e) { return e.first == rec.key; });
            if (it == st.landings.end()) continue;
            s = &st;
            at = static_cast<size_t>(it - st.landings.begin());
            break;
        }
        if (!s) continue;
        const int32_t type = s->landings[at].second;
        s->landings.erase(s->landings.begin() + static_cast<std::ptrdiff_t>(at));
        if (TfRec* t = tf_rec(s->tf)) {
            PlayerAi* pa = player_ai(t->player);
            for (const int32_t oid : rec.units) {
                const ObjectHandle h = store.find(oid);
                if (!unit_ok(h, t->player) || !store.has_behavior(h, objects::beh::Locomotor) || tf_owns(h)) continue;
                if (pa) pa->fs.remove(oid);
                t->units.push_back(h);
                reinforce_joined(*t, oid, type);
            }
            decide(t->player, "tf " + t->name + ": reinforcements unloaded " + type_name(type) + " (" + std::to_string(rec.units.size()) +
                                  " units, " + std::to_string(s->landings.size()) + " in flight)");
        }
        if (s->landings.empty()) {
            s->finished = true;
            if (const auto pit = plans.find(s->plan); pit != plans.end()) pit->second.non_removable = !s->saved_removable;
        }
    }
}

bool reinforce_join_pp(std::vector<TacticalPpPick>& picks, int32_t unit, int32_t type, int32_t tf, TacticalPpPick* filled) {
    for (TacticalPpPick& k : picks)
        if (k.source != PpSource::FreeStore && k.type == type) {
            if (filled) *filled = k;
            k.source = PpSource::FreeStore;
            k.ref = unit;
            return true;
        }
    TacticalPpPick k;
    k.type = type;
    k.source = PpSource::FreeStore;
    k.ref = unit;
    k.tf = tf;
    picks.push_back(k);
    return false;
}

// refixf t06 - SpaceReinforceBlockStatus IsFinished (vf19 0x7236f0 -> placement 0x722e80, AI_TACTICAL §7.4 [H]): one placement
// attempt per Lua poll, then the finish test.
//   1. The TF's LAST pending entry [port: the TF list itself, not a copy taken at creation; a pool entry that left the
//      host's pool is dropped].
//   2. Population room for its type (0x2b03e0: can_reinforce and pop <= cap - max(used, 0)); none -> nothing this poll.
//   3. u = (-cos θ, -sin θ), θ = the player's entry facing. Up to 10 tries (1 with R = 0): P = T + R u clamped into the
//      playable box, z = 0, tested with the host's space_drop_ok; a failure rotates u by +36°.
//   4. All failed -> R += 500 (the next poll tries the next ring). Pass -> the Reinforce event at P (the host spawns the
//      unit), the entry is popped, its spawned objects join the TF, R resets (1000 inside a prevention radius, else 0).
// Finished = timed out (0 <= timeout <= now) or no pending entry left; result = every entry placed. [port: the arrival
// signal 0x19 / in-flight count +0x78 are not modelled - the host's units exist at once and join the TF at the drop.]
void TacticalAiWorld::Impl::space_reinforce_poll(StatusRec& s) {
    TfRec* t = tf_rec(s.tf);
    auto done = [&s](bool ok) {
        s.finished = true;
        s.result = ok ? lua::AiStatusResult::True : lua::AiStatusResult::False;
    };
    if (!t) return done(false);
    if (!t->pending.empty()) {
        const TacticalReinforcement r = t->pending.back();
        const std::vector<TacticalReinforcement> pool = host.reinforcement_pool(t->player);
        const bool in_pool = std::any_of(pool.begin(), pool.end(), [&r](const TacticalReinforcement& e) { return e.key == r.key; });
        if (r.type < 0 || !in_pool) {
            t->pending.pop_back();  // step 1: no type -> pop it, stop for this call
            ++stats["Reinforce (space): pending entry dropped"];
        } else {
            const float cap = host.population_cap(t->player);
            const bool room = host.can_reinforce(t->player) &&
                              (cap < 0.0f || r.population <= cap - std::max(host.population_used(t->player), 0.0f));
            if (room) {
                SpaceDropFrame f;
                if (!host.space_drop_frame(t->player, f)) f = SpaceDropFrame{};
                double sn = 0.0, cs = 1.0;
                dmath::sincos(static_cast<double>(f.entry_facing_deg) * (dmath::kPi / 180.0), sn, cs);
                float ux = static_cast<float>(-cs), uy = static_cast<float>(-sn);
                double s36 = 0.0, c36 = 1.0;
                dmath::sincos(36.0 * (dmath::kPi / 180.0), s36, c36);
                const auto cos36 = static_cast<float>(c36), sin36 = static_cast<float>(s36);
                const int tries = s.ring == 0.0f ? 1 : 10;
                bool placed = false;
                for (int k = 0; k < tries && !placed; ++k) {
                    Vec3f p{s.dest.x + s.ring * ux, s.dest.y + s.ring * uy, 0.0f};
                    if (f.has_box) {
                        p.x = std::clamp(p.x, f.min_x, f.max_x);
                        p.y = std::clamp(p.y, f.min_y, f.max_y);
                    }
                    if (host.space_drop_ok(t->player, r.key, p, f.entry_facing_deg, cfg.constants.space_reinforcement_collision_check_distance)) {
                        const size_t before = store.owned_by(t->player).size();
                        cmd::EventData ev(cmd::EventType::Reinforce);
                        ev.set("unit", static_cast<uint32_t>(r.key)).set_f("x", p.x).set_f("y", p.y).set_f("z", p.z);
                        if (order(t->player, ev, std::string(cfg.campaign ? "campaign" : "skirmish") + " reinforce " + type_name(r.type) +
                                                     " (tf " + t->name + ")") == cmd::RejectReason::None) {
                            placed = true;
                            t->pending.pop_back();
                            const auto owned = store.owned_by(t->player);
                            for (size_t i = before; i < owned.size(); ++i)
                                if (store.active(owned[i]) && store.has_behavior(owned[i], objects::beh::Locomotor)) t->units.push_back(owned[i]);
                            s.ring = s.prevent ? 1000.0f : 0.0f;
                            break;
                        }
                    }
                    const float nx = ux * cos36 - uy * sin36;
                    const float ny = ux * sin36 + uy * cos36;
                    ux = nx;
                    uy = ny;
                }
                if (!placed) s.ring = s.ring + 500.0f;
            }
        }
    }
    const bool timed_out = s.timeout >= 0 && s.timeout <= now;
    if (timed_out || t->pending.empty()) done(t->pending.empty());
}

int32_t TacticalAiWorld::Impl::tf_build_all(int32_t tf) {
    TfRec* t = tf_rec(tf);
    if (t && t->reservations.empty()) claim_pads(*t);
    if (!t || t->reservations.empty()) return -1;
    uint32_t issued = 0;
    for (const auto& [pad, type] : t->reservations) {
        // The three refusals (§7.5), each logged.
        const PadCheck c = host.build_pad_check(t->player, pad, type);
        if (c != PadCheck::Ok) {
            static const char* why[] = {"", "pad is no longer friendly / visible", "pad may be occupied or enemies may be too close",
                                        "rebuild cooldown active"};
            decide(t->player, "build " + type_name(type) + " on pad " + std::to_string(pad) + " refused: " + why[static_cast<int>(c)]);
            continue;
        }
        cmd::EventData ev(cmd::EventType::TacticalBuild);
        ev.set("pad", static_cast<uint32_t>(pad)).set("type_index", static_cast<uint32_t>(type)).set("player", static_cast<uint32_t>(t->player));
        if (order(t->player, ev, "build " + type_name(type) + " on pad " + std::to_string(pad)) == cmd::RejectReason::None) ++issued;
    }
    return new_status(t->plan, tf, issued > 0 ? StatusKind::Done : StatusKind::Failed);
}

int32_t TacticalAiWorld::Impl::tf_build(int32_t tf, std::string_view type_name_in, const lua::AiArg& pad) {
    TfRec* t = tf_rec(tf);
    if (!t) return -1;
    int32_t type = -1;
    for (size_t k = 0; k < w.types.size() && type < 0; ++k)
        if (ai_iequal(w.types[k].name, type_name_in)) type = static_cast<int32_t>(k);
    if (type < 0) return -1;
    int32_t pad_id = pad.object ? id_of(pad.object) : -1;
    if (pad_id < 0 && t->reservations.empty()) claim_pads(*t);
    if (pad_id < 0)
        for (const auto& [p, ty] : t->reservations)
            if (ty == type) pad_id = p;
    if (pad_id < 0) return -1;
    const std::vector<std::pair<int32_t, int32_t>> saved = t->reservations;
    t->reservations = {{pad_id, type}};
    const int32_t s = tf_build_all(tf);
    if (TfRec* again = tf_rec(tf)) again->reservations = saved;
    return s;
}

// ================================================================ queries (§6)

int32_t TacticalAiWorld::Impl::find_target(int32_t script, const lua::AiFindTargetRequest& r) {
    const bool reachable = !r.reachability.empty();
    int32_t player = r.source.player;
    Vec3f origin;
    bool have_origin = false;
    if (player < 0 && r.source.task_force >= 0)
        if (TfRec* t = tf_rec(r.source.task_force)) {
            player = t->player;
            prune_tf(*t);
            if (!t->units.empty()) {
                origin = tf_center(*t);  // TF slot +0x170 [M: the task force's centre]
                have_origin = true;
            }
        }
    if (player < 0 && r.source.object) player = store.owner(r.source.object);
    if (player < 0) player = script_player(script);
    PlayerAi* pa = player_ai(player);
    if (!pa || !pa->ctx.initialised()) {
        ++stats["FindTarget: could not locate tactical AI manager"];
        return -1;
    }
    bool ok = true;
    const uint32_t flags = parse_application_flags(r.application, &ok);
    if (!ok || (flags & app::Global)) {
        ++stats["FindTarget: bad goal application " + r.application];
        return -1;
    }
    const auto p = static_cast<float>(r.probability);
    if (!valid_pick_probability(p)) {
        ++stats["FindTarget: probability out of range"];
        return -1;
    }
    const float wgt = pick_weight(p);
    Reachability level = Reachability::Any;
    if (reachable && !parse_reachability(r.reachability, level)) {
        ++stats["FindTarget: bad reachability " + r.reachability];
        return -1;
    }
    // Reachable_Target: the range / reachability source is parameter 6.
    int32_t source_zone = -1;
    if (reachable) {
        have_origin = false;
        if (r.relative.present && arg_position(r.relative, {}, origin)) {
            have_origin = true;
            const Slot* rs = r.relative.target >= 0 ? slot_of(r.relative.target) : nullptr;
            source_zone = rs && rs->t.zone >= 0 ? rs->t.zone : zone_at(origin);
        }
    }
    const auto max_range = static_cast<float>(r.max_distance > 0.0 ? r.max_distance : 0.0);
    std::vector<ScoredTarget> cands;
    for (size_t i = 0; i < pa->slots.size(); ++i) {
        const Slot& s = pa->slots[i];
        if (!s.alive || !pa->slot_applies(static_cast<int32_t>(i), flags)) continue;
        // A1_READINGS §5 [H]: Applies, reachability (targets with a reachability object, a source given), then binding +
        // Evaluate, then the range test: a filtered target makes no leaf-cache stores and no '#' draws. A source
        // without a zone gives 1e18 (§3.1), so every reachability target is skipped then.
        if (reachable && have_origin && s.t.has_reachability() && cfg.mode == TacticalMode::Land &&
            zone_reachability_cost(pa->zone_paths(), source_zone, s.t.zone, level, cfg.constants) >= kUnreachable)
            continue;
        double v = 0.0;
        if (!evaluate(player, r.perception, s.handle, v)) continue;
        if (max_range > 0.0f && have_origin) {
            const Vec3f q = slot_position(s);
            const float dx = q.x - origin.x, dy = q.y - origin.y, dz = q.z - origin.z;
            if (dx * dx + dy * dy + dz * dz > max_range * max_range) continue;
        }
        cands.push_back({static_cast<uint32_t>(i), v});
    }
    const TargetPick pick = select_target(cands, wgt, rng.synced());
    if (pick.target < 0) return -1;
    return target_id(player, pick.target);
}

ObjectHandle TacticalAiWorld::Impl::find_deadly_enemy(int32_t, const lua::AiArg& source, double radius, int32_t player) {
    const DamageTrackingStore* dt = host.damage_tracking();
    if (!dt) return {};
    const TeamMembersFn members = [this](int32_t id, std::vector<int32_t>& out) {
        if (const TacticalAiObject* f = fact(id)) out.insert(out.end(), f->team_members.begin(), f->team_members.end());
    };
    std::vector<int32_t> victims;
    if (radius > 0.0 && player >= 0) {
        // (location, radius, player): that player's DAMAGE_TRACKING objects within the radius, id order [M].
        Vec3f c;
        if (!arg_position(source, {}, c)) return {};
        std::vector<int32_t> ids;
        for (size_t i = 0; i < w.objects.size(); ++i) {
            const PerceptionObject& o = w.objects[i];
            if (!o.alive || o.owner != player || !dt->tracks(o.id)) continue;
            if (dist3(o.position, c) <= radius) ids.push_back(o.id);
        }
        std::sort(ids.begin(), ids.end());
        victims = std::move(ids);
    } else if (source.task_force >= 0) {
        TfRec* t = tf_rec(source.task_force);
        if (!t) return {};
        prune_tf(*t);
        for (const ObjectHandle u : t->units) collect_tracked(*dt, id_of(u), members, victims);
    } else {
        ObjectHandle h = source.object;
        if (!h && source.target >= 0) h = target_object(source.target);
        if (!store.alive(h)) return {};
        collect_tracked(*dt, id_of(h), members, victims);
    }
    const int32_t a = tactical::find_deadly_enemy(*dt, victims, [this](int32_t id) { return store.alive(store.find(id)); });
    return a == kNoAttacker ? ObjectHandle{} : store.find(a);
}

bool TacticalAiWorld::Impl::most_defended_position(int32_t, const lua::AiArg& source, const lua::AiPoint& point, int32_t player,
                                                   lua::AiPoint& out) {
    Vec3f p;
    if (!arg_position(source, point, p) || !w.is_player(player)) return false;
    DefendedPosition d;
    if (cfg.mode == TacticalMode::Land) {
        // The position's hint zone (land mode slot +0x180 [L: nearest zone centre]); no zones -> nil.
        int32_t z = -1;
        if (const Slot* s = source.target >= 0 ? slot_of(source.target) : nullptr; s && s->t.zone >= 0) z = s->t.zone;
        if (z < 0) z = zone_at(p);
        if (z < 0) return false;
        d = most_defended_position_cells(perc.grid(), w, perc.zone_threat_cells(z), player, rng.synced(),
                                         perc.config().memory_counted_when_not_fogged);
    } else {
        d = most_defended_position_space(perc.grid(), w, p, cfg.constants.space_region_size, player, rng.synced());
    }
    if (!d.found()) return false;
    out.present = true;
    out.x = d.position.x;
    out.y = d.position.y;
    out.z = d.position.z;
    return true;
}

void TacticalAiWorld::Impl::tf_set_flag(int32_t tf, lua::AiTfFlag flag, bool value) {
    TfRec* t = tf_rec(tf);
    const auto pit = t ? plans.find(t->plan) : plans.end();
    if (pit == plans.end()) return;
    switch (flag) {
        case lua::AiTfFlag::PlanResult: pit->second.result = value; break;
        case lua::AiTfFlag::Removable: pit->second.non_removable = !value; break;
        case lua::AiTfFlag::BlockGoalProposal:  // RetreatPlan: Block_Goal_Proposal() [M: the whole goal system]
            // A2 stage 2: the +0x2a0 list (AI_GALACTIC_2 §5.1), keyed by the plan instance
            if (PlayerAi* p = player_ai(t->player)) {
                if (value) p->ctx.goals.block_proposals(static_cast<uint32_t>(t->plan));
                else p->ctx.goals.unblock_proposals(static_cast<uint32_t>(t->plan));
            }
            break;
        default: break;  // Test_Target_Contrast: no PotentialPlan in tactical
    }
}

// ================================================================ Script_ evaluators

// The AI target id `Target` names for a perception handle: the first slot of `self` registered with it, else the first
// slot of the other players in player order [port: the original's context holds the AI target object itself; the
// port's contexts hold perception handles, so a slot with that handle stands for it]; -1 = none (Target = nil).
int32_t TacticalAiWorld::Impl::script_target(int32_t self, int32_t handle) const {
    if (handle < 0) return -1;
    auto find_in = [handle](const PlayerAi& p) -> int32_t {
        for (size_t i = 0; i < p.slots.size(); ++i)
            if (p.slots[i].handle == handle) return static_cast<int32_t>(i);
        return -1;
    };
    if (const PlayerAi* p = player_ai(self))
        if (const int32_t s = find_in(*p); s >= 0) return target_id(self, s);
    for (size_t q = 0; q < ai.size(); ++q)
        if (ai[q] && static_cast<int32_t>(q) != self)
            if (const int32_t s = find_in(*ai[q]); s >= 0) return target_id(static_cast<int32_t>(q), s);
    return -1;
}

// Script_<File>.Evaluate (AI_GALACTIC_2 §4.4, AI_TACTICAL_PERCEPTION §1: unchanged in tactical) [H]: the shared
// Data/Scripts/Evaluators/<File>.lua instance; PlayerObject = Self, Target = the context target as an AI target
// (AITargetLocationWrapper: Get_Game_Object / Get_Distance / Find_Nearest source), nil without one; then
// Evaluate(strings..., number) and Evaluator_Clean_Up(). Only a Lua number succeeds; no cache. The shipped tactical
// users: TargetHasProperty (Need_To_Secure_Contestable_Land), GetDistanceToNearestWithProperty
// (Need_To_PreDefend_Structure), GetDistanceToNearestSpaceField (space area equations); none draws synced RNG
// (FindTarget-based evaluators would, through find_target).
bool TacticalAiWorld::Impl::script_evaluate(const EvalContext& c, std::string_view name, std::span<const Param> params,
                                            double& out) {
    if (!files || !runtime) return false;
    lua::LuaVm* vm = scripts.vm(*runtime, *files, name, errors);
    if (!vm) {
        ++stats["script evaluator missing " + std::string(name)];
        return false;
    }
    vm->set_time(static_cast<double>(free_store_mode_time(mode_frame)));
    vm->set_frame(mode_frame);
    lua::set_player_global(*vm, "PlayerObject", c.self);
    lua::set_ai_target_global(*vm, "Target", script_target(c.self, c.target));
    const bool ok = ScriptEvaluators::evaluate(*vm, name, params, out, errors);
    ++stats[std::string("script evaluator ") + (ok ? "ok " : "FAIL ") + std::string(name)];
    return ok;
}

// ================================================================ TacticalAiWorld

TacticalAiWorld::TacticalAiWorld(objects::ObjectStore& store, const objects::GameObjectTypes* types, const FileProvider* files,
                                 GameRandom& rng, const AiDefinitions& defs, const EquationSet& equations, TacticalAiHost& host,
                                 TacticalAiConfig cfg)
    : impl_(std::make_unique<Impl>(store, types, files, rng, defs, equations, host, std::move(cfg))) {
    Impl& w = *impl_;
    w.world.set_players(w.cfg.lua_players);
    w.pipeline.set_handler([&w](uint64_t, const cmd::Command& c) {
        w.pp_slice.clear();  // OPT2 t10: an order may change what the host answers
        const cmd::RejectReason r = w.host.execute(c.header.player, c.payload);
        ++w.orders;
        if (r != cmd::RejectReason::None) ++w.rejected;
        return r;
    });
    w.bridge.pipeline = &w.pipeline;
    PlanScriptRuntime::VmSetup setup;
    setup.host = [&w] {
        lua::LuaHost h;
        h.store = &w.store;
        h.types = w.types;
        h.rng = &w.rng;
        h.campaign = w.cfg.campaign;
        h.files = w.files;
        return h;
    };
    setup.prepare = [&w](lua::LuaVm& vm) {
        lua::attach_player_world(vm, &w.world);
        lua::attach_object_bridge(vm, &w.bridge);
        lua::attach_ai_host(vm, &w, -1);
        lua::attach_ai_tactical_host(vm, &w);
        // (OI lua: EvaluatePerception / Find_Nearest / position arguments accept AI targets and task forces through
        // the attached hosts; the perception shim is no longer installed.)
    };
    w.runtime = std::make_unique<PlanScriptRuntime>(std::move(setup));
    w.perc.set_script_evaluator([&w](const EvalContext& c, std::string_view name, std::span<const Param> params, double& out) {
        return w.script_evaluate(c, name, params, out);
    });
    w.gate.start_game();
    // Shared perception: threat grid over the map, evaluator handles; land zone graph.
    w.perc.init(w.cfg.mode, w.cfg.constants, w.cfg.map, static_cast<uint32_t>(std::max<size_t>(w.cfg.lua_players.size(), 1)), w.cfg.zones);
    if (w.cfg.mode == TacticalMode::Land && !w.cfg.zones.empty()) w.zones.build(w.cfg.zones);
}

TacticalAiWorld::~TacticalAiWorld() {
    if (!impl_) return;
    for (auto& [id, p] : impl_->plans) p.vm.reset();
    for (auto& a : impl_->ai)
        if (a) a->fs_vm.reset();
    impl_->scripts.clear();
    impl_->runtime.reset();
}

lua::ObjectBridge& TacticalAiWorld::object_bridge() { return impl_->bridge; }
void TacticalAiWorld::set_global_values(lua::GlobalValueStore* store) { impl_->gv_store = store; }  // Int-final
lua::LuaPlayerWorld& TacticalAiWorld::player_world() { return impl_->world; }

bool TacticalAiWorld::load_plans(std::vector<std::string>* diagnostics) {
    Impl& w = *impl_;
    if (!w.files) return false;
    return w.runtime->load(*w.files, w.cfg.plan_files, w.defs, w.library, diagnostics, w.cfg.lua_gc_pf) > 0;
}

bool TacticalAiWorld::start(std::vector<std::string>* diagnostics) {
    Impl& w = *impl_;
    const AiMode mode = ai_mode_of(w.cfg.mode);
    const auto mi = static_cast<size_t>(mode);
    w.ai.clear();
    w.ai.resize(std::max<size_t>(w.cfg.lua_players.size(), 1));
    bool any = false;
    for (const TacticalAiPlayerConfig& pc : w.cfg.players) {
        if (pc.player < 0 || static_cast<size_t>(pc.player) >= w.ai.size()) continue;
        const AiPlayerTypeDef* type = w.defs.find_player(pc.ai_type);
        if (!type) {
            if (diagnostics) diagnostics->push_back("Unrecognized AI player type " + pc.ai_type);
            continue;
        }
        auto pa = std::make_unique<PlayerAi>(&w, pc.player, type, std::clamp(pc.difficulty, 0, 2));
        float sleep = 0.0f;
        const std::string& dname = type->difficulty[static_cast<size_t>(pa->difficulty)];
        for (const TacticalAiDifficulty& d : w.cfg.difficulties)
            if (ai_iequal(d.name, dname)) {
                sleep = w.cfg.mode == TacticalMode::Land ? d.land_goal_sleep : d.space_goal_sleep;
                pa->contrast_mult = w.cfg.mode == TacticalMode::Land ? d.land_contrast : d.space_contrast;  // A2 tactical
            }
        std::vector<GoalSource> sources;
        for (const std::string& set_name : type->function_sets) {
            const GoalFunctionSet* set = w.defs.find_set(set_name);
            if (!set) {
                if (diagnostics) diagnostics->push_back(type->name + ": unknown function set " + set_name);
                continue;
            }
            for (const GoalFunctionEntry& e : set->entries)
                if (w.defs.goals[e.goal].mode == mode) sources.push_back({e.goal, e.function});
        }
        std::vector<uint32_t> templates;
        for (const std::string& t : type->templates[mi]) {
            const uint32_t i = w.defs.template_index(t);
            if (i == kAiNone) {
                if (diagnostics) diagnostics->push_back(type->name + ": unknown template " + t);
            } else {
                templates.push_back(i);
            }
        }
        PlayerAi& p = *pa;
        p.ctx.templates.init(w.defs, w.library, std::move(templates));
        p.ctx.budget.reset(w.defs.categories.size());
        GoalSystemConfig gcfg;
        // Slot 11: Land_ / Space_AI_Goal_Cycle_Sleep_Duration x FPS after a commit (Easy 15 s, §4.5).
        gcfg.sleep_frames = static_cast<int32_t>(static_cast<float>(kAiFps) * sleep);
        p.ctx.goals.init(w.defs, std::move(sources), &p, &p.ctx.learning, &p.ctx.templates, &w.rng.synced(), gcfg);
        p.ctx.attach(&p, [&w, &p](const std::string& eq, double& out) { return w.evaluate(p.player, eq, -1, out); });
        if (w.cfg.stagger) {  // OPT2 t01: a fixed planning phase from the player's AI ordinal (players in index order)
            int32_t ordinal = 0;
            for (const TacticalAiPlayerConfig& o : w.cfg.players) ordinal += o.player < pc.player ? 1 : 0;
            p.ctx.set_stagger(ordinal, static_cast<int32_t>(w.cfg.players.size()));
        }
        // Free-store script (Land / Space FreeStoreScript of the AIPlayerType).
        const std::string& fs = type->free_store_script[mi];
        if (!fs.empty() && w.files) {
            std::vector<uint8_t> bytes;
            const std::string path = "Data\\Scripts\\FreeStore\\" + fs + ".lua";
            if (w.files->read(path, bytes)) {
                std::string err;
                p.fs_vm = w.runtime->make_vm(fs, std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()), path, &err);
                if (p.fs_vm) {
                    const int32_t sid = kFreeStoreScriptBase + pc.player;
                    lua::attach_ai_host(*p.fs_vm, &w, sid);
                    p.fs_vm->call("Base_Definitions", &err);
                    lua::set_free_store_global(*p.fs_vm, sid, false);
                    lua::set_player_global(*p.fs_vm, "PlayerObject", pc.player);
                    lua::set_number_global(*p.fs_vm, "LastService", 0.0);
                    lua::set_number_global(*p.fs_vm, "LastUnitService", 0.0);
                    p.sched = FreeStoreSchedule{};
                    if (!lua::global_is_nil(*p.fs_vm, "ServiceRate")) p.sched.service_rate = lua::global_number(*p.fs_vm, "ServiceRate", 0.0);
                    if (!lua::global_is_nil(*p.fs_vm, "UnitServiceRate"))
                        p.sched.unit_service_rate = lua::global_number(*p.fs_vm, "UnitServiceRate", 0.0);
                    p.fs_vm->create_thread("main");
                } else if (diagnostics) {
                    diagnostics->push_back(err);
                }
            } else if (diagnostics) {
                diagnostics->push_back(path + ": not found");
            }
        }
        w.ai[static_cast<size_t>(pc.player)] = std::move(pa);
        any = true;
    }
    return any;
}

void TacticalAiWorld::update(int64_t now, uint32_t mode_frame) {
    Impl& w = *impl_;
    w.now = now;
    w.mode_frame = mode_frame;
    for (auto& pa : w.ai)  // OPT2 t02: background land path recomputes due on this mode frame
        if (pa) pa->publish_paths(static_cast<int64_t>(mode_frame));
    {  // OPT2 t04: staggered scheduled Lua collections, once per update before any script of this world runs
        std::vector<lua::LuaVm*> vms;  // free stores by player, plans by id, evaluators by name (saved orders)
        for (const auto& pa : w.ai)
            if (pa && pa->fs_vm) vms.push_back(pa->fs_vm.get());
        for (const auto& [id, pi] : w.plans)
            if (pi.vm) vms.push_back(pi.vm.get());
        for (const auto& [key, e] : w.scripts.loaded_vms())
            if (e.vm) vms.push_back(e.vm.get());
        lua::collect_scheduled(vms, static_cast<uint64_t>(now), w.cfg.lua_gc_pf);
    }
    {
        ALAMO_PROFILE_SCOPE("tac.ai.perception");  // OPT2 t08: snapshot, threat grid, signals
        w.refresh();
        w.opportunity_events(w.host.opportunity_targets());  // STRATEGIC_GAPS §5.3
        w.ability_signals(w.host.ability_finished());       // S7 t15 A1: signal 0x29
        w.reinforcements_unloaded(w.host.reinforcements_unloaded());  // a1j t01: signal 0x19 (land landings)
        w.status_signals();                                  // S7 t21 / S7 t15 A1: ambush / ability status signals
    }
    const bool enabled = w.gate.enabled(ai_mode_of(w.cfg.mode));
    // OPT2 t08: one zone per AI player's service (goals, plans, free store; their Lua is also in lua.exec).
    static const profile::ZoneId kPlayerZone[] = {
        profile::register_zone("tac.ai.p0"), profile::register_zone("tac.ai.p1"), profile::register_zone("tac.ai.p2"),
        profile::register_zone("tac.ai.p3"), profile::register_zone("tac.ai.p4"), profile::register_zone("tac.ai.p5"),
        profile::register_zone("tac.ai.p6"), profile::register_zone("tac.ai.p7"), profile::register_zone("tac.ai.p8+")};
    for (auto& pa : w.ai)
        if (pa) {
            const profile::Scope zone(kPlayerZone[std::clamp<int32_t>(pa->player, 0, 8)]);
            pa->ctx.service(now, enabled, false);
        }
    // The pipeline posts a diagnostic CommandResult per direct order and nothing subscribes on this bus: drop them here
    // or the queue grows for the whole battle. Sim code never reads it (events.hpp), so no CRC or saved state depends on it.
    w.pipeline.events().deliver();
}

void TacticalAiWorld::end_battle() {
    Impl& w = *impl_;
    std::vector<int32_t> ids;
    for (const auto& [id, p] : w.plans) ids.push_back(id);
    for (const int32_t id : ids) w.end_plan(id, true);
    for (auto& a : w.ai)
        if (a && a->fs_vm) {
            append_script_errors(*a->fs_vm, "free store " + std::to_string(a->player) + ": ", w.errors);  // S7 t27
            a->fs_vm.reset();
        }
}

void TacticalAiWorld::set_suspended(bool suspended) { impl_->gate.suspend(suspended); }
void TacticalAiWorld::order_replaced(int32_t object_id) { impl_->order_signal(object_id); }
void TacticalAiWorld::refog(int32_t player, float x, float y) { impl_->perc.grid().refog_at(player, x, y, impl_->mode_frame); }
bool TacticalAiWorld::is_ai_player(int32_t player) const { return impl_->player_ai(player) != nullptr; }
int32_t TacticalAiWorld::opponent(int32_t player) const {
    const PlayerAi* p = impl_->player_ai(player);
    return p ? p->ctx.opponent : -1;
}
const AiContext* TacticalAiWorld::context(int32_t player) const {
    const PlayerAi* p = impl_->player_ai(player);
    return p ? &p->ctx : nullptr;
}
size_t TacticalAiWorld::free_store_size(int32_t player) const {
    const PlayerAi* p = impl_->player_ai(player);
    return p ? p->fs.size() : 0;
}
size_t TacticalAiWorld::background_jobs_in_flight() const {
    size_t n = 0;
    for (const auto& a : impl_->ai) n += (a && a->path_job.in_flight()) ? 1u : 0u;
    for (const auto& a : impl_->ai) n += (a && a->ctx.goals.accept_pending()) ? 1u : 0u;  // OPT2 t10: slot-10 solves
    return n;
}
size_t TacticalAiWorld::live_targets(int32_t player) const {
    const PlayerAi* p = impl_->player_ai(player);
    if (!p) return 0;
    size_t n = 0;
    for (const Slot& s : p->slots) n += s.alive ? 1u : 0u;
    return n;
}
size_t TacticalAiWorld::running_plans() const { return impl_->plans.size(); }
const std::vector<TacticalAiDecision>& TacticalAiWorld::decision_log() const { return impl_->log; }
std::vector<std::string> TacticalAiWorld::lua_errors() const {
    std::vector<std::string> e = impl_->errors;
    for (const auto& [id, p] : impl_->plans)
        if (p.vm) append_script_errors(*p.vm, impl_->library.plans[p.plan].name + ": ", e);  // S7 t27: with SCRIPT ERROR lines
    for (const auto& a : impl_->ai)
        if (a && a->fs_vm) append_script_errors(*a->fs_vm, "free store " + std::to_string(a->player) + ": ", e);
    return e;
}
uint64_t TacticalAiWorld::orders_issued() const { return impl_->orders; }
uint64_t TacticalAiWorld::orders_rejected() const { return impl_->rejected; }
size_t TacticalAiWorld::pending_order_results() const { return impl_->pipeline.events().pending().size(); }
const std::map<std::string, uint32_t>& TacticalAiWorld::stats() const {
    impl_->stats["~ evaluations"] = static_cast<uint32_t>(impl_->eval_count);
    return impl_->stats;
}
const PlanLibrary& TacticalAiWorld::plans() const { return impl_->library; }
const PerceptionWorld& TacticalAiWorld::world() const { return impl_->w; }
float TacticalAiWorld::point_danger(int32_t player, float x, float y) const { return impl_->perc.grid().point_danger(impl_->w, player, x, y); }

void TacticalAiWorld::hash(StateHasher& h) const {
    const Impl& w = *impl_;
    h.u32(static_cast<uint32_t>(w.ai.size()));
    for (const auto& a : w.ai) {
        h.boolean(a != nullptr);
        if (!a) continue;
        a->ctx.hash(h);
        h.u32(static_cast<uint32_t>(a->slots.size()));
        for (const Slot& s : a->slots) {
            h.u8(static_cast<uint8_t>(s.t.kind));
            h.i32(s.t.object_id);
            h.i32(s.t.zone);
            h.i32(s.t.cell_x);
            h.i32(s.t.cell_y);
            h.i32(s.handle);
            h.boolean(s.alive);
        }
        a->fs.hash(h);
        a->sched.hash(h);
        h.u32(static_cast<uint32_t>(a->bonuses.size()));
        if (a->paths_built) {  // land path finder (TACTICAL_GAPS §6.1): cadence + all-pairs matrices
            h.i32(a->next_recompute);
            a->paths.hash(h);
            if (a->path_job.in_flight()) h.i64(a->path_job.ready_frame());  // OPT2 t02 (nothing extra in the original timing)
        }
    }
    w.perc.hash(h);
    h.u32(static_cast<uint32_t>(w.plans.size()));
    for (const auto& [id, p] : w.plans) {
        h.i32(id);
        h.i32(p.player);
        h.u32(p.plan);
        h.u32(p.goal_type);
        h.i32(p.slot);
        h.boolean(p.result);
        h.boolean(p.non_removable);
        h.u32(p.live_threads);  // cached on the sim thread (no VM access from hashers)
    }
    h.u32(static_cast<uint32_t>(w.tfs.size()));
    for (const auto& [id, t] : w.tfs) {
        h.i32(id);
        h.u32(static_cast<uint32_t>(t.units.size()));
        for (const ObjectHandle u : t.units) h.i32(w.store.id(u));
        h.u32(static_cast<uint32_t>(t.pending.size()));
        for (const TacticalReinforcement& r : t.pending) h.i32(r.key);
        h.u32(static_cast<uint32_t>(t.reservations.size()));
        if (!t.producing.empty()) {  // only when used, so runs without tactical production keep their CRC
            h.u32(static_cast<uint32_t>(t.producing.size()));
            for (const auto& [ty, b] : t.producing) {
                h.i32(ty);
                h.i32(b);
            }
        }
    }
    // A2 tactical: PotentialPlans and reservations.
    h.u32(static_cast<uint32_t>(w.pps.size()));
    for (const auto& [id, pp] : w.pps) {
        h.i32(id);
        h.i32(pp.player);
        h.u32(pp.plan);
        h.boolean(pp.reserved);
        h.u32(static_cast<uint32_t>(pp.picks.size()));
        for (const TacticalPpPick& k : pp.picks) {
            h.i32(k.type);
            h.u8(static_cast<uint8_t>(k.source));
            h.i32(k.ref);
            h.i32(k.tf);
            h.i32(k.team);
        }
    }
    for (const auto* m : {&w.pool_reserved, &w.pad_reserved}) {
        h.u32(static_cast<uint32_t>(m->size()));
        for (const auto& [k, v] : *m) {
            h.i32(k);
            h.i32(v);
        }
    }
    h.u32(static_cast<uint32_t>(w.statuses.size()));
    for (const auto& [id, s] : w.statuses) {
        h.i32(id);
        h.boolean(s.finished);
        h.u8(static_cast<uint8_t>(s.result));
    }
    h.u32(static_cast<uint32_t>(w.globals.size()));
    for (const auto& [k, v] : w.globals) {
        h.str(k);
        lua::hash_global_value(h, v);  // L1 final: same bytes for non-table values
    }
    w.bridge.hash(h);
    h.u64(w.orders);
    h.u64(w.rejected);
}

void TacticalAiWorld::register_crc(CrcRegistry& registry, const std::string& name) const {
    registry.add(name, [this](StateHasher& h) { hash(h); });
}

// ================================================================ N1 save / load (tactical_ai.hpp)

namespace {

namespace io = save_io;

constexpr uint32_t kTacticalAiSaveVersion = 10;  // 2: six space free-store facts; 3: re-projection stamp; 4: flood sets;
                                                 // 5: refixf t06 space reinforcement status fields; 6: a1g t01 PP+0x100,
                                                 // entry +0x30 moved into the free store; 7: S7 t20/t21/t15 A1 (no had_units /
                                                 // no_units_sent; ambush / ability status fields); 8: a1j t01 land
                                                 // reinforcement status (landings in flight, saved removable flag); 9: OPT3
                                                 // t04 the in-flight background land path recompute; 10: OPT2 t10
                                                 // the pending slot-10 solves (plans + results)
constexpr int kMaxGlobalDepth = 32;
// Lower bounds of one record (count checks only).
constexpr size_t kFactBytes = 12 + 5 + 13 + 17 + 4 + 6;
constexpr size_t kSlotBytes = io::kTargetBytes + 5;
constexpr size_t kPlanBytes = 4 + 4 + 4 + 4 + 4 + 4 + 4 + 4 + 3 + 4 + 1;
constexpr size_t kTfBytes = 4 + 4 + 4 + 4 + 4 + 1 + 4 + 4 + 4 + 4 + 4;
constexpr size_t kReinforcementBytes = 4 + 4 + 4 + 4 + 4 + 1 + 4;
constexpr size_t kStatusBytes = 4 + 4 + 4 + 1 + 1 + 1 + 8 + 8 + 12 + 4 + 12 + 8 + 4 + 1 + 1 + 4 + 4 + 4 + 1 + 4 + 4 + 4 + 1;

void put_handle(ByteWriter& w, const objects::ObjectStore& st, ObjectHandle h) {
    w.u64(h.value);
    w.i32(st.exists(h) ? st.id(h) : objects::kNoObjectId);
}

// The handle of the saved id in the restored store; a handle that was stale (or null) when saved stays stale.
ObjectHandle get_handle(ByteReader& r, const objects::ObjectStore& st) {
    const ObjectHandle raw{r.u64()};
    const objects::ObjectId id = r.i32();
    if (!r.ok()) return {};
    if (id != objects::kNoObjectId) {
        const ObjectHandle h = st.find(id);
        if (!h) r.fail();  // the restored store does not know the object
        return h;
    }
    return st.exists(raw) ? ObjectHandle{} : raw;
}

void put_handles(ByteWriter& w, const objects::ObjectStore& st, const std::vector<ObjectHandle>& v) {
    io::put_count(w, v.size());
    for (const ObjectHandle h : v) put_handle(w, st, h);
}

bool get_handles(ByteReader& r, const objects::ObjectStore& st, std::vector<ObjectHandle>& out) {
    std::vector<ObjectHandle> v(io::count(r, 12));
    for (ObjectHandle& h : v) h = get_handle(r, st);
    if (!r.ok()) return false;
    out = std::move(v);
    return true;
}

// A Lua VM: its LuaVm::write image, length-prefixed.
void put_vm(ByteWriter& w, const lua::LuaVm& vm) {
    ByteWriter img;
    vm.write(img);
    io::put_count(w, img.size());
    w.raw(img.bytes());
}

std::span<const uint8_t> get_image(ByteReader& r) {
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

void put_value(ByteWriter& w, const lua::AiGlobalValue& v) {
    w.u8(static_cast<uint8_t>(v.kind));
    w.f64(v.number);
    w.str(v.text);
    if (v.kind == lua::AiGlobalValue::Table) {
        io::put_count(w, v.keys.size());
        for (size_t i = 0; i < v.keys.size(); ++i) {
            put_value(w, v.keys[i]);
            put_value(w, i < v.values.size() ? v.values[i] : lua::AiGlobalValue{});
        }
    }
}

bool get_value(ByteReader& r, lua::AiGlobalValue& v, int depth) {
    const uint8_t kind = r.u8();
    if (kind > lua::AiGlobalValue::Table || depth > kMaxGlobalDepth) r.fail();
    v.kind = static_cast<lua::AiGlobalValue::Kind>(kind);
    v.number = r.f64();
    v.text = r.str();
    v.keys.clear();
    v.values.clear();
    if (r.ok() && v.kind == lua::AiGlobalValue::Table) {
        const uint32_t n = io::count(r, 26);
        v.keys.resize(n);
        v.values.resize(n);
        for (uint32_t i = 0; i < n && r.ok(); ++i)
            if (!get_value(r, v.keys[i], depth + 1) || !get_value(r, v.values[i], depth + 1)) return false;
    }
    return r.ok();
}

// The 23 admission facts of FreeStoreObjectInfo in declaration order (the 17 land / common facts, then the six
// space facts of A1_READINGS §6.1, save version 2).
template <class F>
auto free_store_flags(F& f) {
    return std::array<decltype(&f.owned_by_player), 23>{
        &f.owned_by_player,         &f.is_transport,         &f.type_create_team,     &f.type_is_dummy,
        &f.type_stay_in_transport,  &f.has_indigenous_or_explosion, &f.has_lobbing_superweapon, &f.has_locomotor,
        &f.is_starship_combatant,   &f.is_team,              &f.type_category_bit_5f, &f.team_has_bit_20,
        &f.type_bit_5b,             &f.subobject_f8_nonempty, &f.in_mode_manager_set, &f.dead_or_removing,
        &f.in_world,                &f.type_squadron_member, &f.type_company_member,  &f.has_special_weapon,
        &f.type_dummy_orbital_structure, &f.type_dummy_star_base, &f.has_tactical_super_weapon};
}

void put_fact(ByteWriter& w, const objects::ObjectStore& st, const TacticalAiObject& f) {
    put_handle(w, st, f.handle);
    w.i32(f.target.id);
    w.boolean(f.target.dead_or_removing);
    w.boolean(f.target.rep_has_land_evaluator);
    w.boolean(f.target.rep_has_space_evaluator);
    w.boolean(f.target.own_has_space_evaluator);
    const UnitAppliesInfo& a = f.applies;
    w.i32(a.object_id);
    w.boolean(a.has_owner);
    w.boolean(a.owner_allied);
    w.boolean(a.has_locomotor);
    w.boolean(a.has_build_objects);
    w.boolean(a.has_cash_point);
    w.f32(a.reinforcement_enabling_radius);
    w.boolean(a.last_state_visible_under_fow);
    w.boolean(a.initial_state_visible_under_fow);
    for (const bool* b : free_store_flags(f.free_store)) w.boolean(*b);
    io::put_vec(w, f.team_members);
    w.boolean(f.moving);
    w.i32(f.attack_target);
    w.boolean(f.damage_tracking);
    w.i32(f.reprojection_stamp);
}

bool get_fact(ByteReader& r, const objects::ObjectStore& st, TacticalAiObject& f) {
    f.handle = get_handle(r, st);
    f.target.id = r.i32();
    f.target.dead_or_removing = r.boolean();
    f.target.rep_has_land_evaluator = r.boolean();
    f.target.rep_has_space_evaluator = r.boolean();
    f.target.own_has_space_evaluator = r.boolean();
    UnitAppliesInfo& a = f.applies;
    a.object_id = r.i32();
    a.has_owner = r.boolean();
    a.owner_allied = r.boolean();
    a.has_locomotor = r.boolean();
    a.has_build_objects = r.boolean();
    a.has_cash_point = r.boolean();
    a.reinforcement_enabling_radius = r.f32();
    a.last_state_visible_under_fow = r.boolean();
    a.initial_state_visible_under_fow = r.boolean();
    for (bool* b : free_store_flags(f.free_store)) *b = r.boolean();
    if (!r.ok() || !io::get_vec(r, f.team_members)) return false;
    f.moving = r.boolean();
    f.attack_target = r.i32();
    f.damage_tracking = r.boolean();
    f.reprojection_stamp = r.i32();
    return r.ok();
}

void put_reinforcement(ByteWriter& w, const TacticalReinforcement& e) {
    w.i32(e.key);
    w.i32(e.type);
    w.f32(e.population);
    w.u32(e.units);
    w.i32(e.member_type);
    w.boolean(e.member_create_team);
    w.i32(e.count);
}

void get_reinforcement(ByteReader& r, TacticalReinforcement& e) {
    e.key = r.i32();
    e.type = r.i32();
    e.population = r.f32();
    e.units = r.u32();
    e.member_type = r.i32();
    e.member_create_team = r.boolean();
    e.count = r.i32();
}

void put_pairs(ByteWriter& w, const std::vector<std::pair<int32_t, int32_t>>& v) {
    io::put_count(w, v.size());
    for (const auto& [a, b] : v) {
        w.i32(a);
        w.i32(b);
    }
}

bool get_pairs(ByteReader& r, std::vector<std::pair<int32_t, int32_t>>& out) {
    std::vector<std::pair<int32_t, int32_t>> v(io::count(r, 8));
    for (auto& [a, b] : v) {
        a = r.i32();
        b = r.i32();
    }
    if (!r.ok()) return false;
    out = std::move(v);
    return true;
}

void put_int_map(ByteWriter& w, const std::map<int32_t, int32_t>& m) {
    io::put_count(w, m.size());
    for (const auto& [k, v] : m) {
        w.i32(k);
        w.i32(v);
    }
}

bool get_int_map(ByteReader& r, std::map<int32_t, int32_t>& out) {
    std::map<int32_t, int32_t> m;
    const uint32_t n = io::count(r, 8);
    for (uint32_t i = 0; i < n && r.ok(); ++i) {
        const int32_t k = r.i32();
        const int32_t v = r.i32();
        if (!r.ok() || (!m.empty() && m.rbegin()->first >= k)) return io::bad(r);
        m.emplace_hint(m.end(), k, v);
    }
    if (!r.ok()) return false;
    out = std::move(m);
    return true;
}

// Every AI VM carries this world's four host pointers (registry light userdata; prepare() / instantiate attach them).
// LuaVm::read keeps the blank VM's attachments, so this normally changes nothing; only pointers that differ are
// re-attached (attach_ai_host also rewrites the script id, which the restored Lua state already holds).
void rebind_vm(TacticalAiWorld::Impl& m, lua::LuaVm& vm, int32_t script_id) {
    if (lua::player_world(vm) != &m.world) lua::attach_player_world(vm, &m.world);
    if (lua::attached_object_bridge(vm) != &m.bridge) lua::attach_object_bridge(vm, &m.bridge);
    if (lua::ai_host(vm) != static_cast<lua::AiScriptHost*>(&m)) lua::attach_ai_host(vm, &m, script_id);
    if (lua::ai_tactical_host(vm) != static_cast<lua::AiTacticalScriptHost*>(&m)) lua::attach_ai_tactical_host(vm, &m);
}

}  // namespace

void TacticalAiWorld::write(ByteWriter& w) const {
    const Impl& m = *impl_;
    const objects::ObjectStore& st = m.store;
    w.u32(kTacticalAiSaveVersion);
    // ---- shape ----
    io::put_count(w, m.ai.size());
    for (const auto& a : m.ai) w.boolean(a != nullptr);
    io::put_count(w, m.library.plans.size());
    io::put_count(w, m.runtime->plan_count());
    w.u8(static_cast<uint8_t>(m.cfg.mode));
    // ---- counters ----
    w.i64(m.now);
    w.u32(m.mode_frame);
    w.i32(m.current_plan);
    for (const int32_t c : {m.next_plan, m.next_tf, m.next_status, m.next_pp}) w.i32(c);
    w.u64(m.orders);
    w.u64(m.rejected);
    m.gate.write(w);
    // ---- the last update's snapshot ----
    m.w.write(w);
    io::put_count(w, m.facts.size());
    for (const TacticalAiObject& f : m.facts) put_fact(w, st, f);
    io::put_count(w, m.prev.size());
    for (const auto& [id, owner] : m.prev) {
        w.i32(id);
        w.i32(owner);
    }
    // ---- shared perception (length-prefixed: read in place as the last fallible step) ----
    {
        ByteWriter pw;
        m.perc.write(pw);
        io::put_count(w, pw.size());
        w.raw(pw.bytes());
    }
    // ---- Lua world, object bridge, GlobalValue map, '#' reseed names ----
    m.world.write(w);
    m.bridge.write(w);
    io::put_count(w, m.globals.size());
    for (const auto& [k, v] : m.globals) {
        w.str(k);
        put_value(w, v);
    }
    io::put_count(w, m.handle_name_crc.size());
    for (const auto& [h, c] : m.handle_name_crc) {
        w.i32(h);
        w.u32(c);
    }
    // ---- AI players ----
    for (const auto& a : m.ai) {
        if (!a) continue;
        const PlayerAi& p = *a;
        p.list.write(w);
        io::put_count(w, p.slots.size());
        for (const Slot& s : p.slots) {
            io::put_target(w, s.t);
            w.i32(s.handle);
            w.boolean(s.alive);
        }
        p.ctx.write(w);
        p.fs.write(w);
        p.sched.write(w);
        w.boolean(p.fs_vm != nullptr);
        if (p.fs_vm) put_vm(w, *p.fs_vm);
        io::put_count(w, p.bonuses.size());
        for (const PendingBonus& b : p.bonuses) {
            w.u32(b.goal_type);
            w.i32(b.target);
            w.f32(b.bonus);
            w.i64(b.deadline);
        }
        io::put_count(w, p.floods.size());
        for (const auto& [key, f] : p.floods) {
            w.i32(key.first);
            w.u8(key.second);
            w.u32(f.stamp);
            io::put_vec(w, f.zones);
        }
        w.boolean(p.paths_built);
        w.i32(p.next_recompute);
        if (p.paths_built) p.paths.write(w);
        w.boolean(p.path_job.in_flight());  // 8: OPT3 t04, the in-flight background path recompute
        if (p.path_job.in_flight()) {
            w.i64(p.path_job.kick_frame());
            w.i64(p.path_job.ready_frame());
            io::put_vec(w, p.path_pend);
        }
        // 10: OPT2 t10, the pending slot-10 solves. Their job is pure, so the save waits for it and writes the results
        // (smaller than the inputs); the loaded run applies them on the goal system's saved ready frame.
        const bool pending = p.ctx.goals.accept_pending();
        w.boolean(pending);
        if (pending) {
            p.fetch_pp();
            io::put_vec(w, p.pp_plans);
            io::put_count(w, p.pp_out.size());
            for (const TacticalPpResult& o : p.pp_out) {
                w.boolean(o.ok);
                w.u32(static_cast<uint32_t>(o.candidates));
                w.str(o.last_veto);
                io::put_count(w, o.picks.size());
                for (const TacticalPpPick& k : o.picks) {
                    w.i32(k.type);
                    w.u8(static_cast<uint8_t>(k.source));
                    w.i32(k.ref);
                    w.i32(k.tf);
                    w.i32(k.team);
                }
            }
        }
    }
    // ---- plans ----
    io::put_count(w, m.plans.size());
    for (const auto& [id, pi] : m.plans) {
        w.i32(id);
        w.i32(pi.player);
        w.u32(pi.plan);
        w.u32(pi.goal_type);
        w.i32(pi.slot);
        w.i32(pi.target_owner);
        w.i32(pi.pp);
        io::put_vec(w, pi.tfs);
        w.boolean(pi.result);
        w.boolean(pi.non_removable);
        w.boolean(pi.ended);
        w.u32(pi.live_threads);
        w.boolean(pi.vm != nullptr);
        if (pi.vm) put_vm(w, *pi.vm);
    }
    // ---- task forces ----
    io::put_count(w, m.tfs.size());
    for (const auto& [id, t] : m.tfs) {
        w.i32(id);
        w.i32(t.plan);
        w.i32(t.player);
        w.u32(t.def);
        w.str(t.name);
        w.u8(static_cast<uint8_t>(t.kind));
        put_handles(w, st, t.units);
        io::put_count(w, t.pending.size());
        for (const TacticalReinforcement& e : t.pending) put_reinforcement(w, e);
        put_pairs(w, t.reservations);
        put_pairs(w, t.producing);
        w.str(t.priorities);
    }
    // ---- blocking statuses ----
    io::put_count(w, m.statuses.size());
    for (const auto& [id, s] : m.statuses) {
        w.i32(id);
        w.i32(s.plan);
        w.i32(s.tf);
        w.u8(static_cast<uint8_t>(s.kind));
        w.boolean(s.finished);
        w.u8(static_cast<uint8_t>(s.result));
        w.i64(s.started);
        w.i64(s.last_order);
        put_handle(w, st, s.unit);
        w.i32(s.target);
        io::put_vec3(w, s.dest);
        w.i64(s.timeout).f32(s.ring).boolean(s.prevent);  // refixf t06
        w.u8(static_cast<uint8_t>(s.direction)).f32(s.min_dist).f32(s.threat).u32(s.plan_frame).boolean(s.lists);  // S7 t21
        for (const std::vector<int32_t>* v : {&s.members, &s.moving}) {
            io::put_count(w, v->size());
            for (const int32_t oid : *v) w.i32(oid);
        }
        put_pairs(w, s.landings);  // a1j t01
        w.boolean(s.saved_removable);
    }
    // ---- PotentialPlans + reservations ----
    io::put_count(w, m.pps.size());
    for (const auto& [id, pp] : m.pps) {
        w.i32(id);
        w.i32(pp.player);
        w.u32(pp.plan);
        w.boolean(pp.reserved);
        io::put_count(w, pp.picks.size());
        for (const TacticalPpPick& k : pp.picks) {
            w.i32(k.type);
            w.u8(static_cast<uint8_t>(k.source));
            w.i32(k.ref);
            w.i32(k.tf);
            w.i32(k.team);
        }
    }
    put_int_map(w, m.pool_reserved);
    put_int_map(w, m.pad_reserved);
    // ---- opportunity-fire subscriptions (AiScriptHost's lists; task-force ids are < next_tf) ----
    std::vector<int32_t> subscribed;
    for (int32_t t = 0; t < m.next_tf; ++t)
        if (const std::vector<int32_t>* s = m.opportunity_fire_subscriptions(t); s && !s->empty()) subscribed.push_back(t);
    io::put_count(w, subscribed.size());
    for (const int32_t t : subscribed) {
        w.i32(t);
        io::put_vec(w, *m.opportunity_fire_subscriptions(t));
    }
    // ---- plan runtime pools (a still-pristine VM by digest), created counter, Script_ evaluators ----
    const PlanScriptRuntime& rt = *m.runtime;
    for (uint32_t p = 0; p < rt.plan_count(); ++p) {
        const auto& pool = rt.pool(p);
        io::put_count(w, pool.size());
        for (const auto& vm : pool) {
            const bool pristine = vm.get() == rt.pristine(p);
            w.boolean(pristine);
            if (pristine)
                w.u64(rt.pristine_digest(p));
            else
                put_vm(w, *vm);
        }
    }
    w.u64(static_cast<uint64_t>(rt.vms_created()));
    const auto& evals = m.scripts.loaded_vms();
    io::put_count(w, evals.size());
    for (const auto& [key, e] : evals) {
        w.str(key);
        w.str(e.name);
        put_vm(w, *e.vm);
    }
    io::put_count(w, m.scripts.missing().size());
    for (const std::string& s : m.scripts.missing()) w.str(s);
}

bool TacticalAiWorld::read(ByteReader& r) {
    Impl& m = *impl_;
    const objects::ObjectStore& st = m.store;
    const size_t lib = m.library.plans.size();
    const size_t goal_types = m.defs.goals.size();
    const auto mi = static_cast<size_t>(ai_mode_of(m.cfg.mode));
    if (!io::version(r, kTacticalAiSaveVersion)) return false;
    // ---- shape: the loader rebuilt the same session ----
    if (r.u32() != m.ai.size() || !r.ok()) return io::bad(r);
    for (const auto& a : m.ai)
        if (r.boolean() != (a != nullptr) || !r.ok()) return io::bad(r);
    if (r.u32() != lib || r.u32() != m.runtime->plan_count() || r.u8() != static_cast<uint8_t>(m.cfg.mode) || !r.ok())
        return io::bad(r);
    auto valid_ai = [&m](int32_t p) { return m.player_ai(p) != nullptr; };
    // ---- counters ----
    const int64_t saved_now = r.i64();
    const uint32_t saved_mode_frame = r.u32();
    const int32_t saved_current_plan = r.i32();
    std::array<int32_t, 4> next{};
    for (int32_t& c : next) c = r.i32();
    const uint64_t saved_orders = r.u64(), saved_rejected = r.u64();
    if (!r.ok() || next[0] < 1 || next[1] < 0 || next[2] < 0 || next[3] < 0) return io::bad(r);
    AiEnableGate new_gate = m.gate;
    if (!new_gate.read(r)) return false;
    // ---- the last update's snapshot ----
    PerceptionWorld new_w;
    uint32_t type_count = 0;
    uint64_t types_digest = 0;
    if (!new_w.read(r, type_count, types_digest)) return false;
    if (!m.w.types.empty() && (type_count != m.w.types.size() || types_digest != m.w.types_digest())) return io::bad(r);
    std::vector<TacticalAiObject> new_facts(io::count(r, kFactBytes));
    for (TacticalAiObject& f : new_facts)
        if (!get_fact(r, st, f)) return false;
    if (!r.ok() || new_facts.size() != new_w.objects.size()) return io::bad(r);  // facts[i] belongs to objects[i]
    std::vector<std::pair<int32_t, int32_t>> new_prev(io::count(r, 8));
    for (size_t i = 0; i < new_prev.size(); ++i) {
        new_prev[i].first = r.i32();
        new_prev[i].second = r.i32();
        if (i > 0 && new_prev[i - 1].first >= new_prev[i].first) r.fail();  // ascending ids (binary searched)
    }
    const std::span<const uint8_t> perc_image = get_image(r);
    if (!r.ok()) return false;
    // ---- Lua world, object bridge, GlobalValue map, '#' reseed names ----
    lua::LuaPlayerWorld new_world = m.world;
    if (!new_world.read(r)) return false;
    lua::ObjectBridge new_bridge = m.bridge;
    if (!new_bridge.read(r)) return false;
    std::map<std::string, lua::AiGlobalValue> new_globals;
    {
        const uint32_t n = io::count(r, 17);
        for (uint32_t i = 0; i < n && r.ok(); ++i) {
            std::string k = r.str();
            lua::AiGlobalValue v;
            if (!get_value(r, v, 0)) return false;
            if (!new_globals.empty() && !(new_globals.rbegin()->first < k)) return io::bad(r);
            new_globals.emplace_hint(new_globals.end(), std::move(k), std::move(v));
        }
    }
    std::map<int32_t, uint32_t> new_names;
    {
        const uint32_t n = io::count(r, 8);
        for (uint32_t i = 0; i < n && r.ok(); ++i) {
            const int32_t h = r.i32();
            const uint32_t c = r.u32();
            if (!r.ok() || (!new_names.empty() && new_names.rbegin()->first >= h)) return io::bad(r);
            new_names.emplace_hint(new_names.end(), h, c);
        }
    }
    if (!r.ok()) return false;
    // ---- AI players ----
    struct StagedPlayer {
        TacticalTargetList list{TacticalMode::Land};
        std::vector<Slot> slots;
        AiContext ctx{AiMode::Land};
        TacticalFreeStore fs;
        FreeStoreSchedule sched;
        bool has_fs = false;
        std::unique_ptr<lua::LuaVm> fs_vm;
        std::vector<PendingBonus> bonuses;
        std::map<std::pair<int32_t, uint8_t>, PlayerAi::FloodSet> floods;
        bool paths_built = false;
        int32_t next_recompute = 0;
        ZoneGraph paths;
        bool path_in_flight = false;  // OPT3 t04
        int64_t path_kick = 0, path_ready = 0;
        std::vector<uint32_t> path_pend;
        bool pp_pending = false;  // OPT2 t10
        std::vector<uint32_t> pp_plans;
        std::vector<TacticalPpResult> pp_out;
    };
    std::vector<StagedPlayer> staged(m.ai.size());
    for (size_t i = 0; i < m.ai.size(); ++i) {
        if (!m.ai[i]) continue;
        PlayerAi& pa = *m.ai[i];
        StagedPlayer& sp = staged[i];
        sp.list = pa.list;
        if (!sp.list.read(r)) return false;
        sp.slots.resize(io::count(r, kSlotBytes));
        for (Slot& s : sp.slots) {
            if (!io::get_target(r, s.t)) return false;
            s.handle = r.i32();
            s.alive = r.boolean();
        }
        if (!r.ok()) return false;
        sp.ctx = pa.ctx;
        if (!sp.ctx.read(r)) return false;
        for (const auto* v : {&sp.ctx.goals.active(), &sp.ctx.goals.proposed()})
            for (const auto& cat : *v)
                for (const Goal& g : cat)
                    if (g.target < -1 || (g.target >= 0 && static_cast<size_t>(g.target) >= sp.slots.size()) ||
                        (g.plan != kAiNone && g.plan >= lib))
                        return io::bad(r);
        if (!sp.fs.read(r) || !sp.sched.read(r)) return false;
        sp.has_fs = r.boolean();
        if (!r.ok()) return false;
        if (sp.has_fs) {
            const std::span<const uint8_t> img = get_image(r);
            if (!r.ok()) return false;
            sp.fs_vm = m.runtime->blank_vm(pa.type->free_store_script[mi]);
            if (!read_vm_image(img, *sp.fs_vm)) return io::bad(r);
            rebind_vm(m, *sp.fs_vm, kFreeStoreScriptBase + pa.player);
        }
        sp.bonuses.resize(io::count(r, 20));
        for (PendingBonus& b : sp.bonuses) {
            b.goal_type = r.u32();
            b.target = r.i32();
            b.bonus = r.f32();
            b.deadline = r.i64();
            if (b.goal_type >= goal_types) r.fail();
        }
        const uint32_t nf = io::count(r, 4 + 1 + 4 + 4);
        for (uint32_t k = 0; k < nf && r.ok(); ++k) {
            const int32_t zone = r.i32();
            const uint8_t level = r.u8();
            PlayerAi::FloodSet f;
            f.stamp = r.u32();
            if (!io::get_vec(r, f.zones)) return false;
            for (const uint32_t z : f.zones)
                if (z >= m.zones.size()) return io::bad(r);
            if (zone < 0 || static_cast<uint32_t>(zone) >= m.zones.size() || level > static_cast<uint8_t>(Reachability::AnyThreat))
                return io::bad(r);
            sp.floods[{zone, level}] = std::move(f);
        }
        sp.paths_built = r.boolean();
        sp.next_recompute = r.i32();
        if (!r.ok()) return false;
        if (sp.paths_built) {
            sp.paths = m.zones;  // the player's graph is a copy of the battle's, re-weighted by its threat
            if (!sp.paths.read(r)) return false;
        }
        sp.path_in_flight = r.boolean();
        if (sp.path_in_flight) {
            sp.path_kick = r.i64();
            sp.path_ready = r.i64();
            if (!io::get_vec(r, sp.path_pend)) return false;
            if (!sp.paths_built || sp.path_ready < sp.path_kick || sp.path_pend.size() != m.zones.size()) return io::bad(r);
        }
        sp.pp_pending = r.boolean();
        if (sp.pp_pending) {
            if (!io::get_vec(r, sp.pp_plans)) return false;
            sp.pp_out.resize(io::count(r, 1 + 4 + 4 + 4));
            for (TacticalPpResult& o : sp.pp_out) {
                o.ok = r.boolean();
                o.candidates = r.u32();
                o.last_veto = r.str();
                o.picks.resize(io::count(r, 17));
                for (TacticalPpPick& k : o.picks) {
                    k.type = r.i32();
                    const uint8_t source = r.u8();
                    if (source > static_cast<uint8_t>(PpSource::Produce)) r.fail();
                    k.source = static_cast<PpSource>(source);
                    k.ref = r.i32();
                    k.tf = r.i32();
                    k.team = r.i32();
                }
            }
            for (const uint32_t pl : sp.pp_plans)
                if (pl >= m.library.plans.size()) return io::bad(r);
            if (!r.ok() || sp.pp_out.size() != sp.pp_plans.size() || !sp.ctx.goals.accept_pending()) return io::bad(r);
        } else if (sp.ctx.goals.accept_pending()) {
            return io::bad(r);
        }
    }
    // ---- plans ----
    std::map<int32_t, PlanInstance> new_plans;
    {
        const uint32_t n = io::count(r, kPlanBytes);
        for (uint32_t i = 0; i < n && r.ok(); ++i) {
            PlanInstance pi;
            pi.id = r.i32();
            pi.player = r.i32();
            pi.plan = r.u32();
            pi.goal_type = r.u32();
            pi.slot = r.i32();
            pi.target_owner = r.i32();
            pi.pp = r.i32();
            if (!r.ok() || !io::get_vec(r, pi.tfs)) return false;
            pi.result = r.boolean();
            pi.non_removable = r.boolean();
            pi.ended = r.boolean();
            pi.live_threads = r.u32();
            const bool has_vm = r.boolean();
            if (!r.ok() || !has_vm || !valid_ai(pi.player) || pi.plan >= lib || pi.plan >= m.runtime->plan_count() ||
                pi.goal_type >= goal_types || pi.id < 1 || pi.id >= next[0] || pi.slot < -1 ||
                (pi.slot >= 0 && static_cast<size_t>(pi.slot) >= staged[static_cast<size_t>(pi.player)].slots.size()) ||
                (!new_plans.empty() && new_plans.rbegin()->first >= pi.id))
                return io::bad(r);
            const std::span<const uint8_t> img = get_image(r);
            if (!r.ok()) return false;
            pi.vm = m.runtime->blank_vm(m.runtime->plan_name(pi.plan));
            if (!read_vm_image(img, *pi.vm)) return io::bad(r);
            rebind_vm(m, *pi.vm, pi.id);
            const int32_t id = pi.id;
            new_plans.emplace_hint(new_plans.end(), id, std::move(pi));
        }
    }
    // ---- task forces ----
    std::map<int32_t, TfRec> new_tfs;
    {
        const uint32_t n = io::count(r, kTfBytes);
        for (uint32_t i = 0; i < n && r.ok(); ++i) {
            TfRec t;
            t.id = r.i32();
            t.plan = r.i32();
            t.player = r.i32();
            t.def = r.u32();
            t.name = r.str();
            const uint8_t kind = r.u8();
            if (kind > static_cast<uint8_t>(lua::AiTaskForceKind::Land)) r.fail();
            t.kind = static_cast<lua::AiTaskForceKind>(kind);
            if (!r.ok() || !get_handles(r, st, t.units)) return false;
            t.pending.resize(io::count(r, kReinforcementBytes));
            for (TacticalReinforcement& e : t.pending) get_reinforcement(r, e);
            if (!r.ok() || !get_pairs(r, t.reservations) || !get_pairs(r, t.producing)) return false;
            t.priorities = r.str();
            if (!r.ok() || !valid_ai(t.player) || t.id < 0 || t.id >= next[1] || (!new_tfs.empty() && new_tfs.rbegin()->first >= t.id))
                return io::bad(r);
            if (const auto pit = new_plans.find(t.plan);
                pit != new_plans.end() && t.def >= m.library.plans[pit->second.plan].task_forces.size())
                return io::bad(r);
            const int32_t id = t.id;
            new_tfs.emplace_hint(new_tfs.end(), id, std::move(t));
        }
    }
    // ---- blocking statuses ----
    std::map<int32_t, StatusRec> new_statuses;
    {
        const uint32_t n = io::count(r, kStatusBytes);
        for (uint32_t i = 0; i < n && r.ok(); ++i) {
            StatusRec s;
            s.id = r.i32();
            s.plan = r.i32();
            s.tf = r.i32();
            const uint8_t kind = r.u8();
            s.finished = r.boolean();
            const uint8_t result = r.u8();
            if (kind > static_cast<uint8_t>(StatusKind::Reinforce) || result > static_cast<uint8_t>(lua::AiStatusResult::False)) r.fail();
            s.kind = static_cast<StatusKind>(kind);
            s.result = static_cast<lua::AiStatusResult>(result);
            s.started = r.i64();
            s.last_order = r.i64();
            s.unit = get_handle(r, st);
            s.target = r.i32();
            s.dest = io::get_vec3(r);
            s.timeout = r.i64();  // refixf t06
            s.ring = r.f32();
            s.prevent = r.boolean();
            s.direction = static_cast<int8_t>(r.u8());  // S7 t21
            s.min_dist = r.f32();
            s.threat = r.f32();
            s.plan_frame = r.u32();
            s.lists = r.boolean();
            for (std::vector<int32_t>* v : {&s.members, &s.moving}) {
                v->resize(io::count(r, 4));
                for (int32_t& oid : *v) oid = r.i32();
            }
            if (!get_pairs(r, s.landings)) return io::bad(r);  // a1j t01
            s.saved_removable = r.boolean();
            if (!r.ok() || s.id < 0 || s.id >= next[2] || (!new_statuses.empty() && new_statuses.rbegin()->first >= s.id))
                return io::bad(r);
            const int32_t id = s.id;
            new_statuses.emplace_hint(new_statuses.end(), id, s);
        }
    }
    // ---- PotentialPlans + reservations ----
    std::map<int32_t, TacPpRec> new_pps;
    {
        const uint32_t n = io::count(r, 16);
        for (uint32_t i = 0; i < n && r.ok(); ++i) {
            const int32_t id = r.i32();
            TacPpRec pp;
            pp.player = r.i32();
            pp.plan = r.u32();
            pp.reserved = r.boolean();
            pp.picks.resize(io::count(r, 17));
            for (TacticalPpPick& k : pp.picks) {
                k.type = r.i32();
                const uint8_t source = r.u8();
                if (source > static_cast<uint8_t>(PpSource::Produce)) r.fail();
                k.source = static_cast<PpSource>(source);
                k.ref = r.i32();
                k.tf = r.i32();
                k.team = r.i32();
            }
            if (!r.ok() || !valid_ai(pp.player) || (pp.plan != kAiNone && pp.plan >= lib) || id < 0 || id >= next[3] ||
                (!new_pps.empty() && new_pps.rbegin()->first >= id))
                return io::bad(r);
            new_pps.emplace_hint(new_pps.end(), id, std::move(pp));
        }
    }
    std::map<int32_t, int32_t> new_pool_reserved, new_pad_reserved;
    if (!r.ok() || !get_int_map(r, new_pool_reserved) || !get_int_map(r, new_pad_reserved))
        return false;
    // ---- opportunity-fire subscriptions ----
    std::vector<std::pair<int32_t, std::vector<int32_t>>> new_subs(io::count(r, 8));
    for (size_t i = 0; i < new_subs.size(); ++i) {
        new_subs[i].first = r.i32();
        if (!r.ok() || !io::get_vec(r, new_subs[i].second)) return false;
        std::vector<int32_t> sorted = new_subs[i].second;
        std::sort(sorted.begin(), sorted.end());
        if (new_subs[i].first < 0 || new_subs[i].first >= next[1] || (i > 0 && new_subs[i - 1].first >= new_subs[i].first) ||
            sorted.empty() || std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end())
            return io::bad(r);  // ascending task forces, non-empty lists of distinct types (add refuses duplicates)
    }
    // ---- plan runtime pools, created counter, Script_ evaluators ----
    std::vector<std::vector<std::unique_ptr<lua::LuaVm>>> pools(m.runtime->plan_count());
    for (uint32_t p = 0; p < pools.size(); ++p) {
        const uint32_t n = io::count(r, 5);
        bool pristine_named = false;
        for (uint32_t k = 0; k < n && r.ok(); ++k) {
            if (r.boolean()) {  // this session's own pristine VM of the plan (restore_pools keeps it at this slot)
                const uint64_t digest = r.u64();
                if (!r.ok() || pristine_named || !m.runtime->pristine(p) || m.runtime->pristine_digest(p) != digest) return io::bad(r);
                pristine_named = true;
                pools[p].push_back(nullptr);
                continue;
            }
            const std::span<const uint8_t> img = get_image(r);
            if (!r.ok()) return false;
            std::unique_ptr<lua::LuaVm> vm = m.runtime->blank_vm(m.runtime->plan_name(p));
            if (!read_vm_image(img, *vm)) return io::bad(r);
            rebind_vm(m, *vm, -1);
            pools[p].push_back(std::move(vm));
        }
    }
    const uint64_t created = r.u64();
    std::map<std::string, ScriptEvaluators::Loaded> evals;
    {
        const uint32_t n = io::count(r, 12);
        for (uint32_t i = 0; i < n && r.ok(); ++i) {
            std::string key = r.str();
            ScriptEvaluators::Loaded e;
            e.name = r.str();
            const std::span<const uint8_t> img = get_image(r);
            if (!r.ok() || key != ai_upper_copy(e.name) || (!evals.empty() && !(evals.rbegin()->first < key))) return io::bad(r);
            e.vm = m.runtime->blank_vm(e.name);
            if (!read_vm_image(img, *e.vm)) return io::bad(r);
            rebind_vm(m, *e.vm, -1);
            evals.emplace_hint(evals.end(), std::move(key), std::move(e));
        }
    }
    std::set<std::string> missing;
    {
        const uint32_t n = io::count(r, 4);
        for (uint32_t i = 0; i < n && r.ok(); ++i) {
            std::string s = r.str();
            if (!r.ok() || (!missing.empty() && !(*missing.rbegin() < s))) return io::bad(r);
            missing.emplace_hint(missing.end(), std::move(s));
        }
    }
    if (!r.ok()) return false;
    // ---- the shared perception: validated on a probe built like the constructor's, then read in place ----
    {
        TacticalPerception probe;
        probe.init(m.cfg.mode, m.cfg.constants, m.cfg.map, static_cast<uint32_t>(std::max<size_t>(m.cfg.lua_players.size(), 1)),
                   m.cfg.zones);
        ByteReader pr(perc_image);
        if (!probe.read(pr) || !pr.done()) return io::bad(r);
        ByteReader pr2(perc_image);
        if (!m.perc.read(pr2)) return io::bad(r);  // same bytes, same geometry: cannot fail after the probe
    }

    // ---- commit ----
    m.now = saved_now;
    m.mode_frame = saved_mode_frame;
    m.current_plan = saved_current_plan;
    const int32_t old_next_tf = m.next_tf;
    m.next_plan = next[0];
    m.next_tf = next[1];
    m.next_status = next[2];
    m.next_pp = next[3];
    m.orders = saved_orders;
    m.rejected = saved_rejected;
    m.gate = new_gate;
    // The PerceptionWorld: everything but this world's types and host callbacks (relink() completes them).
    new_w.types = std::move(m.w.types);
    new_w.object_fogged = std::move(m.w.object_fogged);
    new_w.point_fogged = std::move(m.w.point_fogged);
    new_w.point_blocked = std::move(m.w.point_blocked);
    new_w.inside_shield = std::move(m.w.inside_shield);
    new_w.in_active_base_shield = std::move(m.w.in_active_base_shield);
    new_w.parent_rep_type = std::move(m.w.parent_rep_type);
    new_w.zone_hint = std::move(m.w.zone_hint);
    new_w.nearby_open_pads = std::move(m.w.nearby_open_pads);
    new_w.pad_accepts = std::move(m.w.pad_accepts);
    new_w.hard_points = std::move(m.w.hard_points);
    m.w = std::move(new_w);
    m.relink_types = m.w.types.empty() && type_count > 0;
    m.saved_type_count = type_count;
    m.saved_types_digest = types_digest;
    m.facts = std::move(new_facts);
    m.prev = std::move(new_prev);
    m.world = std::move(new_world);
    m.bridge = std::move(new_bridge);
    m.globals = std::move(new_globals);
    m.handle_name_crc = std::move(new_names);
    for (size_t i = 0; i < m.ai.size(); ++i) {
        if (!m.ai[i]) continue;
        PlayerAi& pa = *m.ai[i];
        StagedPlayer& sp = staged[i];
        pa.list = std::move(sp.list);
        pa.slots = std::move(sp.slots);
        pa.ctx = std::move(sp.ctx);
        pa.fs = std::move(sp.fs);
        pa.sched = sp.sched;
        if (sp.has_fs)
            pa.fs_vm = std::move(sp.fs_vm);
        else
            pa.fs_vm.reset();
        pa.bonuses = std::move(sp.bonuses);
        pa.floods = std::move(sp.floods);
        pa.paths_built = sp.paths_built;
        pa.next_recompute = sp.next_recompute;
        pa.path_job.cancel();
        pa.paths = sp.paths_built ? std::move(sp.paths) : ZoneGraph{};
        pa.path_pend = std::move(sp.path_pend);
        if (sp.path_in_flight) pa.start_paths_job(sp.path_kick, sp.path_ready, {});  // OPT3 t04: again, inline
        pa.accept_cancel();  // OPT2 t10: the saved results stand in for the solves
        pa.pp_plans = std::move(sp.pp_plans);
        pa.pp_out = std::move(sp.pp_out);
        pa.pp_out_ready = sp.pp_pending;
    }
    m.plans = std::move(new_plans);  // the replaced plan VMs are destroyed (not pooled)
    m.tfs = std::move(new_tfs);
    m.statuses = std::move(new_statuses);
    m.pps = std::move(new_pps);
    m.pool_reserved = std::move(new_pool_reserved);
    m.pad_reserved = std::move(new_pad_reserved);
    for (int32_t t = 0; t < std::max(old_next_tf, m.next_tf); ++t)  // the reader's own lists, then the saved ones
        if (const std::vector<int32_t>* s = m.opportunity_fire_subscriptions(t); s && !s->empty()) m.tf_opportunity_fire_subscription(t, 2, 0);
    for (const auto& [t, list] : new_subs)
        for (const int32_t hp : list) m.tf_opportunity_fire_subscription(t, 0, hp);
    m.runtime->restore_pools(std::move(pools), static_cast<size_t>(created));
    m.scripts.restore(std::move(evals), std::move(missing));
    return true;
}

bool TacticalAiWorld::relink() {
    Impl& m = *impl_;
    if (!m.relink_types) return true;
    PerceptionWorld scratch;
    std::vector<TacticalAiObject> f;
    m.host.fill(scratch, f);
    if (scratch.types.size() != m.saved_type_count || scratch.types_digest() != m.saved_types_digest) return false;
    m.w.types = std::move(scratch.types);
    // The callbacks the host installs with the types (its fill() sets them up when the world has no types yet).
    auto take = [](auto& mine, auto& theirs) {
        if (!mine && theirs) mine = std::move(theirs);
    };
    take(m.w.object_fogged, scratch.object_fogged);
    take(m.w.point_fogged, scratch.point_fogged);
    take(m.w.point_blocked, scratch.point_blocked);
    take(m.w.inside_shield, scratch.inside_shield);
    take(m.w.in_active_base_shield, scratch.in_active_base_shield);
    take(m.w.parent_rep_type, scratch.parent_rep_type);
    take(m.w.zone_hint, scratch.zone_hint);
    take(m.w.nearby_open_pads, scratch.nearby_open_pads);
    take(m.w.pad_accepts, scratch.pad_accepts);
    take(m.w.hard_points, scratch.hard_points);
    m.relink_types = false;
    return true;
}

// ================================================================ data helpers

std::vector<TacticalAiDifficulty> load_tactical_ai_difficulties(const FileProvider& files) {
    std::vector<TacticalAiDifficulty> out;
    std::vector<uint8_t> bytes;
    if (!files.read("Data\\XML\\DifficultyAdjustments.xml", bytes)) return out;
    const data::XmlDocument doc =
        data::XmlDocument::parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), "DifficultyAdjustments.xml");
    if (!doc.ok()) return out;
    for (const data::XmlNode n : doc.root().children()) {
        TacticalAiDifficulty d;
        d.name = std::string(n.attribute("Name", n.name()));
        float v = 0.0f;
        if (ai_parse_float(n.child_text("Land_AI_Goal_Cycle_Sleep_Duration"), v)) d.land_goal_sleep = v;
        if (ai_parse_float(n.child_text("Space_AI_Goal_Cycle_Sleep_Duration"), v)) d.space_goal_sleep = v;
        if (ai_parse_float(n.child_text("Land_AI_Contrast_Multiplier"), v)) d.land_contrast = v;
        if (ai_parse_float(n.child_text("Space_AI_Contrast_Multiplier"), v)) d.space_contrast = v;
        out.push_back(std::move(d));
    }
    return out;
}

// AI_GALACTIC §1.9.1 [H] (A1_READINGS §34.2): the loader 0x4e7e30 puts every plan folder into one plan manager, so a
// tactical AI also sees plans outside its mode folder (SpaceMode\RetreatPlan.lua serves Land_Retreat; the AI root's
// GenerateMagicCashDrop serves both skirmish modes). A plan is only picked for the goals its Category names, so the other
// mode's plans are loaded but never run.
std::vector<std::string> tactical_plan_files(TacticalMode, const std::function<std::vector<std::string>(std::string_view)>& list) {
    return list_plan_files(list);
}

}  // namespace alamo::ai::tactical
