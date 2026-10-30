// R5 fog of war, renderer side. See fog_view.hpp.
#include "alamo/render/fog_view.hpp"

#include <algorithm>
#include <cmath>
#include <string_view>

#include "alamo/data/values.hpp"
#include "alamo/data/xml.hpp"
#include "alamo/render/world.hpp"

namespace alamo::render {

bool FogFrameInput::usable() const {
    if (!valid || width == 0 || height == 0 || width > terrain::kMaxFowCells || height > terrain::kMaxFowCells) return false;
    if (state.size() != cells() || fade.size() != cells()) return false;
    for (float f : {x0, y0, x1, y1, cell_w, cell_h})
        if (!std::isfinite(f)) return false;
    return x1 > x0 && y1 > y0 && cell_w > 0.0f && cell_h > 0.0f;
}

uint32_t fog_frames_since_decay(uint32_t frame, uint32_t last_decay) {
    if (frame <= last_decay) return 0;  // F - last - 1 < 0
    const uint32_t d = frame - last_decay - 1u;
    return d > 15u ? 15u : d;
}

uint32_t fog_regrow_per_frame(int32_t regrow_step) { return regrow_step > 0 ? static_cast<uint32_t>(regrow_step) / 16u : 0u; }

bool fog_intensity(const FogFrameInput& in, std::vector<uint8_t>& out) {
    out.clear();
    if (!in.usable()) return false;
    const uint32_t f = fog_frames_since_decay(in.frame, in.last_decay);
    const uint32_t r = fog_regrow_per_frame(in.regrow_step);
    const size_t n = in.cells();
    out.resize(n);
    for (size_t i = 0; i < n; ++i) out[i] = terrain::fow_state_to_intensity(in.state[i], in.fade[i], f, r);
    return true;
}

terrain::FowGridDesc fog_grid_desc(const FogFrameInput& in) {
    terrain::FowGridDesc d;
    if (!in.usable()) return d;
    d.x0 = in.x0;
    d.y0 = in.y0;
    d.x1 = in.x1;
    d.y1 = in.y1;
    d.width = in.width;
    d.height = in.height;
    d.cell_x = in.cell_w;
    d.cell_y = in.cell_h;
    return d;
}

bool parse_fog_colors(std::span<const uint8_t> xml, FogColors& out) {
    const data::XmlDocument doc =
        data::XmlDocument::parse(std::string_view(reinterpret_cast<const char*>(xml.data()), xml.size()), "GameConstants.xml");
    if (!doc.ok()) return false;
    const data::XmlNode root = doc.root();
    auto color = [&](std::string_view tag, uint32_t& v) {
        const data::XmlNode n = root.child(tag);
        if (!n) return;
        float c[4] = {0, 0, 0, 0};
        if (data::parse_floats(n.text_trimmed(), c, 4) != 4) return;
        uint32_t u[4];
        for (int i = 0; i < 4; ++i) {
            if (!std::isfinite(c[i])) return;
            u[i] = c[i] <= 0.0f ? 0u : c[i] >= 255.0f ? 255u : static_cast<uint32_t>(c[i]);
        }
        v = terrain::pack_fow_color(u[0], u[1], u[2], u[3]);
    };
    color("LandFOWColor", out.land_fog);
    color("SetupPhaseFOWColor", out.setup_fog);
    color("SpaceFOWColor", out.space_fog);
    color("SpaceReinforceFOWColor", out.space_reinforce_fog);
    return true;
}

std::array<uint32_t, 256> fog_palette_for(const FogColors& c, bool space, bool setup_phase, bool reinforce) {
    if (space) return terrain::fow_palette(terrain::kSpaceFowVisible, reinforce ? c.space_reinforce_fog : c.space_fog);
    return terrain::fow_palette(terrain::kLandFowVisible, setup_phase ? c.setup_fog : c.land_fog);
}

bool fog_point_visible(const FogFrameInput& in, float x, float y) {
    if (!in.usable() || !in.fow_active) return true;
    if (!std::isfinite(x) || !std::isfinite(y)) return false;
    if (x < in.x0 || x > in.x1 || y < in.y0 || y > in.y1) return false;  // outside the box: fogged
    const float cx = std::clamp(x, in.x0 + 0.001f, in.x1 - 0.001f), cy = std::clamp(y, in.y0 + 0.001f, in.y1 - 0.001f);
    const auto col = static_cast<int64_t>((cx - in.x0) / in.cell_w), row = static_cast<int64_t>((in.y1 - cy) / in.cell_h);
    const int64_t n = static_cast<int64_t>(in.cells());
    const int64_t idx = std::clamp<int64_t>(row * static_cast<int64_t>(in.width) + col, 0, n - 1);
    return in.state[static_cast<size_t>(idx)] != 0;
}

bool FogRenderer::build(const FogFrameInput& in, bool space) {
    if (!fog_intensity(in, intensity_)) return false;
    // R5: the reinforcement overlay shows the sampler's cells (0 blocked / 0xFF allowed) instead of the player's grid.
    stats_.reinforce_sampled = space && reinforce_ && in.reinforce.size() == in.cells();
    if (stats_.reinforce_sampled) intensity_.assign(in.reinforce.begin(), in.reinforce.end());
    const terrain::FowGridDesc d = fog_grid_desc(in);
    if (grid_.width() != d.width || grid_.height() != d.height || grid_.desc().x0 != d.x0 || grid_.desc().y0 != d.y0 ||
        grid_.desc().x1 != d.x1 || grid_.desc().y1 != d.y1 || grid_.desc().cell_x != d.cell_x || grid_.desc().cell_y != d.cell_y)
        grid_ = terrain::FowGrid(d);
    grid_.set_intensity(intensity_);
    grid_.build_texels(fog_palette_for(colors_, space, in.setup_phase, space && reinforce_), blur_, &processed_);
    if (space && !reinforce_) grid_.fade_texel_edges(terrain::kSpaceFowEdgeFadeCells);  // [port] no hard box outline (playtest R4)
    stats_.width = d.width;
    stats_.height = d.height;
    stats_.fogged_cells = stats_.clear_cells = 0;
    for (uint8_t v : processed_) {
        stats_.fogged_cells += v == 0 ? 1u : 0u;
        stats_.clear_cells += v == 255 ? 1u : 0u;
    }
    return true;
}

bool FogRenderer::update(WorldRenderer& world, const FogFrameInput& in, bool space) {
    ++stats_.updates;
    if (!in.usable()) {
        ++stats_.invalid;
        world.set_fow_enabled(false);
        active_ = false;
        return false;
    }
    if (!in.drawn()) {  // FOW-active off or the render option off: no fog drawn (and none in game logic)
        ++stats_.disabled;
        world.set_fow_enabled(false);
        active_ = false;
        return false;
    }
    build(in, space);
    if (space) world.set_space_fow_reinforce(reinforce_);
    const std::vector<uint8_t>& tex = grid_.texels();
    if (have_upload_ && uploaded_w_ == grid_.width() && uploaded_h_ == grid_.height() && uploaded_ == tex) {
        ++stats_.skipped_same;
        world.set_fow_enabled(true);
        active_ = world.fow_enabled() || world.fow_upload_pending();
        return false;
    }
    if (!world.set_fog_of_war(grid_)) {
        world.set_fow_enabled(false);
        active_ = false;
        return false;
    }
    uploaded_ = tex;
    uploaded_w_ = grid_.width();
    uploaded_h_ = grid_.height();
    have_upload_ = true;
    ++stats_.uploads;
    active_ = true;
    return true;
}

uint8_t FogRenderer::value_at(float x, float y) const {
    uint32_t cx = 0, cy = 0;
    if (processed_.empty() || !grid_.desc().cell_of(x, y, cx, cy)) return 255;
    const size_t i = static_cast<size_t>(cy) * grid_.width() + cx;
    return i < processed_.size() ? processed_[i] : uint8_t{255};
}

}  // namespace alamo::render
