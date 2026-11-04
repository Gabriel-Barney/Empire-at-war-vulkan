#include "alamo/sim/campaign_battles.hpp"

#include <algorithm>
#include <cstdlib>

#include "alamo/galactic/campaign.hpp"
#include "alamo/galactic/corruption.hpp"  // S4: corruption missions
#ifdef ALAMO_SIM_HAVE_STORY
#include "alamo/sim/sim_anim.hpp"  // L1/S4 wave 2a story hookup (SimAnimation::world)
#endif
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI
#include "alamo/lua/players.hpp"  // a1j t07: TacticalGalacticContext
#endif
#include "save_io.hpp"  // N1 stage 2

namespace alamo::sim {

// --- battle host (BT pass): the story-block gate in front of the ConflictManager (campaign_battles.hpp) ---
class CampaignBattles::ConflictGate final : public galactic::ConflictHooks {
public:
    explicit ConflictGate(CampaignBattles& b) : b_(&b) {}
    void on_space_conflict(PlanetIndex planet, int32_t attacker) override { b_->cm_->on_space_conflict(planet, attacker); }
    bool on_invade(int32_t attacker, PlanetIndex planet, std::span<const objects::ObjectHandle> units) override {
        // story t07: the last-invade vector 0x140a172b0 (InvadeEvent 0x48ff50, every id of the event; human invades only:
        // the AI's task-force invasions call Land_Invade directly) [port: any human invade reaching the conflict hooks].
        GalacticMode* g = b_->host_->galactic_mode();
        if (g && g->sim() && attacker >= 0 && static_cast<size_t>(attacker) < g->sim()->players().size() &&
            g->sim()->players()[static_cast<size_t>(attacker)].human)
            b_->lk_.last_invade.assign(units.begin(), units.end());
        return b_->cm_->on_invade(attacker, planet, units);
    }
    // 0x2b0350: `player` is attacker or defender of a record in either queue. a1j t05 [port]: a corruption mission's own
    // conflict does not count - it is the battle record, which the original keeps out of the queues until it starts
    // (0x52bf50 -> 0x28f590 clears the flag before queueing it), so it never feeds the story block.
    bool in_queued_conflict(int32_t player) const override {
        for (const auto* q : {&b_->cm_->space_queue(), &b_->cm_->land_queue()})
            for (const galactic::ConflictInfo& c : *q)
                if ((c.attacker == player || c.defender == player) && !b_->mission_record(c)) return true;
        return false;
    }
    void on_service_tail() override {
        // BT §1.4 step 2 (0x2be640, a1j t05): while the story block holds (GalacticSim::story_block: a linked battle record
        // waits and the local player is attacker or defender of a queued conflict) nothing of the conflict processing runs
        // this tick. [port] Not while a corruption mission's conflict is queued: the original starts that record at once,
        // the port runs it through the queue, so blocking would hold it forever.
        GalacticMode* g = b_->host_->galactic_mode();
        if (g && g->sim() && g->sim()->story_block() && !b_->mission_queued()) return;
        b_->cm_->on_service_tail();
    }

private:
    CampaignBattles* b_;
};

bool CampaignBattles::mission_record(const galactic::ConflictInfo& c) const {
    for (const CorruptionMission& m : corruption_queued_)
        if (m.planet == c.planet && m.player == c.attacker && m.kind == c.kind) return true;
    return false;
}

bool CampaignBattles::mission_queued() const {
    for (const auto* q : {&cm_->space_queue(), &cm_->land_queue()})
        for (const galactic::ConflictInfo& c : *q)
            if (mission_record(c)) return true;
    return false;
}
// --- end battle host ---

CampaignBattles::MapLoader CampaignBattles::file_map_loader(const FileProvider& files) {
    const FileProvider* f = &files;
    return [f](const std::string& name, terrain::MapData& out) { return terrain::load_map(*f, name, out); };
}

CampaignBattles::CampaignBattles(SimHost& host, const galactic::ConflictRules& rules, const BattleCatalog& catalog,
                                 MapLoader loader, CampaignBattleOptions options)
    : host_(&host), rules_(&rules), catalog_(&catalog), loader_(std::move(loader)), opt_(options) {}

CampaignBattles::~CampaignBattles() {
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI
    if (lua_close_wait_) lua_close_wait_();
#endif
    if (cm_) {  // N1: the save participants attach() registered
        host_->remove_save_participant("galactic.conflicts");
        host_->remove_save_participant("battles");
    }
    remove_battle_chunks();  // N1 stage 2
    host_->set_after_mode_update({});
    host_->set_tactical_command_hook({});
    host_->set_select_all_hook({});        // q6
    host_->set_galactic_command_hook({});  // Int-final
    if (gate_)  // battle host: the story-block gate was installed in front of the manager
        if (GalacticMode* g = host_->galactic_mode(); g && g->sim() && g->sim()->conflict_hooks() == gate_.get()) g->sim()->set_conflict_hooks(nullptr);
    if (cm_) cm_->detach();
#ifdef ALAMO_SIM_HAVE_STORY
    if (story_fog_session_ && story_fog_session_ == host_->story()) story_fog_session_->set_enable_fow_hook({});  // fog follow-up
#endif
}

bool CampaignBattles::attach(std::vector<std::string>* diag) {
    GalacticMode* g = host_->galactic_mode();
    if (!g || !g->sim()) {
        if (diag) diag->push_back("battles: no galactic campaign running");
        return false;
    }
    cm_ = std::make_unique<galactic::ConflictManager>(*g->sim(), *rules_, host_->random().synced(), opt_.conflict);
    cm_->pause = [this](bool raise) {
        GalacticMode* gm = host_->galactic_mode();
        if (!gm) return;
        if (raise) gm->push_pause();
        else gm->pop_pause();
    };
    cm_->start_battle = [this](const galactic::BattleRequest& r0) {
        // REMAINING_GAPS 6.4: the queued (linked) battle starts here (0x28f590 -> 0x28c640): the story flag is cleared
        // whether the battle is fought or falls back to auto-resolve.
        if (GalacticMode* gm = host_->galactic_mode(); gm && gm->sim()) gm->sim()->clear_linked_battle();
        if (!opt_.tactical) return false;
        galactic::BattleRequest r = r0;
        story_load_map(r);  // S4: STORY_LOAD_TACTICAL_MAP + SET_TACTICAL_MAP override (battle-load screen)
        terrain::MapData map;
        if (!loader_ || !loader_(r.map, map)) {
            log_.push_back("map " + r.map + " not loaded: battle auto-resolved");
            return false;
        }
        if (map.is_space() != (r.info.kind == galactic::ConflictKind::Space)) {
            log_.push_back("map " + r.map + " is not a " + std::string(map.is_space() ? "land" : "space") + " map: battle auto-resolved");
            return false;
        }
        pending_req_ = r;
        pending_map_ = std::move(map);
        pending_ = true;
        return true;
    };
    cm_->attach();
    // --- battle host (BT pass): optional S10 seams (campaign_battles.hpp CampaignBattleOptions "battle host") ---
    gate_ = std::make_unique<ConflictGate>(*this);  // BT §1.4 step 2: the manager runs unless the story block holds (a1j t05)
    g->sim()->set_conflict_hooks(gate_.get());
    // BT §1.6 story lock: a locked human-involved conflict is fought. Source: the option, else the running story
    // (StorySession::autoresolve_locked); unlocked = the ConflictConfig answer (same as no decide callback).
    cm_->decide = [this](const galactic::ConflictInfo& c) {
        bool locked = false;
        if (opt_.autoresolve_locked) locked = opt_.autoresolve_locked(c.planet);
#ifdef ALAMO_SIM_HAVE_STORY
        else if (const StorySession* s = host_->story()) locked = s->autoresolve_locked(c.planet);
#endif
        return locked ? galactic::BattleChoice::Fight : cm_->config().human_choice;
    };
    if (!opt_.map_candidates.empty())  // BT §2.1 random map picker (TACTICAL_GAPS §4.10, [port] deterministic pick)
        cm_->pick_map = [this](bool space, int32_t terrain, int32_t faction, int32_t base_level) {
            return pick_random_map(opt_.map_candidates, space, terrain, faction, base_level);
        };
    // --- end battle host ---
    host_->set_after_mode_update([this](SimHost& h) { hook(h); });
    if (opt_.crc) {
        CrcRegistry& reg = host_->crc_registry();
        cm_->register_crc(reg, "galactic.conflicts");
        reg.add("battle.state", [this](StateHasher& h) {
            h.boolean(battle_ != nullptr);
            if (battle_) battle_->hash(h);
            // AI_GALACTIC_TOKENS §9.4: the planets' persisted tactical built objects, planet order, only planets with
            // records (none: no bytes).
            if (const GalacticMode* gm = host_->galactic_mode(); gm && gm->sim()) {
                const std::span<const galactic::PlanetState> planets = gm->sim()->planets();
                for (size_t pl = 0; pl < planets.size(); ++pl) {
                    const std::vector<galactic::PersistedBuiltObject>& v = planets[pl].built_objects;
                    if (v.empty()) continue;
                    h.u32(static_cast<uint32_t>(pl));
                    h.u32(static_cast<uint32_t>(v.size()));
                    for (const galactic::PersistedBuiltObject& r : v) {
                        h.u32(r.type);
                        h.f32(r.x);
                        h.f32(r.y);
                        h.f32(r.z);
                        h.i32(r.base_level);
                        h.i32(r.owner);
                    }
                }
            }
#ifdef ALAMO_SIM_HAVE_AI_TACTICAL
            h.u32(static_cast<uint32_t>(credits_.size()));
            for (const CreditTransfer& ct : credits_) {
                h.i32(ct.player);
                ct.state.hash(h);
            }
#else
            h.u32(0);  // no credit transfers (same layout without engine/ai)
#endif
            hash_linked(h);  // story t07 / t08 (no bytes while the linked state is at its defaults)
        });
        reg.add("battle.movement", [this](StateHasher& h) {
            h.boolean(battle_ != nullptr);
            if (battle_) battle_->movement().hash_units(h);
        });
        reg.add("battle.combat", [this](StateHasher& h) {
            h.boolean(battle_ != nullptr);
            if (!battle_) return;
            const tactical::combat::CombatSystem& c = battle_->combat();
            c.hash_units(h);
            c.hash_projectiles(h);
            c.hash_modifiers(h);
            c.hash_delayed(h);
        });
        // S10-int: S9 state (abilities.objects + combat.ext), speed factors, particles / containers / dummies.
        reg.add("battle.abilities", [this](StateHasher& h) {
            h.boolean(battle_ != nullptr);
            if (battle_) battle_->hash_abilities(h);
        });
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI
        if (opt_.tactical_ai)  // A1 stage 2 (opt-in: existing goldens keep their system list)
            reg.add("battle.ai", [this](StateHasher& h) {
                h.boolean(ai_ != nullptr);
                if (ai_) ai_->hash(h);
            });
#endif
    }
    // S10-int: ability commands (40 / 43 / 57 / 58) of the running battle at the command phase.
    host_->set_tactical_command_hook([this](SimHost& h, uint8_t player, const commands::EventData& ev) {
        TacticalMode* tm = h.tactical();
        if (!battle_ || !tm || pending_) return commands::RejectReason::Unhandled;
        return battle_->handle_command(*tm, player, ev, h.random().synced());
    });
    // q6 (COMMANDS §2.4.1): Select All in the running battle: TEAM members, garrisoned units and a retreating side are
    // skipped (TacticalBattle::select_all_entry), a container brings its members; only objects whose selection flag is
    // set (Set_Selectable, the tactical bridge's script state: unset = selectable) enter.
    host_->set_select_all_hook([this](SimHost& h, uint8_t, objects::ObjectHandle o, std::vector<objects::ObjectId>& members) {
        TacticalMode* tm = h.tactical();
        if (!battle_ || !tm || pending_) return true;
        const objects::ObjectStore& st = tm->primary();
        auto flag = [this](objects::ObjectId id) {
#ifdef ALAMO_SIM_HAVE_STORY
            const lua::ScriptObjectState* s = story_bridge_ ? story_bridge_->find_state(id) : nullptr;
            return !s || s->selectable != 0;
#else
            (void)this;
            (void)id;
            return true;
#endif
        };
        const objects::ObjectId id = st.id(o);
        if (!flag(id) || !battle_->select_all_entry(id, st.owner(o), members)) return false;
        members.erase(std::remove_if(members.begin(), members.end(), [&](objects::ObjectId m) { return !flag(m); }), members.end());
        return true;
    });
    // Int-final: galactic SpecialAbility commands GalacticSim leaves unhandled (kind 6 = the corruption choice).
    host_->set_galactic_command_hook([this](SimHost&, uint8_t player, const commands::EventData& ev) {
        if (ev.type() == commands::EventType::ResumeGame) return resume_game(player, ev);  // battle host: TACTICAL_GAPS §4.11
        return corruption_command(player, ev);
    });
    // N1 (owner: n1-host): the conflict manager and the between-battle state save with the session.
    SaveParticipant conflicts;
    conflicts.name = "galactic.conflicts";
    conflicts.write = [this](ByteWriter& w) { write_if_saveable(*cm_, w); };
    conflicts.read = [this](ByteReader& r) { return read_if_saveable(*cm_, r); };
    host_->add_save_participant(std::move(conflicts));
    SaveParticipant state;
    state.name = "battles";
    state.write = [this](ByteWriter& w) { write_state(w); };
    state.read = [this](ByteReader& r) { return read_state(r); };
    state.blocker = [this] { return save_blocker(); };
    state.rebuild = [this] { return rebuild(); };  // N1 stage 2: a save taken during a battle
    host_->add_save_participant(std::move(state));
    return true;
}

// --- N1 save / load (owner: n1-host; stage 2 n2-host; campaign_battles.hpp) ---
namespace {
constexpr uint32_t kBattlesSaveVersion = 4;  // 2: + the battle in flight (stage 2); 3: + the linked-battle state (story t07);
                                             // 4: the pad records moved to the planets (galactic save v2, q6)
constexpr uint32_t kBattleLevelVersion = 1;
constexpr uint32_t kMaxBattlesCount = 1u << 24;

uint32_t read_count(ByteReader& r) {
    const uint32_t n = r.u32();
    if (n > kMaxBattlesCount) r.fail();
    return r.ok() ? n : 0;
}

// The battle request (both directions, save_io.hpp).
template <class Ar, class Req>
void request_io(Ar& a, Req& q) {
    a(q.info.kind, q.info.planet, q.info.attacker, q.info.defender, q.info.units, q.info.map);
    a(q.map, q.base_level);
    a.seq(q.forces, [](auto& x, auto& f) { x(f.player, f.start, f.reserve); });
    a(q.starbase, q.structures, q.land_base_destruction_forces_retreat, q.space_station_destruction_forces_retreat);
}

bool load_battle_map(const CampaignBattles::MapLoader& loader, const galactic::BattleRequest& r, terrain::MapData& map) {
    return loader && loader(r.map, map) && map.is_space() == (r.info.kind == galactic::ConflictKind::Space);
}
}  // namespace

std::string CampaignBattles::save_blocker() const {
    // Stage 2: running and pending battles save; a battle that is already over leaves the stack at the next hook.
    if (battle_ && battle_->finished()) return "the battle has ended (the tactical mode is popped at the next tick)";
    if (ar_requested_) return "the battle was stopped for auto-resolve (resolved at the next tick)";
    return {};
}

void CampaignBattles::write_state(ByteWriter& w) const {
    auto write_mission = [&w](const CorruptionMission& m) {
        w.u32(static_cast<uint32_t>(m.planet)).i32(m.player).u8(static_cast<uint8_t>(m.type)).u8(static_cast<uint8_t>(m.kind)).str(m.plots);
    };
    w.u32(kBattlesSaveVersion);
    w.u32(fought_).boolean(fog_active_).boolean(fog_render_).boolean(ar_requested_).boolean(direct_);
    w.u8(static_cast<uint8_t>(last_corruption_));
    w.u32(static_cast<uint32_t>(log_.size()));
    for (const std::string& l : log_) w.str(l);
    w.u32(static_cast<uint32_t>(corruption_queued_.size()));
    for (const CorruptionMission& m : corruption_queued_) write_mission(m);
    write_mission(corruption_active_);
#ifdef ALAMO_SIM_HAVE_STORY
    w.boolean(true).u64(static_cast<uint64_t>(story_linked_));
#else
    w.boolean(false);
#endif
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI
    w.u32(static_cast<uint32_t>(ai_log_.size()));
    for (const std::string& l : ai_log_) w.str(l);
#else
    w.u32(0);
#endif
    // v2: the battle in flight (its request; a running battle's own state is in the battle chunks).
    const uint8_t flight = battle_ ? 2 : pending_ ? 1 : 0;
    w.u8(flight);
    if (flight != 0) {
        save_io::Out o(w);
        request_io(o, pending_req_);
        o(pending_req_.linked);   // v3
        o(pending_req_.sandbox);  // v4
    }
    write_linked(w);  // v3
}

bool CampaignBattles::read_state(ByteReader& r) {
    auto read_mission = [&r](CorruptionMission& m) {
        m.planet = static_cast<PlanetIndex>(r.u32());
        m.player = r.i32();
        m.type = static_cast<int8_t>(r.u8());
        const uint8_t kind = r.u8();
        if (kind != static_cast<uint8_t>(galactic::ConflictKind::Land) && kind != static_cast<uint8_t>(galactic::ConflictKind::Space))
            r.fail();
        m.kind = static_cast<galactic::ConflictKind>(kind);
        m.plots = r.str();
    };
    const uint32_t version = r.u32();
    if (version < 1 || version > kBattlesSaveVersion) return false;
    const uint32_t fought = r.u32();
    const bool fog_active = r.boolean(), fog_render = r.boolean(), ar_requested = r.boolean(), direct = r.boolean();
    const uint8_t last = r.u8();
    if (last > static_cast<uint8_t>(CorruptionStart::MissionQueued)) r.fail();
    std::vector<std::string> log(read_count(r));
    for (std::string& l : log) l = r.str();
    // v1-3: the per-planet pad records lived here {u32 planet, u32 n, n x {u32 type, f32 x, y, z, i32 level, i32 value}};
    // they move to the planets' built_objects below (v4 keeps them in the galactic save).
    std::vector<std::pair<PlanetIndex, std::vector<galactic::PersistedBuiltObject>>> pads(version < 4 ? read_count(r) : 0u);
    for (auto& [planet, recs] : pads) {
        planet = static_cast<PlanetIndex>(r.u32());
        recs.resize(read_count(r));
        for (galactic::PersistedBuiltObject& p : recs) {
            p.type = r.u32();
            p.x = r.f32();
            p.y = r.f32();
            p.z = r.f32();
            p.base_level = r.i32();
            p.owner = r.i32();
        }
    }
    std::vector<CorruptionMission> queued(read_count(r));
    for (CorruptionMission& m : queued) read_mission(m);
    CorruptionMission active;
    read_mission(active);
    const bool has_story = r.boolean();
#ifdef ALAMO_SIM_HAVE_STORY
    if (!has_story) return false;
    const uint64_t linked = r.u64();
#else
    if (has_story) return false;
#endif
    std::vector<std::string> ai_log(read_count(r));
    for (std::string& l : ai_log) l = r.str();
    const uint8_t flight = version >= 2 ? r.u8() : uint8_t{0};
    galactic::BattleRequest req;
    if (flight != 0) {
        save_io::In in(r);
        request_io(in, req);
        if (version >= 3) in(req.linked);
        if (version >= 4) in(req.sandbox);
    }
    LinkedState linked_state;  // v3 (older saves: the defaults, no record)
    if (version >= 3) read_linked(r, linked_state);
    if (!r.ok() || !r.done() || flight > 2) return false;
    // The battle in flight. Running: without a battle (load pass 1) the request is staged for rebuild(); with one (pass 2,
    // after the rebuild) it must be the saved battle. Pending: the map is loaded again (the battle is pushed at the next hook).
    if (flight == 2 && battle_ &&
        (req.info.kind != pending_req_.info.kind || req.info.planet != pending_req_.info.planet || req.map != pending_req_.map))
        return false;
    if (flight != 2 && battle_) return false;  // a running battle the save does not have
    terrain::MapData map;
    if (flight == 1 && !load_battle_map(loader_, req, map)) return false;
    fought_ = fought;
    fog_active_ = fog_active;
    fog_render_ = fog_render;
    ar_requested_ = ar_requested;
    direct_ = direct;
    last_corruption_ = static_cast<CorruptionStart>(last);
    log_ = std::move(log);
    if (!pads.empty())
        if (GalacticMode* gm = host_->galactic_mode(); gm && gm->sim())
            for (auto& [planet, recs] : pads)
                if (std::vector<galactic::PersistedBuiltObject>* dst = gm->sim()->built_objects(planet)) *dst = std::move(recs);
    corruption_queued_ = std::move(queued);
    corruption_active_ = std::move(active);
    lk_ = std::move(linked_state);
#ifdef ALAMO_SIM_HAVE_STORY
    story_linked_ = static_cast<size_t>(linked);
#endif
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI
    ai_log_ = std::move(ai_log);
#endif
    resume_.reset();
    if (flight == 2 && !battle_) resume_ = std::make_unique<galactic::BattleRequest>(std::move(req));
    if (flight == 1) {
        pending_req_ = std::move(req);
        pending_map_ = std::move(map);
    }
    if (!battle_) pending_ = flight == 1;
    if (battle_ && pending_req_.info.planet != kInvalidPlanet) {
        // Pass 2: the galactic read replaced the planet rows; the battle's pad persistence list is relinked (as push_battle
        // links it; no recreate: the battle was set up already).
        GalacticMode* gm = host_->galactic_mode();
        battle_->set_pad_persistence(gm && gm->sim() ? gm->sim()->built_objects(pending_req_.info.planet) : nullptr, false);
    }
    return true;
}

// SaveParticipant::rebuild of "battles" (load, between the passes): the save's running battle, started exactly as hook()
// starts a battle (push_battle); pass 2 then replaces its state.
std::string CampaignBattles::rebuild() {
    if (!resume_) return {};
    galactic::BattleRequest req = std::move(*resume_);
    resume_.reset();
    if (battle_ || pending_) return "a battle is already running";
    if (!opt_.tactical) return "tactical battles are off in this session";
    terrain::MapData map;
    if (!load_battle_map(loader_, req, map)) return "map " + req.map + " did not load as the saved battle's map";
    pending_req_ = std::move(req);
    pending_map_ = std::move(map);
    rebuilding_ = true;  // §9.4: a loaded battle keeps its saved objects (no recreate at its setup)
    push_battle(*host_);
    rebuilding_ = false;
    return battle_ ? std::string() : std::string("the battle did not start");
}

void CampaignBattles::register_battle_chunks() {
    remove_battle_chunks();
    if (!battle_) return;
    using Part = TacticalBattle::SavePart;
    for (const Part p : {Part::Movement, Part::Combat, Part::Abilities, Part::Garrison, Part::Hangar, Part::Fog, Part::Damage}) {
        SaveParticipant sp;
        sp.name = TacticalBattle::save_part_name(p);
        sp.write = [this, p](ByteWriter& w) {
            if (battle_) battle_->write_part(p, w);
        };
        sp.read = [this, p](ByteReader& r) { return battle_ && battle_->read_part(p, r); };
        host_->add_save_participant(std::move(sp));
    }
    SaveParticipant st;  // after the module parts
    st.name = TacticalBattle::save_part_name(Part::State);
    st.write = [this](ByteWriter& w) {
        if (!battle_) return;
        write_section(w, [this](ByteWriter& sub) { battle_->write_part(Part::State, sub); });
        write_section(w, [this](ByteWriter& sub) { write_battle_level(sub); });
    };
    st.read = [this](ByteReader& r) {
        return battle_ && read_section(r, [this](ByteReader& sub) { return battle_->read_part(Part::State, sub); }) &&
               read_section(r, [this](ByteReader& sub) { return read_battle_level(sub); }) && r.done();
    };
    host_->add_save_participant(std::move(st));
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI
    if (ai_) {
        SaveParticipant ap;
        ap.name = "battle.ai";
        ap.write = [this](ByteWriter& w) {
            if (ai_) ai_->write(w);
        };
        ap.read = [this](ByteReader& r) { return ai_ && ai_->read(r); };
        host_->add_save_participant(std::move(ap));
    }
#endif
}

void CampaignBattles::remove_battle_chunks() {
    for (uint8_t p = 0; p < static_cast<uint8_t>(TacticalBattle::SavePart::Count); ++p)
        host_->remove_save_participant(TacticalBattle::save_part_name(static_cast<TacticalBattle::SavePart>(p)));
    host_->remove_save_participant("battle.ai");
}

// battle.state, second section: this object's battle-level state.
void CampaignBattles::write_battle_level(ByteWriter& w) const {
    w.u32(kBattleLevelVersion);
    save_io::Out o(w);
    (void)o;  // unused without engine/ai and the story
#ifdef ALAMO_SIM_HAVE_AI_TACTICAL
    w.boolean(true);
    o.seq(credits_, [](auto& x, auto& c) { x(c.player, c.state.active, c.state.saved, c.state.released); });
#else
    w.boolean(false);
#endif
    w.u64(static_cast<uint64_t>(upgrades_seen_));
#ifdef ALAMO_SIM_HAVE_STORY
    w.boolean(true);
    w.boolean(story_bridge_ != nullptr);
    if (story_bridge_) write_section(w, [this](ByteWriter& sub) { write_if_saveable(*story_bridge_, sub); });
    write_section(w, [this](ByteWriter& sub) { write_if_saveable(story_transports_, sub); });
    o(story_motion_, story_base_ids_);
#ifdef ALAMO_LUA_HAVE_FOG
    w.boolean(true).u32(story_fog_.epoch);
    write_section(w, [this](ByteWriter& sub) { write_if_saveable(story_fog_.cells, sub); });
#else
    w.boolean(false);
#endif
#else
    w.boolean(false);
#endif
}

bool CampaignBattles::read_battle_level(ByteReader& r) {
    if (r.u32() != kBattleLevelVersion || !r.ok() || !battle_) return false;
    save_io::In in(r);
    (void)in;
    const bool has_credits = r.boolean();
#ifdef ALAMO_SIM_HAVE_AI_TACTICAL
    if (!has_credits) return false;
    std::vector<CreditTransfer> credits;
    in.seq(credits, [](auto& x, auto& c) { x(c.player, c.state.active, c.state.saved, c.state.released); });
#else
    if (has_credits) return false;
#endif
    const uint64_t upgrades = r.u64();
    const bool has_story = r.boolean();
    if (!r.ok()) return false;
#ifdef ALAMO_SIM_HAVE_STORY
    if (!has_story) return false;
    const bool has_bridge = r.boolean();
    if (!r.ok() || has_bridge != (story_bridge_ != nullptr)) return false;
    const std::span<const uint8_t> bridge = has_bridge ? r.raw(r.u32()) : std::span<const uint8_t>{};
    const std::span<const uint8_t> transports = r.raw(r.u32());
    std::map<objects::ObjectId, uint8_t> motion;
    std::vector<objects::ObjectId> base_ids;
    in(motion, base_ids);
    const bool has_fog = r.boolean();
#ifdef ALAMO_LUA_HAVE_FOG
    if (!has_fog) return false;
    const uint32_t epoch = r.u32();
    const std::span<const uint8_t> cells = r.raw(r.u32());
#else
    if (has_fog) return false;
#endif
    if (!r.ok() || !r.done() || upgrades > battle_->station_upgrades().size()) return false;
    lua::CinematicTransports tr;
    {
        ByteReader tr_r(transports);
        if (!read_if_saveable(tr, tr_r) || !tr_r.done()) return false;
    }
#ifdef ALAMO_LUA_HAVE_FOG
    lua::FogScriptCells fc;
    {
        ByteReader fc_r(cells);
        if (!read_if_saveable(fc, fc_r) || !fc_r.done()) return false;
    }
#endif
    if (story_bridge_) {  // read in place: the tactical story VMs hold this bridge
        ByteReader br(bridge);
        if (!read_if_saveable(*story_bridge_, br) || !br.done()) return false;
    }
    story_transports_ = std::move(tr);
    story_motion_ = std::move(motion);
    story_base_ids_ = std::move(base_ids);
#ifdef ALAMO_LUA_HAVE_FOG
    // No FogScriptCells::recount: the snapshots' counts are in the fog grids battle.fog loaded (fog_of_war.hpp N1 stage 2).
    story_fog_.epoch = epoch;
    story_fog_.cells = std::move(fc);
#endif
#else
    if (has_story || !r.done() || upgrades > battle_->station_upgrades().size()) return false;
#endif
#ifdef ALAMO_SIM_HAVE_AI_TACTICAL
    credits_ = std::move(credits);
#endif
    upgrades_seen_ = static_cast<size_t>(upgrades);
    return true;
}
// --- end N1 ---

// --- battle host (BT pass): campaign_battles.hpp ConflictGate / resume_game / pick_random_map ---
commands::RejectReason CampaignBattles::resume_game(uint8_t player, const commands::EventData& ev) {
    // ResumeGame (51) execute 0x4ae0d0 -> 0x28c7e0: galactic, conflict waiting in the vote / pending choice (state 0): bit 0
    // -> fight (state 3), bit 1 -> auto-resolve with the force flag (0x29c020, state 6). [port] single player takes the MP
    // rule (the retail SP dialog calls 0x28c7e0 directly, not through the command stream); other states: NotAllowed.
    (void)player;
    if (lk_.banner_up) return linked_begin() ? commands::RejectReason::None : commands::RejectReason::NotAllowed;  // story t07: P13
    if (!cm_ || cm_->state() != galactic::ConflictState::Vote) return commands::RejectReason::NotAllowed;
    cm_->choose(ev.u("flag") != 0 ? galactic::BattleChoice::AutoResolve : galactic::BattleChoice::Fight);
    return commands::RejectReason::None;
}

std::string pick_random_map(const std::vector<CampaignBattleOptions::MapCandidate>& candidates, bool space, int32_t terrain, int32_t faction,
                            int32_t base_level) {
    std::vector<const CampaignBattleOptions::MapCandidate*> m;
    for (const CampaignBattleOptions::MapCandidate& c : candidates)
        if (c.space == space && (c.terrain < 0 || c.terrain == terrain) && (c.faction < 0 || c.faction == faction) &&
            c.max_base_level >= base_level && c.players >= 2 && !c.excluded)
            m.push_back(&c);
    if (m.empty()) return {};
    uint32_t h = 2166136261u;  // FNV-1a over the query
    for (const int32_t v : {space ? 1 : 0, terrain, faction, base_level, static_cast<int32_t>(m.size())})
        for (int i = 0; i < 4; ++i) {
            h ^= (static_cast<uint32_t>(v) >> (8 * i)) & 0xFFu;
            h *= 16777619u;
        }
    return m[h % static_cast<uint32_t>(m.size())]->name;
}
// --- end battle host ---

// --- Int-final: the human corruption command (campaign_battles.hpp corruption_command) ---
commands::EventData corruption_command(uint32_t defiler_id, uint32_t planet_id, int8_t type) {
    uint8_t b[9];
    for (int i = 0; i < 4; ++i) {
        b[i] = static_cast<uint8_t>(defiler_id >> (8 * i));
        b[4 + i] = static_cast<uint8_t>(planet_id >> (8 * i));
    }
    b[8] = static_cast<uint8_t>(type);
    commands::EventData e(commands::EventType::SpecialAbility);
    e.set("kind", kSpecialAbilityCorrupt).set_bytes("data", std::span<const uint8_t>(b, sizeof b));
    return e;
}

bool read_corruption_command(const commands::EventData& ev, uint32_t& defiler_id, uint32_t& planet_id, int8_t& type) {
    if (ev.type() != commands::EventType::SpecialAbility || ev.u("kind") != kSpecialAbilityCorrupt) return false;
    const std::span<const uint8_t> b = ev.bytes_view("data");
    if (b.size() != 9) return false;
    defiler_id = planet_id = 0;
    for (int i = 0; i < 4; ++i) {
        defiler_id |= static_cast<uint32_t>(b[static_cast<size_t>(i)]) << (8 * i);
        planet_id |= static_cast<uint32_t>(b[static_cast<size_t>(4 + i)]) << (8 * i);
    }
    type = static_cast<int8_t>(b[8]);
    return true;
}

commands::RejectReason CampaignBattles::corruption_command(uint8_t player, const commands::EventData& ev) {
    uint32_t defiler = 0, planet_id = 0;
    int8_t type = -1;
    if (!read_corruption_command(ev, defiler, planet_id, type)) return commands::RejectReason::Unhandled;
    GalacticMode* g = host_->galactic_mode();
    if (!g || !g->sim()) return commands::RejectReason::NotAllowed;
    const galactic::GalacticSim& gs = *g->sim();
    const objects::ObjectStore& st = gs.store();
    const objects::ObjectHandle dh = defiler <= 0x7FFFFFFFu ? st.find(static_cast<objects::ObjectId>(defiler)) : objects::ObjectHandle{};
    if (!st.alive(dh) || st.owner(dh) != static_cast<int32_t>(player)) return commands::RejectReason::InvalidUnit;  // §4.5 step 1
    PlanetIndex planet = kInvalidPlanet;
    for (size_t i = 0; i < gs.planets().size(); ++i)
        if (st.exists(gs.planets()[i].object) && static_cast<uint32_t>(st.id(gs.planets()[i].object)) == planet_id) planet = static_cast<PlanetIndex>(i);
    if (planet == kInvalidPlanet) return commands::RejectReason::InvalidTarget;
    last_corruption_ = start_corruption(planet, static_cast<int32_t>(player), type);
    // P4 sim: the defiler departs on both executor paths (P4_SIM_SYSTEMS §5.5): respawn at the planet after
    // Default_Hero_Respawn_Time, despawn now.
    if (last_corruption_ != CorruptionStart::Refused) g->sim()->hero_depart(dh, planet, 0.0f);
    return last_corruption_ == CorruptionStart::Refused ? commands::RejectReason::NotAllowed : commands::RejectReason::None;
}
// --- end Int-final ---

void CampaignBattles::push_battle(SimHost& host) {
    // Start_Game(1|2): push the tactical mode over the galaxy (no RNG reseed), spawn the battle.
    pending_ = false;
    const ModeKind kind = pending_req_.info.kind == galactic::ConflictKind::Space ? ModeKind::Space : ModeKind::Land;
    GameMode& m = host.start_game(kind);
    GalacticMode* g = host.galactic_mode();
#ifdef ALAMO_SIM_HAVE_AI_TACTICAL
    // Land campaign battle: AI credits swapped for the tactical budget (A1 tactical_credits.hpp).
    credits_.clear();
    if (kind == ModeKind::Land && opt_.tactical_budget)
        for (int32_t p : {pending_req_.info.defender, pending_req_.info.attacker}) {
            galactic::econ::CreditAccount* acct = g->sim()->credit_account(p);
            if (!acct) continue;
            CreditTransfer ct;
            ct.player = p;
            ai::tactical::begin_land_battle_credits(opt_.tactical_budget(p), *acct, true, g->sim()->players()[static_cast<size_t>(p)].human, ct.state);
            if (ct.state.active) credits_.push_back(ct);
        }
#endif
    BattleConfig bc = opt_.battle;
    bc.map_battle = direct_;  // start_map_battle: no branched-map discard
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI
    if (opt_.tactical_ai) bc.order_interval = bc.reinforce_interval = 0;  // A1: the tactical AI orders and reinforces
#endif
    battle_ = std::make_unique<TacticalBattle>(*catalog_, *g->sim(), *rules_, pending_req_, bc, host.parallel_for());
    upgrades_seen_ = 0;
    battle_->set_animation(opt_.animation);  // S10-int (null: no sim-owned animation)
    battle_->set_clone_models(opt_.clone_models);  // refixf t04 (null: the battle's own, from the catalog files)
    battle_->set_retreat_increments(opt_.retreat_increment_frames);  // L1 systems
    battle_->set_retreat_flight_increments(opt_.retreat_flight_increments);  // L1 systems
    battle_->set_l1_constants(opt_.l1_constants);  // L1 systems
    battle_->set_weather_data(opt_.weather);       // L1 systems
    battle_->set_l1_factions(opt_.l1_factions);    // L1 systems
    // AI_GALACTIC_TOKENS §9.4: the battle planet's persisted built objects (the battle applies the gate: not in a skirmish,
    // not the sandbox battle). A battle rebuilt by a save load does not recreate them (the save has the battle's objects).
    if (pending_req_.info.planet != kInvalidPlanet)
        battle_->set_pad_persistence(g->sim()->built_objects(pending_req_.info.planet), !rebuilding_);
    {  // battle host: TacticalBuild charges / TacticalSell refunds spend the galactic account ([port] no AI notify)
        galactic::GalacticSim* gsim = g->sim();
        battle_->set_credit_hooks(
            [gsim](int32_t p) { return p >= 0 && static_cast<size_t>(p) < gsim->players().size() ? gsim->credits(p) : 0.0f; },
            [gsim](int32_t p, float d) {
                if (p >= 0 && static_cast<size_t>(p) < gsim->players().size()) gsim->add_credits(p, d, false);
            });
    }
#ifdef ALAMO_SIM_HAVE_FOG
    battle_->set_fog_globals(fog_active_, fog_render_);  // fog: the original's global flag survives battles
#endif
    battle_->setup(static_cast<TacticalMode&>(m), std::move(pending_map_), host.random().synced(), host.players(),
                   host.parallel_for());
    pending_map_ = terrain::MapData{};
    ++fought_;
    corruption_push();  // S4: a corruption mission's battle (before the story attaches its plots)
#ifdef ALAMO_SIM_HAVE_STORY
    story_begin(host, m);  // L1/S4 wave 2a
#endif
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI
    ai_begin(host, m);  // A1 stage 2
    galactic_context_begin(kind);  // a1j t07
#endif
    register_battle_chunks();  // N1 stage 2: the running battle's save chunks
    host.scoring_mode_started(kind, pending_req_.map);  // q6 t09: Game_Mode_Starting_Event(Land / Space, map)
}

// q6 t09 (campaign_battles.hpp ar_log_seen_): Reset_Tactical_Stats once per auto-resolve, at its begin when it is seen
// running, else when its log entry appears.
void CampaignBattles::scoring_auto_resolve() {
    if (!cm_) return;
    const std::vector<galactic::ConflictLogEntry>& log = cm_->log();
    if (!ar_log_init_ || ar_log_seen_ > log.size()) {
        ar_log_seen_ = log.size();
        ar_log_init_ = true;
    }
    for (; ar_log_seen_ < log.size(); ++ar_log_seen_) {
        if (log[ar_log_seen_].how != 1) continue;
        if (ar_reset_sent_) ar_reset_sent_ = false;  // the one seen running ended
        else host_->scoring_reset_tactical_stats();
    }
    const galactic::ConflictState s = cm_->state();
    if ((s == galactic::ConflictState::AutoSetup || s == galactic::ConflictState::AutoRunning) && !ar_reset_sent_) {
        host_->scoring_reset_tactical_stats();
        ar_reset_sent_ = true;
    }
}

void CampaignBattles::hook(SimHost& host) {
    scoring_auto_resolve();  // q6 t09
#ifdef ALAMO_SIM_HAVE_STORY
    if (StorySession* s = host.story(); s && s != story_fog_session_) {  // fog follow-up: ENABLE_FOW story rewards
        s->set_enable_fow_hook([this](bool has_param, int32_t value) { story_fog_reward(has_param, value); });
        story_fog_session_ = s;
    }
#endif
    if (pending_) {
        push_battle(host);
        return;
    }
#ifdef ALAMO_SIM_HAVE_STORY
    if (!battle_) {
        story_landings(host);  // story t07: the last-invade vector from this T9's human landings (before the rewards)
        story_linked(host);    // L1/S4 wave 2b: story-forced battles (LINK_TACTICAL records)
    }
#endif
    if (!battle_ && !pending_ && lk_.present) start_linked_record();  // story t07 / t08: a waiting record (0x28f590 again)
    if (pending_) {  // BT §2.9.3: a linked battle is pushed in the hook that started it (the hook after its reward)
        push_battle(host);
        return;
    }
    if (!battle_) corruption_prune();  // S4: corruption missions whose conflict was dropped
    if (!battle_ && cm_) {  // REMAINING_GAPS 6.4 [port]: a linked conflict that left the queues without a start (dropped by
                            // the validation / auto-resolved) releases the flag, else the galaxy would stay blocked
        GalacticMode* gm = host.galactic_mode();
        if (gm && gm->sim() && gm->sim()->linked_battle_pending() && !lk_.present && cm_->land_queue().empty() &&
            cm_->space_queue().empty() && cm_->state() == galactic::ConflictState::Idle)
            gm->sim()->clear_linked_battle();
    }
    if (!battle_) return;
    TacticalMode* tm = host.tactical();
    if (!tm) return;
    battle_->tick(*tm, host.random().synced(), host.parallel_for(), host.sim_time(), host.frame());
    // L1 systems (TSW_PLANET_DESTROY §4 steps 7-9): the TSW's state-9 frame destroys the galactic planet now, mid-battle
    // (the parent galaxy is mutated while the tactical mode runs); only with a parent conflict (not a map battle).
    if (const int32_t tsw_owner = battle_->tsw_kill_owner(); tsw_owner >= 0 && !direct_ && cm_) {
        GalacticMode* g = host.galactic_mode();
        const PlanetIndex planet = battle_->request().info.planet;
        bool leader = false;
        if (g && g->sim() && g->sim()->tsw_destroy_planet(planet, tsw_owner, &leader)) {
            if (leader) battle_->set_tsw_story_victory(tsw_owner);  // ends with 0x10 at the hold release (§6)
#ifdef ALAMO_SIM_HAVE_STORY
            if (StorySession* s = host.story()) s->planet_destroyed_in_battle(planet, tsw_owner, host.frame());
#endif
        }
    }
    // STATION_HANGARS_SOUND §1.2 step 6: selections and control groups follow an upgraded station (same tick).
    for (; upgrades_seen_ < battle_->station_upgrades().size(); ++upgrades_seen_)
        host.station_replaced(battle_->station_upgrades()[upgrades_seen_].old_station, battle_->station_upgrades()[upgrades_seen_].new_station);
#ifdef ALAMO_SIM_HAVE_STORY
    // S7 t15 A1: signal 0x29 for the story VMs' blocking Activate_Ability (the unit, and its team container).
    if (story_bridge_) {
        const objects::ObjectStore& st = tm->primary();
        for (const objects::ObjectId id : battle_->ability_finished_events()) {
            story_bridge_->ability_finished(id);
            const objects::ObjectHandle h = st.find(id);
            const objects::ObjectHandle team = st.exists(h) ? st.container(h) : objects::ObjectHandle{};
            if (team && st.id(team) != id) story_bridge_->ability_finished(st.id(team));
        }
    }
    story_feed(host);  // L1/S4 wave 2a: before the deaths leave the store (flushed at the next mode update)
#endif
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI
    ai_update();  // A1 stage 2: the AI players' T7 slot of this frame [port: end of T5.battles, T6 is a stub]
#endif
    if (!battle_->finished()) return;
    // Battle end (BT Â§3.1): result -> pop the tactical mode -> write-back to the parent galaxy, same tick.
    log_.push_back(battle_->summary());
    last_losses_ = battle_->unit_losses();
    galactic::BattleResult result = battle_->result();
    // --- P4 sim (owner: galactic agent): a battle stopped by auto_resolve_battle(): the galaxy resolves it from tactical
    // (BT §4.5) and its winner feeds the battle ends below; finish_battle is skipped.
    const bool auto_resolved = ar_requested_;
    ar_requested_ = false;
    if (auto_resolved) {
        scoring_auto_resolve();               // q6 t09: entries before this one are settled first
        host.scoring_reset_tactical_stats();  // q6 t09: the HUD auto-resolve begins inside resolve_battle
        ar_reset_sent_ = true;                // its log entry consumes it
        result.winner = cm_->resolve_battle(result);
        log_.push_back("auto-resolve from tactical: winner " + std::to_string(result.winner));
    }
    // --- end P4 sim ---
#ifdef ALAMO_SIM_HAVE_STORY
    if (StorySession* s = host.story()) s->end_battle(result.winner, host.frame());  // L1/S4 wave 2a: VICTORY
#endif
    const bool corruption_battle = corruption_active_.player >= 0;  // battle host: TACTICAL_GAPS 4.8 (read before corruption_end)
    corruption_end(result.winner);  // S4: corruption mission result (the story session applied it when attached)
    if (opt_.object_removed)  // battle host: every battle object leaves the mode (galactic AI survival learning)
        for (const TacticalBattle::RemovalRecord& r : battle_->removals()) opt_.object_removed(r.owner, r.object_type, r.original_type, r.survived);
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI
    ai_end();  // A1 stage 2: plans released before the mode pops
#endif
#ifdef ALAMO_SIM_HAVE_FOG
    if (const tactical::fog::FogOfWar* f = battle_->fog()) {  // fog: carry FOW-active / render option forward
        fog_active_ = f->fow_active();
        fog_render_ = f->render_option();
    }
#endif
    remove_battle_chunks();  // N1 stage 2
    host.end_game_mode();
    battle_.reset();
#ifdef ALAMO_SIM_HAVE_STORY
    story_bridge_.reset();
#ifdef ALAMO_LUA_HAVE_FOG
    story_fog_.fog = nullptr;  // fog follow-up: the battle's fog is gone; old LuaFOWCells handles become no-ops
    story_fog_.cells.clear();
#endif
#endif
#ifdef ALAMO_SIM_HAVE_AI_TACTICAL
    if (GalacticMode* g = host.galactic_mode())
        for (CreditTransfer& ct : credits_)
            if (galactic::econ::CreditAccount* acct = g->sim()->credit_account(ct.player))
                ai::tactical::end_land_battle_credits(*acct, ct.state, g->sim()->ai_credit_multiplier(ct.player));
    credits_.clear();
#endif
    // P4 sim: an auto-resolved battle was written back above. battle host: a corruption mission skips the normal write-back
    // and BattleWon / Lost (ConflictManager::finish_corruption_mission, TACTICAL_GAPS 4.8).
    const galactic::LinkedResolution resolution = lk_.resolution;  // story t07: GOM+0x61c, cleared by every battle end
    lk_.resolution = galactic::LinkedResolution::None;
    if (direct_) {
        direct_ = false;  // start_map_battle: nothing to write back
    } else {
        linked_end_battle();
        if (!auto_resolved) {
            if (corruption_battle) cm_->finish_corruption_mission(result);
            else cm_->finish_battle(result, resolution);
        }
    }
}

bool CampaignBattles::start_map_battle(galactic::BattleRequest r) {
    if (!opt_.tactical || battle_ || pending_ || !loader_) return false;
    terrain::MapData map;
    if (!loader_(r.map, map) || map.is_space() != (r.info.kind == galactic::ConflictKind::Space)) {
        log_.push_back("map " + r.map + " not loaded as a " + std::string(r.info.kind == galactic::ConflictKind::Space ? "space" : "land") +
                       " map: no map battle");
        return false;
    }
    pending_req_ = std::move(r);
    pending_map_ = std::move(map);
    pending_ = true;
    direct_ = true;
    return true;
}

// --- P4 sim (owner: galactic agent): campaign_battles.hpp auto_resolve_battle ---
bool CampaignBattles::auto_resolve_battle(int32_t player) {
    if (!battle_ || !cm_ || !cm_->battle_active() || ar_requested_) return false;
    if (!battle_->stop_for_auto_resolve(player)) return false;
    ar_requested_ = true;
    return true;
}
// --- end P4 sim ---

bool CampaignBattles::change_owner(objects::ObjectHandle h, int32_t owner) {
    // S10-int: Lua Change_Owner on a tactical host (ObjectBridge::change_owner, L1/S4 ask).
    TacticalMode* tm = host_->tactical();
    if (!battle_ || !tm) return false;
    return battle_->script_change_owner(*tm, h, owner);  // OI host: non-battle objects too (store + trees)
}

// --- S4 (owner: S4 agent): corruption missions (campaign_battles.hpp start_corruption, galactic/corruption.hpp) ---
CampaignBattles::CorruptionStart CampaignBattles::start_corruption(PlanetIndex planet, int32_t player, int8_t type,
                                                                   std::vector<objects::ObjectHandle> units) {
    GalacticMode* g = host_->galactic_mode();
    if (!g || !g->sim() || !cm_) return CorruptionStart::Refused;
    galactic::GalacticSim& gs = *g->sim();
    if (planet >= gs.planets().size() || player < 0 || static_cast<size_t>(player) >= gs.players().size()) return CorruptionStart::Refused;
    const galactic::PlanetState& ps = gs.planets()[planet];
    // §4.5 steps: a planet without corruption in progress (+0x2f4 == -1) [the destroyed / blocked bytes: not modelled];
    // the planet type's option of that type; credits >= cost, deducted; requirements [M: not enforced, port].
    if (!ps.is_planet || ps.owner == player || ps.corruption != galactic::kCorruptionNone) return CorruptionStart::Refused;
    const std::vector<galactic::CorruptionOption> options = galactic::corruption_options(catalog_->types(), ps.type);
    const galactic::CorruptionOption* o = galactic::find_corruption_option(options, type);
    if (!o) return CorruptionStart::Refused;
    const float cost = static_cast<float>(o->credit_cost);
    if (gs.credits(player) < cost) return CorruptionStart::Refused;
    // Mission only with a tactical slot, a non-AI corrupting player, single player [§4.5; AI corrupters apply directly].
    const bool mission = o->tactical && gs.players()[static_cast<size_t>(player)].human;
    // story t08: the mission's record names the planet owner's faction (0x28bd70 refuses an empty defender name).
    std::string defender_faction;
    if (ps.owner >= 0 && static_cast<size_t>(ps.owner) < gs.players().size()) {
        const uint32_t f = gs.players()[static_cast<size_t>(ps.owner)].faction;
        if (f < gs.rules().factions.size()) defender_faction = gs.rules().factions[f].name;
    }
    if (mission && (lk_.present || defender_faction.empty() || o->map.empty())) return CorruptionStart::Refused;
    // --- P4 sim (owner: galactic agent): the executor's order (P4_SIM_SYSTEMS §5.2): credits, the charge, then the
    // planet type's Required_Unit check (no refund when it fails) and the consumed prerequisites.
    if (gs.corruption_charge(player, planet, type, cost) != galactic::CorruptionCharge::Charged) return CorruptionStart::Refused;
    // --- end P4 sim ---
    if (!mission) {
        // GRC 4.3: the direct path starts the timed gain (wrapper 0x3f6810, 5.0 s); completion runs in the planet service.
        gs.begin_corruption_gain(planet, type, player, 5.0f);
        gs.count_corruption(player, type);  // P4 sim: completed-corruption counter (0x2820a0)
        log_.push_back("corruption: type " + std::to_string(type) + " started (5 s)");
        return CorruptionStart::Applied;
    }
    // story t08 (L1_S4_REMAINING §4.5 mission path): 0x28bd70 writes the record, 0x28f590 starts it at once (no normal
    // conflict, so no ground unit has to be in orbit). The other record fields keep the reset values of 0x28c640 [port: the
    // executor's arguments for them are not traced]: not sandbox, P9 1, P10 / P11 off, no resolution, banner on, flagship on.
    LinkedRecord rec;
    rec.kind = o->kind;
    rec.planet = planet;
    rec.attacker = player;
    rec.map = o->map;
    rec.defender = defender_faction;
    rec.plots = o->plots;
    rec.resolution = galactic::LinkedResolution::None;
    rec.corruption_type = type;
    if (!units.empty()) lk_.last_invade = std::move(units);  // [port] the caller's units stand in for the last-invade vector
    bool ok = queue_linked(std::move(rec));
    if (ok) {
        lk_.source = "corruption: mission";
        ok = start_linked_record() != LinkedStart::Refused;  // waiting (banner, another conflict) still counts as queued
    }
    log_.push_back("corruption: type " + std::to_string(type) + " mission on " + o->map + (ok ? " queued" : " refused"));
    if (!ok) {
        if (cost > 0.0f) gs.add_credits(player, cost);  // [port] refunded when the battle cannot be queued
        return CorruptionStart::Refused;
    }
    return CorruptionStart::MissionQueued;
}

// --- story t07 / t08 (owner: link-tactical): the linked-battle record (campaign_battles.hpp, BT §2.7 / §2.9) ---
bool CampaignBattles::queue_linked(LinkedRecord r) {
    GalacticMode* g = host_->galactic_mode();
    if (lk_.present || !g || !g->sim() || !cm_) return false;
    const bool kind_ok = r.kind == galactic::ConflictKind::Land || r.kind == galactic::ConflictKind::Space;
    if (!kind_ok || r.planet >= g->sim()->planets().size() || r.attacker < 0 || r.map.empty() || r.defender.empty()) return false;
    r.banner_shown = false;
    lk_.record = std::move(r);
    lk_.present = true;
    lk_.source.clear();
    g->sim()->queue_linked_battle();            // 0x140b30545 (REMAINING_GAPS 6.4)
    lk_.presentation.skip_battle_dialog = true;  // 0x140b305ed
    return true;
}

bool CampaignBattles::linked_waits_banner(const LinkedRecord& r) const {
    // The one-button pending banner waits for the player's Begin only where a battle dialog answers (a decide callback that
    // returns Pending: the game host's ask_battle_choice); headless runs answer it at once [port].
    if (!cm_ || !cm_->decide) return false;
    galactic::ConflictInfo c;
    c.kind = r.kind;
    c.planet = r.planet;
    c.attacker = r.attacker;
    c.map = r.map;
    if (GalacticMode* g = host_->galactic_mode(); g && g->sim()) c.defender = g->sim()->player_of_faction(r.defender);
    return cm_->decide(c) == galactic::BattleChoice::Pending;
}

CampaignBattles::LinkedStart CampaignBattles::start_linked_record() {
    if (!lk_.present) return LinkedStart::NoRecord;
    if (lk_.banner_up) return LinkedStart::Banner;
    GalacticMode* g = host_->galactic_mode();
    if (!g || !g->sim() || !cm_ || battle_ || pending_) return LinkedStart::Waiting;
    galactic::GalacticSim& gs = *g->sim();
    // Step 1: a normal conflict's pending vote is up (Strategic_Queue_Tactical_Battles, pause raised, state 0): wait.
    if (rules_->queue_tactical_battles && cm_->vote_open()) return LinkedStart::Waiting;
    // [port] another conflict is active: the record waits, except that under the story block that conflict gives way (a1j t05,
    // A1_READINGS §31.7: the 0x2be640 state-3 path, state -1 then 0x28f590). The original takes over in every case.
    if (cm_->state() != galactic::ConflictState::Idle && (!gs.story_block() || cm_->vote_open() || !cm_->yield_to_linked()))
        return LinkedStart::Waiting;
    LinkedRecord& rec0 = lk_.record;
    const bool space = rec0.kind == galactic::ConflictKind::Space;
    const std::string what = (lk_.source.empty() ? std::string("linked battle") : lk_.source) + (space ? " space" : " land") +
                             (rec0.sandbox ? " sandbox" : "");
    // Step 2: the pending banner (P13), once per record.
    if (rec0.banner && !rec0.banner_shown) {
        rec0.banner_shown = true;
        if (linked_waits_banner(rec0)) {
            lk_.banner_up = true;
            g->push_pause();  // 0x28bb80 inline: one galactic pause level while the banner is up
            log_.push_back(what + " banner");
            return LinkedStart::Banner;
        }
    }
    // Step 3: the record is copied and cleared (0x28c640), then checked.
    const LinkedRecord rec = std::move(lk_.record);
    lk_.record = LinkedRecord{};
    lk_.present = false;
    const bool valid = (rec.kind == galactic::ConflictKind::Land || space) && rec.attacker >= 0 && rec.planet < gs.planets().size() &&
                       gs.planets()[rec.planet].is_planet && !rec.map.empty();
    if (!valid) {
        gs.clear_linked_battle();
        log_.push_back(what + " refused");
        return LinkedStart::Refused;
    }
    // Step 4: the defender (-1 when the faction is unknown: the queueing derives one), GOM+0x61c, P9-P11.
    lk_.resolution = rec.resolution;
    LinkedPresentation& pr = lk_.presentation;
    pr.arrival = rec.arrival;
    pr.arrival_cinematic = rec.arrival != 0;  // 0x2cbb50: 0 -> both off, 2 -> both on, anything else -> +0x34e only
    pr.hyperspace_hold = rec.arrival == 2;
    pr.presentation_a = rec.presentation_a;
    pr.letterbox = rec.presentation_a && rec.arrival != 1;
    pr.presentation_b = rec.presentation_b;
    // Steps 5-7: queued at the front and started at once (ConflictManager::start_linked); land takes the last-invade vector.
    galactic::ConflictManager::LinkedBattle b;
    b.kind = rec.kind;
    b.planet = rec.planet;
    b.attacker = rec.attacker;
    b.defender = gs.player_of_faction(rec.defender);
    b.map = rec.map;
    b.sandbox = rec.sandbox;
    b.forced_type = rec.forced_type;
    b.flagship_split = rec.flagship_split;
    if (!space) {
        b.units = std::move(lk_.last_invade);
        lk_.last_invade.clear();
    }
    const bool ok = cm_->start_linked(std::move(b));
    log_.push_back(what + (ok ? " started" : " refused"));
    if (!ok) {  // §2.9.7 [port]: refused and logged (the original starts the battle anyway)
        lk_.resolution = galactic::LinkedResolution::None;
        gs.clear_linked_battle();
        return LinkedStart::Refused;
    }
    if (rec.corruption_type >= 0)  // §4.5: the pending corruption mission (0x28bbf0) of this battle
        corruption_queued_.push_back({rec.planet, rec.attacker, rec.corruption_type, rec.kind, rec.plots});
    return LinkedStart::Started;
}

bool CampaignBattles::linked_begin() {
    if (!lk_.banner_up) return false;
    lk_.banner_up = false;
    if (GalacticMode* g = host_->galactic_mode()) g->pop_pause();
    return true;  // the record starts at the next hook (start_linked_record: the banner was shown)
}

void CampaignBattles::linked_end_battle() {
    lk_.presentation.last_battle_skipped_dialog = lk_.presentation.skip_battle_dialog;
    lk_.presentation.skip_battle_dialog = false;
}

namespace {
template <class Ar, class Rec>
void linked_record_io(Ar& a, Rec& r) {
    a(r.kind, r.planet, r.attacker, r.map, r.defender, r.forced_type, r.plots, r.sandbox, r.arrival, r.presentation_a, r.presentation_b);
    a(r.resolution, r.banner, r.flagship_split, r.banner_shown, r.corruption_type);
}
template <class Ar, class Pr>
void linked_presentation_io(Ar& a, Pr& p) {
    a(p.arrival, p.arrival_cinematic, p.hyperspace_hold, p.presentation_a, p.letterbox, p.presentation_b, p.skip_battle_dialog,
      p.last_battle_skipped_dialog);
}
}  // namespace

void CampaignBattles::write_linked(ByteWriter& w) const {
    save_io::Out o(w);
    o(lk_.present, lk_.banner_up, lk_.source, lk_.resolution, lk_.last_invade, lk_.triggers_seen);
    LinkedRecord rec = lk_.record;
    linked_record_io(o, rec);
    LinkedPresentation pr = lk_.presentation;
    linked_presentation_io(o, pr);
}

void CampaignBattles::read_linked(ByteReader& r, LinkedState& out) {
    save_io::In in(r);
    in(out.present, out.banner_up, out.source, out.resolution, out.last_invade, out.triggers_seen);
    linked_record_io(in, out.record);
    linked_presentation_io(in, out.presentation);
    if (static_cast<uint8_t>(out.resolution) > static_cast<uint8_t>(galactic::LinkedResolution::DestroyAi) ||
        static_cast<uint8_t>(out.record.resolution) > static_cast<uint8_t>(galactic::LinkedResolution::DestroyAi))
        r.fail();
}

void CampaignBattles::hash_linked(StateHasher& h) const {
    const LinkedPresentation& p = lk_.presentation;
    const bool presentation_default = p.arrival == 0 && !p.arrival_cinematic && !p.hyperspace_hold && !p.presentation_a && !p.letterbox &&
                                      !p.presentation_b;
    if (!lk_.present && !lk_.banner_up && lk_.resolution == galactic::LinkedResolution::None && presentation_default) return;
    h.u32(0x4C4B5444u);  // "LKTD"
    h.boolean(lk_.present);
    h.boolean(lk_.banner_up);
    h.u8(static_cast<uint8_t>(lk_.resolution));
    h.i32(p.arrival);
    for (const bool f : {p.arrival_cinematic, p.hyperspace_hold, p.presentation_a, p.letterbox, p.presentation_b}) h.boolean(f);
    if (!lk_.present) return;
    const LinkedRecord& r = lk_.record;
    h.u8(static_cast<uint8_t>(r.kind));
    h.u32(static_cast<uint32_t>(r.planet));
    h.i32(r.attacker);
    h.str(r.map);
    h.str(r.defender);
    h.u32(r.forced_type);
    h.str(r.plots);
    h.i32(r.arrival);
    h.u8(static_cast<uint8_t>(r.resolution));
    for (const bool f : {r.sandbox, r.presentation_a, r.presentation_b, r.banner, r.flagship_split, r.banner_shown}) h.boolean(f);
    h.i32(r.corruption_type);
}
// --- end story t07 / t08 ---

// Missions whose conflict left the queues without a battle (dropped by the validation, auto-resolved) are forgotten.
void CampaignBattles::corruption_prune() {
    if (corruption_queued_.empty() || !cm_) return;
    auto queued = [this](const CorruptionMission& c) {
        for (const auto* q : {&cm_->land_queue(), &cm_->space_queue()})
            for (const galactic::ConflictInfo& i : *q)
                if (i.planet == c.planet && i.attacker == c.player && i.kind == c.kind) return true;
        return false;
    };
    std::erase_if(corruption_queued_, [&](const CorruptionMission& c) {
        if (queued(c)) return false;
        log_.push_back("corruption: mission for type " + std::to_string(c.type) + " dropped (no battle)");
        return true;
    });
}

void CampaignBattles::corruption_push() {
    corruption_active_ = CorruptionMission{};
    for (size_t i = 0; i < corruption_queued_.size(); ++i) {
        const CorruptionMission c = corruption_queued_[i];  // battle host: a copy (the element is erased below; ASan container-overflow)
        if (c.planet != pending_req_.info.planet || c.player != pending_req_.info.attacker || c.kind != pending_req_.info.kind) continue;
        corruption_active_ = c;
        corruption_queued_.erase(corruption_queued_.begin() + static_cast<std::ptrdiff_t>(i));
        log_.push_back("corruption: mission battle for type " + std::to_string(c.type));
        return;
    }
}

void CampaignBattles::corruption_end(int32_t winner) {
    if (corruption_active_.player < 0) return;
    const CorruptionMission c = corruption_active_;
    corruption_active_ = CorruptionMission{};
    const bool won = winner == c.player;
    log_.push_back(std::string("corruption: mission ") + (won ? "won" : "lost"));
    if (won)  // P4 sim: a won mission counts as a completed corruption [M] (P4_SIM_SYSTEMS §5.2 / §8 item 8)
        if (GalacticMode* gm = host_->galactic_mode(); gm && gm->sim()) gm->sim()->count_corruption(c.player, c.type);
#ifdef ALAMO_SIM_HAVE_STORY
    if (host_->story()) return;  // StorySession::end_battle applied it (route_corruption_mission)
#endif
    GalacticMode* g = host_->galactic_mode();
    if (won && g && g->sim() && c.planet < g->sim()->planets().size())  // GRC 4.3: timed gain over GMM +0x1FC (5 s)
        g->sim()->begin_corruption_gain(c.planet, c.type, c.player, 5.0f);
}

void CampaignBattles::story_load_map(galactic::BattleRequest& r) {
#ifdef ALAMO_SIM_HAVE_STORY
    StorySession* s = host_->story();
    if (!s) return;
    const bool space = r.info.kind == galactic::ConflictKind::Space;
    s->load_tactical_map(r.info.planet, space, host_->frame());
    const std::string map = s->take_tactical_map(space);
    if (map.empty()) return;
    log_.push_back("story: SET_TACTICAL_MAP " + map + " replaces " + r.map);
    r.map = map;
#else
    (void)r;
#endif
}
// --- end S4 ---

#ifdef ALAMO_SIM_HAVE_STORY
// --- L1/S4 wave 2a story hookup (story_session.hpp "battle-host calls") ---
void CampaignBattles::story_begin(SimHost& host, GameMode& mode) {
    StorySession* s = host.story();
    // q6: the battle's game-object scripts use this bridge too, so it is built without a story session as well when the
    // host runs object scripts (SimConfig::object_scripts; off by default, so the sessions before q6 are unchanged).
    ObjectScriptHost* scripts = host.object_scripts();
    if ((!s && !scripts) || !battle_) return;
    story_bridge_ = std::make_unique<lua::ObjectBridge>();
    lua::ObjectBridge& b = *story_bridge_;
    b.abilities = &battle_->abilities();
    b.ability_frame = [this] { return battle_ ? battle_->frame() : 0u; };
#ifdef ALAMO_SIM_HAVE_ANIM
    if (opt_.animation) b.anim = &opt_.animation->world(mode);
#endif
    SimHost* h = &host;
    b.anim_frame = [h] { return h->frame(); };
    b.change_owner = [this](objects::ObjectHandle o, int32_t owner) { return change_owner(o, owner); };
    b.garrison_spawn_changed = [this](objects::ObjectHandle o, bool on) {  // Lua Set_Garrison_Spawn -> hangar spawn flag
        TacticalMode* tm = host_->tactical();
        if (battle_ && tm && tm->primary().alive(o)) battle_->set_garrison_spawn(tm->primary().id(o), on);
    };
    b.is_enemy = [this](int32_t x, int32_t y) { return battle_ && battle_->combat().is_enemy(x, y); };
    // refixf t03: Find_All_Objects_Of_Type / Find_Nearest match TEAM containers on their company / team type.
    b.representative_type = [this](objects::ObjectHandle o) {
        TacticalMode* tm = host_->tactical();
        return (battle_ && tm && tm->primary().exists(o)) ? battle_->representative_type(tm->primary().id(o)) : objects::kNone;
    };
    b.pipeline = &host.pipeline();
    // --- L1/S4 wave 2b: Dummy_Barrage_Target for Lua position-targeted abilities, script hooks, feed state ---
    b.spawn_ability_target = [this, h](Vec3 p, int32_t owner) {
        TacticalMode* tm = h->tactical();
        return (battle_ && tm) ? battle_->spawn_dummy_target(*tm, owner, p, h->random().synced()) : objects::kNoObjectId;
    };
    // S7 t31 (AI_TACTICAL §7.3.3): signal 0xb of every host reaches the story VMs' Activate_Ability blocks (the unit, and its
    // team container); their own Activate_Ability raises it through the battle. The listener dies with the battle.
    battle_->add_order_replaced_listener([this](objects::ObjectId id) {
        TacticalMode* tm = host_->tactical();
        if (!story_bridge_ || !tm) return;
        story_bridge_->order_replaced(id);
        const objects::ObjectStore& st = tm->primary();
        const objects::ObjectHandle oh = st.find(id);
        const objects::ObjectHandle team = st.exists(oh) ? st.container(oh) : objects::ObjectHandle{};
        if (team && st.id(team) != id) story_bridge_->order_replaced(st.id(team));
    });
    b.raise_order_replaced = [this](objects::ObjectId id) {
        if (battle_) battle_->raise_order_replaced(id);
    };
    story_hooks_ = lua::TacticalScriptHooks{};
    story_transports_ = lua::CinematicTransports{};
    story_hooks_.spawn = [h](const objects::SpawnParams& p) {
        TacticalMode* tm = h->tactical();
        return tm ? tm->spawn(p) : objects::ObjectHandle{};
    };
    story_hooks_.transports = &story_transports_;
    b.tactical = &story_hooks_;
    // --- L1 bindings (script_remaining.hpp): hooks the battle's systems answer today; the rest keep their
    //     documented "not available" / data-fallback answers (space fields: the type-data fallback) ---
    story_remaining_ = lua::RemainingScriptHooks{};
    story_remaining_.energy = [this](objects::ObjectHandle u) {
        TacticalMode* tm = host_->tactical();
        if (!battle_ || !tm) return -1.0f;
        const objects::ObjectId id = tm->primary().id(u);
        return battle_->combat().has_unit(id) ? battle_->combat().energy(id) : -1.0f;
    };
    // Divert / Is_On_Diversion (L1_S4_REMAINING §2.3): a one-unit order_move of the unit's owner; the goal is the
    // movement system's current goal.
    story_remaining_.move_to = [this, h](objects::ObjectHandle u, Vec3 p) {
        TacticalMode* tm = h->tactical();
        if (!battle_ || !tm || !tm->primary().alive(u)) return false;
        const objects::ObjectId id = tm->primary().id(u);
        const int32_t owner = tm->primary().owner(u);
        const objects::ObjectId one[] = {id};
        return owner >= 0 && battle_->order_move(*tm, static_cast<uint8_t>(owner), one, p.x, p.y, h->random().synced()) ==
                                 commands::RejectReason::None;
    };
    story_remaining_.move_goal = [this, h](objects::ObjectHandle u, Vec3& goal) {
        TacticalMode* tm = h->tactical();
        if (!battle_ || !tm || !tm->primary().alive(u)) return false;
        const objects::ObjectId id = tm->primary().id(u);
        if (!battle_->movement().has_unit(id)) return false;
        const tactical::movement::UnitView v = battle_->movement().view(id);
        if (!v.has_goal) return false;
        goal = Vec3{v.goal.x, v.goal.y, 0.0f};
        return true;
    };
#ifdef ALAMO_SIM_HAVE_GARRISON
    story_remaining_.can_garrison_fire = [this](objects::ObjectHandle u) {
        TacticalMode* tm = host_->tactical();
        return battle_ && tm && battle_->can_garrison_fire(tm->primary().id(u));
    };
#endif
    b.remaining = &story_remaining_;
    // --- end L1 bindings ---
    story_motion_.clear();
    story_base_ids_.clear();
    if (GalacticMode* g = host.galactic_mode(); g && g->sim()) {
        const objects::ObjectStore& gst = g->sim()->store();
        if (gst.exists(pending_req_.starbase)) story_base_ids_.push_back(gst.id(pending_req_.starbase));
        for (const objects::ObjectHandle sh : pending_req_.structures)
            if (gst.exists(sh)) story_base_ids_.push_back(gst.id(sh));
    }
    // --- end L1/S4 wave 2b ---
    StoryBattleStart st;
    st.request = &pending_req_;
    st.store = &mode.primary();
    st.bridge = &b;
    story_fog_begin(st);  // fog follow-up
    l1_hooks(host);       // L1 systems: special weapons, TSW, engines, teleport, hyperspace, fields, weather
    // --- L1 bindings (script_remaining.hpp / script_gaps.hpp): the L1 systems APIs without a hook in l1_hooks ---
    {
        auto bid = [h](objects::ObjectHandle o) {
            TacticalMode* tm = h->tactical();
            return tm ? tm->primary().id(o) : objects::kNoObjectId;
        };
        story_hooks_.hyperspace_out_link = [this, bid](objects::ObjectHandle u, int32_t delay, bool keep) {
            return battle_ && battle_->hyperspace_out(bid(u), delay, keep);
        };
        lua::RemainingScriptHooks& rb = story_remaining_;
        rb.object_in_field = [this, bid](objects::ObjectHandle o, uint32_t kind) { return battle_ ? battle_->object_in_field(bid(o), kind) : -1; };
        rb.bombing_run_available = [this](int32_t p) { return battle_ && battle_->bombing_run_available(p); };
        rb.bombardment_available = [this](int32_t p) { return battle_ && battle_->bombardment_available(p); };
        rb.bombing_run_finished = [this](int32_t p) { return !battle_ || battle_->bombing_run_finished(p); };
        rb.bombing_run_result = [this](int32_t p) { return battle_ ? battle_->bombing_run_result(p) : 1.0f; };
        rb.set_bombing_run_disabled = [this](int32_t p, bool on) {
            if (battle_) battle_->set_bombing_run_disabled(p, on);
        };
        rb.set_bombardment_disabled = [this](int32_t p, bool on) {
            if (battle_) battle_->set_bombardment_disabled(p, on);
        };
        rb.remove_bombardment = [this](int32_t p, bool on) {
            if (battle_) battle_->remove_bombardment(p, on);
        };
        // disable_engines / enable_engines and weather(): no Lua binding of the RE table reads or drives them.
    }
    // --- end L1 bindings ---
    // --- S4 (owner: S4 agent): garrison containment for the tactical story VMs (Is_In_Garrison, Has_Garrison,
    // Get_Garrisoned_Units, Eject_Garrison, Get_Parent_Object): a garrisoned unit's container is its holder, a holder's
    // contents its occupants (entry order); other objects keep the store's container links. ---
    b.container_of = [this](objects::ObjectHandle o) {
        TacticalMode* tm = host_->tactical();
        if (!tm || !battle_) return objects::ObjectHandle{};
        const objects::ObjectStore& s2 = tm->primary();
        if (!s2.exists(o)) return objects::ObjectHandle{};
        const objects::ObjectId holder = battle_->garrison_holder_of(s2.id(o));
        return holder != objects::kNoObjectId ? s2.find(holder) : s2.container(o);
    };
    b.contents_of = [this](objects::ObjectHandle o) {
        std::vector<objects::ObjectHandle> out;
        TacticalMode* tm = host_->tactical();
        if (!tm || !battle_) return out;
        const objects::ObjectStore& s2 = tm->primary();
        if (!s2.exists(o)) return out;
        for (const objects::ObjectId id : battle_->garrisoned_units(s2.id(o))) out.push_back(s2.find(id));
        if (!out.empty()) return out;
        for (const objects::ObjectHandle m : s2.all_objects())
            if (s2.container(m) == o) out.push_back(m);
        return out;
    };
    if (corruption_active_.player >= 0 && !corruption_active_.plots.empty()) st.plots_file = corruption_active_.plots;  // mission plots
    // --- end S4 ---
    if (scripts) {  // q6: Spawn_Unit of an object script makes battle units (TacticalBattle::script_spawn)
        b.spawn_unit = [this, h](const objects::SpawnParams& p) {
            TacticalMode* tm = h->tactical();
            return (battle_ && tm) ? battle_->script_spawn(*tm, p) : objects::ObjectHandle{};
        };
        // q6 t12: companies come down as battle companies (members + TEAM containers), Take_Damage goes through combat.
        // [port] Set only with object scripts on (the bridge is shared with the story VMs of this battle), so sessions
        // without them keep the store-only Take_Damage of the story bindings.
        b.spawn_company = [this, h](const objects::SpawnParams& p) {
            TacticalMode* tm = h->tactical();
            return (battle_ && tm) ? battle_->script_spawn_company(*tm, p) : objects::ObjectHandle{};
        };
        b.take_damage = [this, h](objects::ObjectHandle u, float amount, bool cannot_die) {
            TacticalMode* tm = h->tactical();
            return battle_ && tm && tm->primary().alive(u) && battle_->script_damage(*tm, tm->primary().id(u), amount, cannot_die);
        };
        // q6 (OBJECT_SCRIPTS §10.1-10.3): Make_Invulnerable -> combat's damage-kind gate, Prevent_AI_Usage -> the owner's
        // AI free store, Hide -> the model test. [port] Object-script sessions only, like the hooks above.
        b.invulnerable_changed = [this, h](objects::ObjectHandle u, bool on) {
            TacticalMode* tm = h->tactical();
            if (battle_ && tm && tm->primary().alive(u)) battle_->script_invulnerable(tm->primary().id(u), on);
        };
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI
        b.prevent_ai_usage = [this, h](objects::ObjectHandle u, bool on) {
            TacticalMode* tm = h->tactical();
            if (!ai_ || !tm || !tm->primary().alive(u)) return lua::PreventAiResult::NoAiPlayer;
            const objects::ObjectStore& s2 = tm->primary();
            return ai_->world().prevent_ai_usage(s2.id(u), s2.owner(u), on);
        };
#else
        b.prevent_ai_usage = [](objects::ObjectHandle, bool) { return lua::PreventAiResult::NoAiPlayer; };
#endif
        b.has_model = [this, h](objects::ObjectHandle u) {
            TacticalMode* tm = h->tactical();
            return !battle_ || !tm || battle_->script_has_model(*tm, u);
        };
        b.spawn_facing = [this](Vec3 pos) { return battle_ ? battle_->script_spawn_facing(pos) : Vec3{1.0f, 0.0f, 0.0f}; };
        scripts->set_bridge(mode.primary(), &b);
#if defined(ALAMO_LUA_PLAYERS)
        scripts->set_player_world(mode.primary(), s ? s->player_world() : host.object_script_players());
#endif
    }
    if (!s) return;
    std::vector<std::string> diag;
    s->begin_battle(st, host.frame(), &diag);
    for (std::string& d : diag) log_.push_back(std::move(d));
    if (corruption_active_.player >= 0) s->set_corruption_mission(corruption_active_.player, corruption_active_.type);  // S4
}

// This tick's combat deaths and hardpoint destructions -> StorySession::notify_tactical (names resolved now).
void CampaignBattles::story_feed(SimHost& host) {
    StorySession* s = host.story();
    if (!s || !battle_ || !s->battle_attached()) return;
    const tactical::combat::CombatSystem& cs = battle_->combat();
    for (const tactical::combat::HardpointEvent& e : cs.hardpoint_events()) {
        TacticalStoryNotification n;
        n.kind = TacticalStoryEvent::HardpointDestroyed;
        n.object = e.owner;
        if (cs.has_unit(e.owner)) {  // HardPoints list entry -> HardpointDesc name
            const auto& unit = cs.data().units[cs.desc_index(e.owner)];
            if (e.index < unit.hardpoints.size() && unit.hardpoints[e.index] < cs.data().hardpoints.size())
                n.name = cs.data().hardpoints[unit.hardpoints[e.index]].name;
        }
        if (!n.name.empty()) s->notify_tactical(n);  // the attacker is not in the event: no filter owner
    }
    for (const tactical::combat::DeathEvent& d : cs.deaths()) {
        TacticalStoryNotification n;
        n.kind = TacticalStoryEvent::Destroyed;
        n.object = d.id;
        n.other = d.killer == tactical::combat::kNoObject ? 0 : d.killer;
        s->notify_tactical(n);
    }
    // --- L1/S4 wave 2b feeds ---
    TacticalMode* tm = host.tactical();
    if (!tm) return;
    objects::ObjectStore& store = tm->primary();
    // STORY_DESTROY_BASE: the dead object stands for the request's starbase / a ground structure [inferred].
    for (const tactical::combat::DeathEvent& d : cs.deaths()) {
        const objects::ObjectHandle dh = store.find(d.id);
        if (!dh || std::find(story_base_ids_.begin(), story_base_ids_.end(), store.parent_mode_id(dh)) == story_base_ids_.end()) continue;
        TacticalStoryNotification n;
        n.kind = TacticalStoryEvent::BaseDestroyed;
        n.object = d.id;
        s->notify_tactical(n);
    }
    // STORY_REINFORCE: reserve units deployed this tick.
    for (const objects::ObjectId id : battle_->story_reinforced()) {
        TacticalStoryNotification n;
        n.kind = TacticalStoryEvent::Reinforced;
        n.object = id;
        s->notify_tactical(n);
    }
    // STORY_UNIT_ARRIVED (reached its ordered destination) / STORY_FULL_STOP (speed fell to 0) [inferred: the
    // original's dispatch sites are not traced], from the movement state transitions, ascending id.
    const tactical::movement::MovementSystem& mv = battle_->movement();
    for (const objects::ObjectId id : mv.unit_ids()) {
        const tactical::movement::UnitView v = mv.view(id);
        const uint8_t now = static_cast<uint8_t>((v.arrived ? 1u : 0u) | (v.speed > 0.0f ? 2u : 0u));
        const auto it = story_motion_.find(id);
        const uint8_t before = it == story_motion_.end() ? uint8_t{0} : it->second;
        story_motion_[id] = now;
        TacticalStoryNotification n;
        n.object = id;
        if ((now & 1u) && !(before & 1u)) {
            n.kind = TacticalStoryEvent::UnitArrived;
            s->notify_tactical(n);
        }
        if (!(now & 2u) && (before & 2u)) {
            n.kind = TacticalStoryEvent::FullStop;
            s->notify_tactical(n);
        }
    }
    // --- S4 (owner: S4 agent): STORY_GARRISON_UNIT (name = the entering unit's type, filter owner = its owner, other =
    // the holder [inferred: SingleObjectName on the unit]) and STORY_CAPTURE_STRUCTURE (the captured structure, filter
    // owner = its new owner) from this tick's battle garrison / capture events. ---
    for (const TacticalBattle::StoryGarrisonEvent& e : battle_->story_garrisoned()) {
        TacticalStoryNotification n;
        n.kind = TacticalStoryEvent::Garrisoned;
        n.object = e.unit;
        n.other = e.holder;
        s->notify_tactical(n);
    }
    for (const TacticalBattle::StoryCaptureEvent& e : battle_->story_captured()) {
        TacticalStoryNotification n;
        n.kind = TacticalStoryEvent::StructureCaptured;
        n.object = e.object;
        n.player = e.new_owner;
        s->notify_tactical(n);
    }
    // STORY_CAPTURE_HERO has no dispatcher in 1.121 (L1_S4_REMAINING.md §4.3): never fed.
    // --- end S4 ---
    // Create_Cinematic_Transport landing machines (script_gaps.hpp): clip lengths unknown here (the default), the
    // take-off end destroys the transport.
    if (!story_transports_.entries().empty()) {
        std::vector<objects::ObjectId> gone;
        for (const lua::CinematicLanding& l : story_transports_.entries())
            if (!store.alive(store.find(l.id))) gone.push_back(l.id);
        for (const objects::ObjectId id : gone) story_transports_.remove(id);
        std::vector<lua::CinematicStep> steps;
        story_transports_.service(battle_->frame(), l1_clip_frames(), steps);  // L1 systems: Land / Takeoff clip lengths
        for (const lua::CinematicStep& st : steps) {
            l1_transport_step(st);  // L1 systems: Land / Takeoff playback (sim animation)
            if (st.kind == lua::CinematicStep::Kind::Destroy)
                if (const objects::ObjectHandle th = store.find(st.id); th && store.alive(th)) tm->destroy(th);
        }
    }
}
// --- end L1/S4 wave 2a ---

// --- fog follow-up (campaign_battles.hpp): fog for the tactical story, station special weapon, ENABLE_FOW ---
void CampaignBattles::story_fog_begin(StoryBattleStart& st) {
    // S8: Fire_Special_Weapon on a space station (SCRIPT_GAPS.md §1.1 step 5) -> the battle's CombatSystem (the
    // can-fire test refuses targets hidden from the station's owner with force 1).
    story_hooks_.station_readiness = [this](objects::ObjectHandle h) {
        TacticalMode* tm = host_->tactical();
        return (battle_ && tm) ? battle_->combat().special_weapon_readiness(tm->primary().id(h)) : -1.0f;
    };
    story_hooks_.station_fire = [this](objects::ObjectHandle h, objects::ObjectHandle t, int32_t) {
        TacticalMode* tm = host_->tactical();
        if (!battle_ || !tm) return false;
        const objects::ObjectStore& s = tm->primary();
        return battle_->abilities().combat().special_weapon_fire(s.id(h), s.id(t));
    };
#ifdef ALAMO_SIM_HAVE_FOG
    // STORY_FOG_POSITION_REVEAL / STORY_FOG_OBJECT_REVEAL (FOG_OF_WAR.md §6.6): "not fogged" for the plot's player,
    // force 0 (both fire at once for non-human plot players and while the fog is off, as in the original).
    st.fog_point_visible = [this](int32_t player, double x, double y, double) {
        return battle_ && !battle_->fog_point_fogged(player, static_cast<float>(x), static_cast<float>(y), false);
    };
    st.fog_object_visible = [this](int32_t player, int32_t id) { return battle_ && !battle_->fog_object_hidden(player, id, false); };
#else
    (void)st;
#endif
#if defined(ALAMO_LUA_HAVE_FOG) && defined(ALAMO_SIM_HAVE_FOG)
    // L1: FogOfWar.* / Undo_Reveal of the tactical story VMs on the battle's fog (null when BattleConfig::fog is off).
    story_fog_.fog = battle_ ? battle_->fog() : nullptr;
    story_fog_.epoch = fought_;
    story_fog_.cells.clear();
    story_fog_.object = [this](objects::ObjectHandle h, tactical::fog::FogObject& out) {
        TacticalMode* tm = host_->tactical();
        return battle_ && tm && battle_->fog_object(tm->primary().id(h), out);
    };
    // refixf t03: Find_Nearest with a player (FOW_Object_Hidden, FOG_OF_WAR §5).
    story_fog_.object_hidden = [this](objects::ObjectHandle h, int32_t player, bool force) {
        TacticalMode* tm = host_->tactical();
        return battle_ && tm && tm->primary().exists(h) && battle_->fog_object_hidden(player, tm->primary().id(h), force);
    };
    story_fog_.set_globals = [this](bool fow_active, bool render_option) {
        fog_active_ = fow_active;
        fog_render_ = render_option;
        if (battle_) battle_->set_fog_active(fow_active, render_option);  // battle host (fog agent ask): the battle's carried flags
    };
    story_hooks_.fog = &story_fog_;
#endif
}

void CampaignBattles::story_fog_reward(bool has_param, int32_t value) {
#ifdef ALAMO_SIM_HAVE_FOG
    if (battle_)
        if (tactical::fog::FogOfWar* f = battle_->fog()) {
            f->reward_enable_fow(has_param, value);
            return;
        }
#endif
    // No battle running (or its fog off): the carried global flags, same rule as FogOfWar::reward_enable_fow.
    if (!has_param) {
        fog_active_ = false;
    } else {
        fog_active_ = value != 0;
        fog_render_ = value != 0;
    }
}
// --- end fog follow-up ---

// --- L1/S4 wave 2b: story-forced battles. Each new StorySession::linked_tactical() entry (a LINK_TACTICAL reward) is
// parsed (parse_link_tactical, BT §2.9.1), written as the linked-battle record (queue_linked, 0x28bd70: refused while a
// record waits) and started at once (start_linked_record, 0x28f590; story t07). Entries left after a start wait for a
// later hook (the battle is pending). ---
namespace {
bool same_text(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const char x = (a[i] >= 'a' && a[i] <= 'z') ? static_cast<char>(a[i] - 'a' + 'A') : a[i];
        const char y = (b[i] >= 'a' && b[i] <= 'z') ? static_cast<char>(b[i] - 'a' + 'A') : b[i];
        if (x != y) return false;
    }
    return true;
}
std::string trimmed_text(std::string_view v) {
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.remove_prefix(1);
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.remove_suffix(1);
    return std::string(v);
}
// [port] a reward bool: an integer (atoi != 0) or TRUE / YES; empty = the default.
bool reward_bool(const std::string& v, bool def) {
    if (v.empty()) return def;
    if (same_text(v, "TRUE") || same_text(v, "YES")) return true;
    return std::atoi(v.c_str()) != 0;
}
}  // namespace

// BT §2.9.1: P1 planet (map name or planet type name), P2 SPACE / GROUND (a word the keyword map lacks, as LAND, is land;
// EITHER is refused), P3 attacker faction, P4 map, P5 defender faction (P1-P5 non-empty, else silently nothing), P6 forced
// type, P7 plots, P8 sandbox, P9 arrival (default 1), P10 / P11 presentation, P12 resolution (RETREAT_PLAYER / RETREAT_AI /
// DESTROY_AI; empty or unknown = DESTROY_AI, unknown logged), P13 banner (default on), P14 flagship split (default on).
bool CampaignBattles::parse_link_tactical(const std::vector<std::string>& params, LinkedRecord& out, std::string& why) {
    why.clear();
    GalacticMode* g = host_->galactic_mode();
    if (!g || !g->sim()) return false;
    const galactic::GalacticSim& gs = *g->sim();
    auto param = [&params](size_t i) { return i < params.size() ? trimmed_text(params[i]) : std::string(); };
    for (size_t i = 0; i < 5; ++i)
        if (param(i).empty()) return false;
    PlanetIndex planet = kInvalidPlanet;
    const auto& names = gs.map().planets.name;
    for (PlanetIndex i = 0; i < gs.planets().size() && planet == kInvalidPlanet; ++i) {
        const uint32_t ty = gs.planets()[i].type;
        if ((i < names.size() && same_text(names[i], params[0])) ||
            (ty < gs.rules().types.size() && same_text(gs.rules().types[ty].name, params[0])))
            planet = i;
    }
    const int32_t attacker = gs.player_of_faction(params[2]);
    const std::string mode = param(1);
    if (planet == kInvalidPlanet || attacker < 0 || same_text(mode, "EITHER")) {
        why = "planet / mode / attacker invalid";
        return false;
    }
    out = LinkedRecord{};
    out.kind = same_text(mode, "SPACE") ? galactic::ConflictKind::Space : galactic::ConflictKind::Land;
    out.planet = planet;
    out.attacker = attacker;
    out.map = param(3);
    out.defender = param(4);
    if (const std::string t = param(5); !t.empty())  // P6: unknown type = none
        for (uint32_t i = 0; i < gs.rules().types.size() && out.forced_type == galactic::kNoIndex; ++i)
            if (same_text(gs.rules().types[i].name, t)) out.forced_type = i;
    out.plots = param(6);
    out.sandbox = std::atoi(param(7).c_str()) != 0;
    if (const std::string v = param(8); !v.empty()) out.arrival = std::atoi(v.c_str());
    out.presentation_a = reward_bool(param(9), false);
    out.presentation_b = reward_bool(param(10), false);
    if (const std::string v = param(11); !v.empty()) {
        if (same_text(v, "RETREAT_PLAYER")) out.resolution = galactic::LinkedResolution::RetreatPlayer;
        else if (same_text(v, "RETREAT_AI")) out.resolution = galactic::LinkedResolution::RetreatAi;
        else if (same_text(v, "DESTROY_AI")) out.resolution = galactic::LinkedResolution::DestroyAi;
        else log_.push_back("story: LINK_TACTICAL " + params[0] + ": Unknown resolution " + v);  // 0 -> 3
    }
    out.banner = reward_bool(param(12), true);
    out.flagship_split = reward_bool(param(13), true);
    return true;
}

void CampaignBattles::story_linked(SimHost& host) {
    StorySession* s = host.story();
    if (!s || !cm_) return;
    const std::vector<StoryLinkedTactical>& linked = s->linked_tactical();
    for (; story_linked_ < linked.size() && !pending_; ++story_linked_) {
        const StoryLinkedTactical& l = linked[story_linked_];
        LinkedRecord rec;
        std::string why;
        const std::string name = l.params.empty() ? std::string() : l.params[0];
        if (!parse_link_tactical(l.params, rec, why)) {
            if (!why.empty()) log_.push_back("story: LINK_TACTICAL " + name + " not started (" + why + ")");
            continue;
        }
        if (!queue_linked(std::move(rec))) {
            log_.push_back("story: LINK_TACTICAL " + name + " not started (a linked battle record waits)");
            continue;
        }
        lk_.source = "story: LINK_TACTICAL " + name;
        start_linked_record();  // the reward handler's 0x28f590 (the galaxy is the current mode)
    }
}

// story t07: the last-invade vector from the landings the story routed (STORY_LAND_ON / DEPLOY sources): a human player's
// UnitsLanded = the last `value` surface units of that player at the planet [port: StorySession's own mapping].
void CampaignBattles::story_landings(SimHost& host) {
    StorySession* s = host.story();
    GalacticMode* g = host.galactic_mode();
    if (!s || !g || !g->sim()) return;
    const galactic::GalacticSim& gs = *g->sim();
    const std::vector<StoryTrigger>& tr = s->triggers();
    if (lk_.triggers_seen > tr.size()) lk_.triggers_seen = tr.size();  // another session
    for (; lk_.triggers_seen < tr.size(); ++lk_.triggers_seen) {
        const galactic::GalacticEvent& e = tr[static_cast<size_t>(lk_.triggers_seen)].event;
        if (e.kind != galactic::GalacticEventKind::UnitsLanded || e.value <= 0 || e.planet >= gs.planets().size() || e.player < 0 ||
            static_cast<size_t>(e.player) >= gs.players().size() || !gs.players()[static_cast<size_t>(e.player)].human)
            continue;
        std::vector<objects::ObjectHandle> mine;
        for (const objects::ObjectHandle u : gs.planets()[e.planet].surface)
            if (gs.store().alive(u) && gs.store().owner(u) == e.player) mine.push_back(u);
        const size_t k = std::min(mine.size(), static_cast<size_t>(e.value));
        lk_.last_invade.assign(mine.end() - static_cast<std::ptrdiff_t>(k), mine.end());
    }
}
// --- end L1/S4 wave 2b ---

#ifdef ALAMO_SIM_HAVE_TACTICAL_AI
// a1j t07 (A1_READINGS §38): the parent galaxy of the battle's AI scripts. Target = the battle's conflict planet (0x28a7a0 ->
// 0x28aa50: the parent's head conflict; a map battle has none). Releases (0x6147d0 [H]) draw on the player's land credit
// transfer: amount < 0 or not below what is left (saved - released) = all of it; Add_Credits with aiGain 0 (no
// Credit_Multiplier); released += amount, capped at saved. [port] A player without a transfer releases nothing.
void CampaignBattles::galactic_context_begin(ModeKind kind) {
    if (!ai_ || !opt_.tactical_ai || !opt_.galactic_resolver) return;
    const ai::TokenResolver* res = opt_.galactic_resolver();
    GalacticMode* g = host_->galactic_mode();
    if (!res || !g || !g->sim()) return;
    galactic::GalacticSim* gs = g->sim();
    lua::TacticalGalacticContext c;
    c.source.equations = opt_.tactical_ai->equations;
    c.source.resolver = res;
    const PlanetIndex planet = pending_req_.info.planet;
    c.target = !direct_ && planet < gs->planets().size() ? static_cast<int32_t>(planet) : -1;
    c.land = kind == ModeKind::Land;
    if (opt_.has_galactic_ai) {
        c.has_galactic_ai = opt_.has_galactic_ai;
    } else {
        c.has_galactic_ai = [gs](int32_t p) { return p >= 0 && static_cast<size_t>(p) < gs->players().size() && gs->players()[static_cast<size_t>(p)].ai; };
    }
#ifdef ALAMO_SIM_HAVE_AI_TACTICAL
    c.release_credits = [this, gs](int32_t p, double amount) {
        galactic::econ::CreditAccount* acct = gs->credit_account(p);
        if (!acct) return;
        for (CreditTransfer& ct : credits_) {
            if (ct.player != p) continue;
            const float left = ct.state.saved - ct.state.released;
            float a = static_cast<float>(amount);
            if (a < 0.0f || left <= a) a = left;
            galactic::econ::add_credits(*acct, a, false, 1.0f);
            ct.state.released = std::min(a + ct.state.released, ct.state.saved);
            return;
        }
    };
#endif
    ai_->world().player_world().set_tactical_galactic(std::move(c));
}
#endif

// --- L1 systems (owner: tactical-systems agent): lua::TacticalScriptHooks / RemainingScriptHooks answered by the
// running battle's L1 systems (tactical_battle.hpp "L1 systems"). Handles are the battle mode's primary store. ---
void CampaignBattles::l1_hooks(SimHost& host) {
    SimHost* h = &host;
    auto id_of = [h](objects::ObjectHandle o) {
        TacticalMode* tm = h->tactical();
        return tm ? tm->primary().id(o) : objects::kNoObjectId;
    };
    lua::TacticalScriptHooks& k = story_hooks_;
    k.weapon_readiness = [this, id_of](objects::ObjectHandle w) { return battle_ ? battle_->sw_readiness(id_of(w)) : -1.0f; };
    k.begin_targeting = [this, id_of](objects::ObjectHandle w) { return battle_ && battle_->sw_begin_targeting(id_of(w)); };
    k.fire_at_object = [this, h, id_of](objects::ObjectHandle w, objects::ObjectHandle t) {
        TacticalMode* tm = h->tactical();
        return battle_ && tm && battle_->sw_fire_at_object(*tm, id_of(w), id_of(t));
    };
    k.fire_lobbing_at = [this, h, id_of](objects::ObjectHandle w, Vec3 p) {
        TacticalMode* tm = h->tactical();
        return battle_ && tm && battle_->sw_fire_lobbing_at(*tm, id_of(w), p);
    };
    k.special_weapons_allowed = [this](int32_t player) { return !battle_ || battle_->special_weapons_allowed(player); };
    k.tsw_state = [this, id_of](objects::ObjectHandle t) { return battle_ ? battle_->tsw_state(id_of(t)) : -1; };
    k.tsw_fire = [this, id_of](objects::ObjectHandle t) { return battle_ && battle_->tsw_fire(id_of(t)); };
    k.engines_online = [this, id_of](objects::ObjectHandle u) { return battle_ ? battle_->engines_online(id_of(u)) : -1; };
    k.ground_height = [this](float x, float y) { return battle_ ? battle_->ground_height(x, y) : -3.0e38f; };
    k.teleport = [this, h, id_of](objects::ObjectHandle u, Vec3 p, Vec3 f) {
        if (TacticalMode* tm = h->tactical(); battle_ && tm) battle_->teleport(*tm, id_of(u), p, f);
    };
    // Locomotor +0x2a8 from Start_Cinematic_Space_Retreat (the hook's only caller): keep_parent_link = false (the
    // galactic unit survives; L1_S4_REMAINING §3.4).
    k.hyperspace_out = [this, id_of](objects::ObjectHandle u, int32_t delay) {
        if (battle_) battle_->hyperspace_out(id_of(u), delay, false);
    };
    k.retreat_increment_frames = [this](int32_t player) { return battle_ ? battle_->retreat_increment_frames(player) : 0; };
    // Script spawns through the mode; special weapons / TSWs among them join the L1 systems (Spawn_Special_Weapon).
    k.spawn = [this, h](const objects::SpawnParams& p) {
        TacticalMode* tm = h->tactical();
        if (!tm) return objects::ObjectHandle{};
        const objects::ObjectHandle o = tm->spawn(p);
        if (battle_) battle_->adopt_object(*tm, o);
        return o;
    };
    // Space fields (Is_In_* / Is_Point_In_* / Find_Nearest_Space_Field): the battle's field index (kField* bits are
    // the Lua mask bits).
    lua::RemainingScriptHooks& r = story_remaining_;    r.force_weather = [this, h]() { return battle_ && battle_->force_weather(h->random().synced()); };
    r.opposite_sides_of_shield = [this](Vec3 a, Vec3 b, int32_t) {  // 2-argument form: no player filter
        return battle_ && battle_->opposite_sides_of_shield(a, b, -1, true);
    };
    r.opposite_sides_of_shield4 = [this](Vec3 a, Vec3 b, int32_t player, bool friendly) {
        return battle_ && battle_->opposite_sides_of_shield(a, b, player, friendly);
    };
    r.projectile_types = [this, id_of](objects::ObjectHandle u) {
        return battle_ ? battle_->projectile_types(id_of(u)) : std::vector<objects::TypeIndex>{};
    };
    r.rate_of_damage = [this, id_of](objects::ObjectHandle u) { return battle_ ? battle_->rate_of_damage(id_of(u)) : -1.0; };
    r.time_till_dead = [this, id_of](objects::ObjectHandle u) { return battle_ ? battle_->time_till_dead(id_of(u)) : -1.0; };
    r.final_blow_player = [this, id_of](objects::ObjectHandle o) { return battle_ ? battle_->final_blow_player(id_of(o)) : -1; };
    r.current_projectile = [this, id_of](objects::ObjectHandle u) { return battle_ ? battle_->current_projectile(id_of(u)) : objects::kNone; };
    r.orbital_bombardment = [this, h](int32_t player, Vec3 p) {
        TacticalMode* tm = h->tactical();
        return battle_ && tm && battle_->start_bombardment(*tm, player, p, h->random().synced());
    };
    r.bombing_run = [this, h](int32_t player, Vec3 p) {
        TacticalMode* tm = h->tactical();
        return battle_ && tm && battle_->start_bombing_run(*tm, player, p);
    };
    r.point_in_field = [this](Vec3 p, uint32_t mask) { return battle_ && (battle_->field_kinds_at(p.x, p.y) & mask) != 0; };
    r.nearest_field = [this, h](Vec3 p, uint32_t mask) {
        TacticalMode* tm = h->tactical();
        if (!battle_ || !tm) return objects::ObjectHandle{};
        const objects::ObjectId id = battle_->nearest_field(p.x, p.y, mask);
        return id == objects::kNoObjectId ? objects::ObjectHandle{} : tm->primary().find(id);
    };
    // Int-final: build pads (Build / Get_Build_Pad_Contents / Lock_Build_Pad_Contents -> the battle's pad host).
    r.build = [this, h, id_of](objects::ObjectHandle pad, objects::TypeIndex t, bool check) {
        TacticalMode* tm = h->tactical();
        return (battle_ && tm) ? battle_->pad_build(*tm, id_of(pad), t, check) : -1;
    };
    r.build_pad_contents = [this, h, id_of](objects::ObjectHandle pad) {
        TacticalMode* tm = h->tactical();
        if (!battle_ || !tm) return objects::ObjectHandle{};
        const objects::ObjectId id = battle_->pad_contents(id_of(pad));
        return id == objects::kNoObjectId ? objects::ObjectHandle{} : tm->primary().find(id);
    };
    r.lock_build_pad = [this, id_of](objects::ObjectHandle pad, bool lock) {
        if (battle_) battle_->pad_lock(id_of(pad), lock);
    };
}

std::function<uint32_t(objects::ObjectId, uint8_t)> CampaignBattles::l1_clip_frames() {
    return [this](objects::ObjectId id, uint8_t anim) {
        TacticalMode* tm = host_->tactical();
        return (battle_ && tm) ? battle_->anim_clip_frames(*tm, id, anim) : 0u;
    };
}

void CampaignBattles::l1_transport_step(const lua::CinematicStep& st) {
    TacticalMode* tm = host_->tactical();
    if (!battle_ || !tm) return;
    if (st.kind == lua::CinematicStep::Kind::PlayLand)
        battle_->play_clip(*tm, st.id, lua::kAnimLand, st.start_fraction, st.speed, host_->frame());
    else if (st.kind == lua::CinematicStep::Kind::PlayTakeoff)
        battle_->play_clip(*tm, st.id, lua::kAnimTakeoff, st.start_fraction, st.speed, host_->frame());
}
// --- end L1 systems ---
#endif

// --- Int-final: the hosts' tactical-systems data (campaign_battles.hpp load_tactical_systems) ---
void load_tactical_systems(const FileProvider& provider, const galactic::GalacticRules& rules, const data::GameConstants* constants,
                           CampaignBattleOptions& opt) {
    opt.retreat_increment_frames = load_retreat_increments(provider, rules);
    opt.retreat_flight_increments = load_retreat_flight_increments(provider, rules);
    opt.l1_factions = load_l1_factions(provider, rules);
    if (constants) opt.l1_constants = load_l1_constants(*constants);
    auto wd = std::make_shared<tactical::combat::WeatherData>();
    opt.weather = (tactical::combat::load_weather_data(provider, *wd) && !wd->scenarios.empty()) ? std::move(wd) : nullptr;
}
// --- end Int-final ---

// ---- [port] galactic AI stand-in ----

void CampaignAggressor::operator()(SimHost& host, uint8_t player, uint64_t) const {
    GalacticMode* g = host.galactic_mode();
    if (!g || !g->sim() || host.modes().top() != g || g->paused()) return;
    galactic::GalacticSim& gs = *g->sim();
    const int32_t p = player;
    if (static_cast<size_t>(p) >= gs.players().size() || p == gs.neutral_player()) return;
    const uint32_t f = gs.players()[static_cast<size_t>(p)].faction;
    if (f >= gs.rules().factions.size() || !gs.rules().factions[f].is_playable) return;
    const uint32_t per = period ? period : 1;
    if (static_cast<uint32_t>(gs.frame()) % per != (static_cast<uint32_t>(p) * 97u) % per) return;
    const auto& store = gs.store();
    const auto& types = gs.rules().types;
    // Invade from orbit where the planet is held by a hostile player and nothing is queued there.
    for (uint32_t fi = 0; fi < gs.fleets().size(); ++fi) {
        const galactic::FleetState& fl = gs.fleets()[fi];
        if (!fl.alive || fl.owner != p || fl.moving || fl.planet == kInvalidPlanet) continue;
        const galactic::PlanetState& pl = gs.planets()[fl.planet];
        if (pl.in_battle || pl.owner == p || pl.owner < 0 || !gs.is_hostile(pl.owner, p)) continue;
        bool ground = false;
        for (objects::ObjectHandle m : fl.contents)
            ground = ground || (store.alive(m) && types[store.type(m)].kind == galactic::UnitKind::GroundUnit);
        if (ground) {
            gs.invade(p, fl.planet, {});
            return;
        }
    }
    // Move the first idle combat fleet to the nearest hostile planet.
    for (uint32_t fi = 0; fi < gs.fleets().size(); ++fi) {
        const galactic::FleetState& fl = gs.fleets()[fi];
        if (!fl.alive || fl.owner != p || fl.moving || fl.planet == kInvalidPlanet) continue;
        bool combat = false;
        for (objects::ObjectHandle m : fl.contents)
            combat = combat || (store.alive(m) && types[store.type(m)].ai_combat_power > 0.0f && types[store.type(m)].hyperspace);
        if (!combat) continue;
        const galactic::PlanetGraph& graph = gs.graph();
        std::vector<uint8_t> seen(gs.planets().size(), 0);
        std::vector<PlanetIndex> frontier{fl.planet};
        seen[fl.planet] = 1;
        for (size_t h = 0; h < frontier.size(); ++h) {
            const PlanetIndex at = frontier[h];
            for (uint32_t e = graph.edge_begin(at); e < graph.edge_end(at); ++e) {
                const PlanetIndex nb = graph.neighbour(e);
                if (nb >= seen.size() || seen[nb]) continue;
                seen[nb] = 1;
                const galactic::PlanetState& q = gs.planets()[nb];
                if (q.is_planet && !q.in_battle && q.owner >= 0 && gs.is_hostile(q.owner, p)) {
                    // Ground units on the origin's surface ride along (all but the first one) to invade later.
                    std::vector<objects::ObjectHandle> ride;
                    bool kept = false;
                    for (objects::ObjectHandle u : gs.planets()[fl.planet].surface) {
                        if (!store.alive(u) || store.owner(u) != p) continue;
                        if (!kept) kept = true;
                        else ride.push_back(u);
                    }
                    const PlanetIndex from = fl.planet;
                    if (!ride.empty()) gs.launch_units(p, from, ride);
                    // launch_units may have reallocated the fleet table: look the fleet up again.
                    if (fi < gs.fleets().size() && gs.fleets()[fi].alive) gs.move_fleet(p, fi, nb);
                    return;
                }
                frontier.push_back(nb);
            }
        }
        return;  // only the first idle combat fleet moves per turn
    }
}

}  // namespace alamo::sim
