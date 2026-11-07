// N1 save / load of a SimHost session (owner: n1-host). Chunk list and load procedure: save_session.hpp.
#include <algorithm>
#include <stdexcept>

#include "alamo/galactic/campaign.hpp"
#include "alamo/sim/sim_host.hpp"
#include "alamo/core/log.hpp"
#ifdef ALAMO_SIM_HAVE_STORY
#include "alamo/lua/script_remaining.hpp"  // GlobalValueStore, process_global_values
#include "alamo/lua/script_save.hpp"       // q6 t19: the GameScoring script block
#if defined(ALAMO_LUA_PLAYERS)
#include "alamo/lua/players.hpp"
#endif
#endif

namespace alamo::sim {

namespace {

constexpr uint32_t kChunkVersion = 1;  // envelope version of every host chunk (sub-systems version their own bytes)
constexpr uint32_t kMaxCount = 1u << 24;

// Host chunk names in save / load order (a story session adds its four after "globals").
constexpr const char* kHostChunks[] = {"frame", "objects", "galactic.state", "players", "rng", "commands", "globals"};
constexpr const char* kStoryChunks[] = {"story", "story.lua", "story.bridge", "story.players", "story.presentation"};
// "spatial" (stage 2) follows "objects" while a tactical mode is on the stack; q6 t19: "scoring" (a GameScoring script
// is configured) and "object_scripts" (the object-script host exists) follow the story chunks.
constexpr const char* kReserved[] = {"session", "spatial", "scoring", "object_scripts"};
constexpr uint32_t kScoringSaveVersion = 1;  // the "scoring" chunk's own bytes

bool valid_name(std::string_view n) {
    if (n.empty()) return false;
    for (const char c : n)
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') return false;
    return true;
}

void write_ids(ByteWriter& w, const std::vector<ObjectId>& ids) {
    w.u32(static_cast<uint32_t>(ids.size()));
    for (const ObjectId id : ids) w.i32(id);
}

bool read_ids(ByteReader& r, std::vector<ObjectId>& ids) {
    const uint32_t n = r.u32();
    if (!r.ok() || n > kMaxCount) return false;
    ids.assign(n, kNoObjectId);
    for (ObjectId& id : ids) id = r.i32();
    return r.ok();
}

}  // namespace

// ------------------------------------------------------------------------------------------ descriptor
SessionDescriptor SimHost::session() const {
    SessionDescriptor d = session_;
    d.seed = cfg_.seed;
    d.players = cfg_.players;
    d.scheduler = cfg_.scheduler;
    d.story_crc = cfg_.story_crc;
    if (d.writer.empty()) d.writer = "alamo engine (N1 stage 2)";
    return d;
}

void SimHost::add_save_participant(SaveParticipant p) {
    if (!valid_name(p.name)) throw std::invalid_argument("save participant: bad chunk name '" + p.name + "'");
    if (!p.write || !p.read) throw std::invalid_argument("save participant " + p.name + ": write and read are required");
    auto taken = [&p](const char* n) { return p.name == n; };
    if (std::any_of(std::begin(kHostChunks), std::end(kHostChunks), taken) ||
        std::any_of(std::begin(kStoryChunks), std::end(kStoryChunks), taken) ||
        std::any_of(std::begin(kReserved), std::end(kReserved), taken))
        throw std::invalid_argument("save participant: '" + p.name + "' is a host chunk name");
    for (const SaveParticipant& q : participants_)
        if (q.name == p.name) throw std::invalid_argument("save participant: duplicate chunk name '" + p.name + "'");
    participants_.push_back(std::move(p));
}

void SimHost::remove_save_participant(std::string_view name) {
    participants_.erase(std::remove_if(participants_.begin(), participants_.end(),
                                       [name](const SaveParticipant& p) { return p.name == name; }),
                        participants_.end());
}

bool SimHost::save_allowed(std::string* why) const {
    auto no = [why](std::string m) {
        if (why) *why = std::move(m);
        return false;
    };
    if (modes_.empty()) return no("no session running");
    // Stage 2: a tactical mode saves when an attached system can push it again at load (SaveParticipant::rebuild).
    if (has_tactical_mode() &&
        std::none_of(participants_.begin(), participants_.end(), [](const SaveParticipant& p) { return static_cast<bool>(p.rebuild); }))
        return no("a tactical mode is on the stack but no attached system can rebuild it at load (no battle host)");
    if (story_) {
        const std::string b = story_->save_blocker();
        if (!b.empty()) return no(b);
    }
    // q6 t19: game-object Lua scripts are saved ("object_scripts", OBJECT_SCRIPTS.md §2.6), so they no longer block.
    for (const SaveParticipant& p : participants_)
        if (p.blocker) {
            const std::string b = p.blocker();
            if (!b.empty()) return no(p.name + ": " + b);
        }
    return true;
}

// ---------------------------------------------------------------------------------------- host chunks
void SimHost::write_frame(ByteWriter& w) const {
    w.u32(kChunkVersion);
    w.u64(frame_).u64(batch_crc_).u64(applied_).u64(rejected_);
    const auto depth = static_cast<uint32_t>(modes_.depth());
    w.u32(depth);
    for (uint32_t i = depth; i-- > 0;) {  // bottom -> top
        const GameMode& m = modes_.at(i);
        write_section(w, [&m](ByteWriter& sub) { m.write_state(sub); });
    }
}

bool SimHost::read_frame(ByteReader& r) {
    if (r.u32() != kChunkVersion) return false;
    const uint64_t frame = r.u64(), batch = r.u64(), applied = r.u64(), rejected = r.u64();
    const uint32_t depth = r.u32();
    const auto have = static_cast<uint32_t>(modes_.depth());
    // Load pass 1 (load_base_depth_ != 0): the bottom modes only, the saved upper ones are pushed by the rebuild.
    if (!r.ok() || (load_base_depth_ ? (depth < load_base_depth_ || have != load_base_depth_) : depth != have)) return false;
    for (uint32_t k = 0; k < have; ++k) {  // bottom -> top
        GameMode& m = modes_.at(have - 1 - k);
        if (!read_section(r, [&m](ByteReader& sub) { return m.read_state(sub); })) return false;
    }
    if (!r.ok() || (!load_base_depth_ && !r.done())) return false;
    frame_ = frame;
    batch_crc_ = batch;
    applied_ = applied;
    rejected_ = rejected;
    return true;
}

void SimHost::write_objects(ByteWriter& w) const {
    w.u32(kChunkVersion);
    w.i32(modes_.ids().next());
    const auto depth = static_cast<uint32_t>(modes_.depth());
    w.u32(depth);
    for (uint32_t i = depth; i-- > 0;) {
        const GameMode& m = modes_.at(i);
        w.u8(static_cast<uint8_t>(m.kind()));
        write_section(w, [&m](ByteWriter& sub) { m.primary().write(sub); });  // secondary stores: cosmetic, not saved
    }
}

bool SimHost::read_objects(ByteReader& r) {
    if (r.u32() != kChunkVersion) return false;
    const ObjectId next = r.i32();
    const uint32_t depth = r.u32();
    const auto have = static_cast<uint32_t>(modes_.depth());
    if (!r.ok() || next < 1 || (load_base_depth_ ? (depth < load_base_depth_ || have != load_base_depth_) : depth != have)) return false;
    for (uint32_t k = 0; k < have; ++k) {  // bottom -> top
        GameMode& m = modes_.at(have - 1 - k);
        if (r.u8() != static_cast<uint8_t>(m.kind())) return false;
        if (!read_section(r, [&m](ByteReader& sub) { return m.primary().read(sub); })) return false;
    }
    if (!r.ok() || (!load_base_depth_ && !r.done())) return false;
    modes_.ids().set_next(next);
    return true;
}

bool SimHost::has_tactical_mode() const {
    for (size_t i = 0; i < modes_.depth(); ++i)
        if (modes_.at(i).tactical()) return true;
    return false;
}

// "spatial": per mode bottom -> top a tactical flag + that mode's TreeSet (the CRC "spatial" covers the top tactical one).
void SimHost::write_spatial(ByteWriter& w) const {
    w.u32(kChunkVersion);
    const auto depth = static_cast<uint32_t>(modes_.depth());
    w.u32(depth);
    for (uint32_t i = depth; i-- > 0;) {
        const auto* tm = dynamic_cast<const TacticalMode*>(&modes_.at(i));
        w.boolean(tm != nullptr);
        if (tm) write_section(w, [tm](ByteWriter& sub) { write_if_saveable(tm->trees(), sub); });
    }
}

bool SimHost::read_spatial(ByteReader& r) {
    if (r.u32() != kChunkVersion) return false;
    const uint32_t depth = r.u32();
    if (!r.ok() || depth != modes_.depth()) return false;
    for (uint32_t i = depth; i-- > 0;) {
        auto* tm = dynamic_cast<TacticalMode*>(&modes_.at(i));
        if (r.boolean() != (tm != nullptr) || !r.ok()) return false;
        if (tm && !read_section(r, [tm](ByteReader& sub) { return read_if_saveable(tm->trees(), sub); })) return false;
    }
    return r.ok() && r.done();
}

void SimHost::write_galactic(ByteWriter& w) const {
    w.u32(kChunkVersion);
    const GalacticMode* g = modes_.empty() ? nullptr : dynamic_cast<const GalacticMode*>(&modes_.at(modes_.depth() - 1));
    const galactic::GalacticSim* s = g ? g->sim() : nullptr;
    w.boolean(s != nullptr);
    if (s) write_section(w, [s](ByteWriter& sub) { s->write(sub); });
}

bool SimHost::read_galactic(ByteReader& r) {
    if (r.u32() != kChunkVersion) return false;
    GalacticMode* g = galactic_mode();
    galactic::GalacticSim* s = g ? g->sim() : nullptr;
    const bool has = r.boolean();
    if (!r.ok() || has != (s != nullptr)) return false;
    if (s && !read_section(r, [s](ByteReader& sub) { return s->read(sub); })) return false;
    return r.ok() && r.done();
}

void SimHost::write_players(ByteWriter& w) const {
    w.u32(kChunkVersion);
    w.u32(cfg_.players);
    for (uint32_t p = 0; p < cfg_.players; ++p) write_ids(w, selection_[p]);
    w.boolean(groups_used_);
    for (uint32_t p = 0; p < cfg_.players; ++p)
        for (const std::vector<ObjectId>& g : groups_[p]) write_ids(w, g);
}

bool SimHost::read_players(ByteReader& r) {
    if (r.u32() != kChunkVersion) return false;
    if (r.u32() != cfg_.players || !r.ok()) return false;
    std::array<std::vector<ObjectId>, kMaxPlayers> sel;
    std::array<std::array<std::vector<ObjectId>, 10>, kMaxPlayers> groups;
    for (uint32_t p = 0; p < cfg_.players; ++p)
        if (!read_ids(r, sel[p])) return false;
    const bool used = r.boolean();
    for (uint32_t p = 0; p < cfg_.players; ++p)
        for (std::vector<ObjectId>& g : groups[p])
            if (!read_ids(r, g)) return false;
    if (!r.ok() || !r.done()) return false;
    selection_ = std::move(sel);
    groups_ = std::move(groups);
    groups_used_ = used;
    return true;
}

void SimHost::write_globals(ByteWriter& w) const {
    w.u32(kChunkVersion);
#ifdef ALAMO_SIM_HAVE_STORY
    w.boolean(true);
    write_section(w, [this](ByteWriter& sub) { global_values_.write(sub); });
    write_section(w, [](ByteWriter& sub) { lua::process_global_values().write(sub); });  // VMs without a session table
#else
    w.boolean(false);
#endif
}

bool SimHost::read_globals(ByteReader& r) {
    if (r.u32() != kChunkVersion) return false;
    const bool has = r.boolean();
#ifdef ALAMO_SIM_HAVE_STORY
    if (!r.ok() || !has) return false;
    // Parsed into scratch tables first: the chunk loads both or neither.
    lua::GlobalValueStore session, process;
    if (!read_section(r, [&session](ByteReader& sub) { return session.read(sub); })) return false;
    if (!read_section(r, [&process](ByteReader& sub) { return process.read(sub); })) return false;
    if (!r.done()) return false;
    global_values_ = std::move(session);
    lua::process_global_values() = std::move(process);
    return true;
#else
    return r.ok() && !has && r.done();
#endif
}

// "scoring" (q6 t19, LUA_SAVE.md; GAME_SCORING.md §6): u32 envelope, u32 kScoringSaveVersion, str script name, u8 has VM
// (false: released before the save); with a VM: u32 length + the lua::write_script_block bytes, u8 own player world
// (+ u32 length + LuaPlayerWorld::write: the world start_galactic gives the VM while no story runs); then the queued
// Game_Mode_Starting_Event (mode, map) pairs, the tapped galactic events not delivered yet, the failed flag and the
// service / call / release counters. The load reads the block into the rebuilt session's scoring VM (ScriptLoad::Any:
// its new-game state is replaced whole, the host attachments - GlobalValue, players, bridge, presentation - kept).
bool SimHost::scoring_configured() const {
#ifdef ALAMO_SIM_HAVE_STORY
    return !scoring_name_.empty() && scoring_files_ != nullptr;
#else
    return false;
#endif
}

bool SimHost::write_scoring(ByteWriter& w, std::string* err) const {
    w.u32(kChunkVersion).u32(kScoringSaveVersion);
    w.str(scoring_name_);
#ifdef ALAMO_SIM_HAVE_STORY
    w.boolean(scoring_vm_ != nullptr);
    if (scoring_vm_) {
        ByteWriter block;
        lua::write_script_block(block, *scoring_vm_);
        if (!scoring_vm_->save_error().empty()) {
            if (err) *err = "GameScoring script " + scoring_name_ + ": " + scoring_vm_->save_error();
            return false;
        }
        w.u32(static_cast<uint32_t>(block.size())).raw(block.bytes());
#if defined(ALAMO_LUA_PLAYERS)
        w.boolean(scoring_players_ != nullptr);
        if (scoring_players_) write_section(w, [this](ByteWriter& sub) { scoring_players_->write(sub); });
#else
        w.boolean(false);
#endif
    }
    w.u32(static_cast<uint32_t>(scoring_starts_.size()));
    for (const auto& [mode, map] : scoring_starts_) w.str(mode).str(map);
    w.u32(static_cast<uint32_t>(scoring_events_.size()));
    for (const galactic::GalacticEvent& e : scoring_events_) {
        w.i32(e.frame).u8(static_cast<uint8_t>(e.kind)).i32(e.player).i32(e.other_player);
        w.u32(e.planet).u32(e.other_planet).u32(e.fleet).u32(e.type).i32(e.object).i32(e.value);
    }
    w.boolean(scoring_failed_);
    w.u64(scoring_services_).u64(scoring_calls_).u64(scoring_releases_);
#else
    (void)err;
    w.boolean(false);
#endif
    return true;
}

bool SimHost::read_scoring(ByteReader& r) {
    if (r.u32() != kChunkVersion || r.u32() != kScoringSaveVersion) return false;
    const std::string name = r.str();
    const bool has_vm = r.boolean();
    if (!r.ok() || name != scoring_name_) return false;
#ifdef ALAMO_SIM_HAVE_STORY
    std::span<const uint8_t> block, world_bytes;
    bool own_players = false;
    if (has_vm) {
        const uint32_t n = r.u32();
        if (!r.ok() || n > r.remaining()) return false;
        block = r.raw(n);
        own_players = r.boolean();
        if (own_players) {
            const uint32_t pn = r.u32();
            if (!r.ok() || pn > r.remaining()) return false;
            world_bytes = r.raw(pn);
        }
    }
    const uint32_t ns = r.u32();
    if (!r.ok() || ns > kMaxCount) return false;
    std::vector<std::pair<std::string, std::string>> starts(ns);
    for (auto& [mode, map] : starts) {
        mode = r.str();
        map = r.str();
    }
    const uint32_t ne = r.u32();
    if (!r.ok() || ne > kMaxCount) return false;
    std::vector<galactic::GalacticEvent> events(ne);
    for (galactic::GalacticEvent& e : events) {
        e.frame = r.i32();
        const uint8_t kind = r.u8();
        if (kind >= static_cast<uint8_t>(galactic::GalacticEventKind::Count)) return false;
        e.kind = static_cast<galactic::GalacticEventKind>(kind);
        e.player = r.i32();
        e.other_player = r.i32();
        e.planet = r.u32();
        e.other_planet = r.u32();
        e.fleet = r.u32();
        e.type = r.u32();
        e.object = r.i32();
        e.value = r.i32();
    }
    const bool failed = r.boolean();
    const uint64_t services = r.u64(), calls = r.u64(), releases = r.u64();
    if (!r.ok() || !r.done()) return false;
    if (has_vm) {
        if (!scoring_vm_) return false;  // the rebuilt session has no scoring VM to restore into
#if defined(ALAMO_LUA_PLAYERS)
        if (own_players) {
            if (!scoring_players_) return false;
            ByteReader pr(world_bytes);
            if (!scoring_players_->read(pr) || !pr.ok() || !pr.done()) return false;
        }
#else
        (void)world_bytes;
        if (own_players) return false;
#endif
        ByteReader br(block);
        std::string e;
        if (!lua::read_script_block(br, *scoring_vm_, lua::ScriptLoad::Any, &e)) {
            log::write(log::Level::Warn, "sim", "load: GameScoring script " + scoring_name_ + ": " + e);
            return false;
        }
        scoring_seen_messages_ = scoring_vm_->messages().size();
        scoring_seen_errors_ = scoring_vm_->errors().size();
    } else {
        scoring_vm_.reset();  // released before the save
        scoring_players_.reset();
    }
    scoring_starts_ = std::move(starts);
    scoring_events_ = std::move(events);
    scoring_failed_ = failed;
    scoring_services_ = services;
    scoring_calls_ = calls;
    scoring_releases_ = releases;
    return true;
#else
    return !has_vm && r.ok() && r.done();
#endif
}

// -------------------------------------------------------------------------------------------- save
bool SimHost::save(SaveFile& out, std::string* err) const {
    std::string why;
    if (!save_allowed(&why)) {
        if (err) *err = "save not possible: " + why;
        return false;
    }
    SaveFile f;
    f.tick = frame_;
    f.description = (session_.campaign.empty() ? std::string("session") : session_.campaign) + ", frame " + std::to_string(frame_);
    auto add = [&f](const char* name, uint32_t version, const ByteWriter& w) { f.add(name, version, w); };
    {
        ByteWriter w;
        session().write(w);
        add("session", SessionDescriptor::kVersion, w);
    }
    for (const char* name : kHostChunks) {
        ByteWriter w;
        const std::string_view n = name;
        if (n == "frame") write_frame(w);
        else if (n == "objects") write_objects(w);
        else if (n == "galactic.state") write_galactic(w);
        else if (n == "players") write_players(w);
        else if (n == "rng") random_.write(w);
        else if (n == "commands") pipeline_->write(w);
        else if (n == "globals") write_globals(w);
        add(name, kChunkVersion, w);
        if (n == "objects" && has_tactical_mode()) {
            ByteWriter sw;
            write_spatial(sw);
            add("spatial", kChunkVersion, sw);
        }
    }
    if (story_) {
        ByteWriter s, l, b, p, pr;
        story_->write(s);
        story_->write_scripts(l);
        story_->write_bridge(b);
        story_->write_players(p);
        story_->write_presentation(pr);  // N1 stage 2
        add("story", kChunkVersion, s);
        add("story.lua", kChunkVersion, l);
        add("story.bridge", kChunkVersion, b);
        add("story.players", kChunkVersion, p);
        add("story.presentation", kChunkVersion, pr);
    }
    if (scoring_configured()) {  // q6 t19
        ByteWriter w;
        if (!write_scoring(w, &why)) {
            if (err) *err = "save not possible: " + why;
            return false;
        }
        add("scoring", kChunkVersion, w);
    }
    if (object_scripts_) {  // q6 t19
        ByteWriter w;
        if (!object_scripts_->write(w, &why)) {
            if (err) *err = "save not possible: " + why;
            return false;
        }
        add("object_scripts", kChunkVersion, w);
    }
    for (const SaveParticipant& p : participants_) {
        ByteWriter w;
        p.write(w);
        f.add(p.name, p.version, w);
    }
    out = std::move(f);
    return true;
}

// -------------------------------------------------------------------------------------------- load
// Reads every chunk this session expects (in dependency order: stores before the systems holding handles into them)
// and lists the outcome in `rep` (loaded / failed / missing / unexpected).
void SimHost::read_chunks(const SaveFile& in, LoadReport& rep) {
    std::vector<std::string> expected;
    for (const char* name : kHostChunks) {
        expected.emplace_back(name);
        if (std::string_view(name) == "objects" && has_tactical_mode()) expected.emplace_back("spatial");
    }
    if (story_) expected.insert(expected.end(), std::begin(kStoryChunks), std::end(kStoryChunks));
    if (scoring_configured()) expected.emplace_back("scoring");             // q6 t19
    if (object_scripts_) expected.emplace_back("object_scripts");          // q6 t19
    for (const SaveParticipant& p : participants_) expected.push_back(p.name);
    for (const std::string& name : expected) {
        const SaveChunk* c = in.find(name);
        if (!c) {
            rep.missing.push_back(name);
            continue;
        }
        const SaveParticipant* part = nullptr;
        for (const SaveParticipant& p : participants_)
            if (p.name == name) part = &p;
        const uint32_t version = part ? part->version : kChunkVersion;
        bool ok = c->version == version;
        if (ok) {
            ByteReader r(c->data);
            if (part) ok = part->read(r);
            else if (name == "frame") ok = read_frame(r);
            else if (name == "objects") ok = read_objects(r);
            else if (name == "spatial") ok = read_spatial(r);
            else if (name == "galactic.state") ok = read_galactic(r);
            else if (name == "players") ok = read_players(r);
            else if (name == "rng") ok = random_.read(r);
            else if (name == "commands") ok = pipeline_->read(r);
            else if (name == "globals") ok = read_globals(r);
            else if (name == "story") ok = story_->read(r);
            else if (name == "story.lua") ok = story_->read_scripts(r);
            else if (name == "story.bridge") ok = story_->read_bridge(r);
            else if (name == "story.players") ok = story_->read_players(r);
            else if (name == "story.presentation") ok = story_->read_presentation(r);
            else if (name == "scoring") ok = read_scoring(r);
            else if (name == "object_scripts") {
                // Pass 1 of a battle load skips it: the battle's store is pushed by the rebuild, pass 2 restores every store.
                std::string why;
                ok = load_base_depth_ != 0 || object_scripts_->read(r, &why);
                if (!ok) log::write(log::Level::Warn, "sim", "load: object_scripts: " + why);
            }
            else ok = false;
            ok = ok && r.ok() && (r.done() || load_base_depth_ != 0);
        }
        (ok ? rep.loaded : rep.failed).push_back(name);
    }
    for (const SaveChunk& c : in.chunks())
        if (c.name != "session" && std::find(expected.begin(), expected.end(), c.name) == expected.end())
            rep.unexpected.push_back(c.name);
}

bool SimHost::load(const SaveFile& in, std::string* err, LoadReport* report) {
    LoadReport rep;
    auto finish = [&](bool ok) {
        if (!ok && err) *err = rep.text();
        if (report) *report = rep;
        return ok;
    };
    if (modes_.empty()) {
        rep.descriptor_error = "no session to load into (rebuild the saved session first)";
        return finish(false);
    }
    if (frame_ != 0) {
        rep.descriptor_error = "the session already ran " + std::to_string(frame_) + " ticks (load needs a fresh session)";
        return finish(false);
    }
    SessionDescriptor saved;
    std::string e;
    if (!read_session(in, saved, &e)) {
        rep.descriptor_error = e;
        return finish(false);
    }
    rep.descriptor_error = SessionDescriptor::mismatch(saved, session());
    if (!rep.descriptor_error.empty()) return finish(false);

    // Stage 2: a save taken during a battle has more modes than the rebuilt session (save_session.hpp "LOADING A SAVE
    // TAKEN DURING A BATTLE"): pass 1 restores the bottom modes and the systems' own state, the participants rebuild the
    // upper modes, pass 2 reads everything.
    uint32_t saved_depth = 0;
    if (const SaveChunk* fc = in.find("frame"); fc && fc->version == kChunkVersion) {
        ByteReader r(fc->data);
        (void)r.u32();
        for (int i = 0; i < 4; ++i) (void)r.u64();
        saved_depth = r.u32();
        if (!r.ok()) saved_depth = 0;
    }
    const auto have = static_cast<uint32_t>(modes_.depth());
    if (saved_depth != 0 && saved_depth < have) {
        rep.descriptor_error = "the save has " + std::to_string(saved_depth) + " modes, the rebuilt session " + std::to_string(have);
        return finish(false);
    }
    if (saved_depth > have) {
        LoadReport pass1;
        load_base_depth_ = have;
        read_chunks(in, pass1);
        load_base_depth_ = 0;
        const size_t n = participants_.size();  // rebuild callbacks may register new participants (not rebuilt again)
        for (size_t i = 0; i < n && i < participants_.size(); ++i) {
            const std::function<std::string()> rebuild = participants_[i].rebuild;
            const std::string name = participants_[i].name;
            if (!rebuild) continue;
            const std::string why = rebuild();
            if (!why.empty()) {
                rep.descriptor_error = "rebuild " + name + ": " + why;
                return finish(false);
            }
        }
        if (modes_.depth() != saved_depth) {
            rep.descriptor_error = "the save has " + std::to_string(saved_depth) + " modes, the session after the rebuild " +
                                   std::to_string(modes_.depth()) + " (no attached system rebuilt the saved battle)";
            return finish(false);
        }
    }
    rep.loaded.emplace_back("session");
    read_chunks(in, rep);
    if (rep.ok() && frame_ != in.tick) rep.descriptor_error = "frame chunk (" + std::to_string(frame_) + ") != save tick " +
                                                              std::to_string(in.tick);
    if (!rep.ok()) return finish(false);
    // The CRC log restarts at the loaded frame: the next tick first records the loaded state (tick_once).
    recorder_ = std::make_unique<jobs::CrcRecorder>(*js_, registry_, cfg_.parallel_crc);
    if (story_) story_->set_command_pipeline(pipeline_.get());
    return finish(true);
}

bool SimHost::replay_after_load(const commands::CommandRecording* rec, const commands::SavePoint& at, std::string* err) {
    if (!rec) {
        pipeline_->replay_from(nullptr);
        return true;
    }
    return pipeline_->replay_from_save(rec, at, err);
}

void SimHost::record_after_load(commands::CommandRecording* rec, const commands::SavePoint& at) {
    record_ = rec;
    if (!rec) {
        pipeline_->record_to(nullptr);
        return;
    }
    rec->seed = cfg_.seed;
    pipeline_->record_to(rec, at);
}

}  // namespace alamo::sim
