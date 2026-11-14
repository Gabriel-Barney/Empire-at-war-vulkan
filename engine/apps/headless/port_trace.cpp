// alamo_headless --trace-out: see port_trace.hpp.
#include "port_trace.hpp"

#include <algorithm>
#include <bit>
#include <cstdio>

#include "alamo/core/file_provider.hpp"
#include "alamo/core/host_fs.hpp"
#include "alamo/core/hash.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/galactic/campaign.hpp"
#include "alamo/galactic/galactic_rules.hpp"

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

bool PortStateTrace::open(const std::string& path, const std::vector<std::pair<std::string, std::string>>& meta, uint32_t seed,
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
    out_ << "start 0 0 0 " << hex8(seed) << '\n';
    return true;
}

void PortStateTrace::start(const alamo::galactic::GalacticSim& gs, uint32_t seed) {
    out_ << "F 0 0 0 " << hex8(seed) << " 0 1 0 " << gs.last_cycle_frame() << ' ' << gs.next_cycle_frame() << ' '
         << gs.galactic_day() << '\n';
    // No objects yet: the matching S line of an empty store.
    alamo::StateHasher hr;
    hr.u32(seed);
    alamo::StateHasher ho;
    ho.u32(0);
    char b[160];
    std::snprintf(b, sizeof b, "S 0 0 00000000 00000000 %08x %016llx %016llx ffffffff0000000000000000\n", alamo::parity_rng_crc(seed),
                  static_cast<unsigned long long>(hr.digest()), static_cast<unsigned long long>(ho.digest()));
    out_ << b;
}

uint32_t PortStateTrace::type_index(const alamo::galactic::GalacticSim& gs, uint32_t store_type) {
    const auto& types = gs.rules().types;
    const uint32_t idx = store_type < types.size() ? store_type : static_cast<uint32_t>(types.size());  // fleets: kNone
    // Upper-case names, as the retail T lines (zlib CRC-32 of the upper-case name).
    if (!types_.count(idx)) types_[idx] = idx < types.size() ? alamo::normalize_name(types[idx].name) : std::string("GALACTIC_FLEET");
    return idx;
}

void PortStateTrace::sample(const alamo::galactic::GalacticSim& gs, const Sample& s) {
    namespace gal = alamo::galactic;
    const alamo::objects::ObjectStore& store = gs.store();
    const int32_t mframe = gs.frame();
    const bool key = last_key_ < 0 || mframe - last_key_ >= static_cast<int64_t>(key_every_);
    if (key) last_key_ = mframe;

    // Planet rows by object id, starbases left out (not objects in the retail galactic store).
    std::map<uint32_t, size_t> planet_row;
    std::vector<ObjectHandle> starbases;
    for (size_t i = 0; i < gs.planets().size(); ++i) {
        const gal::PlanetState& p = gs.planets()[i];
        if (!p.is_planet) continue;
        if (store.alive(p.object)) planet_row[static_cast<uint32_t>(store.id(p.object))] = i;
        if (p.starbase) starbases.push_back(p.starbase);
    }
    struct Obj {
        uint32_t id;
        ObjectHandle h;
    };
    std::vector<Obj> live;
    for (ObjectHandle h : store.all_objects()) {
        if (!store.alive(h) || std::find(starbases.begin(), starbases.end(), h) != starbases.end()) continue;
        live.push_back({static_cast<uint32_t>(store.id(h)), h});
    }
    std::sort(live.begin(), live.end(), [](const Obj& a, const Obj& b) { return a.id < b.id; });

    // Digests (q2trace.py validate recomputes them from the O lines).
    alamo::StateHasher ho;
    ho.u32(static_cast<uint32_t>(live.size()));
    uint32_t sum = 0;
    std::string body;
    std::map<uint32_t, std::string> now;
    for (const Obj& o : live) {
        const auto row = planet_row.find(o.id);
        const int32_t owner = row != planet_row.end() ? gs.planets()[row->second].owner : store.owner(o.h);
        const uint32_t t = type_index(gs, store.type(o.h));
        const auto pos = store.position(o.h), face = store.facing(o.h);
        const float health = store.health(o.h);
        const ObjectHandle c = gs.container_of(o.h);
        const uint32_t cid = c && store.exists(c) ? static_cast<uint32_t>(store.id(c)) : kNone;
        const float pa[3] = {pos.x, pos.y, pos.z}, fa[3] = {face.x, face.y, face.z};
        const uint32_t crc = alamo::parity_object_crc(pa, fa, health);
        sum += crc;
        ho.u32(o.id);
        ho.u32(static_cast<uint32_t>(owner));
        ho.u32(alamo::name_crc(types_[t]));
        for (float v : {pos.x, pos.y, pos.z, face.x, face.y, face.z, health}) ho.u32(std::bit_cast<uint32_t>(v));
        std::string line = "O " + std::to_string(o.id) + ' ' + std::to_string(owner) + ' ' + std::to_string(t) + ' ' + fbits(pos.x) +
                           ' ' + fbits(pos.y) + ' ' + fbits(pos.z) + ' ' + fbits(face.x) + ' ' + fbits(face.y) + ' ' + fbits(face.z) +
                           ' ' + fbits(health) + " 0 " + std::to_string(kNone) + ' ' + std::to_string(cid) + ' ' +
                           std::to_string(kNone) + " 00000000 " + hex8(crc) + '\n';
        const auto prev = objects_.find(o.id);
        if (key || prev == objects_.end() || prev->second != line) body += line;
        if (row != planet_row.end()) {
            const gal::PlanetState& p = gs.planets()[row->second];
            const std::string g = "G " + std::to_string(o.id) + ' ' + fbits(p.control) + ' ' + std::to_string(p.base_value) + ' ' +
                                  fbits(p.black_market) + ' ' + fbits(p.piracy_pool) + ' ' + std::to_string(p.corruption) + ' ' +
                                  (p.destroyed ? "1" : "0") + ' ' + std::to_string(p.siphon_player) + '\n';
            const auto gp = planets_.find(o.id);
            if (key || gp == planets_.end() || gp->second != g) body += g;
            planets_[o.id] = g;
        }
        now[o.id] = std::move(line);
    }
    for (const auto& [id, line] : objects_)
        if (!now.count(id)) {
            body += "X " + std::to_string(id) + '\n';
            planets_.erase(id);
        }
    objects_ = std::move(now);
    for (size_t i = 0; i < gs.players().size(); ++i) {
        const gal::PlayerState& p = gs.players()[i];
        const std::string c = "C " + std::to_string(i) + ' ' + fbits(p.account.credits) + ' ' + fbits(p.account.cap) + ' ' +
                              (p.human ? "1" : "0") + ' ' + (p.ai ? "1" : "0") + '\n';
        const auto cp = players_.find(static_cast<uint32_t>(i));
        if (key || cp == players_.end() || cp->second != c) body += c;
        players_[static_cast<uint32_t>(i)] = c;
    }

    alamo::StateHasher hr;
    hr.u32(s.rng_state);
    char head[256];
    std::snprintf(head, sizeof head, "F %llu %u %d %08x %d %u 0 %d %d %d\nS %llu %zu 00000000 %08x %08x %016llx %016llx ffffffff0000000000000000\n",
                  static_cast<unsigned long long>(s.global_frame), s.mode_type, mframe, s.rng_state, gs.paused() ? 1 : 0, s.stack_depth,
                  gs.last_cycle_frame(), gs.next_cycle_frame(), gs.galactic_day(), static_cast<unsigned long long>(s.global_frame),
                  live.size(), sum, alamo::parity_rng_crc(s.rng_state), static_cast<unsigned long long>(hr.digest()),
                  static_cast<unsigned long long>(ho.digest()));
    out_ << head;
    if (key) out_ << "K " << s.global_frame << '\n';
    out_ << body;
}

bool PortStateTrace::finish(const std::string& reason, std::string& err) {
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
