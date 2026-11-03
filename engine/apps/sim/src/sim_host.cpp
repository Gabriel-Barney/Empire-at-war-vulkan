#include "alamo/sim/sim_host.hpp"

#include <algorithm>
#include <limits>

#include "alamo/galactic/campaign.hpp"
#include "alamo/objects/game_object_types.hpp"
#ifdef ALAMO_SIM_HAVE_STORY
#include "alamo/lua/script_remaining.hpp"  // L1 bindings: process-wide GlobalValue store
#endif

namespace alamo::sim {

namespace cmd = alamo::commands;

namespace {
bool finite(float f) { return f - f == 0.0f; }
}  // namespace

SimHost::SimHost(const objects::GameObjectTypes& types, jobs::JobSystem& js, SimConfig cfg)
    : js_(&js),
      cfg_(cfg),
      pf_(jobs::make_parallel_for(js)),
      modes_(types, cfg.players < kMaxPlayers ? cfg.players : kMaxPlayers),
      sched_(js) {
    if (cfg_.players > kMaxPlayers) cfg_.players = kMaxPlayers;
    // Q6: every player may submit orders (a mod's campaign can have more than the scheduler's default 8). Only raised, so
    // sessions of up to 8 players keep their recorded scheduler config.
    if (cfg_.scheduler.max_players < cfg_.players) cfg_.scheduler.max_players = static_cast<uint8_t>(cfg_.players);
    cfg_.scheduler.first_tick = 1;  // the first drain is tick 1 (T2 runs after the frame increment)
    random_.seed_free(cfg_.free_seed);
    make_pipeline(cfg_.scheduler);
    register_crc();
    recorder_ = std::make_unique<jobs::CrcRecorder>(sched_, registry_, cfg_.parallel_crc);
}

void SimHost::make_pipeline(const cmd::SchedulerConfig& cfg) {
    pipeline_ = std::make_unique<cmd::CommandPipeline>(cfg);
    pipeline_->set_handler([this](uint64_t tick, const cmd::Command& c) { return handle(tick, c); });
    pipeline_->set_crc_hook([this](const void* data, size_t size) {
        StateHasher h;
        h.bytes(data, size);
        batch_crc_ = h.digest();
    });
    if (record_) pipeline_->record_to(record_);
}

void SimHost::register_crc() {
    registry_.add("frame", [this](StateHasher& h) {
        h.u64(frame_);
        const auto depth = static_cast<uint32_t>(modes_.depth());
        h.u32(depth);
        for (uint32_t i = depth; i-- > 0;) modes_.at(i).hash(h);  // bottom -> top
#ifdef ALAMO_SIM_HAVE_STORY
        if (global_values_.size() != 0) global_values_.hash(h);  // Int-final: the session GlobalValue table, once used
#endif
    });
    registry_.add("objects", [this](StateHasher& h) {
        const auto depth = static_cast<uint32_t>(modes_.depth());
        h.u32(depth);
        for (uint32_t i = depth; i-- > 0;) {
            h.u8(static_cast<uint8_t>(modes_.at(i).kind()));
            modes_.at(i).primary().hash_state(h);
        }
    });
    registry_.add("galactic.state", [this](StateHasher& h) {
        const GalacticMode* g = modes_.empty() ? nullptr : dynamic_cast<const GalacticMode*>(&modes_.at(modes_.depth() - 1));
        const galactic::GalacticSim* s = g ? g->sim() : nullptr;
        h.boolean(s != nullptr);
        if (s) s->hash_state(h);
    });
    registry_.add("spatial", [this](StateHasher& h) {
        const TacticalMode* tm = modes_.top_tactical();
        h.boolean(tm != nullptr);
        if (tm) tm->trees().hash_state([&h](const void* d, size_t n) { h.bytes(d, n); });
    });
    registry_.add("players", [this](StateHasher& h) {
        h.u32(cfg_.players);
        for (uint32_t p = 0; p < cfg_.players; ++p) {
            h.u32(static_cast<uint32_t>(selection_[p].size()));
            for (ObjectId id : selection_[p]) h.i32(id);
        }
        if (groups_used_)  // P host: control groups, once assigned
            for (uint32_t p = 0; p < cfg_.players; ++p)
                for (const std::vector<ObjectId>& g : groups_[p]) {
                    h.u32(static_cast<uint32_t>(g.size()));
                    for (ObjectId id : g) h.i32(id);
                }
    });
    registry_.add("rng", [this](StateHasher& h) { random_.hash(h); });
    registry_.add("commands", [this](StateHasher& h) { h.u64(batch_crc_); });
    if (cfg_.story_crc)
        registry_.add("story", [this](StateHasher& h) {
            h.boolean(story_ != nullptr);
            if (story_) story_->hash(h);
        });
    registry_.add_parity("objects.parity", [this] {
        const GameMode* t = modes_.top();
        return t ? t->primary().parity_crc() : 0u;
    });
    registry_.add_parity("rng.parity", [this] { return parity_rng_crc(random_.synced().state()); });
}

// ------------------------------------------------------------------------------------- mode stack
GameMode& SimHost::start_game(ModeKind kind) {
    if (modes_.empty()) {
        // New session: FrameSynchronizer reset (frame 0), synced RNG seeded, id counter reset.
        story_.reset();
        if (session_used_) {
            frame_ = 0;
            batch_crc_ = 0;
            make_pipeline(cfg_.scheduler);
            recorder_ = std::make_unique<jobs::CrcRecorder>(sched_, registry_, cfg_.parallel_crc);
            for (auto& s : selection_) s.clear();
            for (auto& pg : groups_)  // P host: control groups
                for (auto& g : pg) g.clear();
            groups_used_ = false;
        }
        session_used_ = true;
#ifdef ALAMO_SIM_HAVE_STORY
        lua::process_global_values().clear();  // L1 bindings: GlobalValue is cleared for a new game (L1_S4_REMAINING §1.5)
        global_values_.clear();                // Int-final: the session-wide table too
#endif
        pacing_.reset();
        random_.seed_session(cfg_.seed);
        modes_.reset_ids();
    }
    GameMode& m = modes_.push(kind);
    // --- OI objects: behaviour Initialise draws its first due frame from the synced RNG (TACTICAL_GAPS 6.4);
    //     primary store only (the secondary store is local / cosmetic) ---
    m.primary().bind_sync_rng(&random_.synced());
    // --- end OI objects ---
    pacing_.select_active(m.tactical());
    pacing_.set_stopped(false);
    if (story_ && m.tactical()) story_->host_event(StoryHostEvent::TacticalStarted, frame_);
    return m;
}

lua::GlobalValueStore* SimHost::global_values() {  // Int-final
#ifdef ALAMO_SIM_HAVE_STORY
    return &global_values_;
#else
    return nullptr;
#endif
}

bool SimHost::story_supported() {
#ifdef ALAMO_SIM_HAVE_STORY
    return true;
#else
    return false;
#endif
}

bool SimHost::start_story(const StoryStart& start, std::vector<std::string>* diagnostics) {
    GalacticMode* g = galactic_mode();
    if (!g || !g->sim() || !story_supported()) {
        if (diagnostics) diagnostics->push_back(story_supported() ? "story: no galactic campaign running"
                                                                  : "story: built without engine/lua");
        return false;
    }
    auto s = std::make_unique<StorySession>(*g->sim(), g->primary(), g->types(), random_);
    s->set_command_pipeline(pipeline_.get());  // orders issued while the scripts load
    s->set_lua_gc_parallel_for(pf_);            // OPT2 t04: scheduled Lua collections on the host's job system
    g->sim()->set_emit_events(true);
    StoryStart st = start;  // Int-final: the session-wide GlobalValue table for every plot VM
    if (!st.global_values) st.global_values = global_values();
    if (!s->start(st, diagnostics)) {
        g->sim()->set_emit_events(false);
        return false;
    }
    story_ = std::move(s);
    return true;
}

GalacticMode* SimHost::galactic_mode() {
    return modes_.empty() ? nullptr : dynamic_cast<GalacticMode*>(&modes_.at(modes_.depth() - 1));
}

galactic::GalacticSim* SimHost::start_galactic(const galactic::GalacticRules& rules, const galactic::GalacticSetup& setup,
                                               std::vector<std::string>* diagnostics) {
    if (setup.players.size() > cfg_.players) {
        if (diagnostics)
            diagnostics->push_back("galactic setup has " + std::to_string(setup.players.size()) + " players, the session " +
                                   std::to_string(cfg_.players) + " (SimConfig::players, at most " + std::to_string(kMaxPlayers) + ")");
        return nullptr;
    }
    auto& mode = static_cast<GalacticMode&>(start_game(ModeKind::Galactic));
    auto sim = std::make_unique<galactic::GalacticSim>(rules, mode.primary(), random_.synced());
    sim->set_parallel_for(pf_);
    if (!sim->start(setup, diagnostics)) {
        end_game_mode();
        return nullptr;
    }
    mode.attach_sim(std::move(sim));
    return mode.sim();
}

bool SimHost::end_game_mode() {
    if (story_) {
        if (modes_.depth() <= 1) story_.reset();  // the galactic mode (bottom) ends: the story with it
        else if (modes_.top() && modes_.top()->tactical()) story_->host_event(StoryHostEvent::TacticalEnded, frame_);
    }
    if (!modes_.pop()) return false;
    if (modes_.empty()) pacing_.set_stopped(true);
    else pacing_.select_active(modes_.top()->tactical());
    return true;
}

// ------------------------------------------------------------------------------------------- loop
bool SimHost::tick_once() {
    if (modes_.empty()) return false;
    if (recorder_->log().records.empty()) recorder_->capture(frame_);  // initial state (after load)
    // T1 Begin_Tick.
    ++frame_;
    profile::begin_tick(frame_);  // OPT1: wall-clock profiler; nothing below reads it
    step("T1.begin_tick");
    // T2 scheduled commands due this frame (handler runs inside, in drain order).
    batch_crc_ = 0;
    pipeline_->begin_tick(frame_);
    step("T2.commands");
    // T4 TreeCull: rebuild dirty spatial trees.
    if (TacticalMode* tm = modes_.top_tactical()) tm->trees().maintain(sim_time(), pf_);
    step("T4.treecull");
    // T5 GameModeManager::Update (only the top mode ticks).
    ModeTickContext ctx;
    ctx.global_frame = frame_;
    ctx.random = &random_;
    ctx.pf = &pf_;
    ctx.players = cfg_.players;
    ctx.trace = trace_;
    modes_.update(ctx);
    step("T5.gmm_update");
    if (after_mode_update_) {  // S10: tactical battle of a galactic conflict (campaign_battles.hpp)
        after_mode_update_(*this);
        step("T5.battles");
    }
    if (object_anim_) {  // S10-int: sim-owned animation (sim_anim.hpp)
        object_anim_(*this);
        step("T5.anim");
    }
    // T6 scene animations [stub].
    // T7 PlayerList update in player index order (AI controllers issue orders here).
    static const char* const kPlayerZone[] = {"T7.p0", "T7.p1", "T7.p2", "T7.p3", "T7.p4", "T7.p5", "T7.p6", "T7.p7", "T7.p8+"};
    for (uint32_t p = 0; p < cfg_.players; ++p) {
        if (controllers_[p]) controllers_[p](*this, static_cast<uint8_t>(p), frame_);
        profile::lap(kPlayerZone[p < 8 ? p : 8]);  // per-player AI / controller time
    }
    step("T7.players");
    // --- P4 host (owner: game host agent): presentation tap before the story drains the galactic events ---
    if (before_story_) before_story_(*this);
    // --- end P4 host ---
    // T8 global Lua [stub]. T9 story (S4): story time = galactic mode time (GALACTIC_ECONOMY 8) = the T9 time argument
    // (float)(mode frame) x seconds-per-frame (STORY.md 4.7 / 7.3), a pure function of the synced frame count.
    if (story_) {
        const GalacticMode* g = galactic_mode();
        const double mode_seconds = StorySession::t9_seconds(g ? g->frame() : frame_);
        story_->set_command_pipeline(pipeline_.get());  // script orders: execute_direct at this frame
        story_->service(frame_, mode_seconds);
        step("T9.story");
    }
    // --- OI host (owner: game host agent): pacing / parity inputs from this tick's presentation state ---
    if (after_story_) after_story_(*this);
    // --- end OI host ---
    // T10 tutorial, T11 campaign end [stubs].
    // T12 Post_Tick.
    modes_.post_tick();
    step("T12.post_tick");
    // T13 End_Tick: events, frame CRC.
    pipeline_->events().deliver();
    recorder_->capture(frame_);
    step("T13.end_tick");
    profile::end_tick();
    return true;
}

uint64_t SimHost::run_fast(uint64_t ticks) {
    uint64_t n = 0;
    while (n < ticks && tick_once()) ++n;
    return n;
}

bool SimHost::loop_iteration(uint32_t now_ms) {
    pacing_.set_stopped(modes_.empty());
    const uint64_t f0 = frame_;
    const bool ticked = pacing_.pace(now_ms) && tick_once();
    // R1: d = (frame_now - F0) x 30 / 1000 (0.03 per tick, as the original).
    const double d = static_cast<double>(frame_ - f0) * static_cast<double>(kSimTicksPerSecond) / 1000.0;
    if (GameMode* t = modes_.top()) t->per_frame_update(d);
    return ticked;
}

uint64_t SimHost::run_realtime(uint64_t ticks, const std::function<uint32_t()>& now_ms,
                               const std::function<void(uint32_t)>& sleep_ms, uint32_t fps_cap,
                               const std::function<void()>& after_tick) {
    const uint32_t cap = fps_cap < 15u ? 15u : fps_cap;
    const uint32_t target = 1000u / cap - 1u;  // FPS cap: floor(1000 / limit) - 1 ms
    uint64_t done = 0;
    while (done < ticks && !modes_.empty()) {
        if (loop_iteration(now_ms())) {
            ++done;
            if (after_tick) after_tick();
        }
        const uint32_t t = now_ms();
        if (have_present_ && t - last_present_ms_ < target) sleep_ms(target - (t - last_present_ms_));
        last_present_ms_ = now_ms();
        have_present_ = true;
    }
    return done;
}

// --------------------------------------------------------------------------------------- commands
cmd::SubmitResult SimHost::submit(uint8_t player, cmd::EventData ev) {
    return pipeline_->submit(player, cmd::CommandSource::Player, std::move(ev));
}

cmd::RejectReason SimHost::execute_direct(uint8_t player, cmd::CommandSource source, const cmd::EventData& ev) {
    return pipeline_->execute_direct(frame_, player, source, ev);
}

void SimHost::record_to(cmd::CommandRecording* rec) {
    record_ = rec;
    if (rec) rec->seed = cfg_.seed;
    pipeline_->record_to(rec);
}

void SimHost::replay_from(const cmd::CommandRecording* rec) {
    if (!rec) {
        pipeline_->replay_from(nullptr);
        return;
    }
    cfg_.seed = static_cast<uint32_t>(rec->seed);
    cfg_.scheduler = rec->config;
    make_pipeline(rec->config);
    pipeline_->replay_from(rec);
}

uint32_t SimHost::wire_id(ObjectHandle h) const {
    const GameMode* t = modes_.top();
    if (!t || !t->primary().exists(h)) return cmd::kNoObject;
    return static_cast<uint32_t>(t->primary().id(h));
}

ObjectHandle SimHost::resolve(uint32_t wire) const {
    const GameMode* t = modes_.top();
    if (!t || wire == cmd::kNoObject || wire > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) return {};
    const ObjectHandle h = t->primary().find(static_cast<ObjectId>(wire));
    return t->primary().active(h) ? h : ObjectHandle{};  // refix t01: dying / removing objects (death clones) are not targets
}

std::vector<ObjectHandle> SimHost::owned_units(uint8_t player, const std::vector<uint32_t>& ids) const {
    std::vector<ObjectHandle> out;
    const GameMode* t = modes_.top();
    if (!t) return out;
    for (uint32_t id : ids) {
        const ObjectHandle h = resolve(id);
        if (!h || t->primary().owner(h) != static_cast<int32_t>(player)) continue;
        if (std::find(out.begin(), out.end(), h) == out.end()) out.push_back(h);
    }
    return out;
}

cmd::RejectReason SimHost::handle(uint64_t tick, const cmd::Command& c) {
    GameMode* top = modes_.top();
    GalacticMode* g = top && !top->tactical() ? static_cast<GalacticMode*>(top) : nullptr;
    // --- P host (owner: game host agent): port-only events (PortExtension 63) the game host consumes itself, in any mode ---
    if (port_hook_ && c.payload.type() == cmd::EventType::PortExtension) {
        const cmd::RejectReason pr = port_hook_(*this, c.header.player, c.payload);
        if (pr != cmd::RejectReason::Unhandled) {
            if (pr == cmd::RejectReason::None) ++applied_;
            else ++rejected_;
            return pr;
        }
    }
    // --- end P host ---
    cmd::RejectReason r = (g && g->sim()) ? g->sim()->handle_command(tick, c) : apply(c.header.player, c.payload);
    if (r == cmd::RejectReason::Unhandled && g && g->sim() && galactic_cmd_) r = galactic_cmd_(*this, c.header.player, c.payload);  // Int-final
    // --- P4 host (owner: game host agent): an original event no system handled (e.g. Withdrawl 49) goes to the port hook ---
    if (r == cmd::RejectReason::Unhandled && port_hook_ && c.payload.type() != cmd::EventType::PortExtension)
        r = port_hook_(*this, c.header.player, c.payload);
    // --- end P4 host ---
    if (r == cmd::RejectReason::None) ++applied_;
    else ++rejected_;
    return r;
}

void SimHost::station_replaced(ObjectId old_station, ObjectId new_station) {
    for (uint32_t p = 0; p < cfg_.players && p < kMaxPlayers; ++p) {
        std::vector<ObjectId>& sel = selection_[p];
        if (std::find(sel.begin(), sel.end(), old_station) != sel.end() &&
            std::find(sel.begin(), sel.end(), new_station) == sel.end())
            sel.push_back(new_station);
        for (std::vector<ObjectId>& g : groups_[p])
            if (std::find(g.begin(), g.end(), old_station) != g.end() && std::find(g.begin(), g.end(), new_station) == g.end())
                g.insert(std::lower_bound(g.begin(), g.end(), new_station), new_station);
    }
}

cmd::RejectReason SimHost::apply(uint8_t player, const cmd::EventData& ev) {
    using R = cmd::RejectReason;
    using T = cmd::EventType;
    GameMode* top = modes_.top();
    if (!top) return R::NotAllowed;
    if (player >= cfg_.players) return R::InvalidPlayer;
    objects::ObjectStore& store = top->primary();
    TacticalMode* tm = modes_.top_tactical();
    std::vector<ObjectId>& sel = selection_[player];
    auto selection_ids = [&sel] {
        std::vector<uint32_t> v;
        v.reserve(sel.size());
        for (ObjectId id : sel) v.push_back(static_cast<uint32_t>(id));
        return v;
    };
    auto order_all = [&](const std::vector<ObjectHandle>& units, UnitOrder o, bool need_locomotor) {
        uint32_t n = 0;
        for (ObjectHandle h : units) {
            if (need_locomotor && !tm->movable(h)) continue;
            tm->set_order(store.id(h), o);
            ++n;
        }
        return n;
    };
    // --- P host (owner: game host agent): human unit orders into the running battle (set_order_hook) ---
    if (tm && order_hook_) {
        const T t = ev.type();
        if (t == T::MoveToPosition || t == T::MoveToPositionFacing || t == T::MoveToRay || t == T::MoveToRayFacing || t == T::Facing ||
            t == T::MoveToObject || t == T::Attack || t == T::Escort || t == T::StopMovement || t == T::Retreat || t == T::Reinforce) {
            const R r = order_hook_(*this, player, ev, sel);
            if (r == R::None) story_order_feed(player, ev, sel);  // S4: STORY_COMMAND_UNIT / STORY_GUARD_UNIT
            if (r != R::Unhandled) return r;
        }
    }
    if (tm && ev.type() == T::ControlGroup) {  // control groups (sim_host.hpp control_group)
        std::vector<ObjectId>& grp = groups_[player][ev.u("group") % 10u];
        if (ev.u("assign") != 0) {
            if (ev.u("add") == 0) grp.clear();
            for (const ObjectId id : sel)
                if (std::find(grp.begin(), grp.end(), id) == grp.end()) grp.push_back(id);
            std::sort(grp.begin(), grp.end());
            groups_used_ = true;
            return R::None;
        }
        std::vector<uint32_t> wire;
        for (const ObjectId id : grp) wire.push_back(static_cast<uint32_t>(id));
        sel.clear();
        for (ObjectHandle h : owned_units(player, wire)) sel.push_back(store.id(h));
        grp = sel;  // dead / lost members leave the group
        return sel.empty() ? R::InvalidUnit : R::None;
    }
    // --- end P host ---
    switch (ev.type()) {
        case T::Select: {
            const auto& ids = ev.ids("ids");
            sel.clear();
            for (ObjectHandle h : owned_units(player, ids)) sel.push_back(store.id(h));
            return (!ids.empty() && sel.empty()) ? R::InvalidUnit : R::None;
        }
        case T::SelectAll: {
            sel.clear();
            for (ObjectHandle h : store.owned_by(player))
                if (store.active(h) && store.has_behavior(h, objects::beh::Selectable)) sel.push_back(store.id(h));
            return R::None;
        }
        case T::MoveToPosition: {
            if (!tm) return R::NotAllowed;
            const float x = ev.f("x"), y = ev.f("y"), z = ev.f("z");
            const tactical::Aabb& b = tm->bounds();
            if (!finite(x) || !finite(y) || !finite(z) || x < b.min.x || x > b.max.x || y < b.min.y || y > b.max.y)
                return R::InvalidPosition;
            UnitOrder o;
            o.kind = OrderKind::Move;
            o.player = player;
            o.issued_frame = frame_;
            o.goal = {x, y, z};
            return order_all(owned_units(player, selection_ids()), o, true) ? R::None : R::InvalidUnit;
        }
        case T::MoveToObject: {
            if (!tm) return R::NotAllowed;
            const ObjectHandle target = resolve(ev.u("target"));
            if (!target) return R::InvalidTarget;
            const auto& movers = ev.ids("movers");
            UnitOrder o;
            o.kind = OrderKind::MoveToObject;
            o.player = player;
            o.issued_frame = frame_;
            o.target = store.id(target);
            return order_all(owned_units(player, movers.empty() ? selection_ids() : movers), o, true) ? R::None
                                                                                                       : R::InvalidUnit;
        }
        case T::Attack: {
            if (!tm) return R::NotAllowed;
            const ObjectHandle target = resolve(ev.u("target"));
            if (!target || store.owner(target) == static_cast<int32_t>(player)) return R::InvalidTarget;
            const auto& attackers = ev.ids("attackers");
            UnitOrder o;
            o.kind = OrderKind::Attack;
            o.player = player;
            o.issued_frame = frame_;
            o.target = store.id(target);
            return order_all(owned_units(player, attackers.empty() ? selection_ids() : attackers), o, false)
                       ? R::None
                       : R::InvalidUnit;
        }
        case T::StopMovement: {
            if (!tm) return R::NotAllowed;
            const auto units = owned_units(player, selection_ids());
            for (ObjectHandle h : units) tm->clear_order(store.id(h));
            return units.empty() ? R::InvalidUnit : R::None;
        }
        default: return (tm && tactical_cmd_) ? tactical_cmd_(*this, player, ev) : R::Unhandled;  // S10-int
    }
}

// --- S4 (owner: S4 agent): order-hook story feed (sim_host.hpp story_order_feed) ---
void SimHost::story_order_feed(uint8_t player, const cmd::EventData& ev, const std::vector<ObjectId>& selection) {
    using T = cmd::EventType;
    StorySession* s = story_.get();
    TacticalMode* tm = modes_.top_tactical();
    const T t = ev.type();
    if (!s || !tm || !s->battle_attached() || s->tactical_player() != static_cast<int32_t>(player)) return;
    if (t != T::MoveToPosition && t != T::MoveToPositionFacing && t != T::MoveToRay && t != T::MoveToRayFacing && t != T::Escort) return;
    const bool ray = t == T::MoveToRay || t == T::MoveToRayFacing;  // P1: the plane point b is the destination
    const objects::ObjectStore& st = tm->primary();
    std::vector<ObjectId> units;
    if (t == T::MoveToPositionFacing && !ev.ids("units").empty()) {
        for (const uint32_t w : ev.ids("units"))
            if (w != cmd::kNoObject && w <= 0x7FFFFFFFu) units.push_back(static_cast<ObjectId>(w));
    } else {
        units = selection;
    }
    ObjectId target = objects::kNoObjectId;
    if (t == T::Escort) {
        const ObjectHandle th = resolve(ev.u("target"));
        if (!th) return;
        target = st.id(th);
    }
    std::vector<ObjectId> done;
    for (const ObjectId id : units) {
        const ObjectHandle h = st.find(id);
        if (!st.active(h) || st.owner(h) != static_cast<int32_t>(player) || id == target) continue;
        if (std::find(done.begin(), done.end(), id) != done.end()) continue;
        done.push_back(id);
        TacticalStoryNotification n;
        n.object = id;
        if (t == T::Escort) {
            n.kind = TacticalStoryEvent::UnitGuarding;
            n.other = target;
        } else {
            n.kind = TacticalStoryEvent::UnitCommanded;
            n.has_position = true;
            const float z = ev.f(ray ? "bz" : "z");
            n.x = ev.f(ray ? "bx" : "x");
            n.y = ev.f(ray ? "by" : "y");
            n.z = finite(z) ? z : st.position(h).z;
        }
        s->notify_tactical(n);
    }
}
// --- end S4 ---

}  // namespace alamo::sim
