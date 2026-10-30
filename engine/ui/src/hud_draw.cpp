#include "alamo/ui/hud_draw.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "alamo/core/hash.hpp"
#include "alamo/ui/hud_font.hpp"

namespace alamo::ui {

namespace {

HudVertex corner(const HudQuad& q, float tx, float ty) {  // tx: 0 left .. 1 right; ty: 0 top .. 1 bottom
    HudVertex v;
    v.x = q.min.x + (q.max.x - q.min.x) * tx;
    v.y = q.max.y + (q.min.y - q.max.y) * ty;
    v.u = q.uv0.x + (q.uv1.x - q.uv0.x) * tx;
    v.v = q.uv0.y + (q.uv1.y - q.uv0.y) * ty;
    v.color = pack_rgba(q.color);
    if (q.gradient) {  // P4: Top_Color at the top edge, Bottom_Color at the bottom edge
        auto mix = [ty](uint8_t a, uint8_t b) {
            return static_cast<uint8_t>(static_cast<float>(a) + (static_cast<float>(b) - static_cast<float>(a)) * ty + 0.5f);
        };
        v.color = pack_rgba({mix(q.color.r, q.color_bottom.r), mix(q.color.g, q.color_bottom.g), mix(q.color.b, q.color_bottom.b),
                             mix(q.color.a, q.color_bottom.a)});
    }
    v.page = q.page;
    return v;
}

// Point on the quad's border at clockwise angle `a` (radians) from 12 o'clock, in (tx, ty) quad coordinates.
void border_point(float a, float& tx, float& ty) {
    const float dx = std::sin(a), dy = -std::cos(a);  // screen-down dy for ty
    const float m = std::max(std::fabs(dx), std::fabs(dy));
    tx = 0.5f + 0.5f * dx / m;
    ty = 0.5f + 0.5f * dy / m;
}

}  // namespace

uint32_t radial_wipe(const HudQuad& q, float fill, HudVertex (&out)[11]) {
    if (!(fill > 0.0f)) return 0;
    if (fill >= 100.0f) {
        out[0] = corner(q, 0, 0);
        out[1] = corner(q, 1, 0);
        out[2] = corner(q, 1, 1);
        out[3] = corner(q, 0, 1);
        return 4;
    }
    constexpr float kTwoPi = 6.28318530717958647692f;
    const float sweep = fill / 100.0f * kTwoPi;
    uint32_t n = 0;
    out[n++] = corner(q, 0.5f, 0.5f);
    out[n++] = corner(q, 0.5f, 0.0f);
    // Boundaries at 45-degree steps: exact corner / edge-midpoint positions (no trig rounding).
    static constexpr float kB[7][2] = {{1, 0}, {1, 0.5f}, {1, 1}, {0.5f, 1}, {0, 1}, {0, 0.5f}, {0, 0}};
    for (uint32_t b = 1; b <= 7; ++b) {
        // b * 12.5 % expressed in fill units avoids comparing angles in floating point.
        if (static_cast<float>(b) * 12.5f < fill) out[n++] = corner(q, kB[b - 1][0], kB[b - 1][1]);
    }
    float tx = 0, ty = 0;
    border_point(sweep, tx, ty);
    out[n++] = corner(q, tx, ty);
    return n;
}

void HudDrawList::clear() {
    for (auto& layer : lists_)
        for (auto& l : layer) l.clear();
}

void HudDrawList::add(uint32_t layer, HudList list, const HudQuad& q) {
    lists_[layer < kHudLayers ? layer : kHudLayers - 1][static_cast<size_t>(list)].push_back(q);
}

size_t HudDrawList::total() const {
    size_t n = 0;
    for (const auto& layer : lists_)
        for (const auto& l : layer) n += l.size();
    return n;
}

const char* hud_effect_name(HudEffect e) {
    static constexpr const char* kNames[kHudEffectCount] = {
        "Engine\\PrimOpaque",          "Engine\\PrimAdditive",          "Engine\\PrimAlpha",
        "Engine\\PrimModulate",        "Engine\\PrimDepthSpriteAdditive", "Engine\\PrimDepthSpriteAlpha",
        "Engine\\PrimDepthSpriteModulate", "Engine\\PrimDiffuseAlpha",  "Engine\\StencilDarken",
        "Engine\\StencilDarkenFinalBlur", "Engine\\PrimHeat",           "Engine\\PrimParticleBumpAlpha",
        "Engine\\PrimDecalBumpAlpha",  "Engine\\PrimAlphaScanlines"};
    const auto i = static_cast<uint32_t>(e);
    return i < kHudEffectCount ? kNames[i] : "";
}

HudEffect hud_list_effect(HudList list) {
    switch (static_cast<uint8_t>(list)) {
        case 0: return HudEffect::PrimOpaque;    // plain quads
        case 1: return HudEffect::PrimAlpha;     // fill dials
        case 2: return HudEffect::PrimAdditive;  // alternate dials
        case 3: return HudEffect::PrimModulate;  // extra quads
        default: return HudEffect::PrimAlpha;    // [port] model text / text
    }
}

void HudDrawList::build_geometry(uint32_t layer, HudList list, std::vector<HudVertex>& vertices,
                                 std::vector<uint32_t>& indices, std::vector<Batch>& batches, size_t merge_floor) const {
    const HudEffect effect = hud_list_effect(list);
    for (const HudQuad& q : items(layer, list)) {
        HudVertex v[11];
        const uint32_t n = radial_wipe(q, q.fill, v);
        if (n < 3) continue;
        const auto base = static_cast<uint32_t>(vertices.size());
        const auto first = static_cast<uint32_t>(indices.size());
        vertices.insert(vertices.end(), v, v + n);
        if (n == 4) {
            for (uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) indices.push_back(base + i);
        } else {
            for (uint32_t i = 1; i + 1 < n; ++i) {
                indices.push_back(base);
                indices.push_back(base + i);
                indices.push_back(base + i + 1);
            }
        }
        const auto count = static_cast<uint32_t>(indices.size()) - first;
        if (batches.size() > merge_floor && batches.back().page == q.page && batches.back().effect == effect &&
            batches.back().first_index + batches.back().index_count == first) {
            batches.back().index_count += count;
        } else {
            batches.push_back({first, count, q.page, effect});
        }
    }
}

void HudDrawList::build_layers(std::vector<HudVertex>& vertices, std::vector<uint32_t>& indices, std::vector<Batch>& batches,
                               LayerBatchRanges* ranges) const {
    for (uint32_t l = 0; l < kHudLayers; ++l) {
        const size_t b0 = batches.size();
        for (uint32_t s = 0; s < kHudSubLists; ++s) build_geometry(l, static_cast<HudList>(s), vertices, indices, batches, b0);
        if (ranges) (*ranges)[l] = {b0, batches.size()};
    }
}

uint64_t HudDrawList::digest() const {
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t l = 0; l < kHudLayers; ++l)
        for (uint32_t s = 0; s < kHudSubLists; ++s) {
            const auto& v = lists_[l][s];
            const uint64_t n = v.size();
            h = fnv1a64(&n, sizeof n, h);
            for (const HudQuad& q : v) {
                const float f[9] = {q.min.x, q.min.y, q.max.x, q.max.y, q.uv0.x, q.uv0.y, q.uv1.x, q.uv1.y, q.fill};
                h = fnv1a64(f, sizeof f, h);
                const uint32_t c = pack_rgba(q.color), pb = q.page;
                h = fnv1a64(&c, sizeof c, h);
                h = fnv1a64(&pb, sizeof pb, h);
                if (q.gradient) {
                    const uint32_t cb = pack_rgba(q.color_bottom);
                    h = fnv1a64(&cb, sizeof cb, h);
                }
            }
        }
    return h;
}

// ---- Emitters -------------------------------------------------------------------------------------
namespace {
bool atlas_uv(const TextureAtlas& atlas, const AtlasEntry& e, Vec2f& uv0, Vec2f& uv1) {
    const auto tw = static_cast<float>(atlas.texture_width), th = static_cast<float>(atlas.texture_height);
    if (tw <= 0 || th <= 0) return false;
    uv0 = {static_cast<float>(e.x) / tw, static_cast<float>(e.y) / th};
    uv1 = {static_cast<float>(e.x + e.width) / tw, static_cast<float>(e.y + e.height) / th};
    return true;
}
}  // namespace

namespace {
bool atlas_quad(HudDrawList& dl, uint32_t layer, HudList list, const TextureAtlas& atlas, std::string_view texture,
                Vec2f center, Vec2f size, float scale, float fraction, Rgba8 color, Vec2f fallback_size, float fill) {
    HudQuad q;
    q.color = color;
    q.fill = fill;
    const AtlasEntry* e = texture.empty() ? nullptr : atlas.find(texture);
    if (e && atlas_uv(atlas, *e, q.uv0, q.uv1)) {
        q.page = kHudPageAtlas;
        if (size.x <= 0 || size.y <= 0) size = {static_cast<float>(e->width), static_cast<float>(e->height)};
    } else {
        if (fallback_size.x <= 0 || fallback_size.y <= 0) return false;
        q.page = kHudPageWhite;
        size = fallback_size;
    }
    size.x *= scale;
    size.y *= scale;
    fraction = std::clamp(fraction, 0.0f, 1.0f);
    if (fraction <= 0.0f) return false;
    q.min = {center.x - 0.5f * size.x, center.y - 0.5f * size.y};
    q.max = {q.min.x + size.x * fraction, center.y + 0.5f * size.y};
    q.uv1.x = q.uv0.x + (q.uv1.x - q.uv0.x) * fraction;
    dl.add(layer, list, q);
    return true;
}
}  // namespace

bool add_atlas_quad(HudDrawList& dl, uint32_t layer, HudList list, const TextureAtlas& atlas, std::string_view texture,
                    Vec2f center, Vec2f size, float scale, Rgba8 color, Vec2f fallback_size, float fill) {
    return atlas_quad(dl, layer, list, atlas, texture, center, size, scale, 1.0f, color, fallback_size, fill);
}

bool add_atlas_quad_clipped(HudDrawList& dl, uint32_t layer, HudList list, const TextureAtlas& atlas,
                            std::string_view texture, Vec2f center, Vec2f size, float scale, float fraction, Rgba8 color,
                            Vec2f fallback_size) {
    return atlas_quad(dl, layer, list, atlas, texture, center, size, scale, fraction, color, fallback_size, 100.0f);
}

void add_movie_quad(HudDrawList& dl, uint32_t layer, Vec2f min, Vec2f max, uint16_t page, bool alpha) {
    HudQuad q;
    q.min = min;
    q.max = max;
    q.color = {255, 255, 255, 255};
    q.page = page;
    dl.add(layer, alpha ? HudList::Alpha : HudList::Opaque, q);
}

float add_text(HudDrawList& dl, uint32_t layer, HudList list, FontCache& fonts, const HudCanvas& canvas,
               std::u32string_view text, Vec2f anchor, const HudTextStyle& style) {
    if (style.font < 0 || text.empty()) return 0.0f;
    const FontMetrics& m = fonts.metrics(style.font);
    const float s = style.scale > 0.0f ? style.scale : 1.0f;
    const float upx = canvas.units_per_pixel_x * s * style.width_scale, upy = canvas.units_per_pixel_y * s;
    const float pad = style.char_padding_px;
    const float width_px = static_cast<float>(fonts.measure(style.font, text)) + pad * static_cast<float>(text.size());
    const float width = width_px * upx;
    // Top-left of the line box in canvas units (+Y up).
    float left = anchor.x;
    if (style.align == TextAlign::Center) left -= 0.5f * width;
    if (style.align == TextAlign::Right) left -= width;
    float top = anchor.y + 0.5f * static_cast<float>(m.line_height) * upy;
    if (style.snap) {
        const Vec2f sn = canvas.snap_to_pixel({left, top});
        left = sn.x;
        top = sn.y;
    }
    const float baseline = top - static_cast<float>(m.ascent) * upy;
    const float page = static_cast<float>(fonts.page_size());
    auto emit = [&](float ox_px, float oy_px, Rgba8 color, bool gradient) {
        float pen = left + ox_px * upx;
        for (char32_t c : text) {
            const FontCache::Glyph* g = fonts.glyph(style.font, c);
            if (!g) continue;
            if (g->width && g->height && g->page < kHudFontPages) {
                HudQuad q;
                q.min.x = pen + static_cast<float>(g->origin_x) * upx;
                q.max.y = baseline + static_cast<float>(g->origin_y) * upy - oy_px * upy;
                q.max.x = q.min.x + static_cast<float>(g->width) * upx;
                q.min.y = q.max.y - static_cast<float>(g->height) * upy;
                q.uv0 = {static_cast<float>(g->x) / page, static_cast<float>(g->y) / page};
                q.uv1 = {static_cast<float>(g->x + g->width) / page, static_cast<float>(g->y + g->height) / page};
                q.color = color;
                q.gradient = gradient;
                q.color_bottom = style.color_bottom;
                q.page = static_cast<uint16_t>(kHudPageFontBase + g->page);
                dl.add(layer, list, q);
            }
            pen += (static_cast<float>(g->advance) + pad) * upx;
        }
    };
    if (style.emboss) emit(1.0f, 1.0f, style.emboss_color, false);
    if (style.outline) {
        emit(-1.0f, 0.0f, style.outline_color, false);
        emit(1.0f, 0.0f, style.outline_color, false);
        emit(0.0f, -1.0f, style.outline_color, false);
        emit(0.0f, 1.0f, style.outline_color, false);
    }
    emit(0.0f, 0.0f, style.color, style.gradient);
    return width;
}

std::vector<std::u32string> wrap_text(FontCache& fonts, int32_t font, std::u32string_view text, float max_width_px) {
    std::vector<std::u32string> lines;
    if (font < 0) return lines;
    auto width = [&](std::u32string_view s) { return static_cast<float>(fonts.measure(font, s)); };
    size_t para = 0;
    while (para <= text.size()) {
        size_t end = text.find(U'\n', para);
        if (end == std::u32string_view::npos) end = text.size();
        const std::u32string_view p = text.substr(para, end - para);
        if (max_width_px <= 0.0f || width(p) <= max_width_px) {
            lines.emplace_back(p);
        } else {
            const size_t first_line = lines.size();
            std::u32string line;
            size_t i = 0;
            while (i < p.size()) {
                // Next word (with its leading spaces when the line is not empty).
                size_t ws = i;
                while (ws < p.size() && p[ws] == U' ') ++ws;
                size_t we = ws;
                while (we < p.size() && p[we] != U' ') ++we;
                const std::u32string_view word = p.substr(ws, we - ws);
                if (word.empty()) break;
                std::u32string candidate = line;
                if (!candidate.empty()) candidate.append(p.substr(i, ws - i));
                candidate.append(word);
                if (width(candidate) <= max_width_px) {
                    line = std::move(candidate);
                    i = we;
                    continue;
                }
                if (!line.empty()) {  // break before the word
                    lines.push_back(std::move(line));
                    line.clear();
                    i = ws;
                    continue;
                }
                // A single word wider than the limit: split between code points (at least one per line).
                size_t k = 0;
                std::u32string part;
                while (k < word.size()) {
                    std::u32string next = part;
                    next.push_back(word[k]);
                    if (!part.empty() && width(next) > max_width_px) break;
                    part = std::move(next);
                    ++k;
                }
                lines.push_back(std::move(part));
                i = ws + k;
            }
            if (!line.empty() || lines.size() == first_line) lines.push_back(std::move(line));
        }
        if (end == text.size()) break;
        para = end + 1;
    }
    return lines;
}

float wrap_width_px(const HudCanvas& canvas, float max_text_width_units, float scale) {
    if (max_text_width_units <= 0.0f) return 0.0f;
    const float s = scale > 0.0f ? scale : 1.0f;
    return max_text_width_units / (canvas.units_per_pixel_x * s) * canvas.text_wrap_factor();
}

namespace {
TextBlock text_block(HudDrawList* dl, uint32_t layer, HudList list, FontCache& fonts, const HudCanvas& canvas,
                     std::u32string_view text, Vec2f anchor, float max_width_units, const HudTextStyle& style) {
    TextBlock tb;
    if (style.font < 0) return tb;
    const float s = style.scale > 0.0f ? style.scale : 1.0f;
    const std::vector<std::u32string> lines = wrap_text(fonts, style.font, text, wrap_width_px(canvas, max_width_units, s * style.width_scale));
    const float lh = static_cast<float>(fonts.metrics(style.font).line_height) * canvas.units_per_pixel_y * s;
    for (size_t i = 0; i < lines.size(); ++i) {
        const float width_px = static_cast<float>(fonts.measure(style.font, lines[i])) + style.char_padding_px * static_cast<float>(lines[i].size());
        const float w = width_px * canvas.units_per_pixel_x * s * style.width_scale;
        tb.width = std::max(tb.width, w);
        if (dl && !lines[i].empty())
            add_text(*dl, layer, list, fonts, canvas, lines[i], {anchor.x, anchor.y - (static_cast<float>(i) + 0.5f) * lh}, style);
    }
    tb.lines = static_cast<uint32_t>(lines.size());
    tb.height = static_cast<float>(lines.size()) * lh;
    return tb;
}
}  // namespace

TextBlock add_text_block(HudDrawList& dl, uint32_t layer, HudList list, FontCache& fonts, const HudCanvas& canvas,
                         std::u32string_view text, Vec2f anchor, float max_width_units, const HudTextStyle& style) {
    return text_block(&dl, layer, list, fonts, canvas, text, anchor, max_width_units, style);
}

TextBlock measure_text_block(FontCache& fonts, const HudCanvas& canvas, std::u32string_view text, float max_width_units,
                             const HudTextStyle& style) {
    return text_block(nullptr, 0, HudList::Text, fonts, canvas, text, {}, max_width_units, style);
}

std::span<const HudFrameStep> hud_frame_steps() {
    static constexpr HudFrameStep kSteps[] = {
        {HudStepKind::Layers, 0, 1},   {HudStepKind::ShellModels, 0, 0}, {HudStepKind::Layers, 2, 4},
        {HudStepKind::HudScene2d, 0, 0}, {HudStepKind::Layers, 5, 6},  {HudStepKind::Models2, 0, 0},
        {HudStepKind::Layers, 7, 8},   {HudStepKind::ShellModels, 1, 1}, {HudStepKind::Teletype, 0, 0},
        {HudStepKind::Layers, 9, 10},  {HudStepKind::Dialogs, 0, 0},     {HudStepKind::Layers, 11, 12},
        {HudStepKind::Cursor, 0, 0},
    };
    return kSteps;
}

}  // namespace alamo::ui
