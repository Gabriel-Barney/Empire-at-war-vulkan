// SP5: the services phase graph (service_graph.hpp).
#include "alamo/tactical/services/service_graph.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <stdexcept>

#include "alamo/core/profile.hpp"

namespace alamo::tactical::services {

const std::string& ServiceGraph::component_name(uint32_t c) const {
    static const std::string unnamed = "?";
    return c < names_.size() ? names_[c] : unnamed;
}

ServiceId ServiceGraph::add(std::string name, ComponentSet reads, ComponentSet writes, Fn fn) {
    ServiceInfo s;
    s.name = std::move(name);
    s.reads = reads | writes;  // a writer also sees what it writes (read-modify-write); keeps conflict() symmetric
    s.writes = writes;
    s.zone = profile::register_zone(("svc." + s.name).c_str());
    info_.push_back(std::move(s));
    fn_.push_back(std::move(fn));
    built_ = false;
    return static_cast<ServiceId>(info_.size() - 1);
}

ServiceId ServiceGraph::barrier(std::string name, Fn fn) {
    const ServiceId id = add(std::move(name), kEverything, kEverything, std::move(fn));
    info_[id].barrier = true;
    return id;
}

void ServiceGraph::order(ServiceId before, ServiceId after) {
    if (before >= after || after >= info_.size()) throw std::invalid_argument("ServiceGraph::order: before must be declared first");
    edges_.emplace_back(before, after);
    built_ = false;
}

void ServiceGraph::clear() {
    info_.clear();
    fn_.clear();
    edges_.clear();
    phases_.clear();
    built_ = false;
}

void ServiceGraph::build() {
    phases_.clear();
    for (ServiceId s = 0; s < info_.size(); ++s) {
        uint32_t p = 0;
        for (ServiceId e = 0; e < s; ++e)
            if (conflict(info_[e], info_[s])) p = std::max(p, info_[e].phase + 1);
        for (const auto& [b, a] : edges_)
            if (a == s) p = std::max(p, info_[b].phase + 1);
        info_[s].phase = p;
        if (phases_.size() <= p) phases_.resize(p + 1);
        phases_[p].push_back(s);  // ascending declaration order within the phase
    }
    built_ = true;
}

void ServiceGraph::run_one(ServiceId s, Checker* check) {
    ServiceInfo& si = info_[s];
    if (!check) {
        profile::Scope z(si.zone);
        fn_[s]();
        return;
    }
    std::array<uint64_t, kMaxComponents> before{};
    for (ComponentSet m = check->checkable; m; m &= m - 1) {
        const auto c = static_cast<uint32_t>(std::countr_zero(m));
        before[c] = check->digest(c);
    }
    {
        profile::Scope z(si.zone);
        fn_[s]();
    }
    for (ComponentSet m = check->checkable; m; m &= m - 1) {
        const auto c = static_cast<uint32_t>(std::countr_zero(m));
        if (check->digest(c) == before[c]) continue;
        si.observed_writes |= component(c);
        if ((si.writes & component(c)) == 0) {
            ++check->violations;
            if (check->on_violation) check->on_violation(si.name, c);
        }
    }
}

void ServiceGraph::run(const ParallelFor& pf, RunMode mode, Checker* check) {
    if (!built_) build();
    if (check && mode == RunMode::Parallel) mode = RunMode::Serial;  // digests need one service at a time
    for (const std::vector<ServiceId>& ph : phases_) {
        if (mode == RunMode::Reverse) {
            for (auto it = ph.rbegin(); it != ph.rend(); ++it) run_one(*it, check);
        } else if (mode == RunMode::Parallel && ph.size() >= 2) {
            pf(static_cast<uint32_t>(ph.size()), 1, [this, &ph](uint32_t b, uint32_t e) {
                for (uint32_t i = b; i < e; ++i) run_one(ph[i], nullptr);
            });
        } else {
            for (const ServiceId s : ph) run_one(s, check);
        }
    }
}

std::string ServiceGraph::describe() const {
    std::string out;
    for (size_t p = 0; p < phases_.size(); ++p) {
        out += "p" + std::to_string(p) + ":";
        for (size_t i = 0; i < phases_[p].size(); ++i) out += (i ? " | " : " ") + info_[phases_[p][i]].name;
        out += "\n";
    }
    return out;
}

}  // namespace alamo::tactical::services
