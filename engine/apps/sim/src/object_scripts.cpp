// q6: game-object Lua scripts (object_scripts.hpp, docs/re/OBJECT_SCRIPTS.md).
#include "alamo/core/ascii.hpp"
#include "alamo/sim/object_scripts.hpp"

#include <cmath>
#include <span>

#include "alamo/objects/game_object_types.hpp"
#include "alamo/sim/game_modes.hpp"
#ifdef ALAMO_SIM_HAVE_STORY
#include "alamo/lua/lua_vm.hpp"
#include "alamo/lua/object_bridge.hpp"
#include "alamo/lua/script_remaining.hpp"
#include "alamo/lua/script_save.hpp"
#if defined(ALAMO_LUA_PLAYERS)
#include "alamo/lua/players.hpp"
#endif
#endif

namespace alamo::sim {

float first_pump_draw_hi(double service_rate) {
    // [port] OBJECT_SCRIPTS.md §8 item 1: hi = ServiceRate (best guess; the decompile does not show xmm2).
    if (!(service_rate >= 0.0) || service_rate > 1.0e30) return 0.0f;
    return static_cast<float>(service_rate);
}

double object_script_time(uint64_t mode_frame) {
    return static_cast<double>(static_cast<float>(mode_frame) * kObjectScriptSecondsPerFrame);
}

// One script instance (the original's LuaScriptClass): its VM plus the pool flags of §1.4 (GAME_SCORING §1 names).
struct ObjectScriptHost::Instance {
    std::string name;
#ifdef ALAMO_SIM_HAVE_STORY
    std::unique_ptr<lua::LuaVm> vm;
#endif
    bool in_use = false;    // +0x123
    bool fresh = true;      // +0x122: Base_Definitions not run yet
    bool poolable = false;  // +0x124
    size_t seen_messages = 0, seen_errors = 0;
};

struct ObjectScriptHost::Script {
    objects::ObjectId id = objects::kNoObjectId;
    objects::ObjectHandle h;
    std::string name;
    Instance* inst = nullptr;       // null = released
    std::unique_ptr<Instance> own;  // an unpooled instance (ScriptPoolCount 0)
    bool live() const { return inst != nullptr; }
};

struct ObjectScriptHost::TypeScript {
    objects::TypeIndex type = objects::kNone;
    Instance* inst = nullptr;
    std::unique_ptr<Instance> own;
};

struct ObjectScriptHost::StoreScripts {
    objects::ObjectStore* store = nullptr;
    GameMode* mode = nullptr;
    lua::ObjectBridge* bridge = nullptr;
    lua::LuaPlayerWorld* world = nullptr;
    std::vector<std::unique_ptr<Script>> scripts;  // attach order; released entries are compacted after a service
    std::vector<std::string> names;                // per type: the Lua_Script name ("" none), filled lazily
    std::vector<uint8_t> named;                    // per type: names[t] resolved
};

ObjectScriptHost::ObjectScriptHost(GameRandom& random) : random_(&random) {}

ObjectScriptHost::~ObjectScriptHost() {
    for (const std::unique_ptr<StoreScripts>& ss : stores_)
        if (ss->store) ss->store->on_created = nullptr;
}

ObjectScriptHost::StoreScripts* ObjectScriptHost::find(const objects::ObjectStore& store) {
    for (const std::unique_ptr<StoreScripts>& ss : stores_)
        if (ss->store == &store) return ss.get();
    return nullptr;
}
const ObjectScriptHost::StoreScripts* ObjectScriptHost::find(const objects::ObjectStore& store) const {
    for (const std::unique_ptr<StoreScripts>& ss : stores_)
        if (ss->store == &store) return ss.get();
    return nullptr;
}

void ObjectScriptHost::set_files(const FileProvider* files) {
    files_ = files;
    if (files_ && !types_loaded_ && !stores_.empty()) load_type_scripts(*stores_.front());
}

void ObjectScriptHost::mode_pushed(GameMode& mode) {
    objects::ObjectStore& st = mode.primary();
    if (find(st)) return;
    auto ss = std::make_unique<StoreScripts>();
    ss->store = &st;
    ss->mode = &mode;
    StoreScripts* p = ss.get();
    stores_.push_back(std::move(ss));
    st.on_created = [this, p](objects::ObjectStore&, objects::ObjectHandle h) { attach(*p, h); };
    if (files_ && !types_loaded_) load_type_scripts(*p);  // §1.5 type setup [port: at the first mode]
}

void ObjectScriptHost::mode_popping(GameMode& mode) {
    objects::ObjectStore& st = mode.primary();
    for (size_t i = 0; i < stores_.size(); ++i) {
        StoreScripts& ss = *stores_[i];
        if (ss.store != &st) continue;
        st.on_created = nullptr;
        for (const std::unique_ptr<Script>& s : ss.scripts)
            if (s->live()) release(ss, *s, stats_.released_mode);
        stores_.erase(stores_.begin() + static_cast<std::ptrdiff_t>(i));
        return;
    }
}

void ObjectScriptHost::set_bridge(const objects::ObjectStore& store, lua::ObjectBridge* bridge) {
    StoreScripts* ss = find(store);
    if (!ss) return;
    ss->bridge = bridge;
#ifdef ALAMO_SIM_HAVE_STORY
    for (const std::unique_ptr<Script>& s : ss->scripts)
        if (s->inst) lua::attach_object_bridge(*s->inst->vm, bridge);
#endif
}

void ObjectScriptHost::set_player_world(const objects::ObjectStore& store, lua::LuaPlayerWorld* world) {
    StoreScripts* ss = find(store);
    if (!ss) return;
    ss->world = world;
#if defined(ALAMO_SIM_HAVE_STORY) && defined(ALAMO_LUA_PLAYERS)
    for (const std::unique_ptr<Script>& s : ss->scripts)
        if (s->inst) lua::attach_player_world(*s->inst->vm, world);
#endif
}

size_t ObjectScriptHost::live() const {
    size_t n = 0;
    for (const std::unique_ptr<StoreScripts>& ss : stores_) n += live(*ss->store);
    return n;
}

size_t ObjectScriptHost::live(const objects::ObjectStore& store) const {
    const StoreScripts* ss = find(store);
    size_t n = 0;
    if (ss)
        for (const std::unique_ptr<Script>& s : ss->scripts) n += s->live() ? 1u : 0u;
    return n;
}

bool ObjectScriptHost::has_script(const objects::ObjectStore& store, objects::ObjectHandle h) const {
    return vm_of(store, h) != nullptr;
}

const lua::LuaVm* ObjectScriptHost::vm_of(const objects::ObjectStore& store, objects::ObjectHandle h) const {
#ifdef ALAMO_SIM_HAVE_STORY
    if (const StoreScripts* ss = find(store))
        for (const std::unique_ptr<Script>& s : ss->scripts)
            if (s->h == h && s->inst) return s->inst->vm.get();
#else
    (void)store;
    (void)h;
#endif
    return nullptr;
}

const lua::LuaVm* ObjectScriptHost::type_vm(objects::TypeIndex t) const {
#ifdef ALAMO_SIM_HAVE_STORY
    for (const std::unique_ptr<TypeScript>& ts : type_scripts_)
        if (ts->type == t && ts->inst) return ts->inst->vm.get();
#else
    (void)t;
#endif
    return nullptr;
}

std::vector<ObjectScriptHost::PoolInfo> ObjectScriptHost::pools() const {
    std::vector<PoolInfo> out;
    for (const auto& [name, list] : pools_) {
        PoolInfo p;
        p.name = name;
        p.size = list.size();
        for (const std::unique_ptr<Instance>& in : list) p.in_use += in->in_use ? 1u : 0u;
        out.push_back(std::move(p));
    }
    return out;
}

uint64_t ObjectScriptHost::error_total() const {
    uint64_t n = 0;
    for (const auto& [text, count] : error_counts_) n += count;
    return n;
}

namespace {
// The error-count key: digit runs become '#' except line numbers ("file.lua:12:"), so the same error of many objects
// counts as one entry.
std::string error_key(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        const char ch = text[i];
        if (ch >= '0' && ch <= '9') {
            size_t j = i;
            while (j < text.size() && text[j] >= '0' && text[j] <= '9') ++j;
            if (i > 0 && text[i - 1] == ':') out.append(text, i, j - i);
            else out.push_back('#');
            i = j;
            continue;
        }
        out.push_back(ch);
        ++i;
    }
    return out;
}

[[maybe_unused]] std::string trimmed_script_name(std::string_view n) {
    auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!n.empty() && ws(n.front())) n.remove_prefix(1);
    while (!n.empty() && ws(n.back())) n.remove_suffix(1);
    if (n.size() > 4 && ascii_iends_with(n, ".lua")) n.remove_suffix(4);  // ".lua" optional (§1.1)
    return std::string(n);
}

// §1.4: ScriptPoolCount as a pool size. Absent (not a number) -> 1; N -> N (whole part; [port] at most 256); 0, a
// negative or a value below 1 -> 0 (no pool).
[[maybe_unused]] size_t pool_size_of(bool present, double count) {
    if (!present) return 1;
    if (!(count >= 1.0)) return 0;
    return count >= 256.0 ? 256u : static_cast<size_t>(count);
}
}  // namespace

void ObjectScriptHost::note(uint64_t frame, const std::string& name, objects::ObjectId id, const std::string& what, bool error) {
    if (error) ++error_counts_[name + ": " + error_key(what)];
    if (messages_.size() < kMaxMessages)
        messages_.push_back(std::to_string(frame) + " " + name +
                            (id == objects::kNoObjectId ? std::string(" (no object)") : " (id " + std::to_string(id) + ")") +
                            ": " + what);
}

std::unique_ptr<ObjectScriptHost::Instance> ObjectScriptHost::load(StoreScripts& ss, const std::string& name, objects::ObjectId id) {
#ifdef ALAMO_SIM_HAVE_STORY
    lua::LuaHost host;
    host.store = ss.store;
    host.types = &ss.mode->types();
    host.rng = random_;
    host.files = files_;
    host.campaign = campaign_;
    lua::LuaVmOptions opt;
    opt.script_name = name;
    opt.upper_type_names = true;  // OBJECT_SCRIPTS §6.1 step 4
    auto in = std::make_unique<Instance>();
    in->name = name;
    in->vm = std::make_unique<lua::LuaVm>(host, opt);
    // §1.4 (0x245e30): a new lua_State, the file's top level run.
    std::string err;
    const bool ok = in->vm->require(name, &err);
    collect(ss.mode->frame(), *in, id);
    if (!ok) {
        ++stats_.load_failures;
        note(ss.mode->frame(), name, id, "load: " + err, true);
        return nullptr;
    }
    ++stats_.loaded;
    return in;
#else
    (void)ss;
    (void)name;
    (void)id;
    return nullptr;
#endif
}

ObjectScriptHost::Instance* ObjectScriptHost::acquire(StoreScripts& ss, const std::string& name, objects::ObjectId id,
                                                      std::unique_ptr<Instance>& owned) {
#ifdef ALAMO_SIM_HAVE_STORY
    Instance* got = nullptr;
    const auto it = pools_.find(name);
    if (it == pools_.end()) {
        // Name never seen: load it, read ScriptPoolCount once.
        std::unique_ptr<Instance> first = load(ss, name, id);
        if (!first) return nullptr;
        double count = 0.0;
        const bool present = first->vm->get_number("ScriptPoolCount", count);
        const size_t n = pool_size_of(present, count);
        if (n == 0) {  // no pool: this and every later instance of the name is unpooled
            owned = std::move(first);
            got = owned.get();
        } else {
            std::vector<std::unique_ptr<Instance>>& list = pools_[name];
            first->poolable = true;
            got = first.get();
            list.push_back(std::move(first));
            for (size_t k = 1; k < n; ++k) {  // the N - 1 extra instances, top level run for each, held by the pool
                std::unique_ptr<Instance> extra = load(ss, name, id);
                if (!extra) break;
                extra->poolable = true;
                list.push_back(std::move(extra));
            }
        }
    } else {
        std::vector<std::unique_ptr<Instance>>& list = it->second;
        for (size_t k = list.size(); k-- > 0;)  // the last free instance in list order
            if (!list[k]->in_use) {
                got = list[k].get();
                ++stats_.reused;
                break;
            }
        if (!got) {  // none free: one more, appended
            std::unique_ptr<Instance> more = load(ss, name, id);
            if (!more) return nullptr;
            more->poolable = true;
            got = more.get();
            list.push_back(std::move(more));
        }
    }
    got->in_use = true;
    lua::LuaVm& vm = *got->vm;
    vm.pool_handout();  // +0x121 = 0, +0x60 = -1
    lua::LuaHost host = vm.host();
    host.store = ss.store;
    host.types = &ss.mode->types();
    host.campaign = campaign_;
    vm.rebind_host(host);
    return got;
#else
    (void)ss;
    (void)name;
    (void)id;
    (void)owned;
    return nullptr;
#endif
}

// §1.3 steps 3 and 4 (shared by object and type instances): the base globals + commands registered again, GlobalValue
// and the bridges of the store, Base_Definitions() for a fresh instance.
namespace {
#ifdef ALAMO_SIM_HAVE_STORY
void attach_engine(lua::LuaVm& vm, lua::GlobalValueStore* globals, lua::LuaPlayerWorld* world, lua::ObjectBridge* bridge) {
    vm.reinstall_engine_globals();
    lua::attach_global_values(vm, globals);
#if defined(ALAMO_LUA_PLAYERS)
    lua::attach_player_world(vm, world);
#else
    (void)world;
#endif
    lua::attach_object_bridge(vm, bridge);
}
#endif
}  // namespace

void ObjectScriptHost::load_type_scripts(StoreScripts& ss) {
    types_loaded_ = true;
#ifdef ALAMO_SIM_HAVE_STORY
    const objects::GameObjectTypes& types = ss.mode->types();
    const uint64_t frame = ss.mode->frame();
    for (objects::TypeIndex t = 0; t < types.size(); ++t) {
        if (!types.get_bool(t, "Execute_Script_On_Type")) continue;
        const std::string name = trimmed_script_name(types.get_string(t, "Lua_Script"));
        if (name.empty()) continue;
        auto ts = std::make_unique<TypeScript>();
        ts->type = t;
        ts->inst = acquire(ss, name, objects::kNoObjectId, ts->own);
        if (!ts->inst) continue;
        lua::LuaVm& vm = *ts->inst->vm;
        attach_engine(vm, globals_, ss.world, ss.bridge);
        std::string err;
        if (ts->inst->fresh && vm.has_function("Base_Definitions") && !vm.call("Base_Definitions", &err))
            note(frame, name, objects::kNoObjectId, "type " + std::to_string(t) + " Base_Definitions: " + err, true);
        ts->inst->fresh = false;
        // [port] the type instance belongs to no store: its bridges are detached once set up.
        lua::attach_object_bridge(vm, nullptr);
#if defined(ALAMO_LUA_PLAYERS)
        lua::attach_player_world(vm, nullptr);
#endif
        collect(frame, *ts->inst, objects::kNoObjectId);
        type_scripts_.push_back(std::move(ts));
    }
#else
    (void)ss;
#endif
}

void ObjectScriptHost::attach(StoreScripts& ss, objects::ObjectHandle h) {
#ifdef ALAMO_SIM_HAVE_STORY
    objects::ObjectStore& st = *ss.store;
    const objects::GameObjectTypes& types = ss.mode->types();
    const objects::TypeIndex t = st.type(h);
    if (t == objects::kNone || t >= types.size() || !files_) return;
    if (ss.names.size() < types.size()) {
        ss.names.resize(types.size());
        ss.named.resize(types.size(), 0);
    }
    if (!ss.named[t]) {
        ss.names[t] = trimmed_script_name(types.get_string(t, "Lua_Script"));
        ss.named[t] = 1;
    }
    if (ss.names[t].empty()) return;
    auto s = std::make_unique<Script>();
    s->id = st.id(h);
    s->h = h;
    s->name = ss.names[t];
    const uint64_t frame = ss.mode->frame();
    // §1.3 step 1: the instance (pool; the top level ran at its load).
    s->inst = acquire(ss, s->name, s->id, s->own);
    if (!s->inst) return;
    lua::LuaVm& vm = *s->inst->vm;
    attach_engine(vm, globals_, ss.world, ss.bridge);  // step 3
    std::string err;
    if (s->inst->fresh && vm.has_function("Base_Definitions") && !vm.call("Base_Definitions", &err))  // step 4
        note(frame, s->name, s->id, "Base_Definitions: " + err, true);  // the script stays attached (its globals as far as they got)
    s->inst->fresh = false;
    vm.create_thread("main");           // step 5; missing main: no thread
    vm.set_object_global("Object", h);  // step 6, last
    ++stats_.attached;
    collect(frame, *s->inst, s->id);
    ss.scripts.push_back(std::move(s));
#else
    (void)ss;
    (void)h;
#endif
}

void ObjectScriptHost::collect(uint64_t frame, Instance& in, objects::ObjectId id) {
#ifdef ALAMO_SIM_HAVE_STORY
    if (!in.vm) return;
    const std::vector<std::string>& msgs = in.vm->messages();
    for (; in.seen_messages < msgs.size(); ++in.seen_messages) {
        const std::string& m = msgs[in.seen_messages];
        note(frame, in.name, id, m, m.rfind("SCRIPT ERROR", 0) == 0);
    }
    const std::vector<std::string>& errs = in.vm->errors();
    for (; in.seen_errors < errs.size(); ++in.seen_errors) {
        ++stats_.thread_errors;
        note(frame, in.name, id, "thread error: " + errs[in.seen_errors], true);
    }
#else
    (void)frame;
    (void)in;
    (void)id;
#endif
}

void ObjectScriptHost::release(StoreScripts& ss, Script& s, uint64_t& counter) {
#ifdef ALAMO_SIM_HAVE_STORY
    if (!s.inst) return;
    Instance& in = *s.inst;
    const uint64_t frame = ss.mode->frame();
    collect(frame, in, s.id);
    if (in.poolable) {
        // §2.3 (0x2488e0): exit flag, threads killed, thread table cleared; then Flush_G(), Base_Definitions() now, the
        // stack reset, +0x122 / +0x123 cleared, a collection: the instance waits in its pool.
        lua::LuaVm& vm = *in.vm;
        vm.pool_kill_threads();
        lua::attach_object_bridge(vm, nullptr);  // [port] see the header (RELEASE)
#if defined(ALAMO_LUA_PLAYERS)
        lua::attach_player_world(vm, nullptr);
#endif
        std::string err;
        if (vm.has_function("Flush_G") && !vm.call("Flush_G", &err)) note(frame, in.name, s.id, "Flush_G: " + err, true);
        if (vm.has_function("Base_Definitions") && !vm.call("Base_Definitions", &err))
            note(frame, in.name, s.id, "Base_Definitions (release): " + err, true);
        vm.collect_garbage();
        in.fresh = false;
        in.in_use = false;
        collect(frame, in, s.id);
        ++stats_.pooled_releases;
    } else {
        s.own.reset();
    }
    s.inst = nullptr;
    ++counter;
#else
    (void)ss;
    (void)s;
    (void)counter;
#endif
}

void ObjectScriptHost::service(GameMode& mode) {
#ifdef ALAMO_SIM_HAVE_STORY
    StoreScripts* ssp = find(mode.primary());
    if (!ssp || ssp->scripts.empty()) return;
    StoreScripts& ss = *ssp;
    objects::ObjectStore& st = *ss.store;
    const uint64_t mode_frame = mode.frame();
    const double now = object_script_time(mode_frame);
    // Newest attached first; scripts attached during the pass (Spawn_Unit in a pump) are appended and wait.
    for (size_t i = ss.scripts.size(); i-- > 0;) {
        Script& s = *ss.scripts[i];  // unique_ptr: stable while later attaches grow the vector
        if (!s.inst) continue;
        if (!st.alive(s.h) || st.dying(s.h)) {  // §2.4: dead (or gone): never resumed again
            release(ss, s, stats_.released_dead);
            continue;
        }
        lua::LuaVm& vm = *s.inst->vm;
        double rate = 0.0;
        if (!vm.get_number("ServiceRate", rate)) continue;  // §3.2 step 3
        double last = 0.0;
        const bool have_last = vm.get_number("LastService", last);
        if (have_last && !(now - last > rate)) continue;  // step 5 (strict, double)
        if (vm.exit_requested()) {                        // §3.3: set before the pump -> release, nothing resumed
            release(ss, s, stats_.released_exit);
            continue;
        }
        vm.set_time(now);
        vm.set_frame(static_cast<uint32_t>(mode_frame));
        vm.service_threads();
        ++stats_.pumps;
        collect(mode_frame, *s.inst, s.id);
        if (vm.exit_requested()) {
            release(ss, s, stats_.released_exit);
            continue;
        }
        if (!have_last) {  // step 7: LastService absent before the pump -> -(one synced draw), else now
            const float r = random_->synced().range_float(0.0f, first_pump_draw_hi(rate));
            ++stats_.first_draws;
            vm.set_number("LastService", -static_cast<double>(r));
        } else {
            vm.set_number("LastService", now);
        }
    }
    // Compact released entries (attach order of the rest kept).
    size_t w = 0;
    for (size_t r = 0; r < ss.scripts.size(); ++r)
        if (ss.scripts[r]->inst) ss.scripts[w++] = std::move(ss.scripts[r]);
    ss.scripts.resize(w);
#else
    (void)mode;
#endif
}

// ------------------------------------------------------------------ q6 t19: save / load (§2.6, header "SAVES")
//
// Layout (kSaveVersion 1): u8 types loaded; the 11 Stats counters (u64, declaration order); u32 pool count, per pool
// (ascending name) str name, u32 instance count, per instance (list order) an INSTANCE record; u32 store count, per
// store (push order) u8 mode kind, u32 script count, per live script (attach order) i32 object id, u64 handle, str name,
// i32 pool slot (-1: an unpooled instance, its INSTANCE record follows); u32 type instance count, per type instance
// (ascending type) u32 type, i32 pool slot (-1: INSTANCE record follows).
// INSTANCE: u8 in use, u8 fresh, u8 poolable, i32 store its LuaHost points to (stores index, -1 none on the stack),
// u32 length + the lua::write_script_block bytes.
namespace {
[[maybe_unused]] constexpr uint32_t kMaxSavedCount = 1u << 20;
#ifdef ALAMO_SIM_HAVE_STORY
struct SavedInstance {
    bool in_use = false, fresh = false, poolable = false;
    int32_t store = -1;
    std::span<const uint8_t> block;
    int32_t holder = -1;  // reader: the store index of the live script holding it, -2 a type instance, -1 nobody
};
#endif
}  // namespace

bool ObjectScriptHost::write(ByteWriter& w, std::string* err) const {
    std::string first_error;
    auto store_index = [this](const objects::ObjectStore* st) -> int32_t {
        for (size_t i = 0; i < stores_.size(); ++i)
            if (stores_[i]->store == st) return static_cast<int32_t>(i);
        return -1;
    };
    auto put = [&](const Instance& in) {
#ifdef ALAMO_SIM_HAVE_STORY
        w.boolean(in.in_use).boolean(in.fresh).boolean(in.poolable).i32(store_index(in.vm->host().store));
        ByteWriter block;
        lua::write_script_block(block, *in.vm);
        if (first_error.empty() && !in.vm->save_error().empty())
            first_error = "object script " + in.name + ": " + in.vm->save_error();
        w.u32(static_cast<uint32_t>(block.size())).raw(block.bytes());
#else
        (void)in;
        (void)store_index;
#endif
    };
    auto pool_slot = [this](const std::string& name, const Instance* p) -> int32_t {
        const auto it = pools_.find(name);
        if (it != pools_.end())
            for (size_t k = 0; k < it->second.size(); ++k)
                if (it->second[k].get() == p) return static_cast<int32_t>(k);
        return -1;
    };
    w.u32(kSaveVersion);
    w.boolean(types_loaded_);
    const Stats& s = stats_;
    for (const uint64_t v : {s.attached, s.load_failures, s.loaded, s.reused, s.pooled_releases, s.pumps, s.first_draws,
                             s.released_exit, s.released_dead, s.released_mode, s.thread_errors})
        w.u64(v);
    w.u32(static_cast<uint32_t>(pools_.size()));
    for (const auto& [name, list] : pools_) {  // std::map: ascending name (rule 3)
        w.str(name).u32(static_cast<uint32_t>(list.size()));
        for (const std::unique_ptr<Instance>& in : list) put(*in);
    }
    w.u32(static_cast<uint32_t>(stores_.size()));
    for (const std::unique_ptr<StoreScripts>& ss : stores_) {
        w.u8(static_cast<uint8_t>(ss->mode->kind()));
        uint32_t n = 0;
        for (const std::unique_ptr<Script>& sc : ss->scripts) n += sc->live() ? 1u : 0u;
        w.u32(n);
        for (const std::unique_ptr<Script>& sc : ss->scripts) {
            if (!sc->live()) continue;
            w.i32(sc->id).u64(sc->h.value).str(sc->name);
            const int32_t slot = sc->own ? -1 : pool_slot(sc->name, sc->inst);
            w.i32(slot);
            if (slot < 0) put(*sc->inst);
        }
    }
    w.u32(static_cast<uint32_t>(type_scripts_.size()));
    for (const std::unique_ptr<TypeScript>& ts : type_scripts_) {
        w.u32(static_cast<uint32_t>(ts->type));
        const int32_t slot = ts->own ? -1 : pool_slot(ts->inst->name, ts->inst);
        w.i32(slot);
        if (slot < 0) put(*ts->inst);
    }
    if (!first_error.empty()) {
        if (err) *err = first_error;
        return false;
    }
    return true;
}

bool ObjectScriptHost::read(ByteReader& r, std::string* err) {
    auto fail = [err](std::string m) {
        if (err) *err = std::move(m);
        return false;
    };
    if (r.u32() != kSaveVersion || !r.ok()) return fail("unknown object script save version");
    const bool types_loaded = r.boolean();
    Stats st;
    for (uint64_t* v : {&st.attached, &st.load_failures, &st.loaded, &st.reused, &st.pooled_releases, &st.pumps,
                        &st.first_draws, &st.released_exit, &st.released_dead, &st.released_mode, &st.thread_errors})
        *v = r.u64();
#ifdef ALAMO_SIM_HAVE_STORY
    auto get = [&](SavedInstance& s) {
        s.in_use = r.boolean();
        s.fresh = r.boolean();
        s.poolable = r.boolean();
        s.store = r.i32();
        const uint32_t n = r.u32();
        if (!r.ok() || n > r.remaining() || s.store < -1 || s.store >= static_cast<int32_t>(stores_.size())) return false;
        s.block = r.raw(n);
        return r.ok();
    };
    struct PoolRec {
        std::string name;
        std::vector<SavedInstance> list;
    };
    std::vector<PoolRec> pools;
    const uint32_t np = r.u32();
    if (!r.ok() || np > kMaxSavedCount) return fail("bad pool count");
    for (uint32_t i = 0; i < np; ++i) {
        PoolRec p;
        p.name = r.str();
        const uint32_t n = r.u32();
        if (!r.ok() || n == 0 || n > kMaxSavedCount || (!pools.empty() && !(pools.back().name < p.name)))
            return fail("bad pool record");
        p.list.resize(n);
        for (SavedInstance& s : p.list)
            if (!get(s) || !s.poolable) return fail("bad pooled instance of " + p.name);
        pools.push_back(std::move(p));
    }
    auto find_pool = [&pools](const std::string& name) -> PoolRec* {
        for (PoolRec& p : pools)
            if (p.name == name) return &p;
        return nullptr;
    };
    // A pool slot taken by a live script (holder >= 0) or a type instance (-2): in use, held once.
    auto take = [&](const std::string& name, int32_t slot, int32_t holder) {
        PoolRec* p = find_pool(name);
        if (!p || slot < 0 || static_cast<size_t>(slot) >= p->list.size()) return false;
        SavedInstance& s = p->list[static_cast<size_t>(slot)];
        if (!s.in_use || s.holder != -1) return false;
        s.holder = holder;
        return true;
    };
    struct ScriptRec {
        objects::ObjectId id = objects::kNoObjectId;
        objects::ObjectHandle h;
        std::string name;
        int32_t slot = -1;
        SavedInstance own;
    };
    const uint32_t ns = r.u32();
    if (!r.ok() || ns != stores_.size())
        return fail("the save has " + std::to_string(ns) + " script stores, the session " + std::to_string(stores_.size()));
    std::vector<std::vector<ScriptRec>> scripts(ns);
    for (uint32_t i = 0; i < ns; ++i) {
        if (r.u8() != static_cast<uint8_t>(stores_[i]->mode->kind())) return fail("script store " + std::to_string(i) + ": another mode");
        const uint32_t n = r.u32();
        if (!r.ok() || n > kMaxSavedCount) return fail("bad script count");
        scripts[i].resize(n);
        for (ScriptRec& sc : scripts[i]) {
            sc.id = r.i32();
            sc.h.value = r.u64();
            sc.name = r.str();
            sc.slot = r.i32();
            if (!r.ok()) return fail("truncated script record");
            if (sc.slot < 0) {
                if (sc.slot != -1 || !get(sc.own) || sc.own.poolable || !sc.own.in_use) return fail("bad unpooled instance of " + sc.name);
                sc.own.holder = static_cast<int32_t>(i);
            } else if (!take(sc.name, sc.slot, static_cast<int32_t>(i))) {
                return fail("script of object " + std::to_string(sc.id) + ": bad pool slot");
            }
        }
    }
    struct TypeRec {
        objects::TypeIndex type = objects::kNone;
        std::string name;
        int32_t slot = -1;
        SavedInstance own;
    };
    const uint32_t nt = r.u32();
    if (!r.ok() || nt > kMaxSavedCount) return fail("bad type instance count");
    std::vector<TypeRec> types(nt);
    for (TypeRec& tr : types) {
        tr.type = static_cast<objects::TypeIndex>(r.u32());
        tr.slot = r.i32();
        if (!r.ok() || stores_.empty() || tr.type >= stores_.front()->mode->types().size()) return fail("bad type instance record");
        tr.name = trimmed_script_name(stores_.front()->mode->types().get_string(tr.type, "Lua_Script"));
        if (tr.slot < 0) {
            if (tr.slot != -1 || !get(tr.own) || tr.own.poolable) return fail("bad unpooled type instance");
            tr.own.holder = -2;
        } else if (!take(tr.name, tr.slot, -2)) {
            return fail("type instance " + std::to_string(tr.type) + ": bad pool slot");
        }
    }
    if (!r.ok() || !r.done()) return fail("trailing or missing bytes");
    for (const PoolRec& p : pools)
        for (const SavedInstance& s : p.list)
            if (s.in_use != (s.holder != -1)) return fail("pool " + p.name + ": an in-use flag does not match its holder");
    if ((np != 0 || nt != 0) && stores_.empty()) return fail("script instances but no store");

    // The from-save attach (§1.3): a new VM by name, the host attachments, the block. Nothing of the file runs.
    auto make = [&](const std::string& name, const SavedInstance& s, std::unique_ptr<Instance>& out, std::string& why) {
        StoreScripts& home = *stores_[static_cast<size_t>(s.store >= 0 ? s.store : 0)];
        lua::LuaHost host;
        host.store = s.store >= 0 ? home.store : nullptr;  // none: the next handout rebinds (and empties the caches)
        host.types = &home.mode->types();
        host.rng = random_;
        host.files = files_;
        host.campaign = campaign_;
        lua::LuaVmOptions opt;
        opt.script_name = name;
        opt.upper_type_names = true;
        auto in = std::make_unique<Instance>();
        in->name = name;
        in->in_use = s.in_use;
        in->fresh = s.fresh;
        in->poolable = s.poolable;
        in->vm = std::make_unique<lua::LuaVm>(host, opt);
        lua::attach_global_values(*in->vm, globals_);
        if (s.holder >= 0) {  // held by a live script: its store's bridges (free / type instances run detached)
            const StoreScripts& ss = *stores_[static_cast<size_t>(s.holder)];
#if defined(ALAMO_LUA_PLAYERS)
            lua::attach_player_world(*in->vm, ss.world);
#endif
            lua::attach_object_bridge(*in->vm, ss.bridge);
        }
        ByteReader br(s.block);
        std::string e;
        if (!lua::read_script_block(br, *in->vm, lua::ScriptLoad::FromSave, &e)) {
            why = "object script " + name + ": " + e;
            return false;
        }
        in->seen_messages = in->vm->messages().size();
        in->seen_errors = in->vm->errors().size();
        out = std::move(in);
        return true;
    };
    std::string why;
    std::map<std::string, std::vector<std::unique_ptr<Instance>>> new_pools;
    for (const PoolRec& p : pools) {
        std::vector<std::unique_ptr<Instance>>& list = new_pools[p.name];
        for (const SavedInstance& s : p.list) {
            std::unique_ptr<Instance> in;
            if (!make(p.name, s, in, why)) return fail(why);
            list.push_back(std::move(in));
        }
    }
    std::vector<std::vector<std::unique_ptr<Script>>> new_scripts(ns);
    for (uint32_t i = 0; i < ns; ++i)
        for (const ScriptRec& sr : scripts[i]) {
            auto sc = std::make_unique<Script>();
            sc->id = sr.id;
            sc->h = sr.h;
            sc->name = sr.name;
            if (sr.slot >= 0) {
                sc->inst = new_pools[sr.name][static_cast<size_t>(sr.slot)].get();
            } else {
                if (!make(sr.name, sr.own, sc->own, why)) return fail(why);
                sc->inst = sc->own.get();
            }
            new_scripts[i].push_back(std::move(sc));
        }
    std::vector<std::unique_ptr<TypeScript>> new_types;
    for (const TypeRec& tr : types) {
        auto ts = std::make_unique<TypeScript>();
        ts->type = tr.type;
        if (tr.slot >= 0) {
            ts->inst = new_pools[tr.name][static_cast<size_t>(tr.slot)].get();
        } else {
            if (!make(tr.name, tr.own, ts->own, why)) return fail(why);
            ts->inst = ts->own.get();
        }
        new_types.push_back(std::move(ts));
    }
    // Commit: the rebuilt session's scripts, pools and type instances are replaced whole (scripts first: they point
    // into the pools).
    for (uint32_t i = 0; i < ns; ++i) stores_[i]->scripts = std::move(new_scripts[i]);
    type_scripts_ = std::move(new_types);
    pools_ = std::move(new_pools);
#else
    // Without engine/lua nothing ever attaches: the chunk holds no instance.
    const uint32_t np = r.u32();
    const uint32_t ns = r.u32();
    if (!r.ok() || np != 0 || ns != stores_.size()) return fail("script instances in a build without engine/lua");
    for (uint32_t i = 0; i < ns; ++i) {
        (void)r.u8();
        if (r.u32() != 0) return fail("script instances in a build without engine/lua");
    }
    if (r.u32() != 0 || !r.ok() || !r.done()) return fail("script instances in a build without engine/lua");
#endif
    types_loaded_ = types_loaded;
    stats_ = st;
    return true;
}

}  // namespace alamo::sim
