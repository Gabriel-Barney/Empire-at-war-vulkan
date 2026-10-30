#include "alamo/render/stealth_fx.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

namespace alamo::render {

namespace {

std::string upper_name(const std::string& s) {
    std::string u(s);
    for (char& c : u) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return u;
}

// Hardpoint model file name -> mesh name: upper-cased, cut at ".ALO" (path components dropped).
std::string attach_mesh_name(const std::string& model_file) {
    std::string u = upper_name(model_file);
    const size_t slash = u.find_last_of("/\\");
    if (slash != std::string::npos) u.erase(0, slash + 1);
    const size_t dot = u.find(".ALO");
    if (dot != std::string::npos) u.erase(dot);
    return u;
}

}  // namespace

void smooth_damp(SmoothSpring& s, float target, float smooth_time, float dt) {
    if (!(smooth_time > 1e-4f) || !std::isfinite(smooth_time)) {
        s.value = target;
        s.velocity = 0.0f;
        return;
    }
    const float omega = 2.0f / smooth_time;
    const float x = omega * dt;
    const float k = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
    const float c = s.value - target;
    const float t = (s.velocity + omega * c) * dt;
    s.velocity = (s.velocity - omega * t) * k;
    s.value = target + (c + t) * k;
}

void smooth_damp_steps(SmoothSpring& s, float target, float smooth_time, uint64_t steps) {
    steps = std::min(steps, kSpringMaxSteps);
    for (uint64_t i = 0; i < steps; ++i) smooth_damp(s, target, smooth_time);
}

void StealthFx::snap(bool applied, bool cloak) {
    spring_ = {applied ? 1.0f : 0.0f, 0.0f};
    cloak_ = cloak;
    shadow_lost_ = applied;
}

void StealthFx::step(bool applied, bool cloak, float smooth_time, uint64_t steps) {
    if (applied) {
        cloak_ = cloak;
        shadow_lost_ = true;  // the stealth counter runs: the stencil-shadow bit is cleared every frame (1.3)
    }
    smooth_damp_steps(spring_, applied ? 1.0f : 0.0f, smooth_time, steps);
    // Force_Cloak sets the bit again once the target is 0 and |v| < 0.01; Stealth_Ability only at the next model set-up.
    if (!applied && cloak_ && std::fabs(spring_.value) < kStealthTintMin) shadow_lost_ = false;
}

materials::float4 StealthFx::tint(const float color[4]) const {
    if (!tinted()) return {1, 1, 1, 1};
    const float v = spring_.value;
    return {1.0f + (color[0] - 1.0f) * v, 1.0f + (color[1] - 1.0f) * v, 1.0f + (color[2] - 1.0f) * v,
            1.0f + (color[3] - 1.0f) * v};
}

uint32_t hide_hardpoint_meshes(const assets::ModelAsset& model, const HardpointLayout& layout, std::span<uint8_t> mask) {
    uint32_t n = 0;
    for (const std::string& file : layout.attach_model) {
        if (file.empty()) continue;
        const std::string want = attach_mesh_name(file);
        if (want.empty()) continue;
        for (size_t i = 0; i < model.meshes.size() && i < mask.size(); ++i)
            if (mask[i] == 0 && upper_name(model.meshes[i].name) == want) {
                mask[i] = 1;
                ++n;
            }
        for (size_t p = 0; p < model.proxies.size() && p < layout.proxies; ++p) {
            const size_t sub = layout.proxy_index(static_cast<uint32_t>(p));
            if (sub < mask.size() && mask[sub] == 0 && upper_name(model.proxies[p].name) == want) {
                mask[sub] = 1;
                ++n;
            }
        }
    }
    return n;
}

}  // namespace alamo::render
