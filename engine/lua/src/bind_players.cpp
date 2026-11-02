// Binding family: players, factions, credits, AI and perception (L1 stage 2). State and backing are
// described in alamo/lua/players.hpp (with the list of [inferred] behaviour); argument checks use the
// original's own validation strings (docs/re/LUA_API.md "Binding map"). Implementations follow the
// error discipline of vm_internal.hpp (Call::fail, never lua_error).
//
// Needs the S3 galactic sim (alamo_galactic_sim) and the perception evaluator (alamo_ai); without them
// (ALAMO_LUA_PLAYERS undefined, engine/lua/CMakeLists.txt) the family table is empty.
#include "vm_internal.hpp"

#if defined(ALAMO_LUA_PLAYERS)

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "alamo/ai/ai_path_finder.hpp"
#include "alamo/ai/perception_equations.hpp"
#include "alamo/lua/object_bridge.hpp"
#if ALAMO_LUA_HAVE_COMMANDS
#include "alamo/commands/pipeline.hpp"
#endif
#include "alamo/core/sim_crc.hpp"
#include "alamo/core/sim_rng.hpp"
#include "alamo/galactic/campaign.hpp"
#include "alamo/galactic/economy.hpp"
#include "alamo/galactic/fleet_path.hpp"
#include "alamo/galactic/special_data.hpp"  // P4 lua: special_name_crc
#include "alamo/lua/players.hpp"
#include "alamo/objects/game_object_types.hpp"

namespace alamo::lua {

namespace {

char ascii_upper(char ch) { return (ch >= 'a' && ch <= 'z') ? static_cast<char>(ch - 'a' + 'A') : ch; }

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (ascii_upper(a[i]) != ascii_upper(b[i])) return false;
    return true;
}

}  // namespace

// ================================================================ LuaPlayerWorld

void LuaPlayerWorld::attach_galactic(galactic::GalacticSim* sim) {
    sim_ = sim;
    if (sim_) state_.resize(sim_->players().size());
    reset_relations();
}

void LuaPlayerWorld::set_players(std::vector<LuaPlayerSetup> players) {
    state_.assign(players.size(), LuaPlayerState{});
    for (size_t i = 0; i < players.size(); ++i) state_[i].setup = std::move(players[i]);
    reset_relations();
}

void LuaPlayerWorld::reset_relations() {
    const size_t n = state_.size();
    relation_.assign(n * n, static_cast<uint8_t>(PlayerRelation::Enemy));
    for (size_t a = 0; a < n; ++a)
        for (size_t b = 0; b < n; ++b) {
            PlayerRelation r = PlayerRelation::Enemy;
            if (a == b)
                r = PlayerRelation::Self;
            else if (is_neutral(static_cast<int32_t>(a)) || is_neutral(static_cast<int32_t>(b)))
                r = PlayerRelation::Neutral;
            relation_[a * n + b] = static_cast<uint8_t>(r);
        }
}

int32_t LuaPlayerWorld::local_player() const {
    if (valid(local_)) return local_;
    for (size_t p = 0; p < state_.size(); ++p)
        if (is_human(static_cast<int32_t>(p))) return static_cast<int32_t>(p);
    return state_.empty() ? -1 : 0;
}

std::string LuaPlayerWorld::faction_name(int32_t p) const {
    if (!valid(p)) return {};
    if (sim_) {
        const uint32_t f = sim_->players()[static_cast<size_t>(p)].faction;
        return f < sim_->rules().factions.size() ? sim_->rules().factions[f].name : std::string();
    }
    return state_[static_cast<size_t>(p)].setup.faction;
}

bool LuaPlayerWorld::is_human(int32_t p) const {
    if (!valid(p)) return false;
    return sim_ ? sim_->players()[static_cast<size_t>(p)].human : state_[static_cast<size_t>(p)].setup.human;
}

bool LuaPlayerWorld::has_ai(int32_t p) const {
    if (!valid(p)) return false;
    return sim_ ? sim_->players()[static_cast<size_t>(p)].ai : state_[static_cast<size_t>(p)].setup.ai;
}

bool LuaPlayerWorld::is_neutral(int32_t p) const {
    if (!valid(p)) return false;
    if (sim_) {
        const uint32_t f = sim_->players()[static_cast<size_t>(p)].faction;
        return f < sim_->rules().factions.size() && sim_->rules().factions[f].is_neutral;
    }
    return state_[static_cast<size_t>(p)].setup.neutral;
}

std::string LuaPlayerWorld::difficulty_name(int32_t p) const {
    if (!valid(p)) return {};
    if (sim_) {
        const uint32_t d = sim_->players()[static_cast<size_t>(p)].difficulty;
        return d < sim_->rules().difficulties.size() ? sim_->rules().difficulties[d].name : std::string();
    }
    return state_[static_cast<size_t>(p)].setup.difficulty;
}

float LuaPlayerWorld::credits(int32_t p) const {
    if (!valid(p)) return 0.0f;
    return sim_ ? sim_->credits(p) : state_[static_cast<size_t>(p)].setup.credits;
}

float LuaPlayerWorld::give_money(int32_t p, float amount) {
    if (!valid(p)) return 0.0f;
    if (sim_) return sim_->add_credits(p, amount, true);
    LuaPlayerSetup& s = state_[static_cast<size_t>(p)].setup;
    galactic::econ::CreditAccount a{s.credits, s.credit_cap};
    const float applied = galactic::econ::add_credits(a, amount, s.ai, s.credit_multiplier);
    s.credits = a.credits;
    return applied;
}

int32_t LuaPlayerWorld::tech_level(int32_t p) const {
    if (!valid(p)) return 0;
    return sim_ ? sim_->players()[static_cast<size_t>(p)].tech_level : state_[static_cast<size_t>(p)].setup.tech_level;
}

void LuaPlayerWorld::set_tech_level(int32_t p, int32_t level) {
    if (!valid(p)) return;
    if (sim_)
        sim_->set_tech_level(p, level);
    else
        state_[static_cast<size_t>(p)].setup.tech_level = level;
}

int32_t LuaPlayerWorld::find_faction_player(std::string_view faction) const {
    for (size_t p = 0; p < state_.size(); ++p)
        if (iequals(faction_name(static_cast<int32_t>(p)), faction)) return static_cast<int32_t>(p);
    return -1;
}

PlayerRelation LuaPlayerWorld::relation(int32_t a, int32_t b) const {
    if (!valid(a) || !valid(b)) return PlayerRelation::Neutral;
    if (a == b) return PlayerRelation::Self;
    // OI lua: with a GalacticSim the sim's relation table is the one source (galactic::Relation = PlayerRelation values).
    static_assert(static_cast<uint8_t>(galactic::Relation::Enemy) == static_cast<uint8_t>(PlayerRelation::Enemy) &&
                  static_cast<uint8_t>(galactic::Relation::Neutral) == static_cast<uint8_t>(PlayerRelation::Neutral) &&
                  static_cast<uint8_t>(galactic::Relation::Ally) == static_cast<uint8_t>(PlayerRelation::Ally) &&
                  static_cast<uint8_t>(galactic::Relation::Self) == static_cast<uint8_t>(PlayerRelation::Self));
    if (sim_) return static_cast<PlayerRelation>(static_cast<uint8_t>(sim_->relation(a, b)));
    const size_t n = state_.size();
    if (relation_.size() != n * n) return PlayerRelation::Neutral;
    return static_cast<PlayerRelation>(relation_[static_cast<size_t>(a) * n + static_cast<size_t>(b)]);
}

void LuaPlayerWorld::set_relation(int32_t a, int32_t b, PlayerRelation r) {
    if (!valid(a) || !valid(b) || a == b || r == PlayerRelation::Self) return;
    if (sim_) {  // OI lua: GalacticSim::set_relation sets both directions (hashed / saved by the sim)
        sim_->set_relation(a, b, static_cast<galactic::Relation>(static_cast<uint8_t>(r)));
        return;
    }
    const size_t n = state_.size();
    if (relation_.size() != n * n) reset_relations();
    relation_[static_cast<size_t>(a) * n + static_cast<size_t>(b)] = static_cast<uint8_t>(r);
    relation_[static_cast<size_t>(b) * n + static_cast<size_t>(a)] = static_cast<uint8_t>(r);
}

int32_t LuaPlayerWorld::enemy_of(int32_t p) const {
    for (size_t q = 0; q < state_.size(); ++q)
        if (relation(p, static_cast<int32_t>(q)) == PlayerRelation::Enemy) return static_cast<int32_t>(q);
    return -1;
}

int32_t LuaPlayerWorld::tech_lock(int32_t p, uint32_t type) const {
    if (!valid(p)) return -1;
    const auto& o = state_[static_cast<size_t>(p)].tech_override;
    const auto it = o.find(type);
    return it == o.end() ? -1 : (it->second ? 1 : 0);
}

void LuaPlayerWorld::hash(StateHasher& h) const {
    h.u32(static_cast<uint32_t>(state_.size()));
    for (const LuaPlayerState& s : state_) {
        h.str(s.setup.faction);
        h.boolean(s.setup.human);
        h.boolean(s.setup.ai);
        h.boolean(s.setup.neutral);
        h.str(s.setup.difficulty);
        h.f32(s.setup.credit_multiplier);
        h.f32(s.setup.credits);
        h.f32(s.setup.credit_cap);
        h.i32(s.setup.tech_level);
        h.i32(s.setup.team);
        h.u32(static_cast<uint32_t>(s.tech_override.size()));
        for (const auto& [type, locked] : s.tech_override) {
            h.u32(type);
            h.boolean(locked);
        }
        h.boolean(s.actor);
        h.i32(s.selected);
        h.i32(s.retreat_requests);
        h.boolean(s.bombing_run_disabled);
        h.boolean(s.orbital_bombardment_disabled);
        h.boolean(s.orbital_bombardment_removed);
        h.boolean(s.black_market_tutorial);
        h.boolean(s.sabotage_tutorial);
        h.u32(static_cast<uint32_t>(s.advisor_hints.size()));
        for (const auto& [name, on] : s.advisor_hints) {
            h.str(name);
            h.boolean(on);
        }
    }
    h.u32(static_cast<uint32_t>(relation_.size()));
    for (const uint8_t r : relation_) h.u8(r);
    h.i32(local_);
    h.boolean(ai_suspended_);
    h.u32(static_cast<uint32_t>(purge_goal_requests.size()));
    for (const int32_t p : purge_goal_requests) h.i32(p);
    h.u32(static_cast<uint32_t>(desire_bonuses.size()));
    for (const DesireBonusRequest& d : desire_bonuses) {
        h.i32(d.player);
        h.str(d.goal);
        h.i32(d.target);
        h.f64(d.bonus);
        h.f64(d.duration);
    }
    h.u32(static_cast<uint32_t>(tactical_releases.size()));
    for (const TacticalCreditRelease& t : tactical_releases) {
        h.i32(t.player);
        h.f64(t.amount);
    }
    h.u32(static_cast<uint32_t>(budget_flushes.size()));
    for (const BudgetFlushRequest& b : budget_flushes) {
        h.i32(b.player);
        h.str(b.category);
    }
}

// ================================================================ N1 save / load

namespace {
constexpr uint32_t kPlayersSaveVersion = 1;
}  // namespace

void LuaPlayerWorld::write(ByteWriter& w) const {
    w.u32(kPlayersSaveVersion).u32(static_cast<uint32_t>(state_.size()));
    for (const LuaPlayerState& s : state_) {
        const LuaPlayerSetup& u = s.setup;
        w.str(u.faction).boolean(u.human).boolean(u.ai).boolean(u.neutral).str(u.difficulty);
        w.f32(u.credit_multiplier).f32(u.credits).f32(u.credit_cap).i32(u.tech_level).i32(u.team);
        w.u32(static_cast<uint32_t>(s.tech_override.size()));
        for (const auto& [type, locked] : s.tech_override) w.u32(type).boolean(locked);
        w.boolean(s.actor).i32(s.selected).i32(s.retreat_requests);
        w.boolean(s.bombing_run_disabled).boolean(s.orbital_bombardment_disabled).boolean(s.orbital_bombardment_removed);
        w.boolean(s.black_market_tutorial).boolean(s.sabotage_tutorial);
        w.u32(static_cast<uint32_t>(s.advisor_hints.size()));
        for (const auto& [name, on] : s.advisor_hints) w.str(name).boolean(on);
    }
    w.u32(static_cast<uint32_t>(relation_.size()));
    for (const uint8_t r : relation_) w.u8(r);
    w.i32(local_).boolean(ai_suspended_);
    w.u32(static_cast<uint32_t>(purge_goal_requests.size()));
    for (const int32_t p : purge_goal_requests) w.i32(p);
    w.u32(static_cast<uint32_t>(desire_bonuses.size()));
    for (const DesireBonusRequest& d : desire_bonuses) w.i32(d.player).str(d.goal).i32(d.target).f64(d.bonus).f64(d.duration);
    w.u32(static_cast<uint32_t>(tactical_releases.size()));
    for (const TacticalCreditRelease& t : tactical_releases) w.i32(t.player).f64(t.amount);
    w.u32(static_cast<uint32_t>(budget_flushes.size()));
    for (const BudgetFlushRequest& b : budget_flushes) w.i32(b.player).str(b.category);
}

bool LuaPlayerWorld::read(ByteReader& r) {
    if (r.u32() != kPlayersSaveVersion || !r.ok()) return false;
    const uint32_t n = r.u32();
    if (!r.ok() || n > r.remaining() / 50 || n > 0xFFFFu) return false;
    if (sim_ && n != sim_->players().size()) return false;
    std::vector<LuaPlayerState> state(n);
    for (LuaPlayerState& s : state) {
        LuaPlayerSetup& u = s.setup;
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
        const uint32_t nt = r.u32();
        if (!r.ok() || nt > r.remaining() / 5) return false;
        for (uint32_t k = 0; k < nt; ++k) {
            const uint32_t type = r.u32();
            const bool locked = r.boolean();
            if (!r.ok() || (!s.tech_override.empty() && type <= s.tech_override.rbegin()->first)) return false;
            s.tech_override.emplace(type, locked);
        }
        s.actor = r.boolean();
        s.selected = r.i32();
        s.retreat_requests = r.i32();
        s.bombing_run_disabled = r.boolean();
        s.orbital_bombardment_disabled = r.boolean();
        s.orbital_bombardment_removed = r.boolean();
        s.black_market_tutorial = r.boolean();
        s.sabotage_tutorial = r.boolean();
        const uint32_t nh = r.u32();
        if (!r.ok() || nh > r.remaining() / 5) return false;
        for (uint32_t k = 0; k < nh; ++k) {
            std::string name = r.str();
            const bool on = r.boolean();
            if (!r.ok() || (!s.advisor_hints.empty() && !(s.advisor_hints.rbegin()->first < name))) return false;
            s.advisor_hints.emplace(std::move(name), on);
        }
    }
    const uint32_t nr = r.u32();
    if (!r.ok() || static_cast<uint64_t>(nr) != static_cast<uint64_t>(n) * n || nr > r.remaining()) return false;
    std::vector<uint8_t> relation(nr);
    for (uint8_t& x : relation) {
        x = r.u8();
        if (x > static_cast<uint8_t>(PlayerRelation::Enemy)) return false;
    }
    const int32_t local = r.i32();
    const bool suspended = r.boolean();
    uint32_t m = r.u32();
    if (!r.ok() || m > r.remaining() / 4) return false;
    std::vector<int32_t> purge(m);
    for (int32_t& p : purge) p = r.i32();
    m = r.u32();
    if (!r.ok() || m > r.remaining() / 28) return false;
    std::vector<DesireBonusRequest> desire(m);
    for (DesireBonusRequest& d : desire) {
        d.player = r.i32();
        d.goal = r.str();
        d.target = r.i32();
        d.bonus = r.f64();
        d.duration = r.f64();
    }
    m = r.u32();
    if (!r.ok() || m > r.remaining() / 12) return false;
    std::vector<TacticalCreditRelease> releases(m);
    for (TacticalCreditRelease& t : releases) {
        t.player = r.i32();
        t.amount = r.f64();
    }
    m = r.u32();
    if (!r.ok() || m > r.remaining() / 8) return false;
    std::vector<BudgetFlushRequest> flushes(m);
    for (BudgetFlushRequest& b : flushes) {
        b.player = r.i32();
        b.category = r.str();
    }
    if (!r.ok()) return false;
    state_ = std::move(state);
    relation_ = std::move(relation);
    local_ = local;
    ai_suspended_ = suspended;
    purge_goal_requests = std::move(purge);
    desire_bonuses = std::move(desire);
    tactical_releases = std::move(releases);
    budget_flushes = std::move(flushes);
    return true;
}

// ================================================================ VM attachment

namespace {

const char kWorldKey[] = "alamo.players.world";
const char kInstancesKey[] = "alamo.players.instances";

LuaPlayerWorld* world_of(lua_State* L) {
    lua_pushstring(L, kWorldKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    void* p = lua_type(L, -1) == LUA_TLIGHTUSERDATA ? lua_touserdata(L, -1) : nullptr;
    lua_pop(L, 1);
    return static_cast<LuaPlayerWorld*>(p);
}

}  // namespace

void attach_player_world(LuaVm& vm, LuaPlayerWorld* world) {
    lua_State* L = vm.state();
    lua_pushstring(L, kWorldKey);
    if (world)
        lua_pushlightuserdata(L, world);
    else
        lua_pushnil(L);
    lua_rawset(L, LUA_REGISTRYINDEX);
}

LuaPlayerWorld* player_world(LuaVm& vm) { return world_of(vm.state()); }

// ================================================================ bindings

using detail::Call;
using detail::Ud;

namespace {

int push_nil(Call& c) {
    lua_pushnil(c.L);
    return 1;
}
int push_bool(Call& c, bool b) {
    lua_pushboolean(c.L, b ? 1 : 0);
    return 1;
}
int push_num(Call& c, double d) {
    lua_pushnumber(c.L, d);
    return 1;
}
int push_str(Call& c, std::string_view s) {
    lua_pushlstring(c.L, s.data(), s.size());
    return 1;
}

bool is_bool(const Call& c, int i) { return c.has(i) && lua_type(c.L, c.idx(i)) == LUA_TBOOLEAN; }
bool to_bool(const Call& c, int i) { return lua_toboolean(c.L, c.idx(i)) != 0; }
bool is_table(const Call& c, int i) { return c.has(i) && lua_type(c.L, c.idx(i)) == LUA_TTABLE; }

uint32_t cls_weighted_list() {
    static const uint32_t c = detail::class_id("WeightedTypeListClass");
    return c;
}
uint32_t cls_distribution() {
    static const uint32_t c = detail::class_id("LuaDiscreteDistributionClass");
    return c;
}

// The world, when the receiver is a valid player of it.
LuaPlayerWorld* self_world(Call& c) {
    LuaPlayerWorld* w = world_of(c.L);
    return (w && w->valid(c.self->id)) ? w : nullptr;
}
int bad_player(Call& c, const char* method) {
    return c.fail("PlayerWrapper::%s -- invalid player (no player world attached to this script)", method);
}

// A player argument of the attached world, -1 if it is not one.
int32_t player_arg(Call& c, int i, const LuaPlayerWorld* w) {
    const Ud* p = c.userdata(i, detail::cls_player());
    return (p && w && w->valid(p->id)) ? p->id : -1;
}

// A live game object argument, null handle otherwise.
objects::ObjectHandle object_arg(Call& c, int i) {
    const Ud* o = c.userdata(i, detail::cls_game_object());
    return (o && detail::object_alive(*c.vm, *o)) ? detail::object_handle(*o) : objects::ObjectHandle{};
}

// The galactic sim when the script's store is the sim's store (galactic scripts).
const galactic::GalacticSim* galactic_of(Call& c, const LuaPlayerWorld* w) {
    const galactic::GalacticSim* sim = w ? w->galactic() : nullptr;
    return (sim && c.vm->host.store == &sim->store()) ? sim : nullptr;
}

// ---- instances of WeightedTypeList / DiscreteDistribution: registry table id -> {items, weights}.

void push_instances(lua_State* L) {
    lua_pushstring(L, kInstancesKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (lua_type(L, -1) == LUA_TTABLE) return;
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushstring(L, kInstancesKey);
    lua_pushvalue(L, -2);
    lua_rawset(L, LUA_REGISTRYINDEX);
}

int new_instance(Call& c, uint32_t cls) {
    lua_State* L = c.L;
    push_instances(L);
    lua_pushstring(L, "n");
    lua_rawget(L, -2);
    const double n = lua_tonumber(L, -1) + 1.0;
    lua_pop(L, 1);
    lua_pushstring(L, "n");
    lua_pushnumber(L, n);
    lua_rawset(L, -3);
    lua_pushnumber(L, n);
    lua_newtable(L);
    lua_pushstring(L, "items");
    lua_newtable(L);
    lua_rawset(L, -3);
    lua_pushstring(L, "weights");
    lua_newtable(L);
    lua_rawset(L, -3);
    lua_rawset(L, -3);
    lua_pop(L, 1);
    Ud ud;
    ud.cls = cls;
    ud.value = static_cast<uint64_t>(n);
    detail::push_engine_userdata(L, ud);
    return 1;
}

bool is_instance(const Ud* u, uint32_t cls) { return u && u->cls == cls && u->binding == detail::kNoBinding && u->value != 0; }

// Pushes field `field` ("items" / "weights") of instance `id`; false (nothing pushed) if absent.
bool push_instance_field(lua_State* L, uint64_t id, const char* field) {
    push_instances(L);
    lua_pushnumber(L, static_cast<lua_Number>(id));
    lua_rawget(L, -2);
    if (lua_type(L, -1) != LUA_TTABLE) {
        lua_pop(L, 2);
        return false;
    }
    lua_pushstring(L, field);
    lua_rawget(L, -2);
    lua_remove(L, -2);
    lua_remove(L, -2);
    if (lua_type(L, -1) != LUA_TTABLE) {
        lua_pop(L, 1);
        return false;
    }
    return true;
}

// Number of consecutive non-nil entries 1..n of the table at `index` (absolute).
int array_length(lua_State* L, int index) {
    int n = 0;
    for (;;) {
        lua_rawgeti(L, index, n + 1);
        const bool nil = lua_isnil(L, -1);
        lua_pop(L, 1);
        if (nil) return n;
        ++n;
    }
}

// ---------------------------------------------------------------- Find_Player

int g_find_player(Call& c) {
    if (c.nargs != 1) return c.fail("Find_Player -- invalid number of parameters. Expected 1, got %d.", c.nargs);
    if (!c.is_string(1)) return c.fail("Find_Player -- invalid type for parameter 1. Expected string.");
    const LuaPlayerWorld* w = world_of(c.L);
    if (!w) return push_nil(c);
    const std::string_view name = c.string(1);
    const int32_t p = iequals(name, "local") ? w->local_player() : w->find_faction_player(name);
    if (!w->valid(p)) return push_nil(c);
    detail::push_player(c.L, *c.vm, p);
    return 1;
}

// ---------------------------------------------------------------- PlayerWrapper

int m_get_faction_name(Call& c) {
    const LuaPlayerWorld* w = self_world(c);
    if (!w) return bad_player(c, "Get_Faction_Name");
    std::string n = w->faction_name(c.self->id);
    for (char& ch : n) ch = ascii_upper(ch);
    return push_str(c, n);
}

int m_get_name(Call& c) {
    const LuaPlayerWorld* w = self_world(c);
    if (!w) return bad_player(c, "Get_Name");
    return push_str(c, w->faction_name(c.self->id));
}

int m_is_human(Call& c) {
    const LuaPlayerWorld* w = self_world(c);
    if (!w) return bad_player(c, "Is_Human");
    return push_bool(c, w->is_human(c.self->id));
}

int m_is_neutral(Call& c) {
    const LuaPlayerWorld* w = self_world(c);
    if (!w) return bad_player(c, "Is_Neutral");
    return push_bool(c, w->is_neutral(c.self->id));
}

int m_get_credits(Call& c) {
    const LuaPlayerWorld* w = self_world(c);
    if (!w) return bad_player(c, "Get_Credits");
    return push_num(c, static_cast<double>(w->credits(c.self->id)));
}

int m_give_money(Call& c) {
    LuaPlayerWorld* w = self_world(c);
    if (!w) return bad_player(c, "Give_Money");
    if (c.nargs < 1) return c.fail("PlayerWrapper::Lua_Give_Money -- Missing required parameter.");
    if (!c.is_number(1)) return c.fail("PlayerWrapper::Lua_Give_Money -- Parameter 1 must be a number.");
    w->give_money(c.self->id, static_cast<float>(c.number(1)));
    return 0;
}

int m_get_difficulty(Call& c) {
    const LuaPlayerWorld* w = self_world(c);
    if (!w) return bad_player(c, "Get_Difficulty");
    std::string d = w->difficulty_name(c.self->id);
    const size_t us = d.find('_');
    if (us != std::string::npos) d.resize(us);
    if (d.empty()) d = "Normal";
    return push_str(c, d);
}

int m_get_tech_level(Call& c) {
    const LuaPlayerWorld* w = self_world(c);
    if (!w) return bad_player(c, "Get_Tech_Level");
    return push_num(c, w->tech_level(c.self->id));
}

int m_set_tech_level(Call& c) {
    LuaPlayerWorld* w = self_world(c);
    if (!w) return bad_player(c, "Set_Tech_Level");
    if (c.nargs < 1) return c.fail("PlayerWrapper::Lua_Set_Tech_Level -- Missing required parameter.");
    if (!c.is_number(1)) return c.fail("PlayerWrapper::Lua_Set_Tech_Level -- Parameter 1 must be a number.");
    w->set_tech_level(c.self->id, detail::to_int(c.number(1)));
    return 0;
}

int relation_query(Call& c, const char* name, bool ally) {
    const LuaPlayerWorld* w = self_world(c);
    if (!w) return bad_player(c, name + 4);  // "Lua_Is_Ally" -> "Is_Ally"
    if (c.nargs != 1) return c.fail("PlayerWrapper::%s -- invalid number of parameters. Expected 1, got %d.", name, c.nargs);
    const int32_t other = player_arg(c, 1, w);
    if (other < 0) return c.fail("PlayerWrapper::%s -- invalid type for parameter 1. Expected player object.", name);
    const PlayerRelation r = w->relation(c.self->id, other);
    return push_bool(c, ally ? (r == PlayerRelation::Self || r == PlayerRelation::Ally) : r == PlayerRelation::Enemy);
}
int m_is_ally(Call& c) { return relation_query(c, "Lua_Is_Ally", true); }
int m_is_enemy(Call& c) { return relation_query(c, "Lua_Is_Enemy", false); }

int relation_change(Call& c, const char* name, PlayerRelation r) {
    LuaPlayerWorld* w = self_world(c);
    if (!w) return bad_player(c, name);
    if (c.nargs != 1) return c.fail("PlayerWrapper::%s -- invalid number of parameters. Expected 1, got %d.", name, c.nargs);
    const int32_t other = player_arg(c, 1, w);
    if (other < 0) return c.fail("PlayerWrapper::%s -- invalid type for parameter 1. Expected player.", name);
    w->set_relation(c.self->id, other, r);
    return 0;
}
int m_make_ally(Call& c) { return relation_change(c, "Make_Ally", PlayerRelation::Ally); }
int m_make_enemy(Call& c) { return relation_change(c, "Make_Enemy", PlayerRelation::Enemy); }

int m_get_enemy(Call& c) {
    const LuaPlayerWorld* w = self_world(c);
    if (!w) return bad_player(c, "Get_Enemy");
    const int32_t e = w->enemy_of(c.self->id);
    if (e < 0) return push_nil(c);
    detail::push_player(c.L, *c.vm, e);
    return 1;
}

int tech_change(Call& c, const char* name, bool locked) {
    LuaPlayerWorld* w = self_world(c);
    if (!w) return bad_player(c, name + 4);
    if (c.nargs != 1) return c.fail("PlayerWrapper::%s -- invalid number of parameters. Expected 1, got %d.", name, c.nargs);
    const Ud* t = c.userdata(1, detail::cls_type());
    if (!t) return c.fail("PlayerWrapper::%s -- invalid type for parameter 1. Expected game object type.", name);
    w->states()[static_cast<size_t>(c.self->id)].tech_override[static_cast<uint32_t>(t->value)] = locked;
    // P4 lua: the same per-player list as the hack lock and story LOCK_UNIT (P4_SIM_SYSTEMS 3.3 step 1, 6.4; player
    // +0x1C8): GalacticSim::enqueue refuses a locked type. Unlock_Tech (0x604540) also joins the unlocked list with no
    // tech check (REMAINING_GAPS 1.5 item 3).
    if (galactic::GalacticSim* sim = w->galactic(); sim && t->value < sim->rules().types.size()) {
        if (locked) sim->lock_type(c.self->id, static_cast<uint32_t>(t->value), true);
        else sim->unlock_tech(c.self->id, static_cast<uint32_t>(t->value));
    }
    return 0;
}
int m_lock_tech(Call& c) { return tech_change(c, "Lua_Lock_Tech", true); }
int m_unlock_tech(Call& c) { return tech_change(c, "Lua_Unlock_Tech", false); }

int m_select_object(Call& c) {
    LuaPlayerWorld* w = self_world(c);
    if (!w) return bad_player(c, "Select_Object");
    if (c.nargs != 1)
        return c.fail("PlayerWrapper::Lua_Select_Object -- invalid number of parameters. Expected 1, got %d.", c.nargs);
    const objects::ObjectHandle h = object_arg(c, 1);
    if (!h) return c.fail("PlayerWrapper::Lua_Select_Object -- invalid type for parameter 1. Expected game object.");
    w->states()[static_cast<size_t>(c.self->id)].selected = c.vm->host.store->id(h);
    return 0;
}

int m_enable_as_actor(Call& c) {
    LuaPlayerWorld* w = self_world(c);
    if (!w) return bad_player(c, "Enable_As_Actor");
    w->states()[static_cast<size_t>(c.self->id)].actor = true;
    return 0;
}

int m_retreat(Call& c) {
    LuaPlayerWorld* w = self_world(c);
    if (!w) return bad_player(c, "Retreat");
    ++w->states()[static_cast<size_t>(c.self->id)].retreat_requests;
    return 0;
}

int m_get_team(Call& c) {
    const LuaPlayerWorld* w = self_world(c);
    if (!w) return bad_player(c, "Get_Team");
    const int32_t team = w->states()[static_cast<size_t>(c.self->id)].setup.team;
    return push_num(c, team >= 0 ? team : c.self->id);
}

int m_get_clan_id(Call& c) {
    if (!self_world(c)) return bad_player(c, "Get_Clan_ID");
    return push_num(c, 0.0);
}

int m_release_credits_for_tactical(Call& c) {
    LuaPlayerWorld* w = self_world(c);
    if (!w) return bad_player(c, "Release_Credits_For_Tactical");
    if (c.nargs > 1)
        return c.fail("PlayerWrapper::Release_Credits_For_Tactical - invalid number of parameters. Expected 0 or 1, got %d.",
                      c.nargs);
    if (c.nargs == 1 && !c.is_number(1))
        return c.fail("PlayerWrapper::Release_Credits_For_Tactical - invalid type for parameter 1. Expected number.");
    w->tactical_releases.push_back({c.self->id, c.nargs == 1 ? c.number(1) : -1.0});
    return 0;
}

// One boolean switch; `missing_style`: the "Missing required parameter." contract instead of the count one.
int bool_switch(Call& c, const char* name, bool missing_style, bool LuaPlayerState::*field) {
    LuaPlayerWorld* w = self_world(c);
    if (!w) return bad_player(c, name);
    if (missing_style) {
        if (c.nargs < 1) return c.fail("PlayerWrapper::%s -- Missing required parameter.", name);
    } else if (c.nargs != 1) {
        return c.fail("PlayerWrapper::%s -- invalid number of parameters. Expected 1, got %d.", name, c.nargs);
    }
    if (!is_bool(c, 1)) return c.fail("PlayerWrapper::%s -- invalid type for parameter 1. Expected boolean.", name);
    w->states()[static_cast<size_t>(c.self->id)].*field = to_bool(c, 1);
    return 0;
}
// L1 final: the land strike managers of the mode (RemainingScriptHooks, script_remaining.hpp) get the switch too.
RemainingScriptHooks* strike_hooks(Call& c) {
    ObjectBridge* b = c.vm->owner ? attached_object_bridge(*c.vm->owner) : nullptr;
    return b ? b->remaining : nullptr;
}
int m_disable_bombing_run(Call& c) {
    const int r = bool_switch(c, "Disable_Bombing_Run", false, &LuaPlayerState::bombing_run_disabled);
    if (r == 0)
        if (RemainingScriptHooks* hk = strike_hooks(c); hk && hk->set_bombing_run_disabled) hk->set_bombing_run_disabled(c.self->id, to_bool(c, 1));
    return r;
}
int m_disable_orbital_bombardment(Call& c) {
    const int r = bool_switch(c, "Disable_Orbital_Bombardment", false, &LuaPlayerState::orbital_bombardment_disabled);
    if (r == 0)
        if (RemainingScriptHooks* hk = strike_hooks(c); hk && hk->set_bombardment_disabled) hk->set_bombardment_disabled(c.self->id, to_bool(c, 1));
    return r;
}
int m_remove_orbital_bombardment(Call& c) {
    const int r = bool_switch(c, "Remove_Orbital_Bombardment", false, &LuaPlayerState::orbital_bombardment_removed);
    if (r == 0)
        if (RemainingScriptHooks* hk = strike_hooks(c); hk && hk->remove_bombardment) hk->remove_bombardment(c.self->id, to_bool(c, 1));
    return r;
}
// P4 lua: the switches are the sim's player flags (P +0x448 / +0x449: black-market tutorial prices, tutorial sabotage
// cost; P4_SIM_SYSTEMS 1.3 / 2.4); the Lua-side copy stays for setups without a galactic sim.
int m_set_black_market_tutorial(Call& c) {
    const int r = bool_switch(c, "Lua_Set_Black_Market_Tutorial", true, &LuaPlayerState::black_market_tutorial);
    if (galactic::GalacticSim* sim = self_world(c) ? self_world(c)->galactic() : nullptr; r == 0 && sim)
        sim->set_bm_tutorial(c.self->id, to_bool(c, 1));
    return r;
}
int m_set_sabotage_tutorial(Call& c) {
    const int r = bool_switch(c, "Lua_Set_Sabotage_Tutorial", true, &LuaPlayerState::sabotage_tutorial);
    if (galactic::GalacticSim* sim = self_world(c) ? self_world(c)->galactic() : nullptr; r == 0 && sim)
        sim->set_sabotage_tutorial(c.self->id, to_bool(c, 1));
    return r;
}

int m_enable_advisor_hints(Call& c) {
    LuaPlayerWorld* w = self_world(c);
    if (!w) return bad_player(c, "Enable_Advisor_Hints");
    if (c.nargs != 2)
        return c.fail("PlayerWrapper::Enable_Advisor_Hints -- invalid number of parameters. Expected 2, got %d.", c.nargs);
    if (!c.is_string(1))
        return c.fail("PlayerWrapper::Enable_Advisor_Hints -- invalid type for parameter 1. Expected string.");
    if (!is_bool(c, 2))
        return c.fail("PlayerWrapper::Enable_Advisor_Hints -- invalid type for parameter 2. Expected boolean.");
    w->states()[static_cast<size_t>(c.self->id)].advisor_hints[std::string(c.string(1))] = to_bool(c, 2);
    return 0;
}

// ---------------------------------------------------------------- AI switches and goal requests

int g_suspend_ai(Call& c) {
    if (c.nargs < 1) return c.fail("LuaSuspendAI -- Expected float parameter 1");
    if (!c.is_number(1)) return c.fail("LuaSuspendAI -- Invalid parameter 1, expected a float");
    if (LuaPlayerWorld* w = world_of(c.L)) w->set_ai_suspended(c.number(1) != 0.0);
    return 0;
}

int g_purge_goals(Call& c) {
    if (c.nargs != 1) return c.fail("Purge_Goals -- invalid number of parameters. Expected 1, got %d.", c.nargs);
    LuaPlayerWorld* w = world_of(c.L);
    const int32_t p = player_arg(c, 1, w);
    if (p < 0) return c.fail("Purge_Goals -- invalid type for parameter 1. Expected a player.");
    w->purge_goal_requests.push_back(p);
    return 0;
}

// GiveDesireBonus(player, goal, target, bonus[, duration]) - the order PGTaskForce.lua uses. Returns nil
// (no blocking status: BlockOnCommand returns at once) [inferred].
int g_give_desire_bonus(Call& c) {
    if (c.nargs != 4 && c.nargs != 5)
        return c.fail("Give_Desire_Bonus : invalid number of parameters. Expected 4, got %d.", c.nargs);
    LuaPlayerWorld* w = world_of(c.L);
    const int32_t p = player_arg(c, 1, w);
    if (p < 0) return c.fail("Give_Desire_Bonus : invalid type for parameter 1. Expected player.");
    if (!c.is_string(2)) return c.fail("Give_Desire_Bonus : invalid type for parameter 2. Expected string.");
    const objects::ObjectHandle h = object_arg(c, 3);
    if (!h) return c.fail("Give_Desire_Bonus : invalid type for parameter 3. Expected AI target location or game object.");
    if (!c.is_number(4)) return c.fail("Give_Desire_Bonus : invalid type for parameter 4. Expected number.");
    if (c.nargs == 5 && !c.is_number(5))
        return c.fail("Give_Desire_Bonus : invalid type for parameter 5. Expected number.");
    DesireBonusRequest d;
    d.player = p;
    d.goal = std::string(c.string(2));
    d.target = c.vm->host.store->id(h);
    d.bonus = c.number(4);
    d.duration = c.nargs == 5 ? c.number(5) : -1.0;
    w->desire_bonuses.push_back(std::move(d));
    return push_nil(c);
}

// Not an exe binding (LUA_API.md: EmmyLua-only); PGInterventions.lua calls it only `if Declare_Goal_Type`.
// The original has no such global, so the guarded call never happens there: a no-op here [inferred].
int g_declare_goal_type(Call&) { return 0; }

// Budget.Flush_Category(category): queued for the AI budget of the script's PlayerObject [inferred].
int m_budget_flush_category(Call& c) {
    if (c.nargs != 1)
        return c.fail("BudgetWrapper::Flush_Category -- Invalid number of parameters. Expected 1, got %d.", c.nargs);
    if (!c.is_string(1)) return c.fail("BudgetWrapper::Flush_Category -- invalid type for parameter 1. Expected string");
    LuaPlayerWorld* w = world_of(c.L);
    if (!w) return 0;
    lua_pushstring(c.L, "PlayerObject");
    lua_rawget(c.L, LUA_GLOBALSINDEX);
    int32_t p = -1;
    if (lua_type(c.L, -1) == LUA_TUSERDATA) {
        const auto* ud = static_cast<const Ud*>(lua_touserdata(c.L, -1));
        if (ud && ud->cls == detail::cls_player() && w->valid(ud->id)) p = ud->id;
    }
    lua_pop(c.L, 1);
    w->budget_flushes.push_back({p, std::string(c.string(1))});
    return 0;
}

// ---------------------------------------------------------------- perception

// Target binding of a perception evaluation [inferred]: a planet object of the galactic sim -> its planet
// row; any other live object -> its object id; anything else -> -1 (unbound).
int32_t perception_target(Call& c, const LuaPlayerWorld* w, int i) {
    const objects::ObjectHandle h = object_arg(c, i);
    if (!h) return -1;
    if (const galactic::GalacticSim* sim = galactic_of(c, w)) {
        const PlanetIndex p = sim->planet_of(h);
        if (p != kInvalidPlanet) return static_cast<int32_t>(p);
    }
    return c.vm->host.store->id(h);
}

// Evaluates equation `name` for (self, target); nil when the source, the equation or a token is missing.
int evaluate_perception(Call& c, const LuaPlayerWorld* w, const PerceptionSource& src, std::string_view name,
                        int32_t self, int32_t target) {
    if (!src.equations || !src.resolver) return push_nil(c);
    const uint32_t eq = src.equations->find(name);
    if (eq == ai::kUnresolved) {
        c.vm->message("[EvaluatePerception] unknown perception '" + std::string(name) + "'");
        return push_nil(c);
    }
    ai::EvalContext ctx;
    ctx.self = self;
    ctx.target = target;
    ctx.location = target;
    ctx.enemy = w->enemy_of(self);
    ctx.human = w->local_player();
    ctx.rng = c.vm->host.rng ? &c.vm->host.rng->synced() : nullptr;  // Script_ evaluators only
    ctx.game_seed = src.game_seed;  // A2 tactical: '#' = own reseeded LCG, 0 synced draws (AI_GALACTIC_3 §7.3)
    const ai::EvalResult r = ai::evaluate(*src.equations, eq, ctx, *src.resolver);
    if (!r.ok) return push_nil(c);
    return push_num(c, r.value);
}

int g_evaluate_perception(Call& c) {
    if (c.nargs != 2 && c.nargs != 3)
        return c.fail("Evaluate_Perception: Invalid number of parameters. Expected 2 or 3, got %d.", c.nargs);
    if (!c.is_string(1)) return c.fail("Evaluate_Perception: Invalid parameter type for parameter 1. Expected string.");
    // OI lua (A1 ask 2): with a tactical AI host attached (the tactical AI VMs) the tactical context evaluates it and
    // the target may be an AITargetLocationWrapper or a task force (replaces the EvaluatePerception shim).
    if (const Ud* pl = c.userdata(2, detail::cls_player())) {
        const int r = detail::tactical_evaluate_perception(c, c.string(1), pl->id, c.nargs == 3 ? 3 : 0);
        if (r != -2) return r;
    }
    const LuaPlayerWorld* w = world_of(c.L);
    const int32_t p = player_arg(c, 2, w);
    if (p < 0) return c.fail("Evaluate_Perception: Invalid parameter type for parameter 2. Expected player object.");
    return evaluate_perception(c, w, w->perception(), c.string(1), p, c.nargs == 3 ? perception_target(c, w, 3) : -1);
}

// Galactic perceptions from any mode (EvaluateInGalacticContext.lua): no target [inferred].
int g_evaluate_in_galactic_context(Call& c) {
    if (c.nargs != 2)
        return c.fail("Evaluate_In_Galactic_Context: Invalid number of parameters. Expected 2, got %d.", c.nargs);
    if (!c.is_string(1))
        return c.fail("Evaluate_In_Galactic_Context: Invalid parameter type for parameter 1. Expected string.");
    const LuaPlayerWorld* w = world_of(c.L);
    const int32_t p = player_arg(c, 2, w);
    if (p < 0)
        return c.fail("Evaluate_In_Galactic_Context: Invalid parameter type for parameter 2. Expected player object.");
    return evaluate_perception(c, w, w->galactic_perception(), c.string(1), p, -1);
}

// ---------------------------------------------------------------- WeightedTypeList / EvaluateTypeList

int m_list_create(Call& c) { return new_instance(c, cls_weighted_list()); }

// Parse(names, weights): replaces the list with names[i] / weights[i] (missing weight = 0) [inferred].
int m_list_parse(Call& c) {
    if (!is_instance(c.self, cls_weighted_list())) return c.fail("WeightedTypeListClass -- Invalid parameters");
    if (c.nargs < 2) return c.fail("WeightedTypeListClass -- Not enough parameters");
    if (!is_table(c, 1) || !is_table(c, 2)) return c.fail("WeightedTypeListClass -- Invalid parameters");
    lua_State* L = c.L;
    const int names = c.idx(1), weights = c.idx(2);
    const int n = array_length(L, names);
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, names, i);
        const bool ok = lua_type(L, -1) == LUA_TSTRING;
        lua_pop(L, 1);
        if (!ok) return c.fail("WeightedTypeListClass -- Invalid parameters");
    }
    if (!push_instance_field(L, c.self->value, "items")) return c.fail("WeightedTypeListClass -- Invalid parameters");
    const int items = lua_gettop(L);
    if (!push_instance_field(L, c.self->value, "weights")) {
        lua_pop(L, 1);
        return c.fail("WeightedTypeListClass -- Invalid parameters");
    }
    const int ws = lua_gettop(L);
    const int old = array_length(L, items);
    for (int i = 1; i <= n || i <= old; ++i) {
        if (i <= n) {
            lua_rawgeti(L, names, i);
            lua_rawgeti(L, weights, i);
            const double wv = lua_type(L, -1) == LUA_TNUMBER ? lua_tonumber(L, -1) : 0.0;
            lua_pop(L, 1);
            lua_pushnumber(L, wv);
        } else {
            lua_pushnil(L);
            lua_pushnil(L);
        }
        lua_rawseti(L, ws, i);
        lua_rawseti(L, items, i);
    }
    lua_pop(L, 2);
    return 0;
}

// EvaluateTypeList(player, target, list) -> {bool, ...}: per list entry, whether the target holds an object
// whose type name or CategoryMask token matches it ("All" = any object) [inferred]. The target's objects:
// itself; in the galaxy also a fleet's members, or a planet's starbase, structures, surface units and
// orbiting fleets' members. nil for a dead / missing target.
int g_evaluate_type_list(Call& c) {
    if (c.nargs != 3) return c.fail("Evaluate_Type_List: Invalid number of parameters. Expected 3, got %d.", c.nargs);
    const LuaPlayerWorld* w = world_of(c.L);
    if (player_arg(c, 1, w) < 0)
        return c.fail("Evaluate_Type_List: Invalid parameter type for parameter 1. Expected Player Wrapper.");
    const Ud* list = c.userdata(3, cls_weighted_list());
    if (!is_instance(list, cls_weighted_list()))
        return c.fail("Evaluate_Type_List: Invalid parameter type for parameter 3. Expected weighted type list.");
    const objects::ObjectHandle target = object_arg(c, 2);
    if (!target) return push_nil(c);

    const objects::ObjectStore& st = *c.vm->host.store;
    std::vector<objects::ObjectHandle> objs{target};
    if (const galactic::GalacticSim* sim = galactic_of(c, w)) {
        auto add_fleet = [&](uint32_t f) {
            if (f < sim->fleets().size() && sim->fleets()[f].alive)
                for (const objects::ObjectHandle m : sim->fleets()[f].contents) objs.push_back(m);
        };
        add_fleet(sim->fleet_of(target));
        const PlanetIndex p = sim->planet_of(target);
        if (p != kInvalidPlanet) {
            const galactic::PlanetState& ps = sim->planets()[p];
            objs.push_back(ps.starbase);
            for (const objects::ObjectHandle s : ps.structures) objs.push_back(s);
            for (const objects::ObjectHandle u : ps.surface) objs.push_back(u);
            for (const uint32_t f : ps.orbit) add_fleet(f);
        }
    }

    std::vector<std::string> items;
    if (!push_instance_field(c.L, list->value, "items")) return push_nil(c);
    const int items_index = lua_gettop(c.L);
    const int n = array_length(c.L, items_index);
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(c.L, items_index, i);
        items.emplace_back(lua_tostring(c.L, -1) ? lua_tostring(c.L, -1) : "");
        lua_pop(c.L, 1);
    }
    lua_pop(c.L, 1);

    const objects::GameObjectTypes* types = c.vm->host.types;
    std::vector<uint8_t> found(items.size(), 0);
    for (const objects::ObjectHandle h : objs) {
        if (!h || !st.alive(h)) continue;
        const objects::TypeIndex t = st.type(h);
        const bool known = types && t < types->raw().type_count();
        for (size_t i = 0; i < items.size(); ++i) {
            if (found[i]) continue;
            if (iequals(items[i], "All")) {
                found[i] = 1;
                continue;
            }
            if (!known) continue;
            if (iequals(types->raw().types().name[t], items[i])) {
                found[i] = 1;
                continue;
            }
            for (const std::string_view tok : types->get_list(t, "CategoryMask"))
                if (iequals(tok, items[i])) found[i] = 1;
        }
    }
    lua_newtable(c.L);
    for (size_t i = 0; i < found.size(); ++i) {
        lua_pushboolean(c.L, found[i] ? 1 : 0);
        lua_rawseti(c.L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

// ---------------------------------------------------------------- DiscreteDistribution

int m_dist_create(Call& c) { return new_instance(c, cls_distribution()); }

int m_dist_insert(Call& c) {
    if (c.nargs != 2)
        return c.fail("DiscreteDistribution::Insert -- invalid number of parameters. Expected 2, got %d.", c.nargs);
    if (!c.is_number(2)) return c.fail("DiscreteDistribution::Insert -- invalid type for parameter 2. Expected number.");
    if (!is_instance(c.self, cls_distribution()) || lua_isnil(c.L, c.idx(1)))
        return c.fail("DiscreteDistribution::Insert -- invalid type for parameter 1.");
    lua_State* L = c.L;
    if (!push_instance_field(L, c.self->value, "items")) return c.fail("DiscreteDistribution::Insert -- invalid distribution.");
    const int items = lua_gettop(L);
    if (!push_instance_field(L, c.self->value, "weights")) {
        lua_pop(L, 1);
        return c.fail("DiscreteDistribution::Insert -- invalid distribution.");
    }
    const int ws = lua_gettop(L);
    const int k = array_length(L, items) + 1;
    lua_pushvalue(L, c.idx(1));
    lua_rawseti(L, items, k);
    lua_pushnumber(L, c.number(2));
    lua_rawseti(L, ws, k);
    lua_pop(L, 2);
    return 0;
}

// Sample(): one synced draw r = range_float(0, W) over the positive weights (float sum, insertion order);
// the first entry whose running sum is >= r; nothing when empty or total <= 0 (L1_S4_REMAINING §1.4 [H]).
int m_dist_sample(Call& c) {
    if (!is_instance(c.self, cls_distribution())) return c.fail("DiscreteDistribution::Sample -- not a distribution.");
    GameRandom* rng = c.vm->host.rng;
    if (!rng) return c.fail("DiscreteDistribution::Sample -- no game random attached to this script");
    lua_State* L = c.L;
    if (!push_instance_field(L, c.self->value, "items")) return push_nil(c);
    const int items = lua_gettop(L);
    if (!push_instance_field(L, c.self->value, "weights")) {
        lua_pop(L, 1);
        return push_nil(c);
    }
    const int ws = lua_gettop(L);
    const int n = array_length(L, items);
    std::vector<float> weight(static_cast<size_t>(n), 0.0f);
    float total = 0.0f;
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, ws, i);
        const float v = static_cast<float>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        if (v > 0.0f) {
            weight[static_cast<size_t>(i - 1)] = v;
            total += v;
        }
    }
    int pick = 0;
    if (total > 0.0f) {
        const float r = rng->synced().range_float(0.0f, total);
        float acc = 0.0f;
        for (int i = 1; i <= n; ++i) {
            const float v = weight[static_cast<size_t>(i - 1)];
            if (v <= 0.0f) continue;
            acc += v;
            pick = i;
            if (r <= acc) break;  // L1 final (L1_S4_REMAINING §1.4): the first cumulative >= r
        }
    }
    lua_pop(L, 1);  // weights
    if (pick == 0) {  // empty / total <= 0: returns nothing (§1.4)
        lua_pop(L, 1);
        return 0;
    }
    lua_rawgeti(L, items, pick);
    lua_remove(L, items);
    return 1;
}

// ---------------------------------------------------------------- galactic queries (L1 stage 3)

// GameObjectTypeWrapper.Is_Build_Locked(player): the player's Lock_Tech list [inferred: the tech tree is not
// modelled]. P4 lua: with a galactic sim the sim's list (Lock_Tech / Unlock_Tech, the super-weapon hack), else the
// script override.
int m_type_is_build_locked(Call& c) {
    if (c.nargs != 1)
        return c.fail("GameObjectTypeWrapper::Is_Build_Locked -- invalid number of parameters. Expected 1, got %d.", c.nargs);
    const LuaPlayerWorld* w = world_of(c.L);
    const int32_t p = player_arg(c, 1, w);
    if (!c.userdata(1, detail::cls_player()))
        return c.fail("GameObjectTypeWrapper::Is_Build_Locked -- invalid type for parameter 1. Expected player");
    if (p < 0) return push_bool(c, false);
    if (const galactic::GalacticSim* sim = w->galactic()) return push_bool(c, sim->type_locked(p, static_cast<uint32_t>(c.self->value)));
    return push_bool(c, w->tech_lock(p, static_cast<uint32_t>(c.self->value)) == 1);
}

// GameObjectTypeWrapper.Is_Affiliated_With(player): the type's <Affiliation> list names the player's
// faction (ASCII case-insensitive).
int m_type_is_affiliated_with(Call& c) {
    if (c.nargs != 1)
        return c.fail("GameObjectTypeWrapper::Is_Affiliated_With -- invalid number of parameters. Expected 1, got %d.", c.nargs);
    if (!c.userdata(1, detail::cls_player()))
        return c.fail("GameObjectTypeWrapper::Is_Affiliated_With -- invalid type for parameter 1. Expected player");
    const LuaPlayerWorld* w = world_of(c.L);
    const int32_t p = player_arg(c, 1, w);
    const objects::GameObjectTypes* ty = c.vm->host.types;
    if (p < 0 || !ty || c.self->value >= ty->raw().type_count()) return push_bool(c, false);
    const std::string faction = w->faction_name(p);
    for (const std::string_view a : ty->raw().get_list(static_cast<objects::TypeIndex>(c.self->value), "Affiliation"))
        if (iequals(a, faction)) return push_bool(c, true);
    return push_bool(c, false);
}

// Planet row of a planet object argument / receiver in the galactic sim (kInvalidPlanet otherwise).
PlanetIndex planet_row(const galactic::GalacticSim* sim, objects::ObjectHandle h) {
    return (sim && h) ? sim->planet_of(h) : kInvalidPlanet;
}

// Find_Path(player, from planet, to planet): the planets of the player's path, source first; nil when there
// is no path. The original (0x69da70) answers from the player's AI execution path finder (GALACTIC.md 4 / 5:
// Get_Path on the AI all-pairs matrices; src == dst -> [src, src]) and rejects players without an AI ("can't
// do this from the point of view of non-ai players"; the test is PlayerState::ai [M]).
//   * attached ai::AiPathQuery (LuaPlayerWorld::attach_ai_paths) that knows the player: its Get_Path, exact.
//   * otherwise (no query, or the player has no finder yet): fallback approximation = GalacticSim graph + fleet
//     A*, all request flags set, with the all-pairs fog rule (GRC 1.1, GALACTIC.md 4 step 2): non-human AI
//     players get the Is_Planet_Revealed gate (auto-reveal honoured, flag 5); the human single-player hop gate
//     on the raw revealed byte never applies to Lua.
int g_find_path(Call& c) {
    if (c.nargs != 3) return c.fail("Find_Path -- invalid number of parameters. Expected 3, got %d.", c.nargs);
    const LuaPlayerWorld* w = world_of(c.L);
    const galactic::GalacticSim* sim = galactic_of(c, w);
    const PlanetIndex a = planet_row(sim, object_arg(c, 2));
    const PlanetIndex b = planet_row(sim, object_arg(c, 3));
    if (sim && a == kInvalidPlanet)
        return c.fail("Find_Path -- invalid parameter for parameter 2. This needs to be a valid planet (or wrapper round one)");
    if (sim && b == kInvalidPlanet)
        return c.fail("Find_Path -- invalid parameter for parameter 3. This needs to be a valid planet (or wrapper round one)");
    const int32_t p = player_arg(c, 1, w);
    if (!sim || p < 0 || static_cast<size_t>(p) >= sim->players().size()) return push_nil(c);
    if (!sim->players()[static_cast<size_t>(p)].ai)
        return c.fail("Find_Path -- can't do this from the point of view of non-ai players");
    std::vector<PlanetIndex> path;
    bool answered = false;
    if (ai::AiPathQuery* q = w->ai_paths()) answered = q->ai_path(p, a, b, path);
    if (!answered) {
        galactic::PathRequest req;
        req.source = a;
        req.destination = b;
        galactic::PathScratch scratch;
        galactic::PlayerView view = sim->player_view(p);
        if (view.is_human) view.revealed = galactic::Bitset{};  // all-pairs: no fog gate for a human AI player
        if (!galactic::find_fleet_path(sim->graph(), view, req, scratch, path)) path.clear();
    }
    if (path.empty()) return push_nil(c);
    lua_newtable(c.L);
    int n = 0;
    for (const PlanetIndex row : path) {
        if (row >= sim->planets().size()) continue;
        detail::push_object(c.L, *c.vm, sim->planets()[row].object);
        lua_rawseti(c.L, -2, ++n);
    }
    return 1;
}

// GetCurrentTime.Galactic_Time(): galactic mode time (mode frame / 30) [inferred]; the VM's time
// without a galactic sim.
int m_galactic_time(Call& c) {
    const LuaPlayerWorld* w = world_of(c.L);
    const galactic::GalacticSim* sim = w ? w->galactic() : nullptr;
    return push_num(c, sim ? static_cast<double>(sim->frame()) / 30.0 : c.vm->time);
}

// Planet facts (GameObjectWrapper on a galactic planet object): starbase level, corruption, destroyed.
const galactic::PlanetState* self_planet(Call& c) {
    const galactic::GalacticSim* sim = galactic_of(c, world_of(c.L));
    const PlanetIndex row = planet_row(sim, detail::object_handle(*c.self));
    return row == kInvalidPlanet ? nullptr : &sim->planets()[row];
}
int m_get_starbase_level(Call& c) {
    const galactic::PlanetState* p = self_planet(c);
    return push_num(c, p ? p->starbase_level : 0);
}
int m_is_corrupted(Call& c) {
    const galactic::PlanetState* p = self_planet(c);
    return push_bool(c, p && p->corruption != galactic::kCorruptionNone);
}
int m_is_planet_destroyed(Call& c) {
    const galactic::PlanetState* p = self_planet(c);
    return push_bool(c, p && p->destroyed);
}

// ---------------------------------------------------------------- L1/S4 wave 1: galactic operations

// Mutable sim for the direct Lua operations (Assemble_Fleet): galactic scripts only.
galactic::GalacticSim* galactic_mut(Call& c) {
    LuaPlayerWorld* w = world_of(c.L);
    galactic::GalacticSim* sim = w ? w->galactic() : nullptr;
    return (sim && c.vm->host.store == &sim->store()) ? sim : nullptr;
}

// Assemble_Fleet(table of units): GalacticSim::assemble_fleet (direct, like the original's Lua path);
// returns the fleet object, nil when no unit qualifies or outside a galactic script.
int g_assemble_fleet(Call& c) {
    if (c.nargs != 1) return c.fail("Assemble_Fleet -- invalid number of parameters. Expected 1, got %d.", c.nargs);
    if (lua_type(c.L, c.idx(1)) != LUA_TTABLE) return c.fail("Assemble_Fleet -- invalid type for parameter 1. Expected lua table of units.");
    std::vector<objects::ObjectHandle> units;
    const int t = c.idx(1);
    const int n = luaL_getn(c.L, t);
    for (int i = 1; i <= n; ++i) {  // array part in index order (deterministic; pairs order is not needed)
        lua_rawgeti(c.L, t, i);
        const Ud* u = (lua_type(c.L, -1) == LUA_TUSERDATA) ? static_cast<const Ud*>(lua_touserdata(c.L, -1)) : nullptr;
        const bool ok = u && u->cls == detail::cls_game_object();
        const objects::ObjectHandle h = (ok && detail::object_alive(*c.vm, *u)) ? detail::object_handle(*u) : objects::ObjectHandle{};
        lua_pop(c.L, 1);
        if (!ok)
            return c.fail("Assemble_Fleet -- unexpected type found in table of units to assemble. These should all be GameObjectWrappers");
        if (h) units.push_back(h);
    }
    galactic::GalacticSim* sim = galactic_mut(c);
    if (!sim) return push_nil(c);
    const uint32_t f = sim->assemble_fleet(units);
    if (f == galactic::kNoFleet) return push_nil(c);
    detail::push_object(c.L, *c.vm, sim->fleets()[f].object);
    return 1;
}

// GameObjectWrapper.Invade() on a fleet (Story_Campaign_Underworld.lua): an Invade event for the fleet's
// planet with the fleet's ground units, issued on the direct path through the attached ObjectBridge's
// pipeline (COMMANDS.md 5) [inferred: the unit list = the fleet's ground units, content order]. Returns
// nothing; not a fleet in orbit / no pipeline: nothing happens.
int m_invade(Call& c) {
    const galactic::GalacticSim* sim = galactic_of(c, world_of(c.L));
    const objects::ObjectHandle self = detail::object_handle(*c.self);
    const uint32_t f = sim ? sim->fleet_of(self) : galactic::kNoFleet;
    if (f == galactic::kNoFleet || sim->fleets()[f].moving || sim->fleets()[f].planet == kInvalidPlanet) return 0;
#if ALAMO_LUA_HAVE_COMMANDS
    ObjectBridge* b = attached_object_bridge(*c.vm->owner);
    if (!b) return 0;
    ++b->orders_issued;
    if (!b->pipeline) return 0;
    const galactic::FleetState& fl = sim->fleets()[f];
    std::vector<uint32_t> ids;
    for (const objects::ObjectHandle m : fl.contents)
        if (sim->store().alive(m) && sim->rules().types[sim->store().type(m)].kind == galactic::UnitKind::GroundUnit)
            ids.push_back(static_cast<uint32_t>(sim->store().id(m)));
    commands::EventData e(commands::EventType::Invade);
    e.set("planet", static_cast<uint32_t>(sim->store().id(sim->planets()[fl.planet].object))).set_ids("units", std::move(ids));
    const uint8_t player = (fl.owner >= 0 && fl.owner < commands::kMaxPlayerSlots) ? static_cast<uint8_t>(fl.owner) : 0;
    b->pipeline->execute_direct(c.vm->frame, player, commands::CommandSource::Script, e);
#endif
    return 0;
}

// WaitForStarbase / WaitForGroundbase(planet, level): a BlockingStatus finished once the planet's starbase
// (ground base) level is >= level. Level range [inferred] 0..5.
constexpr int32_t kMaxBaseLevel = 5;
int wait_for_base(Call& c, bool starbase) {
    const char* who = starbase ? "WaitForStarbase" : "WaitForGroundbase";
    if (c.nargs != 2) return c.fail("WaitForGroundBase : Invalid number of parameters: %d should be 2.", c.nargs);  // (sic, both)
    const galactic::GalacticSim* sim = galactic_of(c, world_of(c.L));
    const objects::ObjectHandle planet = object_arg(c, 1);
    if (!c.userdata(1, detail::cls_game_object()) || (sim && planet_row(sim, planet) == kInvalidPlanet))
        return c.fail("%s : Invalid type for parameter 1. Expected planet game object.", who);
    const int32_t level = c.is_number(2) ? detail::to_int(c.number(2)) : -1;
    if (level < 0 || level > kMaxBaseLevel)
        return c.fail("%s : Invalid value %d for parameter 2. Must be between %d and %d.", starbase ? "WaitForStarBase" : "WaitForGroundBase",
                      level, 0, kMaxBaseLevel);
    Ud u;
    u.cls = detail::class_id("BlockingStatus");
    u.reserved = starbase ? detail::kBlockStarbaseLevel : detail::kBlockGroundbaseLevel;
    u.id = c.userdata(1, detail::cls_game_object())->id;
    u.value = static_cast<uint64_t>(level);
    detail::push_engine_userdata(c.L, u);
    return 1;
}
int g_wait_for_starbase(Call& c) { return wait_for_base(c, true); }
int g_wait_for_groundbase(Call& c) { return wait_for_base(c, false); }

// Next base type of a planet [inferred]: the first type (type index order) of the base kind whose Base_Level
// is the planet's current level + 1 and whose Affiliation names the planet owner's faction (or is empty).
int next_base_type(Call& c, objects::ObjectHandle planet, bool starbase) {
    const galactic::GalacticSim* sim = galactic_of(c, world_of(c.L));
    const PlanetIndex row = planet_row(sim, planet);
    if (row == kInvalidPlanet) return push_nil(c);
    const galactic::PlanetState& p = sim->planets()[row];
    const int32_t want = (starbase ? p.starbase_level : sim->ground_base_level(row)) + 1;
    const uint32_t faction = (p.owner >= 0 && static_cast<size_t>(p.owner) < sim->players().size())
                                 ? sim->players()[static_cast<size_t>(p.owner)].faction
                                 : galactic::kNoIndex;
    const galactic::UnitKind kind = starbase ? galactic::UnitKind::StarBase : galactic::UnitKind::GroundStructure;
    const auto& types = sim->rules().types;
    for (uint32_t t = 0; t < types.size(); ++t) {
        if (types[t].kind != kind || types[t].base_level != want) continue;
        if (!types[t].affiliation.empty() &&
            std::find(types[t].affiliation.begin(), types[t].affiliation.end(), faction) == types[t].affiliation.end())
            continue;
        detail::push_type(c.L, *c.vm, t);
        return 1;
    }
    return push_nil(c);
}
int g_get_next_base_type(Call& c, bool starbase) {
    const char* who = starbase ? "GetNextStarbaseType" : "GetNextGroundbaseType";
    if (c.nargs != 1) return c.fail("%s : invalid number of parameters. Expected 1, got %d.", who, c.nargs);
    const galactic::GalacticSim* sim = galactic_of(c, world_of(c.L));
    const objects::ObjectHandle planet = object_arg(c, 1);
    if (!c.userdata(1, detail::cls_game_object()) || (sim && planet_row(sim, planet) == kInvalidPlanet))
        return c.fail("%s : invalid parameter type. Expected planet game object.", who);
    return next_base_type(c, planet, starbase);
}
int g_get_next_starbase_type(Call& c) { return g_get_next_base_type(c, true); }
int g_get_next_groundbase_type(Call& c) { return g_get_next_base_type(c, false); }
int m_get_next_starbase_type(Call& c) { return next_base_type(c, detail::object_handle(*c.self), true); }

// Get_Is_Planet_AI_Usable() [inferred]: a galactic planet that is neither hidden (inaccessible) nor destroyed.
int m_get_is_planet_ai_usable(Call& c) {
    const galactic::PlanetState* p = self_planet(c);
    return push_bool(c, p && !p->hidden && !p->destroyed);
}

// Can_Land_On_Planet(planet) [inferred]: this object (or, for a fleet, one of its members) is a ground unit
// and the planet's surface is accessible, not hidden and not destroyed.
int m_can_land_on_planet(Call& c) {
    if (c.nargs != 1)
        return c.fail("GameObjectWrapper::Can_Land_On_Planet -- invalid number of parameters. Expected 1, got %d.", c.nargs);
    if (!c.userdata(1, detail::cls_game_object()))
        return c.fail("GameObjectWrapper::Can_Land_On_Planet -- invalid type for parameter 1. Expected game object.");
    const galactic::GalacticSim* sim = galactic_of(c, world_of(c.L));
    const PlanetIndex row = planet_row(sim, object_arg(c, 1));
    if (row == kInvalidPlanet) return push_bool(c, false);
    const galactic::PlanetState& p = sim->planets()[row];
    if (p.hidden || p.destroyed || p.type >= sim->rules().types.size() || !sim->rules().types[p.type].surface_accessible)
        return push_bool(c, false);
    const objects::ObjectHandle self = detail::object_handle(*c.self);
    auto ground = [sim](objects::ObjectHandle h) {
        return sim->store().alive(h) && sim->store().type(h) < sim->rules().types.size() &&
               sim->rules().types[sim->store().type(h)].kind == galactic::UnitKind::GroundUnit;
    };
    if (ground(self)) return push_bool(c, true);
    const uint32_t f = sim->fleet_of(self);
    if (f != galactic::kNoFleet)
        for (const objects::ObjectHandle m : sim->fleets()[f].contents)
            if (ground(m)) return push_bool(c, true);
    return push_bool(c, false);
}

constexpr BindingKind G = BindingKind::Global;
constexpr BindingKind M = BindingKind::Method;

constexpr detail::ImplEntry kImpl[] = {
    // galactic queries (stage 3)
    {G, "", "Find_Path", g_find_path},
    {M, "GameObjectTypeWrapper", "Is_Build_Locked", m_type_is_build_locked},
    {M, "GameObjectTypeWrapper", "Is_Affiliated_With", m_type_is_affiliated_with},
    {M, "LuaGetTime", "Galactic_Time", m_galactic_time},
    {M, "GameObjectWrapper", "Get_Starbase_Level", m_get_starbase_level},
    {M, "GameObjectWrapper", "Is_Corrupted", m_is_corrupted},
    {M, "GameObjectWrapper", "Is_Planet_Destroyed", m_is_planet_destroyed},
    // galactic operations (L1/S4 wave 1)
    {G, "", "Assemble_Fleet", g_assemble_fleet},
    {G, "", "WaitForStarbase", g_wait_for_starbase},
    {G, "", "WaitForGroundbase", g_wait_for_groundbase},
    {G, "", "GetNextStarbaseType", g_get_next_starbase_type},
    {G, "", "GetNextGroundbaseType", g_get_next_groundbase_type},
    {M, "GameObjectWrapper", "Get_Next_Starbase_Type", m_get_next_starbase_type},
    {M, "GameObjectWrapper", "Invade", m_invade},
    {M, "GameObjectWrapper", "Get_Is_Planet_AI_Usable", m_get_is_planet_ai_usable},
    {M, "GameObjectWrapper", "Can_Land_On_Planet", m_can_land_on_planet},
    // globals
    {G, "", "Find_Player", g_find_player},
    {G, "", "Suspend_AI", g_suspend_ai},
    {G, "", "EvaluatePerception", g_evaluate_perception},
    {G, "", "Evaluate_In_Galactic_Context", g_evaluate_in_galactic_context},
    {G, "", "EvaluateTypeList", g_evaluate_type_list},
    {G, "", "Purge_Goals", g_purge_goals},
    {G, "", "GiveDesireBonus", g_give_desire_bonus},
    {G, "", "Declare_Goal_Type", g_declare_goal_type},
    // PlayerWrapper
    {M, "PlayerWrapper", "Get_Faction_Name", m_get_faction_name},
    {M, "PlayerWrapper", "Get_Name", m_get_name},
    {M, "PlayerWrapper", "Is_Human", m_is_human},
    {M, "PlayerWrapper", "Is_Neutral", m_is_neutral},
    {M, "PlayerWrapper", "Get_Credits", m_get_credits},
    {M, "PlayerWrapper", "Give_Money", m_give_money},
    {M, "PlayerWrapper", "Get_Difficulty", m_get_difficulty},
    {M, "PlayerWrapper", "Get_Tech_Level", m_get_tech_level},
    {M, "PlayerWrapper", "Set_Tech_Level", m_set_tech_level},
    {M, "PlayerWrapper", "Is_Ally", m_is_ally},
    {M, "PlayerWrapper", "Is_Enemy", m_is_enemy},
    {M, "PlayerWrapper", "Make_Ally", m_make_ally},
    {M, "PlayerWrapper", "Make_Enemy", m_make_enemy},
    {M, "PlayerWrapper", "Get_Enemy", m_get_enemy},
    {M, "PlayerWrapper", "Lock_Tech", m_lock_tech},
    {M, "PlayerWrapper", "Unlock_Tech", m_unlock_tech},
    {M, "PlayerWrapper", "Select_Object", m_select_object},
    {M, "PlayerWrapper", "Enable_As_Actor", m_enable_as_actor},
    {M, "PlayerWrapper", "Retreat", m_retreat},
    {M, "PlayerWrapper", "Get_Team", m_get_team},
    {M, "PlayerWrapper", "Get_Clan_ID", m_get_clan_id},
    {M, "PlayerWrapper", "Release_Credits_For_Tactical", m_release_credits_for_tactical},
    {M, "PlayerWrapper", "Disable_Bombing_Run", m_disable_bombing_run},
    {M, "PlayerWrapper", "Disable_Orbital_Bombardment", m_disable_orbital_bombardment},
    {M, "PlayerWrapper", "Remove_Orbital_Bombardment", m_remove_orbital_bombardment},
    {M, "PlayerWrapper", "Set_Black_Market_Tutorial", m_set_black_market_tutorial},
    {M, "PlayerWrapper", "Set_Sabotage_Tutorial", m_set_sabotage_tutorial},
    {M, "PlayerWrapper", "Enable_Advisor_Hints", m_enable_advisor_hints},
    // AI data objects
    {M, "WeightedTypeListClass", "Create", m_list_create},
    {M, "WeightedTypeListClass", "Parse", m_list_parse},
    {M, "LuaDiscreteDistributionClass", "Create", m_dist_create},
    {M, "LuaDiscreteDistributionClass", "Insert", m_dist_insert},
    {M, "LuaDiscreteDistributionClass", "Sample", m_dist_sample},
    {M, "BudgetWrapper", "Flush_Category", m_budget_flush_category},
};

}  // namespace

std::span<const detail::ImplEntry> detail::impl_players() { return kImpl; }

// WaitForStarbase / WaitForGroundbase blocks: finished when the planet (by object id) reaches the level, or
// when it is gone / the script has no galactic sim (nothing to wait for).
bool detail::galactic_block_finished(Call& c) {
    const galactic::GalacticSim* sim = galactic_of(c, world_of(c.L));
    if (!sim) return true;
    const PlanetIndex row = planet_row(sim, sim->store().find(c.self->id));
    if (row == kInvalidPlanet) return true;
    const auto level = static_cast<int32_t>(c.self->value);
    if (c.self->reserved == kBlockStarbaseLevel) return sim->planets()[row].starbase_level >= level;
    if (c.self->reserved == kBlockGroundbaseLevel) return sim->ground_base_level(row) >= level;
    return true;
}

// --- P4 lua: FoC galactic special systems (vm_internal.hpp) ----------------------------------------------------------

// Activate_Ability_Galactic (0x570670, P4_SIM_SYSTEMS 6.1, L1P4_LUA_GAPS 1): optional object-type argument (wrapper or
// type name, galactic_ability_type_arg; a bad one fires nothing). Not an ability name: the trigger tries every
// Ground_Activated ability of the unit's type. The object must be in orbit:
//   - a fleet at a planet: each direct member (content order: the port's fleets carry the original's transports as
//     the company objects themselves) whose representative type (GalacticSim::representative_type: squadron /
//     company type) is the argument's;
//   - a unit in a fleet at a planet (a company object = the original's transport: its cargo): that unit, the argument
//     is not applied;
//   - a company member view (refix t04: a unit inside a transport): silently nothing;
//   - a fleet in transit, a planet or a free object: "is not at a planet"; a unit on the surface: "is landed"; nothing.
// Returns no values (the caller pushes none).
bool detail::galactic_activate_ability(Call& c) {
    galactic::GalacticSim* sim = galactic_mut(c);
    if (!sim) return false;
    uint32_t filter = objects::kNone;
    if (!galactic_ability_type_arg(c, "GameObjectWrapper::Activate_Ability_Galactic", filter)) return true;
    if (detail::is_member_view(*c.self)) return true;  // parent = a transport: not a planet, not a fleet (1.2 case 3)
    const objects::ObjectHandle self = detail::object_handle(*c.self);
    if (!sim->store().alive(self)) return true;
    uint32_t f = sim->fleet_of(self);
    const bool is_fleet = f != galactic::kNoFleet;
    if (!is_fleet) f = sim->fleet_containing(self);
    auto error = [&c](const char* what) { c.vm->message(std::string("SCRIPT ERROR: GameObjectWrapper::Activate_Ability_Galactic -- ") + what); };
    if (f == galactic::kNoFleet) {
        const PlanetIndex at = planet_row(sim, sim->container_of(self));
        if (at != kInvalidPlanet) error("object is landed. Must be in orbit to activate abilities.");
        else if (!sim->container_of(self) || sim->planet_of(self) != kInvalidPlanet) error("object is not at a planet");
        return true;  // inside a transport / team: silently nothing
    }
    const galactic::FleetState& fl = sim->fleets()[f];
    if (fl.moving || fl.planet == kInvalidPlanet) {
        error("object is not at a planet");
        return true;
    }
    std::vector<std::pair<objects::ObjectHandle, uint32_t>> todo;
    if (is_fleet) {
        for (const objects::ObjectHandle m : fl.contents)
            if (sim->store().alive(m) && (filter == objects::kNone || sim->representative_type(m) == filter))
                todo.emplace_back(m, fl.planet);
    } else {
        todo.emplace_back(self, fl.planet);
    }
    galactic_trigger(c, todo);
    return true;
}

void detail::galactic_trigger(Call& c, const std::vector<std::pair<objects::ObjectHandle, uint32_t>>& units) {
    galactic::GalacticSim* sim = galactic_mut(c);
    if (!sim) return;
    for (const auto& [u, planet] : units)
        if (sim->store().alive(u)) sim->ground_activated_trigger(u, planet);
}

bool detail::black_market_allows(Call& c, int32_t player, std::string_view ability_name, bool must_be_bought) {
    const LuaPlayerWorld* w = world_of(c.L);
    const galactic::GalacticSim* sim = w ? w->galactic() : nullptr;
    if (!sim || !must_be_bought) return true;
    return sim->bm_allows_unit_ability(player, galactic::special_name_crc(ability_name), true);
}
// --- end P4 lua ---

objects::ObjectHandle detail::galactic_spawn_unit(Call& c, uint32_t type, int32_t player, objects::ObjectHandle location) {
    galactic::GalacticSim* sim = galactic_mut(c);
    const PlanetIndex row = planet_row(sim, location);
    if (row == kInvalidPlanet) return {};
    return sim->spawn_unit(player, row, type);
}

}  // namespace alamo::lua

#else  // !ALAMO_LUA_PLAYERS

namespace alamo::lua {
std::span<const detail::ImplEntry> detail::impl_players() { return {}; }
bool detail::galactic_block_finished(Call&) { return true; }
// P4 lua stubs: no galactic sim in this build.
bool detail::galactic_activate_ability(Call&) { return false; }
void detail::galactic_trigger(Call&, const std::vector<std::pair<objects::ObjectHandle, uint32_t>>&) {}
bool detail::black_market_allows(Call&, int32_t, std::string_view, bool) { return true; }
objects::ObjectHandle detail::galactic_spawn_unit(Call&, uint32_t, int32_t, objects::ObjectHandle) { return {}; }
}  // namespace alamo::lua

#endif
