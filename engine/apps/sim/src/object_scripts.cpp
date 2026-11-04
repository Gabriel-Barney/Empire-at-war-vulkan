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

struct ObjectScriptHost::Script {
    objects::ObjectId id = objects::kNoObjectId;
    objects::ObjectHandle h;
    std::string name;
#ifdef ALAMO_SIM_HAVE_STORY
    std::unique_ptr<lua::LuaVm> vm;
#endif
    size_t seen_messages = 0, seen_errors = 0;
    bool live() const {
#ifdef ALAMO_SIM_HAVE_STORY
        return vm != nullptr;
#else
        return false;
#endif
    }
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

void ObjectScriptHost::mode_pushed(GameMode& mode) {
    objects::ObjectStore& st = mode.primary();
    if (find(st)) return;
    auto ss = std::make_unique<StoreScripts>();
    ss->store = &st;
    ss->mode = &mode;
    StoreScripts* p = ss.get();
    stores_.push_back(std::move(ss));
    st.on_created = [this, p](objects::ObjectStore&, objects::ObjectHandle h) { attach(*p, h); };
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
        if (s->vm) lua::attach_object_bridge(*s->vm, bridge);
#endif
}

void ObjectScriptHost::set_player_world(const objects::ObjectStore& store, lua::LuaPlayerWorld* world) {
    StoreScripts* ss = find(store);
    if (!ss) return;
    ss->world = world;
#if defined(ALAMO_SIM_HAVE_STORY) && defined(ALAMO_LUA_PLAYERS)
    for (const std::unique_ptr<Script>& s : ss->scripts)
        if (s->vm) lua::attach_player_world(*s->vm, world);
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
            if (s->h == h && s->vm) return s->vm.get();
#else
    (void)store;
    (void)h;
#endif
    return nullptr;
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
}  // namespace

void ObjectScriptHost::note(uint64_t frame, const Script& s, const std::string& what, bool error) {
    if (error) ++error_counts_[s.name + ": " + error_key(what)];
    if (messages_.size() < kMaxMessages)
        messages_.push_back(std::to_string(frame) + " " + s.name + " (id " + std::to_string(s.id) + "): " + what);
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
    lua::LuaHost host;
    host.store = &st;
    host.types = &types;
    host.rng = random_;
    host.files = files_;
    host.campaign = campaign_;
    lua::LuaVmOptions opt;
    opt.script_name = s->name;
    opt.upper_type_names = true;  // OBJECT_SCRIPTS §6.1 step 4
    auto vm = std::make_unique<lua::LuaVm>(host, opt);
    const uint64_t frame = ss.mode->frame();
    // §1.3: the file's top level (pool load), then the base globals + commands of the attach.
    std::string err;
    if (!vm->require(s->name, &err)) {
        ++stats_.load_failures;
        note(frame, *s, "load: " + err, true);
        return;
    }
    if (globals_) lua::attach_global_values(*vm, globals_);
#if defined(ALAMO_LUA_PLAYERS)
    if (ss.world) lua::attach_player_world(*vm, ss.world);
#endif
    if (ss.bridge) lua::attach_object_bridge(*vm, ss.bridge);
    if (vm->has_function("Base_Definitions") && !vm->call("Base_Definitions", &err))
        note(frame, *s, "Base_Definitions: " + err, true);  // the script stays attached (its globals as far as they got)
    vm->create_thread("main");                // missing main: no thread
    vm->set_object_global("Object", h);       // last (§1.3 step 6)
    s->vm = std::move(vm);
    ++stats_.attached;
    collect(ss, *s);
    ss.scripts.push_back(std::move(s));
#else
    (void)ss;
    (void)h;
#endif
}

void ObjectScriptHost::collect(StoreScripts& ss, Script& s) {
#ifdef ALAMO_SIM_HAVE_STORY
    if (!s.vm) return;
    const uint64_t frame = ss.mode->frame();
    const std::vector<std::string>& msgs = s.vm->messages();
    for (; s.seen_messages < msgs.size(); ++s.seen_messages) {
        const std::string& m = msgs[s.seen_messages];
        note(frame, s, m, m.rfind("SCRIPT ERROR", 0) == 0);
    }
    const std::vector<std::string>& errs = s.vm->errors();
    for (; s.seen_errors < errs.size(); ++s.seen_errors) {
        ++stats_.thread_errors;
        note(frame, s, "thread error: " + errs[s.seen_errors], true);
    }
#else
    (void)ss;
    (void)s;
#endif
}

void ObjectScriptHost::release(StoreScripts& ss, Script& s, uint64_t& counter) {
#ifdef ALAMO_SIM_HAVE_STORY
    if (!s.vm) return;
    collect(ss, s);
    s.vm.reset();
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
        if (!s.vm) continue;
        if (!st.alive(s.h) || st.dying(s.h)) {  // §2.4: dead (or gone): never resumed again
            release(ss, s, stats_.released_dead);
            continue;
        }
        lua::LuaVm& vm = *s.vm;
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
        collect(ss, s);
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
        if (ss.scripts[r]->vm) ss.scripts[w++] = std::move(ss.scripts[r]);
    ss.scripts.resize(w);
#else
    (void)mode;
#endif
}

}  // namespace alamo::sim
