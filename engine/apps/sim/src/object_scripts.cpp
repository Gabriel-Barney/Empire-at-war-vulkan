// q6: game-object Lua scripts (object_scripts.hpp, docs/re/OBJECT_SCRIPTS.md).
#include "alamo/sim/object_scripts.hpp"

#include <cmath>

#include "alamo/objects/game_object_types.hpp"
#include "alamo/sim/game_modes.hpp"
#ifdef ALAMO_SIM_HAVE_STORY
#include "alamo/lua/lua_vm.hpp"
#include "alamo/lua/object_bridge.hpp"
#include "alamo/lua/script_remaining.hpp"
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
    if (n.size() > 4) {  // ".lua" optional (§1.1)
        const std::string_view ext = n.substr(n.size() - 4);
        auto low = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; };
        if (ext[0] == '.' && low(ext[1]) == 'l' && low(ext[2]) == 'u' && low(ext[3]) == 'a') n.remove_suffix(4);
    }
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

}  // namespace alamo::sim
