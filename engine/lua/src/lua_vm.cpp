// LuaVm: state setup (libraries, overrides, engine globals), loading, script threads.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <span>

#include <lualib.h>

#include "alamo/core/file_provider.hpp"
#include "alamo/core/hash.hpp"
#include "alamo/core/log.hpp"
#include "alamo/lua/script_loader.hpp"
#include "vm_internal.hpp"

namespace alamo::lua {

namespace {

const char kThreadValuesKey[] = "alamo.threadvalues";

LuaVm::Impl* vm_of(lua_State* L) { return static_cast<LuaVm::Impl*>(lua_touserdata(L, lua_upvalueindex(1))); }

int raise(lua_State* L, const char* msg) {
    luaL_where(L, 1);
    lua_pushstring(L, msg);
    lua_concat(L, 2);
    return lua_error(L);
}

int panic(lua_State* L) {
    const char* msg = lua_tostring(L, -1);
    std::fprintf(stderr, "alamo lua: unprotected error: %s\n", msg ? msg : "?");
    std::abort();
}

// Loads `name` (a script name through the roots, or a game-relative path) as a chunk on L's stack.
// Returns false with err set. All C++ temporaries die before returning.
bool load_script(lua_State* L, const LuaVm::Impl& vm, std::string_view name, bool as_path, char* err, size_t cap) noexcept {
    try {
        if (!vm.host.files) {
            std::snprintf(err, cap, "cannot load '%.*s': no file provider", static_cast<int>(name.size()), name.data());
            return false;
        }
        std::vector<uint8_t> bytes;
        std::string path;
        if (as_path && vm.host.files->read(name, bytes)) path = std::string(name);
        if (path.empty()) path = read_script(*vm.host.files, name, bytes);
        if (path.empty()) {
            std::snprintf(err, cap, "cannot find script '%.*s' in Data\\Scripts", static_cast<int>(name.size()), name.data());
            return false;
        }
        const std::string chunk_name = "@" + path;
        const int st = luaL_loadbuffer(L, reinterpret_cast<const char*>(bytes.data()), bytes.size(), chunk_name.c_str());
        if (st != 0) {
            const char* m = lua_tostring(L, -1);
            std::snprintf(err, cap, "%s", m ? m : "load error");
            lua_pop(L, 1);
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        std::snprintf(err, cap, "cannot load script: %s", e.what());
        return false;
    }
}

// require(name) with 5.0 semantics (_LOADED table, _REQUIREDNAME), files through the FileProvider.
// _LOADED[name] is set to true BEFORE the chunk runs (guards cyclic requires; 5.0 sets it after).
int l_require(lua_State* L) {
    char err[640];
    if (lua_type(L, 1) != LUA_TSTRING) return raise(L, "require: expected a script name");
    lua_settop(L, 1);
    lua_pushstring(L, "_LOADED");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_type(L, 2) != LUA_TTABLE) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushstring(L, "_LOADED");
        lua_pushvalue(L, -2);
        lua_rawset(L, LUA_GLOBALSINDEX);
    }
    lua_pushvalue(L, 1);
    lua_rawget(L, 2);
    if (lua_toboolean(L, -1)) return 1;
    lua_pop(L, 1);
    if (!load_script(L, *vm_of(L), std::string_view(lua_tostring(L, 1), lua_strlen(L, 1)), false, err, sizeof err))
        return raise(L, err);
    lua_pushvalue(L, 1);
    lua_pushboolean(L, 1);
    lua_rawset(L, 2);
    lua_pushstring(L, "_REQUIREDNAME");
    lua_pushvalue(L, 1);
    lua_rawset(L, LUA_GLOBALSINDEX);
    lua_call(L, 0, 1);
    if (!lua_toboolean(L, -1)) {
        lua_pop(L, 1);
        lua_pushboolean(L, 1);
    }
    lua_pushvalue(L, 1);
    lua_pushvalue(L, -2);
    lua_rawset(L, 2);
    return 1;
}

// loadfile(path): chunk, or nil + message.
int l_loadfile(lua_State* L) {
    char err[640];
    if (lua_type(L, 1) != LUA_TSTRING) return raise(L, "loadfile: expected a file name");
    if (!load_script(L, *vm_of(L), std::string_view(lua_tostring(L, 1), lua_strlen(L, 1)), true, err, sizeof err)) {
        lua_pushnil(L);
        lua_pushstring(L, err);
        return 2;
    }
    return 1;
}

// dofile(path): runs the chunk, returns its results.
int l_dofile(lua_State* L) {
    char err[640];
    if (lua_type(L, 1) != LUA_TSTRING) return raise(L, "dofile: expected a file name");
    lua_settop(L, 1);
    if (!load_script(L, *vm_of(L), std::string_view(lua_tostring(L, 1), lua_strlen(L, 1)), true, err, sizeof err))
        return raise(L, err);
    lua_call(L, 0, LUA_MULTRET);
    return lua_gettop(L) - 1;
}

void print_line(LuaVm::Impl& vm, const char* s) noexcept {
    try {
        log::write(log::Level::Info, "lua", s);
        vm.message(std::string("[print] ") + s);
    } catch (...) {
    }
}

// print(...): tab-separated tostring() of the arguments, to the engine log (never stdout).
int l_print(lua_State* L) {
    const int n = lua_gettop(L);
    lua_pushstring(L, "tostring");
    lua_rawget(L, LUA_GLOBALSINDEX);
    const int tostring_index = n + 1;
    int parts = 0;
    for (int i = 1; i <= n; ++i) {
        if (i > 1) {
            lua_pushstring(L, "\t");
            ++parts;
        }
        lua_pushvalue(L, tostring_index);
        lua_pushvalue(L, i);
        lua_call(L, 1, 1);
        if (lua_type(L, -1) != LUA_TSTRING) return raise(L, "`tostring' must return a string to `print'");
        ++parts;
    }
    if (parts == 0) lua_pushstring(L, "");
    else lua_concat(L, parts);
    print_line(*vm_of(L), lua_tostring(L, -1));
    return 0;
}

void set_function(lua_State* L, LuaVm::Impl& vm, const char* name, lua_CFunction fn) {
    lua_pushstring(L, name);
    lua_pushlightuserdata(L, &vm);
    lua_pushcclosure(L, fn, 1);
    lua_rawset(L, LUA_GLOBALSINDEX);
}

const char* reader_chunk(lua_State*, void* data, size_t* size) {
    auto* sv = static_cast<std::string_view*>(data);
    if (sv->empty()) {
        *size = 0;
        return nullptr;
    }
    const char* p = sv->data();
    *size = sv->size();
    *sv = std::string_view();
    return p;
}

}  // namespace

// ---------------------------------------------------------------- Impl

void LuaVm::Impl::message(std::string line) { messages.push_back(std::move(line)); }

void LuaVm::Impl::kill_thread(int32_t id) {
    const auto it = threads.find(id);
    if (it != threads.end()) it->second.killed = true;
}

void LuaVm::Impl::reap() {
    for (auto it = threads.begin(); it != threads.end();) {
        if (!it->second.killed) {
            ++it;
            continue;
        }
        luaL_unref(L, LUA_REGISTRYINDEX, it->second.ref);
        lua_pushstring(L, kThreadValuesKey);
        lua_rawget(L, LUA_REGISTRYINDEX);
        lua_pushnumber(L, it->first);
        lua_pushnil(L);
        lua_rawset(L, -3);
        lua_pop(L, 1);
        it = threads.erase(it);
    }
}

int32_t LuaVm::Impl::start_thread(lua_State* from, int fn_index, int args_first, int nargs, std::string name) {
    if (lua_type(from, fn_index) != LUA_TFUNCTION) return -1;
    lua_State* co = lua_newthread(from);
    const int ref = luaL_ref(from, LUA_REGISTRYINDEX);
    lua_pushvalue(from, fn_index);
    lua_xmove(from, co, 1);
    for (int i = 0; i < nargs; ++i) {
        lua_pushvalue(from, args_first + i);
        lua_xmove(from, co, 1);
    }
    const int32_t id = next_thread++;
    ThreadRec& t = threads[id];
    t.co = co;
    t.ref = ref;
    t.function = std::move(name);
    t.nargs = nargs;
    return id;
}

// ---------------------------------------------------------------- LuaVm

LuaVm::LuaVm(const LuaHost& host_in, LuaVmOptions options_in)
    : impl_(std::make_unique<Impl>()), owner_(std::this_thread::get_id()) {
    Impl& vm = *impl_;
    vm.owner = this;
    vm.host = host_in;
    vm.options = std::move(options_in);
    vm.unbound.assign(detail::counter_slots(), 0);
    lua_State* L = lua_open();
    if (!L) {
        std::fprintf(stderr, "alamo lua: cannot create a Lua state\n");
        std::abort();
    }
    vm.L = L;
    lua_atpanic(L, panic);
    // The original's script libraries: base (with coroutine), table, string. No io/os/debug/loadlib.
    luaopen_base(L);
    luaopen_table(L);
    luaopen_string(L);
    lua_settop(L, 0);
    if (vm.options.math_library) detail::install_math_library(L, vm);
    // File and console access go through the engine.
    set_function(L, vm, "require", l_require);
    set_function(L, vm, "loadfile", l_loadfile);
    set_function(L, vm, "dofile", l_dofile);
    set_function(L, vm, "print", l_print);
    lua_pushstring(L, kThreadValuesKey);
    lua_newtable(L);
    lua_rawset(L, LUA_REGISTRYINDEX);
    detail::install_metatable(L, vm);
    detail::install_globals(L, vm);
    lua_settop(L, 0);
}

LuaVm::~LuaVm() {
    if (impl_ && impl_->L) lua_close(impl_->L);
}

void LuaVm::check_owner() const {
    if (!on_owner_thread()) {
        std::fprintf(stderr, "alamo lua: LuaVm used from a thread that does not own it\n");
        std::abort();
    }
}

LuaHost& LuaVm::host() {
    check_owner();
    return impl_->host;
}

lua_State* LuaVm::state() {
    check_owner();
    return impl_->L;
}

bool LuaVm::compile(std::string_view chunk, std::string_view chunk_name, std::string* err) {
    check_owner();
    lua_State* L = impl_->L;
    std::string_view data = chunk;
    const std::string name = "@" + std::string(chunk_name);
    const int st = lua_load(L, reader_chunk, &data, name.c_str());
    if (st != 0) {
        if (err) *err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "load error";
        lua_pop(L, 1);
        return false;
    }
    lua_pop(L, 1);
    return true;
}

bool LuaVm::run(std::string_view chunk, std::string_view chunk_name, std::string* err) {
    check_owner();
    lua_State* L = impl_->L;
    std::string_view data = chunk;
    const std::string name = "@" + std::string(chunk_name);
    int st = lua_load(L, reader_chunk, &data, name.c_str());
    if (st == 0) st = lua_pcall(L, 0, 0, 0);
    if (st != 0) {
        if (err) *err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "error";
        lua_pop(L, 1);
        return false;
    }
    return true;
}

bool LuaVm::require(std::string_view name, std::string* err) {
    check_owner();
    lua_State* L = impl_->L;
    lua_pushstring(L, "require");
    lua_rawget(L, LUA_GLOBALSINDEX);
    lua_pushlstring(L, name.data(), name.size());
    if (lua_pcall(L, 1, 0, 0) != 0) {
        if (err) *err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "error";
        lua_pop(L, 1);
        return false;
    }
    return true;
}

bool LuaVm::call(std::string_view global_function, std::string* err) {
    check_owner();
    lua_State* L = impl_->L;
    lua_pushlstring(L, global_function.data(), global_function.size());
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_type(L, -1) != LUA_TFUNCTION) {
        lua_pop(L, 1);
        if (err) *err = std::string(global_function) + " is not a global function";
        return false;
    }
    if (lua_pcall(L, 0, 0, 0) != 0) {
        if (err) *err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "error";
        lua_pop(L, 1);
        return false;
    }
    return true;
}

int32_t LuaVm::create_thread(std::string_view global_function) {
    check_owner();
    lua_State* L = impl_->L;
    lua_pushlstring(L, global_function.data(), global_function.size());
    lua_rawget(L, LUA_GLOBALSINDEX);
    const int top = lua_gettop(L);
    const int32_t id = impl_->start_thread(L, top, top + 1, 0, std::string(global_function));
    lua_settop(L, top - 1);
    return id;
}

size_t LuaVm::service_threads() {
    check_owner();
    Impl& vm = *impl_;
    std::vector<int32_t> ids;
    ids.reserve(vm.threads.size());
    for (const auto& [id, t] : vm.threads)
        if (!t.killed) ids.push_back(id);
    for (const int32_t id : ids) {
        if (vm.exit_requested) break;
        const auto it = vm.threads.find(id);
        if (it == vm.threads.end() || it->second.killed) continue;
        ThreadRec& t = it->second;  // std::map: stable across insertions made by the script
        const int nargs = t.started ? 0 : t.nargs;
        t.started = true;
        vm.current_thread = id;
        const int st = lua_resume(t.co, nargs);
        vm.current_thread = -1;
        if (st != 0) {
            const char* m = lua_tostring(t.co, -1);
            vm.errors.push_back("thread " + std::to_string(id) + " (" + t.function + "): " + (m ? m : "error"));
            log::write(log::Level::Warn, "lua", vm.errors.back());
            t.killed = true;
        } else {
            lua_Debug ar;
            if (lua_getstack(t.co, 0, &ar) == 0) {
                t.killed = true;  // returned
            } else if (lua_gettop(t.co) >= 1 && lua_type(t.co, -1) == LUA_TBOOLEAN && !lua_toboolean(t.co, -1)) {
                t.killed = true;  // coroutine.yield(false): PGBase ScriptExit
            }
        }
        lua_settop(t.co, 0);
    }
    if (vm.exit_requested)
        for (auto& [id, t] : vm.threads) t.killed = true;
    vm.reap();
    return vm.threads.size();
}

size_t LuaVm::thread_count() const {
    check_owner();
    size_t n = 0;
    for (const auto& [id, t] : impl_->threads) n += t.killed ? 0u : 1u;
    return n;
}

bool LuaVm::thread_active(int32_t id) const {
    check_owner();
    const auto it = impl_->threads.find(id);
    return it != impl_->threads.end() && !it->second.killed;
}

bool LuaVm::exit_requested() const {
    check_owner();
    return impl_->exit_requested;
}

void LuaVm::set_time(double seconds) {
    check_owner();
    impl_->time = seconds;
}

void LuaVm::set_frame(uint32_t frame) {
    check_owner();
    impl_->frame = frame;
}

void LuaVm::post_event(std::string_view name) {
    check_owner();
    lua_State* L = impl_->L;
    lua_pushlstring(L, name.data(), name.size());
    detail::queue_event(L, lua_gettop(L), 0);
    lua_pop(L, 1);
}

void LuaVm::set_object_global(std::string_view name, objects::ObjectHandle h) {
    check_owner();
    lua_State* L = impl_->L;
    lua_pushlstring(L, name.data(), name.size());
    detail::push_object(L, *impl_, h);
    lua_rawset(L, LUA_GLOBALSINDEX);
}

const std::vector<std::string>& LuaVm::messages() const {
    check_owner();
    return impl_->messages;
}

const std::vector<std::string>& LuaVm::errors() const {
    check_owner();
    return impl_->errors;
}

std::vector<UnboundHit> LuaVm::unbound_hits() const {
    check_owner();
    std::vector<UnboundHit> out;
    const auto table = binding_table();
    for (size_t i = 0; i < impl_->unbound.size(); ++i) {
        if (impl_->unbound[i] == 0) continue;
        UnboundHit h;
        h.count = impl_->unbound[i];
        if (i < table.size()) {
            h.name = table[i].kind == BindingKind::Global ? std::string(table[i].name)
                                                          : std::string(table[i].cls) + "." + table[i].name;
        } else {
            for (uint32_t s = 0; s < detail::method_slots().size(); ++s)
                if (detail::extra_counter_slot(s) == i) h.name = detail::method_slots()[s].label;
        }
        out.push_back(std::move(h));
    }
    std::sort(out.begin(), out.end(), [](const UnboundHit& a, const UnboundHit& b) { return a.name < b.name; });
    return out;
}

uint64_t LuaVm::unbound_total() const {
    check_owner();
    return impl_->unbound_total;
}

// ---------------------------------------------------------------- N1 save / load
//
// Layout (little-endian): u32 version, u8 saved (0: str reason - read() refuses it), u64 FNV-1a 64 of the rest of the
// record (alamo/core/hash.hpp), f64 time, u32 frame, i32 next thread id, u8 exit requested, u8 multiplayer Free_Random warned, u32 thread count, per thread (id order):
// i32 id, i32 registry ref, str function, i32 nargs, u8 started, u8 killed, u32 coroutine object index; then the
// heap (detail::write_heap, vm_persist.cpp) to the end of the chunk.

namespace {
constexpr uint32_t kVmSaveVersion = 1;
}

void LuaVm::write(ByteWriter& w) const {
    check_owner();
    const Impl& vm = *impl_;
    std::string err;
    ByteWriter heap;
    std::vector<lua_State*> cos;
    for (const auto& [id, t] : vm.threads) cos.push_back(t.co);
    std::vector<uint32_t> co_index;
    bool ok = vm.current_thread == -1 && vm.store_tag == 0;
    if (!ok) err = "write() called while a script runs";
    else ok = detail::write_heap(vm.L, &vm, cos, co_index, heap, err);
    w.u32(kVmSaveVersion);
    if (!ok) {
        impl_->save_error = err.empty() ? std::string("unknown error") : err;
        log::write(log::Level::Error, "lua", "cannot save script '" + vm.options.script_name + "': " + impl_->save_error);
        w.boolean(false).str(impl_->save_error);
        return;
    }
    impl_->save_error.clear();
    ByteWriter body;
    body.f64(vm.time).u32(vm.frame).i32(vm.next_thread).boolean(vm.exit_requested).boolean(vm.mp_free_random_warned);
    body.u32(static_cast<uint32_t>(vm.threads.size()));
    size_t k = 0;
    for (const auto& [id, t] : vm.threads) {
        body.i32(id).i32(t.ref).str(t.function).i32(t.nargs).boolean(t.started).boolean(t.killed).u32(co_index[k++]);
    }
    body.raw(heap.bytes());
    // A checksum of the rest: any damaged byte is refused before the heap is rebuilt.
    w.boolean(true).u64(fnv1a64(body.bytes().data(), body.size())).raw(body.bytes());
}

namespace {

// LuaVm::read after the version / checksum: the engine fields, then the heap; commits only when everything is valid.
bool read_vm_body(LuaVm::Impl& vm, ByteReader& r) {
    const double time = r.f64();
    const uint32_t frame = r.u32();
    const int32_t next_thread = r.i32();
    const bool exit_requested = r.boolean();
    const bool warned = r.boolean();
    const uint32_t n = r.u32();
    if (!r.ok() || next_thread < 0 || n > r.remaining() / 22) return false;
    std::map<int32_t, ThreadRec> threads;
    std::vector<int32_t> ids;
    std::vector<uint32_t> co_index;
    for (uint32_t i = 0; i < n; ++i) {
        const int32_t id = r.i32();
        ThreadRec t;
        t.ref = r.i32();
        t.function = r.str();
        t.nargs = r.i32();
        t.started = r.boolean();
        t.killed = r.boolean();
        const uint32_t co = r.u32();
        if (!r.ok() || id < 0 || id >= next_thread || (!ids.empty() && id <= ids.back()) || t.ref <= 0 || t.nargs < 0)
            return false;
        ids.push_back(id);
        co_index.push_back(co);
        threads.emplace(id, std::move(t));
    }
    lua_State* L = nullptr;
    std::vector<lua_State*> states;
    std::string err;
    if (!detail::read_heap(r, vm.L, &vm, panic, co_index, states, L, err)) {
        log::write(log::Level::Warn, "lua", "cannot load script '" + vm.options.script_name + "': " + err);
        return false;
    }
    // Commit: the restored state replaces the VM's own (closed; nothing outside Impl points into it).
    size_t k = 0;
    for (auto& [id, t] : threads) t.co = states[k++];
    lua_State* old = vm.L;
    vm.L = L;
    vm.threads = std::move(threads);
    vm.time = time;
    vm.frame = frame;
    vm.next_thread = next_thread;
    vm.exit_requested = exit_requested;
    vm.mp_free_random_warned = warned;
    vm.current_thread = -1;
    vm.store_tag = 0;
    vm.save_error.clear();
    lua_close(old);
    return true;
}

}  // namespace

bool LuaVm::read(ByteReader& r) {
    check_owner();
    if (r.u32() != kVmSaveVersion || !r.ok()) return false;
    const bool saved = r.boolean();
    if (!r.ok() || !saved) return false;
    const uint64_t sum = r.u64();
    const std::span<const uint8_t> rest = r.raw(r.remaining());
    if (!r.ok() || fnv1a64(rest.data(), rest.size()) != sum) return false;
    ByteReader body(rest);
    return read_vm_body(*impl_, body);
}

const std::string& LuaVm::save_error() const {
    check_owner();
    return impl_->save_error;
}

LuaVm::HeapInfo LuaVm::heap_info() const {
    check_owner();
    const detail::HeapStats s = detail::heap_stats(impl_->L);
    HeapInfo h;
    h.serial = s.serial;
    h.bytes = s.bytes;
    h.threshold = s.threshold;
    h.strings = s.strings;
    return h;
}

int LuaVm::resolve_method(std::string_view cls, std::string_view name) const {
    const uint32_t c = detail::class_id(cls);
    if (c == detail::kNoClass) return -1;
    const uint32_t s = detail::find_method(c, name);
    if (s == detail::kNoBinding) return -1;
    return detail::method_slots()[s].fn ? 1 : 0;
}

}  // namespace alamo::lua
