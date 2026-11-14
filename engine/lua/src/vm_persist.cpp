// N1 save / load of one Lua 5.0.2 heap (LuaVm::write / read, lua_vm.cpp). Written clean-room from the vendored Lua
// 5.0.2 sources (lobject.h, lstate.h, lgc.c, ldo.c, lvm.c); no persistence library is used or copied.
//
// WHAT IS SAVED: every object of the heap, in the collector's own list order - the main thread, G->rootgc, then
// G->rootudata, then each thread's open upvalues - so a restored VM is the same heap object for object:
//   * FULL objects (everything reachable from the registry, the default metatable and the main thread, following
//     weak references too; plus unfinalized userdata whose metatable has __gc, and the thread that owns an open
//     upvalue of a reachable closure) are saved with their contents;
//   * every other object is garbage the next collection frees. It is saved as a HUSK: its type, its allocation sizes
//     and its GC bits only (strings keep their bytes; a coroutine keeps its open upvalues, because freeing it closes
//     them and that consumes creation serials). A husk is unreachable after the load exactly as before.
// So Lua's byte count (nblocks), the string count and every allocation size are identical after a load: the next
// collections run at the same points and free the same bytes, and freeing garbage consumes the same serials. read()
// checks this: the rebuilt heap must account for exactly the saved byte count.
// Every collectable object keeps its creation serial (tables / functions / userdata / threads hash by it, CMake
// patch) and the counter is restored. Tables keep their exact node layout (key, value, chain link per node,
// firstfree), so pairs() order and every later insertion are unchanged. Threads keep their stack, their CallInfo
// frames (base, top, state, saved pc as an offset into the closure's code, tail calls), stack_last (5.0.2's
// restore_stack_limit quirk), hook counters and open upvalues (stack level, list order).
//
// VALUES: nil, boolean, number (bit pattern), string (index into the sorted string table), table / function /
// userdata / thread (object index), the VM's own light userdata (engine closures' upvalue 1), a registry
// "attachment" (a light userdata at a string key of the registry: a host pointer - re-taken from the VM being read
// into, by key; NULL when it has none) and a dead key (a collected key left in a node whose value is nil).
// C functions are saved by NAME: every C function reachable from a fresh VM's globals ("G.<name>",
// "G.<table>.<name>") and registry tables ("R.<key>.<name>"), plus the closures created at run time
// (runtime_cfunctions_*: coroutine.wrap, string.gfind, bound methods, presentation blocks). Engine userdata
// (bindings' Ud under the shared metatable) are saved as (class name, binding name, value, id, reserved).
//
// NOT SAVEABLE (write fails with a reason, nothing usable written): a thread inside a running call (a frame with an
// active luaV_execute: write() was called from a binding), a debug hook, userdata waiting in the finalizer list, a
// light userdata other than the two kinds above, a C function that is not in the name table. A coroutine can only be
// suspended in coroutine.yield called directly from Lua: Lua 5.0.2 refuses to yield across a C call or metamethod
// ("attempt to yield across metamethod/C-call boundary"), so a saved coroutine never has C state to persist.
#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "alamo/core/bytes.hpp"
#include "vm_internal.hpp"

// Lua internals: the PATCHED copies in the build dir (alamo_serial in CommonHeader / global_State). Included last:
// they define short macros (cast, G, gt, registry, check_exp, ...).
#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lgc.h"
#include "lmem.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "ltm.h"
#include "lzio.h"

namespace alamo::lua::detail {

namespace {

constexpr uint32_t kHeapFormat = 1;
constexpr uint32_t kNoIndex = 0xFFFFFFFFu;

// Object kinds (one index space; 0 = the main thread).
enum : uint8_t { kTable = 1, kLuaClosure, kCClosure, kUserdata, kThread, kProto, kUpvalClosed, kUpvalOpen };
// Value tags.
enum : uint8_t { vNil = 0, vBool, vLight, vNumber, vString, vTable, vFunction, vUserdata, vThread, vDeadKey };
enum : uint8_t { kLightVm = 0, kLightAttachment = 1 };
enum : uint8_t { kUdRaw = 0, kUdEngine = 1 };

// Plausibility caps for sizes that are not backed by saved bytes (husks): no real script heap comes near them.
constexpr uint64_t kMaxHeapBytes = 1ull << 31;
constexpr int kMaxTableBits = 24;  // ltable.c MAXBITS (int >= 32 bits)
constexpr int32_t kMaxStack = 1 << 24;

// ltable.c / lfunc.c size formulas (sizeCclosure / sizeLclosure live in lfunc.c).
uint64_t table_bytes(int lsizenode, int64_t sizearray) {
    return sizeof(::Table) + (lsizenode > 0 ? (uint64_t{1} << lsizenode) * sizeof(::Node) : 0u) +
           static_cast<uint64_t>(sizearray) * sizeof(::TObject);
}
uint64_t cclosure_bytes(int n) {
    return static_cast<uint64_t>(static_cast<int64_t>(sizeof(::CClosure)) + static_cast<int64_t>(sizeof(::TObject)) * (n - 1));
}
uint64_t lclosure_bytes(int n) {
    return static_cast<uint64_t>(static_cast<int64_t>(sizeof(::LClosure)) + static_cast<int64_t>(sizeof(::TObject*)) * (n - 1));
}
uint64_t thread_bytes(int64_t stacksize, int64_t size_ci) {
    return sizeof(::lua_State) + static_cast<uint64_t>(stacksize) * sizeof(::TObject) +
           static_cast<uint64_t>(size_ci) * sizeof(::CallInfo);
}
uint64_t proto_bytes(const int64_t sizes[6]) {  // code, k, p, lineinfo, locvars, upvalues
    return sizeof(::Proto) + static_cast<uint64_t>(sizes[0]) * sizeof(::Instruction) +
           static_cast<uint64_t>(sizes[1]) * sizeof(::TObject) + static_cast<uint64_t>(sizes[2]) * sizeof(::Proto*) +
           static_cast<uint64_t>(sizes[3]) * sizeof(int) + static_cast<uint64_t>(sizes[4]) * sizeof(::LocVar) +
           static_cast<uint64_t>(sizes[5]) * sizeof(::TString*);
}

// ================================================================ C function names

struct CFunctionNames {
    std::unordered_map<lua_CFunction, uint32_t> id_of;  // lookup only, never iterated
    std::vector<std::pair<std::string, lua_CFunction>> list;  // sorted by name
    std::vector<std::string> aliases;  // "name=saved name" for every other name of a listed function (diagnostics)
    lua_CFunction find(std::string_view name) const {
        const auto it = std::lower_bound(list.begin(), list.end(), name,
                                         [](const auto& e, std::string_view n) { return e.first < n; });
        return (it != list.end() && it->first == name) ? it->second : nullptr;
    }
};

void collect_cfunctions(lua_State* L, int t, const std::string& prefix, bool nested, const void* skip,
                        std::vector<std::pair<std::string, lua_CFunction>>& out) {
    lua_pushnil(L);
    while (lua_next(L, t) != 0) {
        if (lua_type(L, -2) == LUA_TSTRING) {
            const std::string key = prefix + std::string(lua_tostring(L, -2), lua_strlen(L, -2));
            if (lua_iscfunction(L, -1)) {
                out.emplace_back(key, lua_tocfunction(L, -1));
            } else if (nested && lua_type(L, -1) == LUA_TTABLE && lua_topointer(L, -1) != skip) {
                collect_cfunctions(L, lua_gettop(L), key + ".", false, skip, out);
            }
        }
        lua_pop(L, 1);
    }
}

// Calls lib.fn(args...) given as a chunk on a scratch state and returns the C function of the closure it returns.
lua_CFunction returned_cfunction(lua_State* L, const char* chunk) {
    lua_CFunction f = nullptr;
    if (luaL_loadbuffer(L, chunk, std::strlen(chunk), "=persist") == 0 && lua_pcall(L, 0, 1, 0) == 0)
        f = lua_tocfunction(L, -1);
    lua_settop(L, 0);
    return f;
}

// Built once per process from a pristine VM (every library + engine binding, math table included). A function
// reachable under several names gets the shortest (then smallest) one.
const CFunctionNames& cfunction_names() {
    static const CFunctionNames names = [] {
        std::vector<std::pair<std::string, lua_CFunction>> cand;
        LuaHost host;
        LuaVmOptions opt;
        opt.math_library = true;
        LuaVm vm(host, opt);
        lua_State* L = vm.state();
        lua_settop(L, 0);
        lua_pushvalue(L, LUA_GLOBALSINDEX);
        collect_cfunctions(L, 1, "G.", true, lua_topointer(L, 1), cand);
        lua_settop(L, 0);
        lua_pushvalue(L, LUA_REGISTRYINDEX);
        lua_pushnil(L);
        while (lua_next(L, 1) != 0) {
            if (lua_type(L, -2) == LUA_TSTRING) {
                const std::string key = "R." + std::string(lua_tostring(L, -2), lua_strlen(L, -2));
                if (lua_iscfunction(L, -1)) cand.emplace_back(key, lua_tocfunction(L, -1));
                else if (lua_type(L, -1) == LUA_TTABLE) collect_cfunctions(L, lua_gettop(L), key + ".", false, nullptr, cand);
            }
            lua_pop(L, 1);
        }
        lua_settop(L, 0);
        // Library closures created at run time (static functions of lbaselib.c / lstrlib.c).
        if (lua_CFunction f = returned_cfunction(L, "return coroutine.wrap(function() end)")) cand.emplace_back("~coroutine.wrap", f);
        if (lua_CFunction f = returned_cfunction(L, "return string.gfind('', '')")) cand.emplace_back("~string.gfind", f);
        for (const auto list : {runtime_cfunctions_bindings(), runtime_cfunctions_presentation()})
            for (const NamedCFunction& e : list) cand.emplace_back(e.name, e.fn);
        std::sort(cand.begin(), cand.end(), [](const auto& a, const auto& b) {
            return a.first.size() != b.first.size() ? a.first.size() < b.first.size() : a.first < b.first;
        });
        CFunctionNames n;
        std::unordered_map<lua_CFunction, std::string> first;  // lookup only
        for (const auto& [name, fn] : cand) {
            if (!fn) continue;
            if (const auto it = first.find(fn); it != first.end()) {
                n.aliases.push_back(name + "=" + it->second);
                continue;
            }
            first.emplace(fn, name);
            n.id_of.emplace(fn, 0u);
            n.list.emplace_back(name, fn);
        }
        std::sort(n.aliases.begin(), n.aliases.end());
        std::sort(n.list.begin(), n.list.end());
        for (uint32_t i = 0; i < n.list.size(); ++i) n.id_of[n.list[i].second] = i;
        return n;
    }();
    return names;
}

// A string of the heap by content, WITHOUT creating it (luaS_newlstr's lookup). Null when absent.
const ::TString* find_string(lua_State* L, std::string_view s) {
    ::lu_hash h = static_cast<::lu_hash>(s.size());
    const size_t step = (s.size() >> 5) + 1;
    for (size_t l1 = s.size(); l1 >= step; l1 -= step)
        h = h ^ ((h << 5) + (h >> 2) + static_cast<::lu_hash>(static_cast<unsigned char>(s[l1 - 1])));
    const ::stringtable& tb = G(L)->strt;
    for (::GCObject* o = tb.hash[h & static_cast<::lu_hash>(tb.size - 1)]; o != nullptr; o = o->gch.next) {
        const ::TString* ts = gcotots(o);
        if (ts->tsv.len == s.size() && std::memcmp(s.data(), getstr(ts), s.size()) == 0) return ts;
    }
    return nullptr;
}

// The registry table at string key `key` (null when absent or not a table), without allocating.
const ::Table* registry_table(lua_State* L, const char* key) {
    const ::TString* k = find_string(L, key);
    if (!k) return nullptr;
    const ::TObject* v = luaH_getstr(hvalue(registry(L)), const_cast<::TString*>(k));
    return ttistable(v) ? hvalue(v) : nullptr;
}

bool has_gc_metamethod(const global_State* g, const ::Table* mt) {
    if (!mt || (mt->flags & (1u << TM_GC))) return false;  // fasttm: the absence cache says no
    const ::TObject* tm = luaH_getstr(const_cast<::Table*>(mt), g->tmname[TM_GC]);
    return !ttisnil(tm);
}

// ================================================================ write

struct Saver {
    lua_State* L = nullptr;
    global_State* g = nullptr;
    const void* vm_self = nullptr;
    const ::Table* engine_meta = nullptr;
    const ::Table* reg = nullptr;
    std::vector<::GCObject*> obj;
    std::vector<uint8_t> kind;
    std::vector<uint8_t> full;
    std::vector<uint32_t> owner;  // open upvalue -> its thread's index
    std::unordered_map<const void*, uint32_t> index;      // object -> index (lookup only)
    std::vector<const ::TString*> strs;                   // sorted by content
    std::unordered_map<const void*, uint32_t> str_index;  // lookup only
    std::vector<uint32_t> cfns;                           // name ids used, first-use order
    std::unordered_map<uint32_t, uint32_t> cfn_local;     // name id -> index in cfns
    std::vector<uint32_t> work;
    std::string err;

    bool fail(std::string why) {
        if (err.empty()) err = std::move(why);
        return false;
    }
    void add(::GCObject* o, uint8_t k, uint32_t own = kNoIndex) {
        index.emplace(o, static_cast<uint32_t>(obj.size()));
        obj.push_back(o);
        kind.push_back(k);
        full.push_back(0);
        owner.push_back(own);
    }
    uint32_t find(const void* p) const {
        const auto it = index.find(p);
        return it == index.end() ? kNoIndex : it->second;
    }
    static uint8_t value_kind(int tt, const ::GCObject* o) {
        switch (tt) {
        case LUA_TTABLE: return kTable;
        case LUA_TFUNCTION: return o->cl.c.isC ? kCClosure : kLuaClosure;
        case LUA_TUSERDATA: return kUserdata;
        case LUA_TTHREAD: return kThread;
        default: return 0;
        }
    }

    bool enumerate() {
        add(valtogco(g->mainthread), kThread);
        for (::GCObject* o = g->rootgc; o != nullptr; o = o->gch.next) {
            switch (o->gch.tt) {
            case LUA_TTABLE: add(o, kTable); break;
            case LUA_TFUNCTION: add(o, o->cl.c.isC ? kCClosure : kLuaClosure); break;
            case LUA_TPROTO: add(o, kProto); break;
            case LUA_TUPVAL: add(o, kUpvalClosed); break;
            case LUA_TTHREAD: add(o, kThread); break;
            default: return fail("unknown object type in the collector list");
            }
        }
        for (::GCObject* o = g->rootudata; o != nullptr; o = o->gch.next) {
            if (o->gch.tt != LUA_TUSERDATA) return fail("unknown object type in the userdata list");
            add(o, kUserdata);
        }
        if (g->tmudata) return fail("userdata are waiting for their __gc finalizers");
        const size_t nobj = obj.size();
        for (size_t i = 0; i < nobj; ++i) {
            if (kind[i] != kThread) continue;
            for (::GCObject* u = gcototh(obj[i])->openupval; u != nullptr; u = u->gch.next) {
                if (u->gch.tt != LUA_TUPVAL) return fail("bad open upvalue list");
                add(u, kUpvalOpen, static_cast<uint32_t>(i));
            }
        }
        // Strings: all of them (fixed, live and garbage), sorted by content - canonical whatever the hash layout.
        for (int i = 0; i < g->strt.size; ++i)
            for (::GCObject* o = g->strt.hash[i]; o != nullptr; o = o->gch.next) strs.push_back(gcotots(o));
        if (strs.size() != static_cast<size_t>(g->strt.nuse)) return fail("string table count mismatch");
        std::sort(strs.begin(), strs.end(), [](const ::TString* a, const ::TString* b) {
            return std::string_view(getstr(a), a->tsv.len) < std::string_view(getstr(b), b->tsv.len);
        });
        for (uint32_t i = 0; i < strs.size(); ++i) str_index.emplace(strs[i], i);
        return true;
    }

    // ---- reachability (full objects) ----
    bool reach_obj(const void* p, uint8_t want) {
        const uint32_t i = find(p);
        if (i == kNoIndex) return fail("a reference to an object outside the heap lists");
        if (want && kind[i] != want && !(want == kLuaClosure && kind[i] == kCClosure)) return fail("an object of an unexpected type");
        if (!full[i]) {
            full[i] = 1;
            work.push_back(i);
        }
        return true;
    }
    bool reach(const ::TObject* v) {
        switch (ttype(v)) {
        case LUA_TTABLE: return reach_obj(gcvalue(v), kTable);
        case LUA_TFUNCTION: return reach_obj(gcvalue(v), kLuaClosure);  // either closure kind
        case LUA_TUSERDATA: return reach_obj(gcvalue(v), kUserdata);
        case LUA_TTHREAD: return reach_obj(gcvalue(v), kThread);
        default: return true;  // strings are always saved; the rest has no references
        }
    }
    bool propagate() {
        while (!work.empty()) {
            const uint32_t i = work.back();
            work.pop_back();
            ::GCObject* o = obj[i];
            switch (kind[i]) {
            case kTable: {
                const ::Table* t = gcotoh(o);
                if (!reach_obj(t->metatable, kTable)) return false;
                for (int k = 0; k < t->sizearray; ++k)
                    if (!reach(&t->array[k])) return false;
                if (t->lsizenode)
                    for (int k = 0; k < sizenode(t); ++k) {
                        const ::Node* n = gnode(t, k);
                        if (ttisnil(gval(n))) continue;  // a dead key may dangle: never followed
                        if (!reach(gkey(n)) || !reach(gval(n))) return false;
                    }
                break;
            }
            case kLuaClosure: {
                const ::LClosure& c = o->cl.l;
                if (!reach(&c.g) || !reach_obj(c.p, kProto)) return false;
                for (int k = 0; k < c.nupvalues; ++k)
                    if (!c.upvals[k] || !reach_obj(c.upvals[k], 0)) return fail(err.empty() ? "a closure without its upvalue" : err);
                break;
            }
            case kCClosure: {
                const ::CClosure& c = o->cl.c;
                for (int k = 0; k < c.nupvalues; ++k)
                    if (!reach(&c.upvalue[k])) return false;
                break;
            }
            case kUserdata:
                if (!reach_obj(gcotou(o)->uv.metatable, kTable)) return false;
                break;
            case kThread: {
                const lua_State* th = gcototh(o);
                if (!reach(gt(th))) return false;
                for (const ::TObject* s = th->stack; s < th->top; ++s)
                    if (!reach(s)) return false;
                for (::GCObject* u = th->openupval; u != nullptr; u = u->gch.next)
                    if (!reach_obj(u, kUpvalOpen)) return false;
                break;
            }
            case kProto: {
                const ::Proto* p = gcotop(o);
                for (int k = 0; k < p->sizek; ++k)
                    if (!reach(&p->k[k])) return false;
                for (int k = 0; k < p->sizep; ++k)
                    if (!reach_obj(p->p[k], kProto)) return false;
                break;
            }
            case kUpvalClosed:
                if (!reach(&gcotouv(o)->value)) return false;
                break;
            case kUpvalOpen: {
                // The value lives in the owning thread's stack: that thread is saved in full (even when it is
                // itself garbage - the collector then closes the upvalue when it frees the thread).
                const uint32_t t = owner[i];
                if (!full[t]) {
                    full[t] = 1;
                    work.push_back(t);
                }
                break;
            }
            default: return fail("bad object kind");
            }
        }
        return true;
    }
    bool mark_full() {
        full[0] = 1;
        work.push_back(0);
        if (!reach(registry(L)) || !reach(defaultmeta(L)) || !propagate()) return false;
        // Userdata the next collection hands to their __gc finalizer: the finalizer runs with them, so they and
        // everything they reach are saved in full.
        for (uint32_t i = 0; i < obj.size(); ++i) {
            if (kind[i] != kUserdata || full[i]) continue;
            const ::Udata* u = gcotou(obj[i]);
            if ((u->uv.marked & (1u << 1)) && has_gc_metamethod(g, u->uv.metatable)) {
                full[i] = 1;
                work.push_back(i);
                if (!propagate()) return false;
            }
        }
        return true;
    }

    // ---- serialization ----
    bool w_value(ByteWriter& w, const ::TObject* v, bool registry_value) {
        switch (ttype(v)) {
        case LUA_TNIL: w.u8(vNil); return true;
        case LUA_TBOOLEAN: w.u8(vBool).u32(static_cast<uint32_t>(bvalue(v))); return true;
        case LUA_TLIGHTUSERDATA:
            if (pvalue(v) == vm_self) {
                w.u8(vLight).u8(kLightVm);
                return true;
            }
            if (registry_value) {
                w.u8(vLight).u8(kLightAttachment);
                return true;
            }
            return fail("a light userdata outside the registry attachments");
        case LUA_TNUMBER: w.u8(vNumber).u64(std::bit_cast<uint64_t>(nvalue(v))); return true;
        case LUA_TSTRING: {
            const auto it = str_index.find(tsvalue(v));
            if (it == str_index.end()) return fail("a string outside the string table");
            w.u8(vString).u32(it->second);
            return true;
        }
        case LUA_TTABLE:
        case LUA_TFUNCTION:
        case LUA_TUSERDATA:
        case LUA_TTHREAD: {
            const uint32_t i = find(gcvalue(v));
            if (i == kNoIndex || !full[i] || value_kind(ttype(v), gcvalue(v)) != kind[i])
                return fail("a reference to an unsaved object");
            const uint8_t tag = ttype(v) == LUA_TTABLE      ? vTable
                                : ttype(v) == LUA_TFUNCTION ? vFunction
                                : ttype(v) == LUA_TUSERDATA ? vUserdata
                                                            : vThread;
            w.u8(tag).u32(i);
            return true;
        }
        default: return fail("a value of an unknown type");
        }
    }
    // The key of a node whose value is nil: it may be a collected object (dangling) - looked up, never read.
    void w_dead_key(ByteWriter& w, const ::TObject* k) {
        switch (ttype(k)) {
        case LUA_TNIL: w.u8(vNil); return;
        case LUA_TBOOLEAN: w.u8(vBool).u32(static_cast<uint32_t>(bvalue(k))); return;
        case LUA_TNUMBER: w.u8(vNumber).u64(std::bit_cast<uint64_t>(nvalue(k))); return;
        case LUA_TSTRING: {
            const auto it = str_index.find(k->value.gc);
            if (it != str_index.end()) w.u8(vString).u32(it->second);
            else w.u8(vDeadKey);
            return;
        }
        case LUA_TTABLE:
        case LUA_TFUNCTION:
        case LUA_TUSERDATA:
        case LUA_TTHREAD: {
            const uint32_t i = find(k->value.gc);
            const uint8_t want = static_cast<uint8_t>(ttype(k) == LUA_TTABLE      ? kTable
                                                      : ttype(k) == LUA_TUSERDATA ? kUserdata
                                                      : ttype(k) == LUA_TTHREAD   ? kThread
                                                                                  : 0);
            const bool same = i != kNoIndex && full[i] &&
                              (want ? kind[i] == want : (kind[i] == kLuaClosure || kind[i] == kCClosure));
            if (same) {
                const uint8_t tag = ttype(k) == LUA_TTABLE      ? vTable
                                    : ttype(k) == LUA_TFUNCTION ? vFunction
                                    : ttype(k) == LUA_TUSERDATA ? vUserdata
                                                                : vThread;
                w.u8(tag).u32(i);
            } else {
                w.u8(vDeadKey);
            }
            return;
        }
        default: w.u8(vDeadKey); return;  // LUA_TNONE (cleared weak key), light userdata
        }
    }
    uint32_t w_string_ref(const ::TString* s) {
        if (!s) return kNoIndex;
        const auto it = str_index.find(s);
        return it == str_index.end() ? kNoIndex - 1 : it->second;  // kNoIndex - 1: caught below
    }
    bool w_str(ByteWriter& w, const ::TString* s) {
        const uint32_t i = w_string_ref(s);
        if (i == kNoIndex - 1) return fail("a prototype string outside the string table");
        w.u32(i);
        return true;
    }

    bool w_table(ByteWriter& w, uint32_t i) {
        const ::Table* t = gcotoh(obj[i]);
        if (!full[i]) {
            w.u8(t->lsizenode).i32(t->sizearray);
            return true;
        }
        const uint32_t mt = find(t->metatable);
        if (mt == kNoIndex || !full[mt] || kind[mt] != kTable) return fail("a table metatable outside the heap");
        w.u8(t->flags).u8(t->lsizenode).i32(t->sizearray).u32(mt);
        for (int k = 0; k < t->sizearray; ++k)
            if (!w_value(w, &t->array[k], false)) return false;
        if (t->lsizenode == 0) {
            if (t->node != g->dummynode) return fail("an empty hash part that is not the dummy node");
            return true;
        }
        const bool is_reg = t == reg;
        for (int k = 0; k < sizenode(t); ++k) {
            const ::Node* n = gnode(t, k);
            // A light userdata key hashes by its address, which a restored VM does not keep.
            if (ttislightuserdata(gkey(n))) return fail("a light userdata table key");
            if (ttisnil(gval(n))) {
                w_dead_key(w, gkey(n));
                w.u8(vNil);
            } else {
                if (!w_value(w, gkey(n), false)) return false;
                if (ttislightuserdata(gval(n)) && pvalue(gval(n)) != vm_self && !(is_reg && ttisstring(gkey(n))))
                    return fail("a light userdata outside the registry attachments");
                if (!w_value(w, gval(n), is_reg)) return false;
            }
            w.i32(n->next ? static_cast<int32_t>(n->next - t->node) : -1);
        }
        w.i32(static_cast<int32_t>(t->firstfree - t->node));
        return true;
    }

    bool w_udata(ByteWriter& w, uint32_t i) {
        const ::Udata* u = gcotou(obj[i]);
        const size_t len = u->uv.len;
        if (!full[i]) {
            w.u64(len);
            return true;
        }
        const uint32_t mt = find(u->uv.metatable);
        if (mt == kNoIndex || !full[mt] || kind[mt] != kTable) return fail("a userdata metatable outside the heap");
        w.u32(mt).u64(len);
        const auto* payload = reinterpret_cast<const uint8_t*>(u + 1);
        if (u->uv.metatable == engine_meta && len == sizeof(Ud)) {
            Ud ud;
            std::memcpy(&ud, payload, sizeof ud);
            const auto& classes = class_names();
            const auto table = binding_table();
            if (ud.cls < classes.size() && (ud.binding == kNoBinding || ud.binding < table.size())) {
                w.u8(kUdEngine);
                w.str(classes[ud.cls]);
                w.str(ud.binding == kNoBinding ? std::string_view() : std::string_view(table[ud.binding].name));
                w.u64(ud.value).i32(ud.id).i32(ud.reserved);
                return true;
            }
        }
        w.u8(kUdRaw);
        w.raw(std::span<const uint8_t>(payload, len));
        return true;
    }

    bool w_thread(ByteWriter& w, uint32_t i) {
        const lua_State* th = gcototh(obj[i]);
        if (!full[i]) {
            w.i32(th->stacksize).u32(th->size_ci);
            return true;
        }
        if (th->errorJmp || th->nCcalls || th->hook || th->hookmask || th->errfunc)
            return fail("a thread inside a call, a protected call or with a debug hook");
        const int32_t nci = static_cast<int32_t>(th->ci - th->base_ci) + 1;
        w.i32(th->stacksize).u32(th->size_ci);
        w.i32(static_cast<int32_t>(th->top - th->stack)).i32(static_cast<int32_t>(th->stack_last - th->stack));
        if (th->base != th->ci->base) return fail("a thread whose base is not its frame's");
        w.u8(th->allowhook).i32(th->basehookcount).i32(th->hookcount);
        if (!w_value(w, gt(th), false)) return false;
        w.i32(nci);
        for (const ::CallInfo* ci = th->base_ci; ci <= th->ci; ++ci) {
            if (ci->state & CI_HASFRAME) return fail("a thread with a running Lua frame (write() called from a script)");
            w.i32(static_cast<int32_t>(ci->base - th->stack)).i32(static_cast<int32_t>(ci->top - th->stack)).i32(ci->state);
            if (ci->state & CI_C) {
                w.i32(-1).i32(0);
            } else {
                const ::TObject* f = ci->base - 1;
                if (!ttisfunction(f) || clvalue(f)->c.isC) return fail("a Lua frame without its Lua closure");
                const ::Proto* p = clvalue(f)->l.p;
                w.i32(static_cast<int32_t>(ci->u.l.savedpc - p->code)).i32(ci->u.l.tailcalls);
            }
        }
        for (const ::TObject* s = th->stack; s < th->top; ++s)
            if (!w_value(w, s, false)) return false;
        return true;
    }

    bool w_proto(ByteWriter& w, uint32_t i) {
        const ::Proto* p = gcotop(obj[i]);
        if (!full[i]) {
            w.i32(p->sizecode).i32(p->sizek).i32(p->sizep).i32(p->sizelineinfo).i32(p->sizelocvars).i32(p->sizeupvalues);
            return true;
        }
        if (!w_str(w, p->source)) return false;
        w.i32(p->lineDefined).u8(p->nups).u8(p->numparams).u8(p->is_vararg).u8(p->maxstacksize);
        w.i32(p->sizecode);
        for (int k = 0; k < p->sizecode; ++k) w.u32(static_cast<uint32_t>(p->code[k]));
        w.i32(p->sizek);
        for (int k = 0; k < p->sizek; ++k) {
            const int tt = ttype(&p->k[k]);
            if (tt != LUA_TNIL && tt != LUA_TBOOLEAN && tt != LUA_TNUMBER && tt != LUA_TSTRING)
                return fail("a prototype constant that is not nil / boolean / number / string");
            if (!w_value(w, &p->k[k], false)) return false;
        }
        w.i32(p->sizep);
        for (int k = 0; k < p->sizep; ++k) {
            const uint32_t c = find(p->p[k]);
            if (c == kNoIndex || !full[c] || kind[c] != kProto) return fail("a nested prototype outside the heap");
            w.u32(c);
        }
        w.i32(p->sizelineinfo);
        for (int k = 0; k < p->sizelineinfo; ++k) w.i32(p->lineinfo[k]);
        w.i32(p->sizelocvars);
        for (int k = 0; k < p->sizelocvars; ++k) {
            if (!w_str(w, p->locvars[k].varname)) return false;
            w.i32(p->locvars[k].startpc).i32(p->locvars[k].endpc);
        }
        w.i32(p->sizeupvalues);
        for (int k = 0; k < p->sizeupvalues; ++k)
            if (!w_str(w, p->upvalues[k])) return false;
        return true;
    }

    bool w_object(ByteWriter& w, uint32_t i) {
        const ::GCObject* o = obj[i];
        w.u8(kind[i]).u8(full[i]);
        if (kind[i] == kUpvalOpen) {
            const ::UpVal* u = gcotouv(o);
            const lua_State* th = gcototh(obj[owner[i]]);
            if (u->v < th->stack || u->v >= th->stack + th->stacksize) return fail("an open upvalue outside its stack");
            if (full[owner[i]] && u->v >= th->top) return fail("an open upvalue above its thread's top");
            w.u32(owner[i]).i32(static_cast<int32_t>(u->v - th->stack));
            return true;
        }
        w.u32(o->gch.alamo_serial).u8(o->gch.marked);
        switch (kind[i]) {
        case kTable: return w_table(w, i);
        case kLuaClosure: {
            const ::LClosure& c = o->cl.l;
            w.u8(c.nupvalues);
            if (!full[i]) return true;
            const uint32_t p = find(c.p);
            if (p == kNoIndex || !full[p] || kind[p] != kProto) return fail("a closure prototype outside the heap");
            w.u32(p);
            if (!w_value(w, &c.g, false)) return false;
            for (int k = 0; k < c.nupvalues; ++k) {
                const uint32_t u = find(c.upvals[k]);
                if (u == kNoIndex || !full[u] || (kind[u] != kUpvalOpen && kind[u] != kUpvalClosed))
                    return fail("a closure upvalue outside the heap");
                w.u32(u);
            }
            return true;
        }
        case kCClosure: {
            const ::CClosure& c = o->cl.c;
            w.u8(c.nupvalues);
            if (!full[i]) return true;
            const CFunctionNames& names = cfunction_names();
            const auto it = names.id_of.find(c.f);
            if (it == names.id_of.end()) return fail("a C function that is not in the persistence name table");
            const auto [pos, added] = cfn_local.emplace(it->second, static_cast<uint32_t>(cfns.size()));
            if (added) cfns.push_back(it->second);
            w.u32(pos->second);
            for (int k = 0; k < c.nupvalues; ++k)
                if (!w_value(w, &c.upvalue[k], false)) return false;
            return true;
        }
        case kUserdata: return w_udata(w, i);
        case kThread: return w_thread(w, i);
        case kProto: return w_proto(w, i);
        case kUpvalClosed:
            if (!full[i]) return true;
            return w_value(w, &gcotouv(o)->value, false);
        default: return fail("bad object kind");
        }
    }
};

// ================================================================ read: parsed image

struct PVal {
    uint8_t tag = vNil;
    uint32_t ref = 0;
    uint64_t bits = 0;
};
struct PNode {
    PVal key, val;
    int32_t next = -1;
};
struct PCi {
    int32_t base = 0, top = 0, state = 0, savedpc = -1, tailcalls = 0;
};
struct PObj {
    uint8_t kind = 0;
    bool full = false;
    uint8_t marked = 0;
    uint32_t serial = 0;
    // table
    uint8_t flags = 0, lsizenode = 0;
    int32_t sizearray = 0;
    uint32_t meta = kNoIndex;
    std::vector<PVal> vals;  // table array part / C closure upvalues / thread stack / proto constants
    std::vector<PNode> nodes;
    int32_t firstfree = 0;
    // closures
    uint8_t nup = 0;
    uint32_t proto = kNoIndex;
    PVal env;  // Lua closure env / thread globals / closed upvalue value
    std::vector<uint32_t> refs;  // Lua closure upvalues / proto nested protos / thread open upvalues
    lua_CFunction fn = nullptr;
    // userdata
    uint64_t len = 0;
    std::vector<uint8_t> bytes;
    // thread
    int32_t stacksize = 0, top = 0, stack_last = 0, basehookcount = 0, hookcount = 0;
    uint32_t size_ci = 0;
    uint8_t allowhook = 1;
    std::vector<PCi> cis;
    // proto
    int32_t psize[6] = {0, 0, 0, 0, 0, 0};  // code, k, p, lineinfo, locvars, upvalues
    uint32_t source = kNoIndex;
    int32_t line = 0;
    uint8_t nups = 0, numparams = 0, is_vararg = 0, maxstack = 0;
    std::vector<uint32_t> code;
    std::vector<int32_t> lineinfo;
    std::vector<int32_t> locvars;  // name, startpc, endpc triples (name as uint32 bits)
    std::vector<uint32_t> upnames;
    // open upvalue
    uint32_t thread = kNoIndex;
    int32_t level = 0;
};

struct Image {
    std::vector<std::string> strings;
    std::vector<lua_CFunction> cfns;
    std::vector<PObj> obj;
    uint32_t registry = kNoIndex, defaultmeta = kNoIndex;
    uint32_t serial = 0;
    uint64_t nblocks = 0, threshold = 0, buffsize = 0;
    int32_t strt_size = 0;
};

struct Loader {
    ByteReader& r;
    std::string& err;
    bool fail(const char* why) {
        if (err.empty()) err = why;
        r.fail();
        return false;
    }
    // A count of records of at least `min_bytes` each that must fit in the rest of the data.
    bool count(uint32_t& n, size_t min_bytes) {
        n = r.u32();
        if (!r.ok()) return fail("truncated");
        if (min_bytes && n > r.remaining() / min_bytes) return fail("a count past the end of the data");
        return true;
    }
    bool size(int32_t& n, size_t min_bytes) {
        n = r.i32();
        if (!r.ok()) return fail("truncated");
        if (n < 0 || (min_bytes && static_cast<uint32_t>(n) > r.remaining() / min_bytes)) return fail("a bad size");
        return true;
    }
    bool value(PVal& v) {
        v.tag = r.u8();
        switch (v.tag) {
        case vNil:
        case vDeadKey: break;
        case vBool: v.bits = r.u32(); break;
        case vLight:
            v.bits = r.u8();
            if (v.bits > kLightAttachment) return fail("a bad light userdata kind");
            break;
        case vNumber: v.bits = r.u64(); break;
        case vString:
        case vTable:
        case vFunction:
        case vUserdata:
        case vThread: v.ref = r.u32(); break;
        default: return fail("a bad value tag");
        }
        return r.ok() || fail("truncated");
    }
    bool values(std::vector<PVal>& out, size_t n) {
        out.resize(n);
        for (PVal& v : out)
            if (!value(v)) return false;
        return true;
    }

    bool object(PObj& o, size_t ncfn) {
        o.kind = r.u8();
        const uint8_t f = r.u8();
        if (!r.ok()) return fail("truncated");
        if (f > 1) return fail("a bad full flag");
        o.full = f == 1;
        if (o.kind == kUpvalOpen) {
            o.thread = r.u32();
            o.level = r.i32();
            return r.ok() || fail("truncated");
        }
        o.serial = r.u32();
        o.marked = r.u8();
        switch (o.kind) {
        case kTable: {
            if (o.full) o.flags = r.u8();
            o.lsizenode = r.u8();
            o.sizearray = r.i32();
            if (!r.ok()) return fail("truncated");
            if (o.lsizenode > kMaxTableBits || o.sizearray < 0 || o.sizearray > (1 << kMaxTableBits)) return fail("a bad table size");
            if (!o.full) return true;
            o.meta = r.u32();
            if (static_cast<uint64_t>(o.sizearray) > r.remaining() || !values(o.vals, static_cast<size_t>(o.sizearray))) return fail("truncated");
            if (o.lsizenode) {
                const size_t n = size_t{1} << o.lsizenode;
                if (n > r.remaining() / 6) return fail("truncated");
                o.nodes.resize(n);
                for (PNode& nd : o.nodes) {
                    if (!value(nd.key) || !value(nd.val)) return false;
                    nd.next = r.i32();
                }
                o.firstfree = r.i32();
            }
            return r.ok() || fail("truncated");
        }
        case kLuaClosure:
        case kCClosure: {
            o.nup = r.u8();
            if (!r.ok()) return fail("truncated");
            if (!o.full) return true;
            if (o.kind == kLuaClosure) {
                o.proto = r.u32();
                if (!value(o.env)) return false;
                if (static_cast<size_t>(o.nup) > r.remaining() / 4) return fail("truncated");
                o.refs.resize(o.nup);
                for (uint32_t& u : o.refs) u = r.u32();
            } else {
                const uint32_t id = r.u32();
                if (!r.ok()) return fail("truncated");
                if (id >= ncfn) return fail("a bad C function index");
                o.refs.push_back(id);
                if (!values(o.vals, o.nup)) return false;
            }
            return r.ok() || fail("truncated");
        }
        case kUserdata: {
            if (o.full) o.meta = r.u32();
            o.len = r.u64();
            if (!r.ok()) return fail("truncated");
            if (o.len > kMaxHeapBytes) return fail("a bad userdata size");
            if (!o.full) return true;
            const uint8_t enc = r.u8();
            if (enc == kUdRaw) {
                if (o.len > r.remaining()) return fail("truncated");
                const auto b = r.raw(static_cast<size_t>(o.len));
                o.bytes.assign(b.begin(), b.end());
            } else if (enc == kUdEngine) {
                if (o.len != sizeof(Ud)) return fail("an engine userdata of a bad size");
                const std::string cls = r.str();
                const std::string binding = r.str();
                Ud ud;
                ud.value = r.u64();
                ud.id = r.i32();
                ud.reserved = r.i32();
                if (!r.ok()) return fail("truncated");
                ud.cls = class_id(cls);
                if (ud.cls == kNoClass) return fail("an engine userdata of an unknown class");
                ud.binding = binding.empty() ? kNoBinding : global_row(binding);
                if (!binding.empty() && ud.binding == kNoBinding) return fail("an engine userdata of an unknown binding");
                o.bytes.resize(sizeof ud);
                std::memcpy(o.bytes.data(), &ud, sizeof ud);
            } else {
                return fail("a bad userdata encoding");
            }
            return r.ok() || fail("truncated");
        }
        case kThread: {
            o.stacksize = r.i32();
            o.size_ci = r.u32();
            if (!r.ok()) return fail("truncated");
            if (o.stacksize < EXTRA_STACK + 1 || o.stacksize > kMaxStack || o.size_ci < 1 || o.size_ci > 0xFFFFu)
                return fail("a bad thread stack size");
            if (!o.full) return true;
            o.top = r.i32();
            o.stack_last = r.i32();
            o.allowhook = r.u8();
            o.basehookcount = r.i32();
            o.hookcount = r.i32();
            if (!value(o.env)) return false;
            int32_t nci = 0;
            if (!size(nci, 20)) return false;
            o.cis.resize(static_cast<size_t>(nci));
            for (PCi& c : o.cis) {
                c.base = r.i32();
                c.top = r.i32();
                c.state = r.i32();
                c.savedpc = r.i32();
                c.tailcalls = r.i32();
            }
            if (!r.ok()) return fail("truncated");
            if (o.top < 1 || o.top > o.stacksize || static_cast<uint32_t>(o.top) > r.remaining()) return fail("a bad thread top");
            return values(o.vals, static_cast<size_t>(o.top));
        }
        case kProto: {
            if (!o.full) {
                for (int32_t& s : o.psize) s = r.i32();
                if (!r.ok()) return fail("truncated");
                for (const int32_t s : o.psize)
                    if (s < 0 || s > (1 << 26)) return fail("a bad prototype size");
                return true;
            }
            o.source = r.u32();
            o.line = r.i32();
            o.nups = r.u8();
            o.numparams = r.u8();
            o.is_vararg = r.u8();
            o.maxstack = r.u8();
            if (!size(o.psize[0], 4)) return false;
            o.code.resize(static_cast<size_t>(o.psize[0]));
            for (uint32_t& c : o.code) c = r.u32();
            if (!size(o.psize[1], 1) || !values(o.vals, static_cast<size_t>(o.psize[1]))) return false;
            if (!size(o.psize[2], 4)) return false;
            o.refs.resize(static_cast<size_t>(o.psize[2]));
            for (uint32_t& c : o.refs) c = r.u32();
            if (!size(o.psize[3], 4)) return false;
            o.lineinfo.resize(static_cast<size_t>(o.psize[3]));
            for (int32_t& c : o.lineinfo) c = r.i32();
            if (!size(o.psize[4], 12)) return false;
            o.locvars.resize(static_cast<size_t>(o.psize[4]) * 3);
            for (int32_t& c : o.locvars) c = r.i32();
            if (!size(o.psize[5], 4)) return false;
            o.upnames.resize(static_cast<size_t>(o.psize[5]));
            for (uint32_t& c : o.upnames) c = r.u32();
            return r.ok() || fail("truncated");
        }
        case kUpvalClosed:
            if (!o.full) return true;
            return value(o.env);
        default: return fail("a bad object kind");
        }
    }

    bool image(Image& im) {
        if (r.u32() != kHeapFormat || !r.ok()) return fail("unknown heap format");
        uint32_t n = 0;
        if (!count(n, 4)) return false;
        const CFunctionNames& names = cfunction_names();
        for (uint32_t i = 0; i < n; ++i) {
            const std::string name = r.str();
            if (!r.ok()) return fail("truncated");
            lua_CFunction f = names.find(name);
            if (!f) return fail("a C function this build does not have");
            im.cfns.push_back(f);
        }
        if (!count(n, 4)) return false;
        im.strings.reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
            im.strings.push_back(r.str());
            if (!r.ok()) return fail("truncated");
            if (i > 0 && !(im.strings[i - 1] < im.strings[i])) return fail("strings not in canonical order");
        }
        if (!count(n, 2)) return false;
        if (n == 0) return fail("no main thread");
        im.obj.resize(n);
        for (PObj& o : im.obj)
            if (!object(o, im.cfns.size())) return false;
        im.registry = r.u32();
        im.defaultmeta = r.u32();
        im.serial = r.u32();
        im.nblocks = r.u64();
        im.threshold = r.u64();
        im.strt_size = r.i32();
        im.buffsize = r.u64();
        if (!r.ok()) return fail("truncated");
        if (!r.done()) return fail("trailing bytes");
        return true;
    }
};

// ================================================================ read: validation

struct Validator {
    const Image& im;
    std::string& err;
    bool fail(const char* why) {
        if (err.empty()) err = why;
        return false;
    }
    bool is(uint32_t i, uint8_t k) const { return i < im.obj.size() && im.obj[i].kind == k && im.obj[i].full; }
    bool is_closure(uint32_t i) const { return is(i, kLuaClosure) || is(i, kCClosure); }
    // A value in a full object: references must be full objects of the right kind (husks are unreachable).
    bool value(const PVal& v, bool attachment_ok = false) {
        switch (v.tag) {
        case vNil:
        case vBool:
        case vNumber: return true;
        case vLight: return v.bits == kLightVm || attachment_ok || fail("a misplaced registry attachment");
        case vString: return v.ref < im.strings.size() || fail("a bad string index");
        case vTable: return is(v.ref, kTable) || fail("a bad table reference");
        case vFunction: return is_closure(v.ref) || fail("a bad function reference");
        case vUserdata: return is(v.ref, kUserdata) || fail("a bad userdata reference");
        case vThread: return is(v.ref, kThread) || fail("a bad thread reference");
        default: return fail("a misplaced dead key");
        }
    }
    bool str_ref(uint32_t s) { return s == kNoIndex || s < im.strings.size() || fail("a bad prototype string"); }

    bool table(const PObj& o, bool is_registry) {
        if (!is(o.meta, kTable)) return fail("a bad table metatable");
        for (const PVal& v : o.vals)
            if (!value(v)) return false;
        if (!o.lsizenode) return true;
        const int32_t n = static_cast<int32_t>(o.nodes.size());
        if (o.firstfree < 0 || o.firstfree >= n) return fail("a bad firstfree");
        std::vector<uint8_t> in(o.nodes.size(), 0);
        for (const PNode& nd : o.nodes) {
            if (nd.val.tag == vNil) {
                if (nd.key.tag == vLight) return fail("a light userdata key");
                if (nd.key.tag != vDeadKey && !value(nd.key)) return false;
            } else {
                if (nd.key.tag == vNil || nd.key.tag == vDeadKey || nd.key.tag == vLight) return fail("a bad table key");
                if (!value(nd.key)) return false;
                const bool attachment = nd.val.tag == vLight && nd.val.bits == kLightAttachment;
                if (attachment && !(is_registry && nd.key.tag == vString)) return fail("a misplaced registry attachment");
                if (!value(nd.val, attachment)) return false;
            }
            if (nd.next < -1 || nd.next >= n) return fail("a bad node chain");
            if (nd.next >= 0 && ++in[static_cast<size_t>(nd.next)] > 1) return fail("a node chain that merges");
        }
        // Chains are lists: walking from every head (in-degree 0) must visit every node with a successor or a
        // predecessor; anything left is a cycle.
        std::vector<uint8_t> seen(o.nodes.size(), 0);
        for (int32_t h = 0; h < n; ++h) {
            if (in[static_cast<size_t>(h)]) continue;
            for (int32_t k = h; k >= 0; k = o.nodes[static_cast<size_t>(k)].next) seen[static_cast<size_t>(k)] = 1;
        }
        for (size_t k = 0; k < seen.size(); ++k)
            if (!seen[k]) return fail("a node chain cycle");
        return true;
    }

    const PObj* lua_closure_at(const PObj& th, int32_t slot) const {
        if (slot < 0 || slot >= static_cast<int32_t>(th.vals.size())) return nullptr;
        const PVal& f = th.vals[static_cast<size_t>(slot)];
        if (f.tag != vFunction || !is(f.ref, kLuaClosure) || !is(im.obj[f.ref].proto, kProto)) return nullptr;
        return &im.obj[f.ref];
    }

    bool thread(const PObj& o, bool main) {
        if (!value(o.env) || o.env.tag != vTable) return fail("a thread without a globals table");
        for (const PVal& v : o.vals)
            if (!value(v)) return false;
        if (o.stack_last < 0 || o.stack_last >= o.stacksize) return fail("a bad stack_last");
        if (o.allowhook > 1) return fail("a bad allowhook");
        const size_t nci = o.cis.size();
        if (nci < 1 || nci > o.size_ci) return fail("a bad CallInfo count");
        if (main && nci != 1) return fail("a main thread inside a call");
        const PCi& b = o.cis[0];
        if (b.state != CI_C || b.base != 1 || b.top < b.base || b.top > o.stacksize) return fail("a bad base CallInfo");
        if (nci == 1) return o.top >= b.base || fail("a bad thread top");
        // Suspended in coroutine.yield: Lua frames (each calling the next with OP_CALL), then the C yield frame.
        const PCi& y = o.cis[nci - 1];
        if (y.state != (CI_C | CI_YIELD)) return fail("a thread suspended outside coroutine.yield");
        if (o.top < y.base || o.top > o.stacksize) return fail("a bad thread top");
        if (y.base < 2 || y.base > o.top || y.top < y.base || y.top > o.stacksize) return fail("a bad yield frame");
        const PVal& yf = o.vals[static_cast<size_t>(y.base - 1)];
        if (yf.tag != vFunction || !is(yf.ref, kCClosure)) return fail("a yield frame without its C function");
        for (size_t j = 1; j + 1 < nci; ++j) {
            const PCi& c = o.cis[j];
            const bool last = j + 2 == nci;
            if (c.state != (last ? CI_SAVEDPC : (CI_SAVEDPC | CI_CALLING))) return fail("a Lua frame in an unexpected state");
            if (c.base <= o.cis[j - 1].base || c.base > o.top) return fail("a bad Lua frame base");
            const PObj* cl = lua_closure_at(o, c.base - 1);
            if (!cl) return fail("a Lua frame without its Lua closure");
            const PObj& p = im.obj[cl->proto];
            if (c.top != c.base + p.maxstack || c.top > o.stacksize) return fail("a bad Lua frame top");
            if (c.tailcalls < 0) return fail("a bad tail call count");
            if (c.savedpc < 1 || c.savedpc > static_cast<int32_t>(p.code.size())) return fail("a bad saved pc");
            const ::Instruction ins = static_cast<::Instruction>(p.code[static_cast<size_t>(c.savedpc - 1)]);
            const OpCode op = GET_OPCODE(ins);
            if (op != OP_CALL && !(last && op == OP_TAILCALL)) return fail("a saved pc that is not after a call");
            // The callee's function slot lies in this frame's registers.
            const int32_t callee = o.cis[j + 1].base - 1;
            if (callee < c.base || callee >= c.top) return fail("a callee outside its caller's frame");
        }
        return true;
    }

    bool proto(const PObj& o) {
        if (!str_ref(o.source)) return false;
        if (o.code.empty()) return fail("a prototype without code");
        if (o.maxstack > MAXSTACK || o.numparams + o.is_vararg > o.maxstack || o.is_vararg > 1)
            return fail("a bad prototype frame size");
        if (o.lineinfo.size() != o.code.size() && !o.lineinfo.empty()) return fail("a bad line info size");
        for (const PVal& v : o.vals) {
            if (v.tag != vNil && v.tag != vBool && v.tag != vNumber && v.tag != vString) return fail("a bad prototype constant");
            if (!value(v)) return false;
        }
        for (const uint32_t p : o.refs)
            if (!is(p, kProto)) return fail("a bad nested prototype");
        for (size_t k = 0; k < o.locvars.size(); k += 3)
            if (!str_ref(static_cast<uint32_t>(o.locvars[k]))) return false;
        for (const uint32_t s : o.upnames)
            if (!str_ref(s)) return false;
        return true;
    }

    bool run(const std::vector<uint32_t>& thread_index, uint64_t& predicted) {
        if (!is(0, kThread)) return fail("object 0 is not the main thread");
        if (!is(im.registry, kTable) || !is(im.defaultmeta, kTable)) return fail("bad roots");
        if (im.strt_size < MINSTRTABSIZE || im.strt_size > (1 << 26) || (im.strt_size & (im.strt_size - 1)))
            return fail("a bad string table size");
        if (im.buffsize > kMaxHeapBytes) return fail("a bad buffer size");
        for (const uint32_t t : thread_index)
            if (t == 0 || !is(t, kThread)) return fail("an engine thread that is not a saved coroutine");
        // Open upvalues: each list strictly descending in level, inside its thread's stack.
        std::vector<int32_t> last_level(im.obj.size(), 0x7FFFFFFF);
        uint64_t bytes = sizeof(::lua_State) + sizeof(::global_State);
        for (const std::string& s : im.strings) bytes += sizestring(s.size());
        bytes += static_cast<uint64_t>(im.strt_size) * sizeof(::GCObject*) + im.buffsize;
        for (uint32_t i = 0; i < im.obj.size(); ++i) {
            const PObj& o = im.obj[i];
            if (o.kind == kUpvalOpen) {
                if (o.thread >= im.obj.size() || im.obj[o.thread].kind != kThread) return fail("an open upvalue without its thread");
                const PObj& th = im.obj[o.thread];
                if (o.level < 0 || o.level >= (th.full ? th.top : th.stacksize)) return fail("an open upvalue outside its stack");
                if (o.level >= last_level[o.thread]) return fail("open upvalues out of order");
                last_level[o.thread] = o.level;
                if (o.full != th.full) return fail("an open upvalue of a mismatched thread");
                bytes += sizeof(::UpVal);
                continue;
            }
            if (o.marked & 1u) return fail("an object marked mid-collection");
            switch (o.kind) {
            case kTable:
                if (o.marked & ~6u) return fail("bad table GC bits");
                if (o.full && !table(o, i == im.registry)) return false;
                bytes += table_bytes(o.lsizenode, o.sizearray);
                break;
            case kLuaClosure:
                if (o.marked) return fail("bad closure GC bits");
                if (o.full) {
                    if (!is(o.proto, kProto) || im.obj[o.proto].nups != o.nup) return fail("a closure / prototype upvalue mismatch");
                    if (!value(o.env) || o.env.tag != vTable) return fail("a closure without a globals table");
                    for (const uint32_t u : o.refs)
                        if (!is(u, kUpvalClosed) && !is(u, kUpvalOpen)) return fail("a bad closure upvalue");
                }
                bytes += lclosure_bytes(o.nup);
                break;
            case kCClosure:
                if (o.marked) return fail("bad closure GC bits");
                if (o.full)
                    for (const PVal& v : o.vals)
                        if (!value(v)) return false;
                bytes += cclosure_bytes(o.nup);
                break;
            case kUserdata:
                if (o.marked & ~2u) return fail("bad userdata GC bits");
                if (o.full && !is(o.meta, kTable)) return fail("a bad userdata metatable");
                bytes += sizeudata(o.len);
                break;
            case kThread:
                if (o.marked) return fail("bad thread GC bits");
                if (o.full && !thread(o, i == 0)) return false;
                if (!o.full && i == 0) return fail("a husk main thread");
                bytes += i == 0 ? static_cast<uint64_t>(o.stacksize) * sizeof(::TObject) + o.size_ci * sizeof(::CallInfo)
                                : thread_bytes(o.stacksize, o.size_ci);
                break;
            case kProto: {
                if (o.marked) return fail("bad prototype GC bits");
                if (o.full && !proto(o)) return false;
                const int64_t s[6] = {o.psize[0], o.psize[1], o.psize[2], o.psize[3], o.psize[4], o.psize[5]};
                bytes += proto_bytes(s);
                break;
            }
            case kUpvalClosed:
                if (o.marked) return fail("bad upvalue GC bits");
                if (o.full && !value(o.env)) return false;
                bytes += sizeof(::UpVal);
                break;
            default: return fail("a bad object kind");
            }
            if (bytes > kMaxHeapBytes) return fail("an implausible heap size");
        }
        predicted = bytes;
        if (bytes != im.nblocks) return fail("the heap does not account for its saved byte count");
        return true;
    }
};

// ================================================================ read: build

struct Builder {
    explicit Builder(const Image& image) : im(image) {}
    const Image& im;
    lua_State* L = nullptr;  // the new state
    lua_State* L_old = nullptr;
    void* vm_self = nullptr;
    std::vector<::TString*> strs;
    std::vector<::GCObject*> obj;
    ::Table* dm = nullptr;  // the restored default metatable

    void set(::TObject* out, const PVal& v, const ::TObject* attachment = nullptr) {
        switch (v.tag) {
        case vBool: setbvalue(out, static_cast<int>(v.bits)); break;
        case vLight:
            if (v.bits == kLightVm) {
                setpvalue(out, vm_self);
            } else {
                void* p = (attachment && ttislightuserdata(attachment)) ? pvalue(attachment) : nullptr;
                setpvalue(out, p);
            }
            break;
        case vNumber: setnvalue(out, std::bit_cast<double>(v.bits)); break;
        case vString: setsvalue(out, strs[v.ref]); break;
        case vTable: sethvalue(out, &obj[v.ref]->h); break;
        case vFunction: setclvalue(out, &obj[v.ref]->cl); break;
        case vUserdata: setuvalue(out, &obj[v.ref]->u); break;
        case vThread: setthvalue(out, &obj[v.ref]->th); break;
        case vDeadKey: out->tt = LUA_TNONE; out->value.gc = nullptr; break;
        default: setnilvalue(out); break;
        }
    }

    // The value the VM being read into has at registry[key] (attachments), read without allocating.
    const ::TObject* old_registry_value(const std::string& key) const {
        const ::TString* k = find_string(L_old, key);
        if (!k) return nullptr;
        return luaH_getstr(hvalue(registry(L_old)), const_cast<::TString*>(k));
    }

    void alloc_array(::Proto* p, const int32_t s[6]) {
        p->code = luaM_newvector(L, s[0], ::Instruction);
        p->sizecode = s[0];
        p->k = luaM_newvector(L, s[1], ::TObject);
        p->sizek = s[1];
        for (int k = 0; k < s[1]; ++k) setnilvalue(&p->k[k]);
        p->p = luaM_newvector(L, s[2], ::Proto*);
        p->sizep = s[2];
        for (int k = 0; k < s[2]; ++k) p->p[k] = nullptr;
        p->lineinfo = luaM_newvector(L, s[3], int);
        p->sizelineinfo = s[3];
        p->locvars = luaM_newvector(L, s[4], ::LocVar);
        p->sizelocvars = s[4];
        for (int k = 0; k < s[4]; ++k) p->locvars[k] = ::LocVar{nullptr, 0, 0};
        p->upvalues = luaM_newvector(L, s[5], ::TString*);
        p->sizeupvalues = s[5];
        for (int k = 0; k < s[5]; ++k) p->upvalues[k] = nullptr;
        for (int k = 0; k < s[0]; ++k) p->code[k] = 0;
        for (int k = 0; k < s[3]; ++k) p->lineinfo[k] = 0;
    }

    void resize_thread(lua_State* th, const PObj& o) {
        luaD_reallocstack(th, o.stacksize);
        luaD_reallocCI(th, static_cast<int>(o.size_ci));
        for (int k = 0; k < th->stacksize; ++k) setnilvalue(th->stack + k);
    }

    // Pass 1: every object, with its allocation sizes (contents are filled by pass 2).
    void create() {
        strs.reserve(im.strings.size());
        for (const std::string& s : im.strings) strs.push_back(luaS_newlstr(L, s.data(), s.size()));
        obj.assign(im.obj.size(), nullptr);
        ::TObject nil;
        setnilvalue(&nil);
        nil.value.gc = nullptr;
        for (size_t i = 0; i < im.obj.size(); ++i) {
            const PObj& o = im.obj[i];
            switch (o.kind) {
            case kTable: obj[i] = valtogco(luaH_new(L, o.sizearray, o.lsizenode)); break;
            case kLuaClosure: {
                ::Closure* c = luaF_newLclosure(L, o.nup, &nil);
                c->l.p = nullptr;
                for (int k = 0; k < o.nup; ++k) c->l.upvals[k] = nullptr;
                obj[i] = valtogco(c);
                break;
            }
            case kCClosure: {
                ::Closure* c = luaF_newCclosure(L, o.nup);
                c->c.f = nullptr;
                for (int k = 0; k < o.nup; ++k) setnilvalue(&c->c.upvalue[k]);
                obj[i] = valtogco(c);
                break;
            }
            case kUserdata: {
                ::Udata* u = luaS_newudata(L, static_cast<size_t>(o.len));
                if (o.len) std::memset(u + 1, 0, static_cast<size_t>(o.len));
                obj[i] = valtogco(u);
                break;
            }
            case kThread: {
                lua_State* th = i == 0 ? L : luaE_newthread(L);
                resize_thread(th, o);
                setnilvalue(gt(th));
                obj[i] = valtogco(th);
                break;
            }
            case kProto: {
                ::Proto* p = luaF_newproto(L);
                alloc_array(p, o.psize);
                obj[i] = valtogco(p);
                break;
            }
            case kUpvalClosed: {
                ::UpVal* u = luaM_new(L, ::UpVal);
                luaC_link(L, valtogco(u), LUA_TUPVAL);
                u->v = &u->value;
                setnilvalue(&u->value);
                obj[i] = valtogco(u);
                break;
            }
            case kUpvalOpen: {
                ::UpVal* u = luaM_new(L, ::UpVal);
                u->tt = LUA_TUPVAL;
                u->marked = 1;  // open upvalues are never collected (lfunc.c)
                u->alamo_serial = 0;
                u->next = nullptr;
                setnilvalue(&u->value);
                u->v = &u->value;
                obj[i] = valtogco(u);
                break;
            }
            default: break;
            }
        }
        dm = &obj[im.defaultmeta]->h;
    }

    // Pass 2: contents.
    void fill() {
        for (size_t i = 0; i < im.obj.size(); ++i) {
            const PObj& o = im.obj[i];
            ::GCObject* x = obj[i];
            switch (o.kind) {
            case kTable: {
                ::Table* t = &x->h;
                t->marked = o.marked;
                if (!o.full) {
                    t->metatable = dm;
                    break;
                }
                t->flags = o.flags;
                t->metatable = &obj[o.meta]->h;
                for (int k = 0; k < t->sizearray; ++k) set(&t->array[k], o.vals[static_cast<size_t>(k)]);
                if (o.lsizenode) {
                    const bool is_reg = i == im.registry;
                    for (size_t k = 0; k < o.nodes.size(); ++k) {
                        ::Node* n = &t->node[k];
                        const PNode& p = o.nodes[k];
                        set(gkey(n), p.key);
                        const ::TObject* att = nullptr;
                        if (is_reg && p.val.tag == vLight && p.val.bits == kLightAttachment)
                            att = old_registry_value(im.strings[p.key.ref]);
                        set(gval(n), p.val, att);
                        n->next = p.next >= 0 ? &t->node[p.next] : nullptr;
                    }
                    t->firstfree = &t->node[o.firstfree];
                }
                break;
            }
            case kLuaClosure: {
                ::LClosure& c = x->cl.l;
                if (!o.full) break;
                c.p = &obj[o.proto]->p;
                set(&c.g, o.env);
                for (size_t k = 0; k < o.refs.size(); ++k) c.upvals[k] = &obj[o.refs[k]]->uv;
                break;
            }
            case kCClosure: {
                ::CClosure& c = x->cl.c;
                if (!o.full) break;
                c.f = im.cfns[o.refs[0]];
                for (size_t k = 0; k < o.vals.size(); ++k) set(&c.upvalue[k], o.vals[k]);
                break;
            }
            case kUserdata: {
                ::Udata* u = &x->u;
                u->uv.marked = o.marked;
                u->uv.metatable = o.full ? &obj[o.meta]->h : dm;
                if (o.full && !o.bytes.empty()) std::memcpy(u + 1, o.bytes.data(), o.bytes.size());
                break;
            }
            case kThread: {
                lua_State* th = &x->th;
                th->marked = o.marked;
                if (!o.full) break;
                set(gt(th), o.env);
                for (size_t k = 0; k < o.vals.size(); ++k) set(th->stack + k, o.vals[k]);
                th->top = th->stack + o.top;
                th->stack_last = th->stack + o.stack_last;
                for (size_t k = 0; k < o.cis.size(); ++k) {
                    const PCi& c = o.cis[k];
                    ::CallInfo* ci = th->base_ci + k;
                    ci->base = th->stack + c.base;
                    ci->top = th->stack + c.top;
                    ci->state = c.state;
                    ci->u.l.savedpc = nullptr;  // Lua frames: set below, once every closure has its prototype
                    ci->u.l.pc = nullptr;
                    ci->u.l.tailcalls = c.tailcalls;
                }
                th->ci = th->base_ci + (o.cis.size() - 1);
                th->base = th->ci->base;
                th->allowhook = o.allowhook;
                th->basehookcount = o.basehookcount;
                th->hookcount = o.hookcount;
                th->hookmask = 0;
                th->hookinit = 0;
                th->hook = nullptr;
                th->nCcalls = 0;
                th->errfunc = 0;
                th->errorJmp = nullptr;
                break;
            }
            case kProto: {
                ::Proto* p = &x->p;
                if (!o.full) break;
                p->source = o.source == kNoIndex ? nullptr : strs[o.source];
                p->lineDefined = o.line;
                p->nups = o.nups;
                p->numparams = o.numparams;
                p->is_vararg = o.is_vararg;
                p->maxstacksize = o.maxstack;
                for (size_t k = 0; k < o.code.size(); ++k) p->code[k] = static_cast<::Instruction>(o.code[k]);
                for (size_t k = 0; k < o.vals.size(); ++k) set(&p->k[k], o.vals[k]);
                for (size_t k = 0; k < o.refs.size(); ++k) p->p[k] = &obj[o.refs[k]]->p;
                for (size_t k = 0; k < o.lineinfo.size(); ++k) p->lineinfo[k] = o.lineinfo[k];
                for (size_t k = 0; k < o.locvars.size() / 3; ++k) {
                    const auto name = static_cast<uint32_t>(o.locvars[3 * k]);
                    p->locvars[k].varname = name == kNoIndex ? nullptr : strs[name];
                    p->locvars[k].startpc = o.locvars[3 * k + 1];
                    p->locvars[k].endpc = o.locvars[3 * k + 2];
                }
                for (size_t k = 0; k < o.upnames.size(); ++k) p->upvalues[k] = o.upnames[k] == kNoIndex ? nullptr : strs[o.upnames[k]];
                break;
            }
            case kUpvalClosed:
                if (o.full) set(&x->uv.value, o.env);
                break;
            default: break;
            }
        }
        // Saved pcs of suspended Lua frames: an offset into the frame closure's code.
        for (size_t i = 0; i < im.obj.size(); ++i) {
            const PObj& o = im.obj[i];
            if (o.kind != kThread || !o.full) continue;
            lua_State* th = &obj[i]->th;
            for (size_t k = 0; k < o.cis.size(); ++k) {
                ::CallInfo* ci = th->base_ci + k;
                if (!(o.cis[k].state & CI_C)) ci->u.l.savedpc = clvalue(ci->base - 1)->l.p->code + o.cis[k].savedpc;
            }
        }
        // Open upvalues: each thread's list in saved order, pointing into its (now final) stack.
        std::vector<::GCObject**> tail(im.obj.size(), nullptr);
        for (size_t i = 0; i < im.obj.size(); ++i) {
            const PObj& o = im.obj[i];
            if (o.kind != kUpvalOpen) continue;
            lua_State* th = &obj[o.thread]->th;
            ::UpVal* u = &obj[i]->uv;
            u->v = th->stack + o.level;
            ::GCObject**& t = tail[o.thread];
            if (!t) t = &th->openupval;
            *t = obj[i];
            t = &obj[i]->gch.next;
        }
    }

    // The collector lists in saved order, roots, serials, global sizes. The lua_open tables (registry, globals,
    // default metatable) are replaced by the saved ones and freed.
    void finish(::Table* old_dm, ::Table* old_gt, ::Table* old_reg) {
        global_State* g = G(L);
        ::GCObject* first_gc = nullptr;
        ::GCObject** gc_tail = &first_gc;
        ::GCObject* first_ud = nullptr;
        ::GCObject** ud_tail = &first_ud;
        for (size_t i = 1; i < im.obj.size(); ++i) {
            const uint8_t k = im.obj[i].kind;
            if (k == kUpvalOpen) continue;
            ::GCObject** & t = k == kUserdata ? ud_tail : gc_tail;
            *t = obj[i];
            t = &obj[i]->gch.next;
        }
        *gc_tail = nullptr;
        *ud_tail = nullptr;
        g->rootgc = first_gc;
        g->rootudata = first_ud;
        sethvalue(registry(L), &obj[im.registry]->h);
        sethvalue(defaultmeta(L), dm);
        for (::Table* t : {old_dm, old_gt, old_reg}) luaH_free(L, t);
        for (size_t i = 0; i < im.obj.size(); ++i)
            if (im.obj[i].kind != kUpvalOpen) obj[i]->gch.alamo_serial = im.obj[i].serial;
        g->alamo_serial = im.serial;
        if (g->strt.size != im.strt_size) luaS_resize(L, im.strt_size);
        luaZ_resizebuffer(L, &g->buff, static_cast<size_t>(im.buffsize));
        g->GCthreshold = static_cast<::lu_mem>(im.threshold);
    }

    // Hash parts must be consistent with the keys' hashes (ltable.c newkey walks a colliding key's chain from its
    // main position and relies on it): every live key is on its main position's chain, and firstfree is free.
    bool tables_consistent() const {
        for (size_t i = 0; i < im.obj.size(); ++i) {
            if (im.obj[i].kind != kTable || !im.obj[i].full || im.obj[i].lsizenode == 0) continue;
            ::Table* t = &obj[i]->h;
            const int size = sizenode(t);
            if (!ttisnil(gkey(t->firstfree))) return false;
            for (int k = 0; k < size; ++k) {
                ::Node* n = gnode(t, k);
                if (ttisnil(gval(n))) continue;
                if (ttisnumber(gkey(n)) && nvalue(gkey(n)) != nvalue(gkey(n))) return false;  // NaN key
                const ::Node* p = luaH_mainposition(t, gkey(n));
                int steps = 0;
                while (p != nullptr && p != n && steps++ < size) p = p->next;
                if (p != n) return false;
            }
        }
        return true;
    }

    // On a refused image: no finalizer may run while the new state is closed.
    void neutralize() {
        for (::GCObject* o = G(L)->rootudata; o != nullptr; o = o->gch.next) gcotou(o)->uv.metatable = dm;
    }
};

}  // namespace

// ================================================================ API

bool write_heap(lua_State* L, const void* vm_self, const std::vector<lua_State*>& threads, std::vector<uint32_t>& thread_index,
                ByteWriter& w, std::string& err) {
    Saver s;
    s.L = L;
    s.g = G(L);
    s.vm_self = vm_self;
    s.reg = hvalue(registry(L));
    s.engine_meta = registry_table(L, "alamo.meta");
    if (L != s.g->mainthread) {
        err = "not the main state";
        return false;
    }
    if (!s.enumerate() || !s.mark_full()) {
        err = s.err;
        return false;
    }
    thread_index.clear();
    for (lua_State* th : threads) {
        const uint32_t i = s.find(th);
        if (i == kNoIndex || s.kind[i] != kThread || !s.full[i] || i == 0) {
            err = "an engine thread that is not a reachable coroutine";
            return false;
        }
        thread_index.push_back(i);
    }
    ByteWriter body;
    for (uint32_t i = 0; i < s.obj.size(); ++i)
        if (!s.w_object(body, i)) {
            err = s.err;
            return false;
        }
    const uint32_t reg = s.find(s.reg);
    const uint32_t dmi = ttistable(defaultmeta(L)) ? s.find(hvalue(defaultmeta(L))) : kNoIndex;
    if (reg == kNoIndex || dmi == kNoIndex) {
        err = "registry or default metatable outside the heap";
        return false;
    }
    body.u32(reg).u32(dmi);
    body.u32(s.g->alamo_serial).u64(s.g->nblocks).u64(s.g->GCthreshold).i32(s.g->strt.size).u64(s.g->buff.buffsize);

    const CFunctionNames& names = cfunction_names();
    w.u32(kHeapFormat);
    w.u32(static_cast<uint32_t>(s.cfns.size()));
    for (const uint32_t id : s.cfns) w.str(names.list[id].first);
    w.u32(static_cast<uint32_t>(s.strs.size()));
    for (const ::TString* t : s.strs) w.str(std::string_view(getstr(t), t->tsv.len));
    w.u32(static_cast<uint32_t>(s.obj.size()));
    w.raw(body.bytes());
    return true;
}

bool read_heap(ByteReader& r, lua_State* L_old, void* vm_self, lua_CFunction panic, const std::vector<uint32_t>& thread_index,
               std::vector<lua_State*>& thread_states, lua_State*& out, std::string& err) {
    Image im;
    Loader ld{r, err};
    if (!ld.image(im)) return false;
    Validator v{im, err};
    uint64_t predicted = 0;
    if (!v.run(thread_index, predicted)) return false;

    lua_State* L = lua_open();
    if (!L) {
        err = "cannot create a Lua state";
        return false;
    }
    lua_atpanic(L, panic);
    ::Table* old_dm = hvalue(defaultmeta(L));
    ::Table* old_gt = hvalue(gt(L));
    ::Table* old_reg = hvalue(registry(L));
    G(L)->GCthreshold = ~static_cast<::lu_mem>(0);  // no collection while the heap is half-built (none is triggered anyway)
    Builder b(im);
    b.L = L;
    b.L_old = L_old;
    b.vm_self = vm_self;
    b.create();
    b.fill();
    b.finish(old_dm, old_gt, old_reg);

    bool ok = G(L)->nblocks == static_cast<::lu_mem>(predicted) && G(L)->strt.nuse == static_cast<::ls_nstr>(im.strings.size());
    if (!ok) err = "the rebuilt heap does not match its saved byte / string count";
    if (ok && !b.tables_consistent()) {
        ok = false;
        err = "a table whose node layout does not match its keys";
    }
    for (size_t i = 0; ok && i < im.obj.size(); ++i)
        if (im.obj[i].kind == kProto && im.obj[i].full && !luaG_checkcode(&b.obj[i]->p)) {
            ok = false;
            err = "invalid bytecode";
        }
    if (!ok) {
        b.neutralize();
        lua_close(L);
        return false;
    }
    thread_states.clear();
    for (const uint32_t t : thread_index) thread_states.push_back(&b.obj[t]->th);
    out = L;
    return true;
}

std::vector<std::string> cfunction_aliases() { return cfunction_names().aliases; }

LuaVm::HeapInfo heap_stats(lua_State* L) {
    LuaVm::HeapInfo s;
    s.serial = G(L)->alamo_serial;
    s.bytes = G(L)->nblocks;
    s.threshold = G(L)->GCthreshold;
    s.strings = static_cast<uint32_t>(G(L)->strt.nuse);
    return s;
}

}  // namespace alamo::lua::detail
