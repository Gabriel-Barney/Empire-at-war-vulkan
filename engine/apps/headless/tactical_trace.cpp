// alamo_headless --skirmish-ai --trace-out: see tactical_trace.hpp.
#include "tactical_trace.hpp"

#include <algorithm>
#include <bit>
#include <cstdio>

#include "alamo/core/file_provider.hpp"
#include "alamo/core/host_fs.hpp"
#include "alamo/core/hash.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/galactic/campaign.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/sim/game_modes.hpp"
#include "alamo/sim/tactical_battle.hpp"

namespace {

using alamo::objects::ObjectHandle;

constexpr uint32_t kNone = 0x3FFFFFu;  // retail "no object" id

std::string hex8(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "%08x", v);
    return b;
}

std::string fbits(float v) { return hex8(std::bit_cast<uint32_t>(v)); }

}  // namespace

bool PortTacticalTrace::open(const std::string& path, const std::vector<std::pair<std::string, std::string>>& meta, uint32_t seed,
                             std::string& err) {
    path_ = path;
    out_.open(alamo::long_path(alamo::path_from_utf8(path)), std::ios::binary | std::ios::trunc);
    if (!out_) {
        err = "cannot write " + path;
        return false;
    }
    out_ << "Q2TRACE 1\n";
    for (const auto& [k, v] : meta) out_ << "meta " << k << ' ' << v << '\n';
    out_ << "meta key_every " << key_every_ << '\n';
    out_ << "seed " << hex8(seed) << " 00000000 0\n";
    out_ << "end_header\n";
    return true;
}

void PortTacticalTrace::sample(const alamo::sim::TacticalMode& tm, const alamo::sim::TacticalBattle& tb,
                               const alamo::galactic::GalacticSim& gs, const Sample& s) {
    const alamo::objects::ObjectStore& store = tm.primary();
    const alamo::objects::GameObjectTypes& types = tm.types();
    const auto& combat = tb.combat();
    const uint32_t mframe = tb.stats().frames;
    if (!started_) {
        started_ = true;
        out_ << "start " << s.global_frame << ' ' << s.mode_type << ' ' << mframe << ' ' << hex8(s.rng_state) << '\n';
    }
    const bool key = last_key_ < 0 || static_cast<int64_t>(mframe) - last_key_ >= static_cast<int64_t>(key_every_);
    if (key) last_key_ = mframe;

    struct Obj {
        uint32_t id;
        ObjectHandle h;
    };
    std::vector<Obj> live;
    for (ObjectHandle h : store.all_objects())
        if (store.alive(h)) live.push_back({static_cast<uint32_t>(store.id(h)), h});
    std::sort(live.begin(), live.end(), [](const Obj& a, const Obj& b) { return a.id < b.id; });

    // Digests over the objects not flagged dead (q2trace.py validate recomputes them from the O lines).
    alamo::StateHasher ho;
    uint32_t n_live = 0, sum = 0;
    std::vector<uint32_t> digest;  // per live object: id, owner, type crc, 7 f32 bits
    std::string body;
    std::map<uint32_t, std::string> now;
    for (const Obj& o : live) {
        const alamo::objects::ObjectId oid = store.id(o.h);
        bool dead = false;
        float health = store.health(o.h);
        if (combat.has_unit(oid)) {
            combat_ids_.insert(o.id);
            dead = !combat.alive(oid);
            health = dead ? 0.0f : combat.health(oid);
        } else if (combat_ids_.count(o.id)) {  // removed from combat, the store object not yet gone
            dead = true;
            health = 0.0f;
        }
        const uint32_t t = store.type(o.h) < types.size() ? store.type(o.h) : kNone;
        if (!types_.count(t))
            types_[t] = t != kNone ? alamo::normalize_name(types.raw().types().name[t]) : std::string("UNKNOWN");
        const int32_t owner = store.owner(o.h);
        const auto pos = store.position(o.h), face = store.facing(o.h);
        const float pa[3] = {pos.x, pos.y, pos.z}, fa[3] = {face.x, face.y, face.z};
        const uint32_t crc = alamo::parity_object_crc(pa, fa, health);
        if (!dead) {
            ++n_live;
            sum += crc;
            digest.push_back(o.id);
            digest.push_back(static_cast<uint32_t>(owner));
            digest.push_back(alamo::name_crc(types_[t]));
            for (float v : {pos.x, pos.y, pos.z, face.x, face.y, face.z, health}) digest.push_back(std::bit_cast<uint32_t>(v));
        }
        std::string line = "O " + std::to_string(o.id) + ' ' + std::to_string(owner) + ' ' + std::to_string(t) + ' ' + fbits(pos.x) +
                           ' ' + fbits(pos.y) + ' ' + fbits(pos.z) + ' ' + fbits(face.x) + ' ' + fbits(face.y) + ' ' + fbits(face.z) +
                           ' ' + fbits(health) + (dead ? " 2 " : " 0 ") + std::to_string(kNone) + ' ' + std::to_string(kNone) + ' ' +
                           std::to_string(kNone) + " 00000000 " + hex8(crc) + '\n';
        const auto prev = objects_.find(o.id);
        if (key || prev == objects_.end() || prev->second != line) body += line;
        now[o.id] = std::move(line);
    }
    ho.u32(n_live);
    for (uint32_t v : digest) ho.u32(v);
    for (const auto& [id, line] : objects_)
        if (!now.count(id)) body += "X " + std::to_string(id) + '\n';
    objects_ = std::move(now);
    for (size_t i = 0; i < gs.players().size(); ++i) {
        const alamo::galactic::PlayerState& p = gs.players()[i];
        const std::string c = "C " + std::to_string(i) + ' ' + fbits(p.account.credits) + ' ' + fbits(p.account.cap) + ' ' +
                              (p.human ? "1" : "0") + ' ' + (p.ai ? "1" : "0") + '\n';
        const auto cp = players_.find(static_cast<uint32_t>(i));
        if (key || cp == players_.end() || cp->second != c) body += c;
        players_[static_cast<uint32_t>(i)] = c;
    }

    alamo::StateHasher hr;
    hr.u32(s.rng_state);
    char head[256];
    std::snprintf(head, sizeof head, "F %llu %u %u %08x 0 %u 0\nS %llu %u 00000000 %08x %08x %016llx %016llx ffffffff0000000000000000\n",
                  static_cast<unsigned long long>(s.global_frame), s.mode_type, mframe, s.rng_state, s.stack_depth,
                  static_cast<unsigned long long>(s.global_frame), n_live, sum, alamo::parity_rng_crc(s.rng_state),
                  static_cast<unsigned long long>(hr.digest()), static_cast<unsigned long long>(ho.digest()));
    out_ << head;
    if (!shot_ && combat.stats().shots > 0) {
        shot_ = true;
        out_ << "M first_shot_frame " << mframe << '\n';
    }
    if (key) out_ << "K " << s.global_frame << '\n';
    out_ << body;
}

bool PortTacticalTrace::finish(const std::string& reason, std::string& err) {
    for (const auto& [idx, name] : types_) {
        const std::string crc = hex8(alamo::name_crc(name));
        out_ << "T " << idx << ' ' << crc << ' ' << crc << ' ' << name << '\n';
    }
    out_ << "E " << reason << "\nend\n";
    out_.close();
    if (!out_) {
        err = "write failed: " + path_;
        return false;
    }
    return true;
}
