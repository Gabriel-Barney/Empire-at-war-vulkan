// SP5 (docs/SIM_PARALLEL_DESIGN.md §5, "Services chain"): the phase graph of a tick's services.
//
// Each service declares the components (bits of a 64-bit set, named by the host) it reads and writes. build() places
// the services in phases: a service goes into the first phase after every EARLIER-declared service it conflicts with
// (one writes what the other reads or writes) or that an explicit order() edge puts before it. So any two conflicting
// services keep their declaration order, and the services of one phase touch disjoint data and commute: running a
// phase's services one after another in any order, or at the same time, gives the result of the declared serial order.
// A barrier reads and writes everything (a profile lap, a step whose sets are not declared narrowly yet).
//
// Retail-defined order (tier 1, e.g. which service picks a target first) that the data sets do not already imply is
// declared with order(); such an edge is kept even if the sets are later narrowed.
//
// Rule 3: run() gives the same result in every RunMode as long as the declarations are right; the declarations are
// what the checks verify:
//   * Checker (debug check): the run is serial; around each service the digest of every checkable component is taken,
//     and a component that changed but is not in the service's write set is a violation (on_violation, and counted).
//     The components each service really changed are recorded (observed_writes), for narrowing declarations.
//   * RunMode::Reverse: each phase serially in reverse declaration order. An undeclared read or write between two
//     services of one phase changes the result, which a CRC comparison against a Serial run shows (the reads half of
//     the check, which digests cannot see).
// Determinism: the phases and their order are a function of the declarations only (no pointers, no timing). A
// Parallel run hands a phase with two or more services to the ParallelFor one service per index; the services
// themselves must follow rule 3 internally (per-index outputs, deferred writes committed in canonical order).
#pragma once
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "alamo/core/parallel.hpp"

namespace alamo::tactical::services {

using ComponentSet = uint64_t;
inline constexpr uint32_t kMaxComponents = 64;
inline constexpr ComponentSet kEverything = ~ComponentSet{0};
constexpr ComponentSet component(uint32_t c) { return ComponentSet{1} << c; }

using ServiceId = uint32_t;

enum class RunMode : uint8_t {
    Serial,    // every service in declaration order on the calling thread (the reference)
    Parallel,  // the services of a phase at the same time through the ParallelFor; phases in order
    Reverse,   // check: each phase serially, its services in reverse declaration order
};

// The debug check of the declared write sets (see the header comment).
struct Checker {
    // Digest of one component's state; called only for components in `checkable`.
    std::function<uint64_t(uint32_t component)> digest;
    ComponentSet checkable = 0;
    // A service changed a component it does not declare as written.
    std::function<void(const std::string& service, uint32_t component)> on_violation;
    uint64_t violations = 0;
};

struct ServiceInfo {
    std::string name;
    ComponentSet reads = 0, writes = 0;
    bool barrier = false;
    uint32_t phase = 0;                 // set by build()
    ComponentSet observed_writes = 0;   // components a checked run saw this service change
    uint16_t zone = 0xFFFF;             // profile zone "svc.<name>" (wall clock only)
};

class ServiceGraph {
public:
    using Fn = std::function<void()>;

    // Names for describe() and violation reports (index = component bit). Optional.
    void set_component_names(std::vector<std::string> names) { names_ = std::move(names); }
    const std::string& component_name(uint32_t c) const;

    ServiceId add(std::string name, ComponentSet reads, ComponentSet writes, Fn fn);
    ServiceId barrier(std::string name, Fn fn);
    // Explicit edge: `after` runs in a later phase than `before` (both added; before < after in declaration order).
    void order(ServiceId before, ServiceId after);
    // Places the services in phases (call after the last add / order; run() builds when needed).
    void build();
    void clear();

    void run(const ParallelFor& pf, RunMode mode, Checker* check = nullptr);

    size_t size() const { return info_.size(); }
    const ServiceInfo& info(ServiceId s) const { return info_[s]; }
    uint32_t phase_count() const { return static_cast<uint32_t>(phases_.size()); }
    std::span<const ServiceId> phase(uint32_t p) const { return phases_[p]; }
    // Two services conflict: one writes what the other reads or writes.
    static bool conflict(const ServiceInfo& a, const ServiceInfo& b) {
        return (a.writes & (b.reads | b.writes)) != 0 || (b.writes & a.reads) != 0;
    }
    // One line per phase: "p3: fog_service | pads_tick".
    std::string describe() const;

private:
    void run_one(ServiceId s, Checker* check);

    std::vector<ServiceInfo> info_;
    std::vector<Fn> fn_;
    std::vector<std::pair<ServiceId, ServiceId>> edges_;
    std::vector<std::vector<ServiceId>> phases_;
    std::vector<std::string> names_;
    bool built_ = false;
};

}  // namespace alamo::tactical::services
