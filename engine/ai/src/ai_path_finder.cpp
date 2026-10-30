#include "alamo/ai/ai_path_finder.hpp"

namespace alamo::ai {

galactic::AllPairsInput ai_path_input(std::span<const double> node_value, const galactic::Bitset* revealed,
                                      bool has_ai_system, bool is_human, std::span<const float> z_offset) {
    galactic::AllPairsInput in;
    in.node_value = node_value;
    in.revealed = ai_path_fog_gate(has_ai_system, is_human) ? revealed : nullptr;
    in.z_offset = z_offset;
    return in;
}

void AiPathFinder::recompute(const galactic::PlanetGraph& graph, const galactic::AllPairsInput& input,
                             const ParallelFor& pf) {
    cache_.update(graph, input, pf);
    ++runs_;
    // N1: keep the input so a load can rebuild the matrices (and the cache's change detection) exactly.
    in_value_.assign(input.node_value.begin(), input.node_value.end());
    in_cost_.assign(input.node_cost.begin(), input.node_cost.end());
    in_has_revealed_ = input.revealed != nullptr;
    in_revealed_ = in_has_revealed_ ? *input.revealed : galactic::Bitset{};
    in_zoff_.assign(input.z_offset.begin(), input.z_offset.end());
    StateHasher h;
    const galactic::AllPairsMatrices& m = cache_.matrices();
    h.u32(m.size());
    h.span(m.weights());
    h.span(m.preds());
    digest_ = h.digest();
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

void AiPathFinder::reset() { *this = AiPathFinder{}; }

bool AiPathFinder::get_path(PlanetIndex from, PlanetIndex to, std::vector<PlanetIndex>& out,
                            std::vector<uint32_t>* hop_weights) const {
    if (!created_) {
        out.clear();
        if (hop_weights) hop_weights->clear();
        return false;
    }
    return cache_.matrices().get_path(from, to, out, hop_weights);
}

std::vector<PlanetIndex> AiPathFinder::path(PlanetIndex from, PlanetIndex to) const {
    std::vector<PlanetIndex> out;
    get_path(from, to, out);
    return out;
}

uint32_t AiPathFinder::weight(PlanetIndex from, PlanetIndex to) const {
    const galactic::AllPairsMatrices& m = cache_.matrices();
    if (!created_ || !m.is_node(from) || !m.is_node(to)) return galactic::kUnlinked;
    return m.weight(from, to);
}

void AiPathFinder::hash(StateHasher& h) const {
    h.boolean(created_);
    if (!created_) return;
    h.i32(next_);
    h.u32(runs_);
    h.u64(digest_);
}

galactic::AllPairsInput AiPathFinder::last_input() const {
    galactic::AllPairsInput in;
    in.node_value = in_value_;
    in.node_cost = in_cost_;
    in.revealed = in_has_revealed_ ? &in_revealed_ : nullptr;
    in.z_offset = in_zoff_;
    return in;
}

}  // namespace alamo::ai
