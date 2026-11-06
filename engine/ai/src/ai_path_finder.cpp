#include "alamo/ai/ai_path_finder.hpp"

namespace alamo::ai {

uint64_t matrices_digest(const galactic::AllPairsMatrices& m) {
    StateHasher h;
    h.u32(m.size());
    h.span(m.weights());
    h.span(m.preds());
    return h.digest();
}

galactic::AllPairsInput ai_path_input(std::span<const double> node_value, const galactic::Bitset* revealed,
                                      bool has_ai_system, bool is_human, std::span<const float> z_offset) {
    galactic::AllPairsInput in;
    in.node_value = node_value;
    in.revealed = ai_path_fog_gate(has_ai_system, is_human) ? revealed : nullptr;
    in.z_offset = z_offset;
    return in;
}

void AiPathFinder::Input::assign(const galactic::AllPairsInput& in) {
    value.assign(in.node_value.begin(), in.node_value.end());
    cost.assign(in.node_cost.begin(), in.node_cost.end());
    has_revealed = in.revealed != nullptr;
    revealed = has_revealed ? *in.revealed : galactic::Bitset{};
    zoff.assign(in.z_offset.begin(), in.z_offset.end());
}

galactic::AllPairsInput AiPathFinder::Input::view() const {
    galactic::AllPairsInput in;
    in.node_value = value;
    in.node_cost = cost;
    in.revealed = has_revealed ? &revealed : nullptr;
    in.z_offset = zoff;
    return in;
}

void AiPathFinder::recompute(const galactic::PlanetGraph& graph, const galactic::AllPairsInput& input,
                             const ParallelFor& pf) {
    if (cache_.update(graph, input, pf)) ++matrix_runs_;
    use_front_ = false;
    ++runs_;
    // N1: keep the input so a load can rebuild the matrices (and the cache's change detection) exactly.
    in_.assign(input);
    digest_ = matrices_digest(cache_.matrices());
}

// OPT2 t02: a cadence recompute on the background hook, applied at frame + latency. A result still in flight (only when
// the latency is not below the 90-frame period) is applied first.
void AiPathFinder::kick(int32_t frame, const galactic::PlanetGraph& graph, const galactic::AllPairsInput& input,
                        const ParallelFor& pf) {
    if (job_.in_flight()) publish(ready_frame());
    ++runs_;
    pend_.assign(input);
    start_job(frame, frame + static_cast<int32_t>(latency_), graph, pf, bg_);
}

void AiPathFinder::start_job(int32_t frame, int32_t ready, const galactic::PlanetGraph& graph, const ParallelFor& pf,
                             const BackgroundStart& bg) {
    // The job gets its own copy of the graph (a few vectors per planet), so it does not depend on the sim's lifetime: a
    // host may tear the sim down while the job system still drains the job.
    job_.kick(frame, ready, bg, [g = graph, in = pend_, pf](Result& out) {
        galactic::compute_all_pairs(g, in.view(), out.m, pf);
        out.digest = matrices_digest(out.m);
    });
}

bool AiPathFinder::publish(int32_t frame) {
    if (!job_.due(frame)) return false;
    Result& r = job_.finish();
    if (r.digest != digest_) ++matrix_runs_;
    std::swap(front_, r.m);
    use_front_ = true;
    digest_ = r.digest;
    in_ = std::move(pend_);
    pend_ = Input{};
    return true;
}

void AiPathFinder::create(const galactic::PlanetGraph& graph, const galactic::AllPairsInput& input, int32_t frame,
                          int32_t slot, int32_t k, const ParallelFor& pf) {
    created_ = true;
    recompute(graph, input, pf);
    next_ = galactic::first_recompute_due(frame, slot, k);
}

void AiPathFinder::create_lazy(const galactic::PlanetGraph& graph, const galactic::AllPairsInput& input,
                               const ParallelFor& pf) {
    created_ = true;
    recompute(graph, input, pf);
    next_ = galactic::kLazyCreatedNextRecompute;
}

void AiPathFinder::reset() {
    job_.cancel();
    const uint32_t latency = latency_;
    BackgroundStart bg = std::move(bg_);
    *this = AiPathFinder{};
    set_background(latency, std::move(bg));
}

bool AiPathFinder::get_path(PlanetIndex from, PlanetIndex to, std::vector<PlanetIndex>& out,
                            std::vector<uint32_t>* hop_weights) const {
    if (!created_) {
        out.clear();
        if (hop_weights) hop_weights->clear();
        return false;
    }
    return published().get_path(from, to, out, hop_weights);
}

std::vector<PlanetIndex> AiPathFinder::path(PlanetIndex from, PlanetIndex to) const {
    std::vector<PlanetIndex> out;
    get_path(from, to, out);
    return out;
}

uint32_t AiPathFinder::weight(PlanetIndex from, PlanetIndex to) const {
    const galactic::AllPairsMatrices& m = published();
    if (!created_ || !m.is_node(from) || !m.is_node(to)) return galactic::kUnlinked;
    return m.weight(from, to);
}

PlanetIndex cheapest_stage(const AiPathFinder& f, std::span<const PlanetIndex> candidates, PlanetIndex target) {
    PlanetIndex best = kInvalidPlanet;
    uint32_t best_w = galactic::kUnlinked;
    for (const PlanetIndex s : candidates) {
        const uint32_t w = f.weight(s, target);
        if (w == galactic::kUnlinked) continue;
        if (best == kInvalidPlanet || w < best_w) {
            best = s;
            best_w = w;
        }
    }
    return best;
}

void AiPathFinder::hash(StateHasher& h) const {
    h.boolean(created_);
    if (!created_) return;
    h.i32(next_);
    h.u32(runs_);
    h.u64(digest_);
    if (job_.in_flight()) h.i64(job_.ready_frame());  // OPT2 t02 (nothing extra in the original timing)
}

}  // namespace alamo::ai
