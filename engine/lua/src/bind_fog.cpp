// Binding family: fog of war (fog follow-up, L1; docs/re/FOG_OF_WAR.md §6). Host contract, error discipline and the
// snapshot rules: alamo/lua/fog_bridge.hpp. [H] rules are the doc's; port choices are marked [port].
//
// Determinism: no RNG; the snapshots live in FogScriptCells (a vector in ascending handle id). FogOfWar itself is
// order-independent (FOG_OF_WAR §7) and not in the frame CRC.
#include <bit>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <utility>

#include "alamo/core/sim_crc.hpp"
#include "alamo/lua/ai_bridge.hpp"
#include "alamo/lua/fog_bridge.hpp"
#include "alamo/lua/object_bridge.hpp"
#include "alamo/lua/script_gaps.hpp"
#include "vm_internal.hpp"

namespace alamo::lua {

using detail::Call;
using detail::Ud;
using objects::ObjectHandle;

namespace {

constexpr const char* kName = "LuaFOWRevealCommandClass";

// The original's script error helper: logged, no unwind; the binding returns nothing.
int script_error(Call& c, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    c.vm->message(std::string("SCRIPT ERROR: ") + buf);
    return 0;
}

// The FogOfWar global called as a function: no documented form [port].
int g_fog_of_war(Call&) { return 0; }

#ifdef ALAMO_LUA_HAVE_FOG
namespace fw = tactical::fog;

uint32_t cls_cells() {
    static const uint32_t c = detail::class_id("LuaFOWCellsClass");
    return c;
}
uint32_t cls_position() {
    static const uint32_t c = detail::class_id("PositionWrapper");
    return c;
}
uint32_t cls_target() {
    static const uint32_t c = detail::class_id("AITargetLocationWrapper");
    return c;
}

FogScriptHooks* fog_hooks(Call& c) {
    ObjectBridge* b = c.vm->owner ? attached_object_bridge(*c.vm->owner) : nullptr;
    TacticalScriptHooks* t = b ? b->tactical : nullptr;
    return t ? t->fog : nullptr;
}

// The fog's view of an object: the host hook, else [port] owner + position only (sight 0 -> radius 10, no model).
bool fog_object(Call& c, FogScriptHooks& hk, ObjectHandle h, fw::FogObject& out) {
    if (hk.object) return hk.object(h, out);
    objects::ObjectStore* st = c.vm->host.store;
    if (!st || !st->alive(h)) return false;
    const Vec3 p = st->position(h);
    out = fw::FogObject{};
    out.owner = st->owner(h);
    out.x = p.x;
    out.y = p.y;
    out.z = p.z;
    return true;
}

// A live game object argument (nullptr otherwise).
bool object_arg(Call& c, int i, ObjectHandle& out) {
    const Ud* o = c.userdata(i, detail::cls_game_object());
    if (!o || !detail::object_alive(*c.vm, *o)) return false;
    out = detail::object_handle(*o);
    return true;
}

// A position-convertible argument: game object, PositionWrapper (bind_objects.cpp layout: x / y float bits in
// Ud::value, z in Ud::id), AI target with an object.
bool position_arg(Call& c, int i, Vec3& out) {
    objects::ObjectStore* st = c.vm->host.store;
    ObjectHandle h;
    if (object_arg(c, i, h)) {
        out = st->position(h);
        return true;
    }
    if (const Ud* p = c.userdata(i, cls_position())) {
        out = {std::bit_cast<float>(static_cast<uint32_t>(p->value & 0xFFFFFFFFu)), std::bit_cast<float>(static_cast<uint32_t>(p->value >> 32)),
               std::bit_cast<float>(p->id)};
        return true;
    }
    if (const Ud* t = c.userdata(i, cls_target())) {
        AiScriptHost* ai = c.vm->owner ? ai_host(*c.vm->owner) : nullptr;
        const ObjectHandle th = ai ? ai->target_object(t->id) : ObjectHandle{};
        if (st && th && st->alive(th)) {
            out = st->position(th);
            return true;
        }
    }
    return false;
}

void push_cells(Call& c, FogScriptHooks& hk, fw::FogCells cells) {
    Ud u;
    u.cls = cls_cells();
    u.id = hk.cells.add(std::move(cells));
    u.value = hk.epoch;
    detail::push_engine_userdata(c.L, u);
}

// FogOfWar.Reveal(player, target [, radius [, denseRadius]]) (§6.2, 0x6a5700).
int m_reveal(Call& c) {
    if (c.nargs < 2) return script_error(c, "%s -- Requires at least 2 parameters: (player, object).", kName);
    const Ud* player = c.userdata(1, detail::cls_player());
    if (!player) return script_error(c, "%s -- Expected player object as first parameter.", kName);
    FogScriptHooks* hk = fog_hooks(c);
    if (!hk || !hk->fog) return script_error(c, "%s -- Command only valid in a tactical game", kName);
    ObjectHandle h;
    if (c.nargs == 2 && object_arg(c, 2, h)) {
        // RevealObject with force 0: own sight when allied to the player, footprint otherwise.
        fw::FogObject o;
        if (!fog_object(c, *hk, h, o)) return script_error(c, "%s -- Undable to get a valid position", kName);
        push_cells(c, *hk, hk->fog->lua_reveal_object(player->id, o));
        return 1;
    }
    if (!c.has(3)) return script_error(c, "%s -- Expected 3rd parameter for reveal radius.", kName);
    // [port] a non-number radius / dense radius reads through a null pointer in the original: an error here.
    if (!c.is_number(3)) return script_error(c, "%s -- Expected number as 3rd parameter.", kName);
    if (c.has(4) && !c.is_number(4)) return script_error(c, "%s -- Expected number as 4th parameter.", kName);
    Vec3 p{};
    if (!position_arg(c, 2, p)) return script_error(c, "%s -- Undable to get a valid position", kName);
    const auto radius = static_cast<float>(c.number(3));
    const float dense = c.has(4) ? static_cast<float>(c.number(4)) : radius;
    push_cells(c, *hk, hk->fog->lua_reveal_position(player->id, p.x, p.y, radius, dense));
    return 1;
}

// FogOfWar.Reveal_All(player) (§6.5, 0x6a5b00): returns nothing; no Lua way to undo it.
int m_reveal_all(Call& c) {
    if (c.nargs < 1) return script_error(c, "%s -- Requires at least 1 parameters: (player).", kName);
    const Ud* player = c.userdata(1, detail::cls_player());
    if (!player) return script_error(c, "%s -- Expected player object as first parameter.", kName);
    FogScriptHooks* hk = fog_hooks(c);
    if (!hk || !hk->fog) return script_error(c, "%s -- Command only valid in a tactical game", kName);
    hk->fog->reveal_all(player->id);
    return 0;
}

// FogOfWar.Temporary_Reveal(player, object [, radius]) (§6.5, 0x6a5cf0): flash, returns nothing.
int m_temporary_reveal(Call& c) {
    const Ud* player = c.userdata(1, detail::cls_player());
    if (!player) return script_error(c, "%s -- Expected player object as first parameter.", kName);
    ObjectHandle h;
    if (!object_arg(c, 2, h)) return script_error(c, "%s -- Expected object as 2nd parameter.", kName);
    if (c.has(3) && !c.is_number(3)) return script_error(c, "%s -- Expected number as 3rd parameter.", kName);
    FogScriptHooks* hk = fog_hooks(c);
    if (!hk || !hk->fog) return script_error(c, "%s -- Command only valid in a tactical game", kName);
    fw::FogObject o;
    if (!fog_object(c, *hk, h, o)) return script_error(c, "%s -- Expected object as 2nd parameter.", kName);
    hk->fog->temporary_reveal(player->id, o, c.has(3) ? static_cast<float>(c.number(3)) : 0.0f);
    return 0;
}

// FogOfWar.Disable_Rendering([bool]) (§6.5, 0x6a53b0): FOW-active = render option = not arg. Without an argument
// the doc's outcome is "fog off" [M: read as a missing argument disabling].
int m_disable_rendering(Call& c) {
    const bool disable = !c.has(1) || lua_toboolean(c.L, c.idx(1)) != 0;
    FogScriptHooks* hk = fog_hooks(c);
    if (hk && hk->fog) hk->fog->disable_rendering(disable);
    else if (hk && hk->set_globals) hk->set_globals(!disable, !disable);
    return 0;
}

// cells.Undo_Reveal() (§6.3, 0x6a5560): decrement and empty; a second call (or another fog's handle) does nothing.
int m_undo_reveal(Call& c) {
    FogScriptHooks* hk = fog_hooks(c);
    if (!hk || c.self->value != hk->epoch) return 0;
    fw::FogCells* cells = hk->cells.find(c.self->id);
    if (!cells) return 0;
    if (hk->fog) hk->fog->undo_reveal(*cells);
    hk->cells.erase(c.self->id);
    return 0;
}
#else
// Without the fog library every tactical command answers as outside a tactical mode.
int no_tactical(Call& c) {
    if (!c.userdata(1, detail::cls_player())) return script_error(c, "%s -- Expected player object as first parameter.", kName);
    return script_error(c, "%s -- Command only valid in a tactical game", kName);
}
int m_reveal(Call& c) {
    if (c.nargs < 2) return script_error(c, "%s -- Requires at least 2 parameters: (player, object).", kName);
    return no_tactical(c);
}
int m_reveal_all(Call& c) {
    if (c.nargs < 1) return script_error(c, "%s -- Requires at least 1 parameters: (player).", kName);
    return no_tactical(c);
}
int m_temporary_reveal(Call& c) { return no_tactical(c); }
int m_disable_rendering(Call&) { return 0; }
int m_undo_reveal(Call&) { return 0; }
#endif

constexpr BindingKind G = BindingKind::Global;
constexpr BindingKind M = BindingKind::Method;

constexpr detail::ImplEntry kImpl[] = {
    {G, "", "FogOfWar", g_fog_of_war},
    {M, "LuaFOWRevealCommandClass", "Reveal", m_reveal},
    {M, "LuaFOWRevealCommandClass", "Reveal_All", m_reveal_all},
    {M, "LuaFOWRevealCommandClass", "Temporary_Reveal", m_temporary_reveal},
    {M, "LuaFOWRevealCommandClass", "Disable_Rendering", m_disable_rendering},
    {M, "LuaFOWCellsClass", "Undo_Reveal", m_undo_reveal},
};

}  // namespace

std::span<const detail::ImplEntry> detail::impl_fog() { return kImpl; }

#ifdef ALAMO_LUA_HAVE_FOG
// ---------------------------------------------------------------- FogScriptCells

int32_t FogScriptCells::add(tactical::fog::FogCells cells) {
    Entry e;
    e.id = next_++;
    e.cells = std::move(cells);
    list_.push_back(std::move(e));
    return list_.back().id;
}

tactical::fog::FogCells* FogScriptCells::find(int32_t id) {
    for (Entry& e : list_)
        if (e.id == id) return &e.cells;
    return nullptr;
}

void FogScriptCells::erase(int32_t id) {
    for (auto it = list_.begin(); it != list_.end(); ++it)
        if (it->id == id) {
            list_.erase(it);
            return;
        }
}

void FogScriptCells::recount(tactical::fog::FogOfWar& fog) const {
    for (const Entry& e : list_) fog.recount(e.cells);
}

namespace {
constexpr uint32_t kFogCellsSaveVersion = 1;
}

void FogScriptCells::write(ByteWriter& w) const {
    w.u32(kFogCellsSaveVersion).i32(next_).u32(static_cast<uint32_t>(list_.size()));
    for (const Entry& e : list_) {
        w.i32(e.id).i32(e.cells.player).u32(static_cast<uint32_t>(e.cells.cells.size()));
        for (const uint32_t cell : e.cells.cells) w.u32(cell);
    }
}

bool FogScriptCells::read(ByteReader& r) {
    if (r.u32() != kFogCellsSaveVersion || !r.ok()) return false;
    const int32_t next = r.i32();
    const uint32_t n = r.u32();
    if (!r.ok() || next < 1 || n > r.remaining() / 12) return false;
    std::vector<Entry> list;
    list.reserve(n);
    int32_t prev = 0;
    for (uint32_t i = 0; i < n; ++i) {
        Entry e;
        e.id = r.i32();
        e.cells.player = r.i32();
        const uint32_t m = r.u32();
        if (!r.ok() || e.id <= prev || e.id >= next || m > r.remaining() / 4) return false;
        prev = e.id;
        e.cells.cells.resize(m);
        for (uint32_t& cell : e.cells.cells) cell = r.u32();
        if (!r.ok()) return false;
        list.push_back(std::move(e));
    }
    list_ = std::move(list);
    next_ = next;
    return true;
}

void FogScriptCells::hash(StateHasher& h) const {
    h.u32(static_cast<uint32_t>(list_.size()));
    for (const Entry& e : list_) {
        h.i32(e.id);
        h.i32(e.cells.player);
        h.u32(static_cast<uint32_t>(e.cells.cells.size()));
        for (const uint32_t cell : e.cells.cells) h.u32(cell);
    }
}
#endif

}  // namespace alamo::lua
