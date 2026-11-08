// N1 host-level save support: the session descriptor and the definition fingerprints (save_session.hpp).
#include "alamo/sim/save_session.hpp"

#include <algorithm>

#include "alamo/core/sim_crc.hpp"
#include "alamo/galactic/campaign.hpp"
#include "alamo/galactic/galactic_rules.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/object_types.hpp"

namespace alamo::sim {

namespace {

constexpr uint32_t kMaxCount = 1u << 20;  // sanity bound for counts read from a save

void write_scheduler(ByteWriter& w, const commands::SchedulerConfig& c) {
    // late / mode / turn / max_ahead: the retired lockstep bytes (commands/scheduler.hpp), kept as constants.
    w.u32(c.delay).u8(c.max_players).u8(commands::kSchedLateByte).u64(c.first_tick).u8(commands::kSchedModeByte);
    w.u32(commands::kSchedTurn).u32(commands::kSchedMaxAhead);
}

bool read_scheduler(ByteReader& r, commands::SchedulerConfig& c) {
    c.delay = r.u32();
    c.max_players = r.u8();
    const uint8_t late = r.u8();
    c.first_tick = r.u64();
    const uint8_t mode = r.u8();
    const uint32_t turn = r.u32(), max_ahead = r.u32();
    return r.ok() && late == commands::kSchedLateByte && mode == commands::kSchedModeByte && turn == commands::kSchedTurn &&
           max_ahead == commands::kSchedMaxAhead;
}

bool same_scheduler(const commands::SchedulerConfig& a, const commands::SchedulerConfig& b) {
    // first_tick is the host's (SimHost forces 1).
    return a.delay == b.delay && a.max_players == b.max_players;
}

std::vector<std::pair<std::string, std::string>> sorted(std::vector<std::pair<std::string, std::string>> v) {
    std::sort(v.begin(), v.end());
    return v;
}

void hash_vec3(StateHasher& h, const Vec3& v) {
    h.f32(v.x);
    h.f32(v.y);
    h.f32(v.z);
}

std::string hex64(uint64_t v) {
    static const char* d = "0123456789abcdef";
    std::string s(16, '0');
    for (int i = 15; i >= 0; --i) {
        s[static_cast<size_t>(i)] = d[v & 0xFu];
        v >>= 4;
    }
    return s;
}

}  // namespace

// ------------------------------------------------------------------------------------------- descriptor
std::string SessionDescriptor::option(std::string_view key, std::string_view fallback) const {
    for (const auto& [k, v] : options)
        if (k == key) return v;
    return std::string(fallback);
}

void SessionDescriptor::set_option(std::string key, std::string value) {
    for (auto& [k, v] : options)
        if (k == key) {
            v = std::move(value);
            return;
        }
    options.emplace_back(std::move(key), std::move(value));
}

void SessionDescriptor::write(ByteWriter& w) const {
    w.u32(kVersion);
    w.str(kind).str(campaign).str(data_root);
    w.u32(static_cast<uint32_t>(data_args.size()));
    for (const std::string& a : data_args) w.str(a);
    w.u32(seed).u32(players);
    write_scheduler(w, scheduler);
    w.boolean(story_crc);
    w.u32(static_cast<uint32_t>(setup.size()));
    for (const SessionPlayer& p : setup) w.str(p.faction).boolean(p.human).boolean(p.ai).str(p.ai_type).str(p.difficulty);
    w.str(difficulty);
    const auto opts = sorted(options);
    w.u32(static_cast<uint32_t>(opts.size()));
    for (const auto& [k, v] : opts) w.str(k).str(v);
    w.u64(data_fingerprint);
    w.str(writer);
}

bool SessionDescriptor::read(ByteReader& r) {
    if (r.u32() != kVersion) return false;
    SessionDescriptor d;
    d.kind = r.str();
    d.campaign = r.str();
    d.data_root = r.str();
    const uint32_t na = r.u32();
    if (!r.ok() || na > kMaxCount) return false;
    for (uint32_t i = 0; i < na && r.ok(); ++i) d.data_args.push_back(r.str());
    d.seed = r.u32();
    d.players = r.u32();
    if (!read_scheduler(r, d.scheduler)) return false;
    d.story_crc = r.boolean();
    const uint32_t np = r.u32();
    if (!r.ok() || np > kMaxCount) return false;
    for (uint32_t i = 0; i < np && r.ok(); ++i) {
        SessionPlayer p;
        p.faction = r.str();
        p.human = r.boolean();
        p.ai = r.boolean();
        p.ai_type = r.str();
        p.difficulty = r.str();
        d.setup.push_back(std::move(p));
    }
    d.difficulty = r.str();
    const uint32_t no = r.u32();
    if (!r.ok() || no > kMaxCount) return false;
    for (uint32_t i = 0; i < no && r.ok(); ++i) {
        std::string k = r.str();
        std::string v = r.str();
        d.options.emplace_back(std::move(k), std::move(v));
    }
    d.data_fingerprint = r.u64();
    d.writer = r.str();
    if (!r.ok() || !r.done()) return false;
    *this = std::move(d);
    return true;
}

std::string SessionDescriptor::mismatch(const SessionDescriptor& s, const SessionDescriptor& c) {
    if (s.kind != c.kind) return "session kind '" + s.kind + "' != '" + c.kind + "'";
    if (s.campaign != c.campaign) return "campaign '" + s.campaign + "' != '" + c.campaign + "'";
    if (s.data_args != c.data_args) return "data arguments (mod path) differ";
    if (s.seed != c.seed) return "session seed " + std::to_string(s.seed) + " != " + std::to_string(c.seed);
    if (s.players != c.players) return "player slots " + std::to_string(s.players) + " != " + std::to_string(c.players);
    if (!same_scheduler(s.scheduler, c.scheduler)) return "command scheduler settings differ";
    if (s.story_crc != c.story_crc) return "story CRC option differs";
    if (s.setup.size() != c.setup.size())
        return "player count " + std::to_string(s.setup.size()) + " != " + std::to_string(c.setup.size());
    for (size_t i = 0; i < s.setup.size(); ++i)
        if (!(s.setup[i] == c.setup[i])) return "player " + std::to_string(i) + " setup differs (" + s.setup[i].faction + ")";
    if (s.difficulty != c.difficulty) return "difficulty '" + s.difficulty + "' != '" + c.difficulty + "'";
    if (sorted(s.options) != sorted(c.options)) return "session options differ";
    if (s.data_fingerprint != c.data_fingerprint)
        return "game data differs (fingerprint " + hex64(s.data_fingerprint) + " != " + hex64(c.data_fingerprint) + ")";
    return {};
}

bool read_session(const SaveFile& save, SessionDescriptor& out, std::string* err) {
    const SaveChunk* c = save.find("session");
    if (!c) {
        if (err) *err = "save has no session chunk";
        return false;
    }
    ByteReader r(c->data);
    if (c->version != SessionDescriptor::kVersion || !out.read(r)) {
        if (err) *err = "session chunk: unknown version or bad data";
        return false;
    }
    return true;
}

std::string LoadReport::text() const {
    if (!descriptor_error.empty()) return "save does not match this session: " + descriptor_error;
    auto list = [](const std::vector<std::string>& v) {
        std::string s;
        for (const std::string& n : v) s += (s.empty() ? "" : ", ") + n;
        return s;
    };
    std::string s;
    if (!failed.empty()) s += "chunks failed to read: " + list(failed);
    if (!missing.empty()) s += std::string(s.empty() ? "" : "; ") + "chunks missing from the save: " + list(missing);
    if (!unexpected.empty()) s += std::string(s.empty() ? "" : "; ") + "chunks the session has no system for: " + list(unexpected);
    if (s.empty()) s = "loaded " + std::to_string(loaded.size()) + " chunks";
    return s;
}

// ------------------------------------------------------------------------------------------ fingerprints
uint64_t fingerprint_types(const objects::GameObjectTypes& types) {
    StateHasher h(0x5E55F1E1u);
    h.u32(static_cast<uint32_t>(types.size()));
    if (types.size() == 0) return h.digest();
    const objects::ObjectTypeDatabase& raw = types.raw();
    const objects::TypeColumns& tc = raw.types();
    const objects::FieldColumns& fc = raw.fields();
    h.u32(static_cast<uint32_t>(tc.size()));
    h.u32(static_cast<uint32_t>(fc.size()));
    for (size_t t = 0; t < tc.size(); ++t) {
        h.str(tc.name[t]);
        h.str(raw.element_name(static_cast<objects::TypeIndex>(t)));
    }
    for (size_t f = 0; f < fc.size(); ++f) {
        h.str(raw.tags().name[fc.tag[f]]);
        h.u32(static_cast<uint32_t>(fc.type[f]));
        h.u16(fc.depth[f]);
        h.str(fc.text[f]);
    }
    return h.digest();
}

uint64_t fingerprint_galactic(const galactic::GalacticRules& rules, const galactic::GalacticSetup& setup) {
    StateHasher h(0x6A1AC71Cu);
    h.u32(static_cast<uint32_t>(rules.factions.size()));
    for (const galactic::FactionRules& f : rules.factions) {
        h.str(f.name);
        h.boolean(f.is_neutral);
        h.boolean(f.can_control_planets);
        h.boolean(f.is_playable);
        h.boolean(f.benefits_from_corruption);
        h.f32(f.hyperspace_speed_factor);
    }
    h.u32(static_cast<uint32_t>(rules.difficulties.size()));
    for (const galactic::DifficultyRules& d : rules.difficulties) {
        h.str(d.name);
        h.f32(d.credit_multiplier);
        h.f32(d.galactic_build_time_multiplier);
    }
    h.u32(static_cast<uint32_t>(rules.types.size()));
    for (const galactic::TypeInfo& t : rules.types) {
        h.str(t.name);
        h.u8(static_cast<uint8_t>(t.kind));
        h.i32(t.build_cost);
        h.i32(t.build_time_seconds);
        h.i32(t.tech_level);
        h.i32(t.required_ground_base);
        h.i32(t.required_star_base);
        h.i32(t.base_level);
        h.i32(t.squadron_capacity);
        h.boolean(t.hyperspace);
        h.f32(t.hyperspace_speed);
        h.f32(t.ai_combat_power);
        h.i32(t.population);
        h.u64(t.categories);
        h.i32(t.credit_value);
        h.i32(t.capture_bonus);
    }
    const GalaxyMap& m = setup.map;
    h.str(m.campaign_name);
    h.f32(m.max_fleet_move_distance);
    h.u32(static_cast<uint32_t>(m.planets.size()));
    for (size_t i = 0; i < m.planets.size(); ++i) {
        h.str(m.planets.name[i]);
        hash_vec3(h, m.planets.position[i]);
        h.u32(m.planets.flags[i]);
    }
    h.u32(static_cast<uint32_t>(m.routes.size()));
    for (size_t i = 0; i < m.routes.size(); ++i) {
        h.str(m.routes.name[i]);
        h.u32(m.routes.point_a[i]);
        h.u32(m.routes.point_b[i]);
        h.f32(m.routes.hs_speed_factor[i]);
    }
    h.u32(static_cast<uint32_t>(setup.planet_type.size()));
    for (const uint32_t t : setup.planet_type) h.u32(t);
    h.u32(static_cast<uint32_t>(setup.players.size()));
    for (const galactic::PlayerSetup& p : setup.players) {
        h.u32(p.faction);
        h.boolean(p.human);
        h.boolean(p.ai);
        h.u32(p.difficulty);
        h.f32(p.credits);
        h.i32(p.tech_level);
        h.i32(p.max_tech_level);
        h.u32(p.home);
    }
    h.u32(static_cast<uint32_t>(setup.forces.size()));
    for (const galactic::StartingForce& f : setup.forces) {
        h.i32(f.player);
        h.u32(f.planet);
        h.u32(f.type);
    }
    h.u32(static_cast<uint32_t>(setup.leaning.size()));
    for (const int32_t l : setup.leaning) h.i32(l);
    h.boolean(setup.tech_sweep_at_start);
    h.boolean(setup.planet_auto_reveal);
    return h.digest();
}

uint64_t combine_fingerprints(std::initializer_list<uint64_t> parts) {
    StateHasher h(0xC0B1u);
    h.u32(static_cast<uint32_t>(parts.size()));
    for (const uint64_t p : parts) h.u64(p);
    return h.digest();
}

}  // namespace alamo::sim
