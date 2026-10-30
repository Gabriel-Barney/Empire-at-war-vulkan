#include "alamo/sim/campaign_battles.hpp"

#include <algorithm>

#include "alamo/galactic/campaign.hpp"
#include "alamo/galactic/corruption.hpp"  // S4: corruption missions
#ifdef ALAMO_SIM_HAVE_STORY
#include "alamo/sim/sim_anim.hpp"  // L1/S4 wave 2a story hookup (SimAnimation::world)
#endif
#include "save_io.hpp"  // N1 stage 2

namespace alamo::sim {

// --- battle host (BT pass): the story-block gate in front of the ConflictManager (campaign_battles.hpp) ---
class CampaignBattles::ConflictGate final : public galactic::ConflictHooks {
public:
    explicit ConflictGate(CampaignBattles& b) : b_(&b) {}
    void on_space_conflict(PlanetIndex planet, int32_t attacker) override { b_->cm_->on_space_conflict(planet, attacker); }
    bool on_invade(int32_t attacker, PlanetIndex planet, std::span<const objects::ObjectHandle> units) override {
        return b_->cm_->on_invade(attacker, planet, units);
    }
    void on_service_tail() override {
        // BT §1.4 step 2: in single player, while a story / tutorial sequence is active and the local (human) player is
        // attacker or defender of any queued conflict, nothing of the conflict processing runs this tick.
        if (b_->opt_.story_sequence_active && b_->opt_.story_sequence_active() && human_involved()) return;
        b_->cm_->on_service_tail();
    }

private:
    bool human_involved() const {
        GalacticMode* g = b_->host_->galactic_mode();
        if (!g || !g->sim()) return false;
        const auto& ps = g->sim()->players();
        auto human = [&ps](int32_t p) { return p >= 0 && static_cast<size_t>(p) < ps.size() && ps[static_cast<size_t>(p)].human; };
        for (const auto* q : {&b_->cm_->space_queue(), &b_->cm_->land_queue()})
            for (const galactic::ConflictInfo& c : *q)
                if (human(c.attacker) || human(c.defender)) return true;
        return false;
    }
    CampaignBattles* b_;
};
// --- end battle host ---

CampaignBattles::MapLoader CampaignBattles::file_map_loader(const FileProvider& files) {
    const FileProvider* f = &files;
    return [f](const std::string& name, terrain::MapData& out) { return terrain::load_map(*f, name, out); };
}

CampaignBattles::CampaignBattles(SimHost& host, const galactic::ConflictRules& rules, const BattleCatalog& catalog,
                                 MapLoader loader, CampaignBattleOptions options)
    : host_(&host), rules_(&rules), catalog_(&catalog), loader_(std::move(loader)), opt_(options) {}

CampaignBattles::~CampaignBattles() {
    if (cm_) {  // N1: the save participants attach() registered
        host_->remove_save_participant("galactic.conflicts");
        host_->remove_save_participant("battles");
    }
    remove_battle_chunks();  // N1 stage 2
    host_->set_after_mode_update({});
    host_->set_tactical_command_hook({});
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
    if (opt_.story_sequence_active) {  // BT §1.4 step 2: the gate forwards to the manager unless the story block holds
        gate_ = std::make_unique<ConflictGate>(*this);
        g->sim()->set_conflict_hooks(gate_.get());
    }
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
            for (const auto& [pl, v] : pad_records_) {  // REMAINING_GAPS §6.5: only planets with records (none: no bytes)
                if (v.empty()) continue;
                h.u32(static_cast<uint32_t>(pl));
                h.u32(static_cast<uint32_t>(v.size()));
                for (const TacticalBattle::PadRecord& r : v) {
                    h.u32(r.type);
                    h.f32(r.x);
                    h.f32(r.y);
                    h.f32(r.z);
                    h.i32(r.level);
                    h.i32(r.value);
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
constexpr uint32_t kBattlesSaveVersion = 2;  // 2: + the battle in flight (stage 2)
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
    w.u32(static_cast<uint32_t>(pad_records_.size()));
    for (const auto& [planet, recs] : pad_records_) {
        w.u32(static_cast<uint32_t>(planet)).u32(static_cast<uint32_t>(recs.size()));
        for (const TacticalBattle::PadRecord& p : recs) w.u32(p.type).f32(p.x).f32(p.y).f32(p.z).i32(p.level).i32(p.value);
    }
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
    }
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
    if (version != 1 && version != kBattlesSaveVersion) return false;
    const uint32_t fought = r.u32();
    const bool fog_active = r.boolean(), fog_render = r.boolean(), ar_requested = r.boolean(), direct = r.boolean();
    const uint8_t last = r.u8();
    if (last > static_cast<uint8_t>(CorruptionStart::MissionQueued)) r.fail();
    std::vector<std::string> log(read_count(r));
    for (std::string& l : log) l = r.str();
    std::vector<std::pair<PlanetIndex, std::vector<TacticalBattle::PadRecord>>> pads(read_count(r));
    for (auto& [planet, recs] : pads) {
        planet = static_cast<PlanetIndex>(r.u32());
        recs.resize(read_count(r));
        for (TacticalBattle::PadRecord& p : recs) {
            p.type = r.u32();
            p.x = r.f32();
            p.y = r.f32();
            p.z = r.f32();
            p.level = r.i32();
            p.value = r.i32();
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
    }
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
    pad_records_ = std::move(pads);
    corruption_queued_ = std::move(queued);
    corruption_active_ = std::move(active);
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
        // Pass 2: pad_records_ was replaced; the battle's pad persistence list is relinked (as push_battle links it).
        std::vector<TacticalBattle::PadRecord>* recs = nullptr;
        for (auto& [pl, v] : pad_records_)
            if (pl == pending_req_.info.planet) recs = &v;
        if (!recs) recs = &pad_records_.emplace_back(pending_req_.info.planet, std::vector<TacticalBattle::PadRecord>{}).second;
        battle_->set_pad_persistence(recs);
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
    push_battle(*host_);
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
    battle_ = std::make_unique<TacticalBattle>(*catalog_, *g->sim(), *rules_, pending_req_, bc);
    upgrades_seen_ = 0;
    battle_->set_animation(opt_.animation);  // S10-int (null: no sim-owned animation)
    battle_->set_retreat_increments(opt_.retreat_increment_frames);  // L1 systems
    battle_->set_retreat_flight_increments(opt_.retreat_flight_increments);  // L1 systems
    battle_->set_l1_constants(opt_.l1_constants);  // L1 systems
    battle_->set_weather_data(opt_.weather);       // L1 systems
    battle_->set_l1_factions(opt_.l1_factions);    // L1 systems
    if (pending_req_.info.planet != kInvalidPlanet) {  // REMAINING_GAPS §6.5: the battle planet's pad persistence list
        std::vector<TacticalBattle::PadRecord>* recs = nullptr;
        for (auto& [pl, v] : pad_records_)
            if (pl == pending_req_.info.planet) recs = &v;
        if (!recs) recs = &pad_records_.emplace_back(pending_req_.info.planet, std::vector<TacticalBattle::PadRecord>{}).second;
        battle_->set_pad_persistence(recs);
    }
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
#endif
    register_battle_chunks();  // N1 stage 2: the running battle's save chunks
}

void CampaignBattles::hook(SimHost& host) {
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
    if (!battle_) story_linked(host);  // L1/S4 wave 2b: story-forced battles (LINK_TACTICAL)
#endif
    if (!battle_) corruption_prune();  // S4: corruption missions whose conflict was dropped
    if (!battle_ && cm_) {  // REMAINING_GAPS 6.4 [port]: a linked conflict that left the queues without a start (dropped by
                            // the validation / auto-resolved) releases the flag, else the galaxy would stay blocked
        GalacticMode* gm = host.galactic_mode();
        if (gm && gm->sim() && gm->sim()->linked_battle_pending() && cm_->land_queue().empty() && cm_->space_queue().empty() &&
            cm_->state() == galactic::ConflictState::Idle)
            gm->sim()->clear_linked_battle();
    }
    if (!battle_) return;
    TacticalMode* tm = host.tactical();
    if (!tm) return;
    battle_->tick(*tm, host.random().synced(), host.parallel_for(), host.sim_time(), host.frame());
    // STATION_HANGARS_SOUND §1.2 step 6: selections and control groups follow an upgraded station (same tick).
    for (; upgrades_seen_ < battle_->station_upgrades().size(); ++upgrades_seen_)
        host.station_replaced(battle_->station_upgrades()[upgrades_seen_].old_station, battle_->station_upgrades()[upgrades_seen_].new_station);
#ifdef ALAMO_SIM_HAVE_STORY
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
    if (direct_) {
        direct_ = false;  // start_map_battle: nothing to write back
    } else if (!auto_resolved) {
        if (corruption_battle) cm_->finish_corruption_mission(result);
        else cm_->finish_battle(result);
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
    if (mission && o->kind == galactic::ConflictKind::Land && units.empty()) {
        // [port] a land conflict needs the attacker's units in orbit (ConflictManager validation): every ground unit of
        // the player's fleets at the planet (fleet order, content order), as GalacticSim::invade with an empty list.
        for (const uint32_t fi : gs.fleets_at(planet, player))
            for (const objects::ObjectHandle h : gs.fleets()[fi].contents)
                if (gs.store().alive(h) && gs.store().type(h) < gs.rules().types.size() &&
                    gs.rules().types[gs.store().type(h)].kind == galactic::UnitKind::GroundUnit)
                    units.push_back(h);
        if (units.empty()) return CorruptionStart::Refused;
    }
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
    const bool ok = o->kind == galactic::ConflictKind::Space ? cm_->initiate_space_conflict(planet, player, ps.owner, o->map, true)
                                                              : cm_->land_invade(planet, std::move(units), player, ps.owner, o->map, true);
    log_.push_back("corruption: type " + std::to_string(type) + " mission on " + o->map + (ok ? " queued" : " refused"));
    if (!ok) {
        if (cost > 0.0f) gs.add_credits(player, cost);  // [port] refunded when the battle cannot be queued
        return CorruptionStart::Refused;
    }
    corruption_queued_.push_back({planet, player, type, o->kind, o->plots});
    gs.queue_linked_battle();  // REMAINING_GAPS 6.4: the mission queue (0x52bf50) raises the same flag
    return CorruptionStart::MissionQueued;
}

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
    if (!s || !battle_) return;
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
    b.pipeline = &host.pipeline();
    // --- L1/S4 wave 2b: Dummy_Barrage_Target for Lua position-targeted abilities, script hooks, feed state ---
    b.spawn_ability_target = [this, h](Vec3 p, int32_t owner) {
        TacticalMode* tm = h->tactical();
        return (battle_ && tm) ? battle_->spawn_dummy_target(*tm, owner, p, h->random().synced()) : objects::kNoObjectId;
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

// --- L1/S4 wave 2b: story-forced battles. Each new StorySession::linked_tactical() entry (a LINK_TACTICAL reward:
// Reward_Param1 planet (map name or planet type name), Param2 SPACE / LAND / GROUND, Param3 attacker faction, Param4
// map, Param5 defender faction [inferred order, story_session.hpp]) queues a conflict at the front of the
// ConflictManager queue (initiate_space_conflict / land_invade without units); started on a later galactic tick.
void CampaignBattles::story_linked(SimHost& host) {
    StorySession* s = host.story();
    GalacticMode* g = host.galactic_mode();
    if (!s || !g || !g->sim() || !cm_) return;
    galactic::GalacticSim& gs = *g->sim();
    const std::vector<StoryLinkedTactical>& linked = s->linked_tactical();
    auto same = [](std::string_view a, std::string_view b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            const char x = (a[i] >= 'a' && a[i] <= 'z') ? static_cast<char>(a[i] - 'a' + 'A') : a[i];
            const char y = (b[i] >= 'a' && b[i] <= 'z') ? static_cast<char>(b[i] - 'a' + 'A') : b[i];
            if (x != y) return false;
        }
        return true;
    };
    for (; story_linked_ < linked.size(); ++story_linked_) {
        const StoryLinkedTactical& l = linked[story_linked_];
        if (l.params.size() < 3) continue;
        PlanetIndex planet = kInvalidPlanet;
        const auto& names = gs.map().planets.name;
        for (PlanetIndex i = 0; i < gs.planets().size() && planet == kInvalidPlanet; ++i) {
            const uint32_t ty = gs.planets()[i].type;
            if ((i < names.size() && same(names[i], l.params[0])) ||
                (ty < gs.rules().types.size() && same(gs.rules().types[ty].name, l.params[0])))
                planet = i;
        }
        const int32_t attacker = gs.player_of_faction(l.params[2]);
        const int32_t defender = l.params.size() > 4 && !l.params[4].empty() ? gs.player_of_faction(l.params[4]) : galactic::kNoPlayer;
        const std::string map = l.params.size() > 3 ? l.params[3] : std::string();
        if (planet == kInvalidPlanet || attacker < 0) {
            log_.push_back("story: LINK_TACTICAL " + l.params[0] + " not started (planet / attacker unknown)");
            continue;
        }
        const bool space = same(l.params[1], "SPACE");
        const bool ok = space ? cm_->initiate_space_conflict(planet, attacker, defender, map, true)
                              : cm_->land_invade(planet, {}, attacker, defender, map, true);
        if (ok) gs.queue_linked_battle();  // REMAINING_GAPS 6.4: the battle-blocking story flag (cleared at its start)
        log_.push_back("story: LINK_TACTICAL " + l.params[0] + (space ? " space" : " land") + (ok ? " queued" : " refused"));
    }
}
// --- end L1/S4 wave 2b ---

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
