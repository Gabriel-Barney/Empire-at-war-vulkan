#include "alamo/ui/hud_markers.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "alamo/core/ascii.hpp"
#include "alamo/data/values.hpp"
#include "alamo/data/xml.hpp"
#include "alamo/ui/hud_font.hpp"

namespace alamo::ui {

namespace {

// Trimmed (data::trim), then ASCII upper case.
std::string upper(std::string_view s) { return ascii_upper(data::trim(s)); }

void dot(HudDrawList& dl, uint32_t layer, Vec2f c, float half, Rgba8 color) {
    HudQuad q;
    q.min = {c.x - half, c.y - half};
    q.max = {c.x + half, c.y + half};
    q.color = color;
    dl.add(layer, HudList::Alpha, q);
}

// Dots along a pixel segment every `step` px (both ends included once by the caller's chaining). Returns dots.
uint32_t dotted(HudDrawList& dl, uint32_t layer, const HudCanvas& cv, float x0, float y0, float x1, float y1, float step,
                float size_px, Rgba8 color, float& carry) {
    const float dx = x1 - x0, dy = y1 - y0;
    const float len = std::sqrt(dx * dx + dy * dy);
    uint32_t n = 0;
    float t = carry;
    for (; t <= len; t += step) {
        const float f = len > 0.0f ? t / len : 0.0f;
        dot(dl, layer, cv.pixel_to_canvas(x0 + dx * f, y0 + dy * f), 0.5f * size_px * cv.units_per_pixel_y, color);
        ++n;
        if (n > 4096) break;  // degenerate projections
    }
    carry = t - len;
    return n;
}

}  // namespace

uint32_t add_waypoint_lines(HudDrawList& dl, const HudCanvas& canvas, const View& view, std::span<const HudWaypointLine> lines,
                            uint32_t layer) {
    uint32_t dots = 0;
    for (const HudWaypointLine& l : lines) {
        float carry = 0.0f;
        for (size_t i = 0; i + 1 < l.points.size(); ++i) {
            float ax = 0, ay = 0, bx = 0, by = 0;
            if (!project(view, l.points[i], ax, ay) || !project(view, l.points[i + 1], bx, by)) {
                carry = 0.0f;
                continue;
            }
            dots += dotted(dl, layer, canvas, ax, ay, bx, by, l.step_px, l.dot_px, l.color, carry);
        }
        float ex = 0, ey = 0;
        if (l.end_px > 0.0f && l.points.size() > 1 && project(view, l.points.back(), ex, ey)) {
            dot(dl, layer, canvas.pixel_to_canvas(ex, ey), 0.5f * l.end_px * canvas.units_per_pixel_y, l.color);
            ++dots;
        }
    }
    return dots;
}

std::vector<Vec3> remaining_route(Vec3 unit, std::span<const Vec3> path, Vec3 goal) {
    std::vector<Vec3> out{unit};
    if (path.empty()) {
        out.push_back(goal);
        return out;
    }
    // The segment nearest the unit (2-D); a one-point path is its own "segment". Ties keep the later segment, so a unit
    // standing on a waypoint continues past it.
    size_t from = 0;
    float best = 3.0e38f;
    for (size_t i = 0; i < path.size(); ++i) {
        const Vec3 a = path[i];
        const Vec3 b = i + 1 < path.size() ? path[i + 1] : a;
        const float dx = b.x - a.x, dy = b.y - a.y, len2 = dx * dx + dy * dy;
        float t = len2 > 0.0f ? ((unit.x - a.x) * dx + (unit.y - a.y) * dy) / len2 : 0.0f;
        t = std::clamp(t, 0.0f, 1.0f);
        const float px = a.x + dx * t - unit.x, py = a.y + dy * t - unit.y, d2 = px * px + py * py;
        if (d2 <= best) {
            best = d2;
            from = i + 1 < path.size() ? i + 1 : i;
        }
    }
    for (size_t i = from; i < path.size(); ++i) out.push_back(path[i]);
    return out;
}

Rgba8 queue_step_color(HudQueuedStep::Kind k) {
    switch (k) {
        case HudQueuedStep::Kind::Move: return {120, 255, 120, 220};
        case HudQueuedStep::Kind::Attack: return {255, 90, 70, 230};
        case HudQueuedStep::Kind::Ability: return {110, 170, 255, 230};
        case HudQueuedStep::Kind::Other: break;
    }
    return {200, 200, 200, 200};
}

QueueMarkerStats add_queue_markers(HudDrawList& dl, FontCache* fonts, const HudCanvas& canvas, const View& view,
                                   std::span<const HudOrderQueue> queues, uint32_t layer) {
    QueueMarkerStats st;
    const int32_t font = fonts ? fonts->font(kFallbackFontFace, 7.0f, canvas.screen_height) : -1;
    for (const HudOrderQueue& q : queues) {
        float px = 0, py = 0;
        bool have_prev = project(view, q.position, px, py);
        float carry = 0.0f;
        int32_t number = 0;
        for (const HudQueuedStep& s : q.steps) {
            ++number;
            if (!s.has_point) continue;
            float sx = 0, sy = 0;
            if (!project(view, s.point, sx, sy)) {
                have_prev = false;
                continue;
            }
            const Rgba8 col = queue_step_color(s.kind);
            if (have_prev) st.dots += dotted(dl, layer, canvas, px, py, sx, sy, 9.0f, 3.0f, col, carry);
            // Numbered marker.
            dot(dl, layer, canvas.pixel_to_canvas(sx, sy), 3.5f * canvas.units_per_pixel_y, col);
            if (font >= 0) {
                HudTextStyle ts;
                ts.font = font;
                ts.color = col;
                ts.outline = true;
                char num[8];
                std::snprintf(num, sizeof num, "%d", number);
                std::u32string t;
                for (const char* c = num; *c; ++c) t.push_back(static_cast<char32_t>(*c));
                add_text(dl, layer, HudList::Text, *fonts, canvas, t, canvas.pixel_to_canvas(sx + 6.0f, sy - 7.0f), ts);
            }
            ++st.markers;
            px = sx;
            py = sy;
            have_prev = true;
        }
    }
    return st;
}

void MoveAckConfig::parse(const data::XmlNode& root) {
    static constexpr const char* kEffects[4] = {"GUI_Move_Command_Ack_Effect", "GUI_Double_Click_Move_Command_Ack_Effect",
                                                "GUI_Attack_Move_Command_Ack_Effect", "GUI_Guard_Move_Command_Ack_Effect"};
    static constexpr const char* kRadar[3] = {"GUI_Movement_Click_Radar_Event_Name",
                                              "GUI_Movement_Double_Click_Radar_Event_Name",
                                              "GUI_Attack_Movement_Click_Radar_Event_Name"};
    for (int i = 0; i < 4; ++i)
        if (const std::string_view v = root.child_text(kEffects[i]); !v.empty()) effect[i] = std::string(v);
    for (int i = 0; i < 3; ++i)
        if (const std::string_view v = root.child_text(kRadar[i]); !v.empty()) radar_event[i] = std::string(v);
    radar_event[3] = radar_event[0];
    scale_land = data::parse_float_or(root.child_text("GUI_Move_Acknowledge_Scale_Land"), scale_land);
    scale_space = data::parse_float_or(root.child_text("GUI_Move_Acknowledge_Scale_Space"), scale_space);
}

MoveAckRequest MoveAcks::add(const MoveAckConfig& cfg, MoveAckKind kind, Vec3 position, bool land) {
    const auto k = static_cast<size_t>(kind);
    Item it;
    it.position = position;
    it.kind = kind;
    it.lifetime = cfg.lifetime;
    items_.push_back(it);
    MoveAckRequest r;
    r.effect = cfg.effect[k];
    r.scale = land ? cfg.scale_land : cfg.scale_space;
    r.position = position;
    r.radar_event = cfg.radar_event[k];
    return r;
}

void MoveAcks::update(float dt) {
    for (Item& i : items_) i.age += dt;
    items_.erase(std::remove_if(items_.begin(), items_.end(), [](const Item& i) { return i.age >= i.lifetime; }), items_.end());
}

uint32_t MoveAcks::draw(HudDrawList& dl, const HudCanvas& canvas, const View& view, uint32_t layer) const {
    static constexpr Rgba8 kColor[4] = {{80, 255, 80, 255}, {80, 255, 80, 255}, {255, 80, 60, 255}, {80, 160, 255, 255}};
    uint32_t n = 0;
    for (const Item& i : items_) {
        float sx = 0, sy = 0;
        if (!project(view, i.position, sx, sy)) continue;
        const float f = std::clamp(i.age / std::max(i.lifetime, 1e-3f), 0.0f, 1.0f);
        const float r = 18.0f * (1.0f - 0.7f * f);  // pixels
        Rgba8 c = kColor[static_cast<size_t>(i.kind)];
        c.a = static_cast<uint8_t>(std::lround(255.0f * (1.0f - f)));
        constexpr int kDots = 12;
        for (int d = 0; d < kDots; ++d) {
            const float a = static_cast<float>(d) * (6.28318530717958647692f / kDots);
            dot(dl, layer, canvas.pixel_to_canvas(sx + r * std::cos(a), sy + 0.6f * r * std::sin(a)), 1.5f * canvas.units_per_pixel_y, c);
            ++n;
        }
    }
    return n;
}

void ReticleTable::set(std::string_view variant, std::string_view type, std::string texture) {
    map_[upper(variant) + "|" + upper(type)] = std::move(texture);
}

void ReticleTable::parse(const data::XmlNode& root) {
    constexpr std::string_view kPrefix = "HARDPOINT_TARGET_RETICLE_", kSuffix = "_TEXTURE";
    for (data::XmlNode n : root.children()) {
        const std::string tag = upper(n.name());
        if (tag == "HARDPOINT_TARGET_RETICLE_ENEMY_SCREEN_SIZE") {
            enemy_screen_size = data::parse_float_or(n.text_trimmed(), enemy_screen_size);
            continue;
        }
        if (tag == "HARDPOINT_TARGET_RETICLE_FRIENDLY_SCREEN_SIZE") {
            friendly_screen_size = data::parse_float_or(n.text_trimmed(), friendly_screen_size);
            continue;
        }
        if (tag.size() <= kPrefix.size() + kSuffix.size() || tag.compare(0, kPrefix.size(), kPrefix) != 0 ||
            tag.compare(tag.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0)
            continue;
        const std::string variant = tag.substr(kPrefix.size(), tag.size() - kPrefix.size() - kSuffix.size());
        const auto pair = data::parse_pair(n.text_trimmed());
        if (!pair) continue;
        set(variant, pair->first, std::string(data::trim(pair->second)));
    }
}

std::string ReticleTable::texture(const HudHardPoint& hp) const {
    std::string variant;
    if (hp.enemy) {
        variant = hp.tracked ? "ENEMY_TRACKED" : "ENEMY";
    } else if (hp.repairing) {
        variant = "FRIENDLY_REPAIRING";
    } else if (hp.disabled) {
        variant = hp.tracked ? "FRIENDLY_DISABLED_TRACKED" : "FRIENDLY_DISABLED";
    } else {
        variant = hp.tracked ? "FRIENDLY_TRACKED" : "FRIENDLY";
    }
    const auto it = map_.find(variant + "|" + upper(hp.type));
    return it == map_.end() ? std::string{} : it->second;
}

std::vector<HudHardPoint> filter_hardpoints(std::span<const HudHardPoint> points, std::span<const HudUnit> units) {
    // HUD_GAPS §1.1 [H]: hover only - selection never creates reticles.
    std::vector<uint32_t> shown;  // hovered ship ids, sorted
    for (const HudUnit& u : units)
        if (u.hovered && !u.stealthed) shown.push_back(u.id);  // R5: no reticles on hostile stealthed ships (ABILITY_VISUALS 1.4)
    std::sort(shown.begin(), shown.end());
    std::vector<HudHardPoint> out;
    for (const HudHardPoint& hp : points)
        if (hp.parent == 0 || std::binary_search(shown.begin(), shown.end(), hp.parent)) out.push_back(hp);
    return out;
}

Rgba8 reticle_tint(const HudHardPoint& hp) {
    if (hp.disabled) return {128, 128, 128, 255};
    const float f = std::clamp(hp.health, 0.0f, 1.0f);
    if (f >= 0.66f) return {32, 255, 32, 255};
    if (f >= 0.33f) return {255, 255, 32, 255};
    return {255, 32, 32, 255};
}

bool reticle_flash_on(const HudHardPoint& hp, double now) {
    if (hp.flash_start < 0.0 || now < hp.flash_start) return false;
    const auto frame = static_cast<int64_t>(std::floor((now - hp.flash_start) * 30.0));
    return frame < 60 && (frame / 3) % 2 == 0;
}

Vec2f reticle_size(const HudHardPoint& hp, const ReticleTable& table, const HudCanvas& canvas, double now) {
    const float frac = hp.enemy ? table.enemy_screen_size : table.friendly_screen_size;
    Vec2f s{frac * canvas.width, frac * (4.0f / 3.0f) * canvas.height};
    if (reticle_flash_on(hp, now)) s = {0.5f * s.x, 0.5f * s.y};
    return s;
}

int32_t reticle_at(const HudCanvas& canvas, const View& view, const ReticleTable& table, std::span<const HudHardPoint> points,
                   float px, float py, double now) {
    const Vec2f cur = canvas.pixel_to_canvas(px, py);
    for (size_t i = 0; i < points.size(); ++i) {
        float sx = 0, sy = 0;
        if (!project(view, points[i].position, sx, sy)) continue;
        const Vec2f c = canvas.pixel_to_canvas(sx, sy);
        const Vec2f s = reticle_size(points[i], table, canvas, now);
        if (std::fabs(cur.x - c.x) <= 0.5f * s.x && std::fabs(cur.y - c.y) <= 0.5f * s.y) return static_cast<int32_t>(i);
    }
    return -1;
}

uint32_t add_hardpoint_reticles(HudDrawList& dl, const TextureAtlas& atlas, const HudCanvas& canvas, const View& view,
                                const ReticleTable& table, std::span<const HudHardPoint> points, uint32_t layer,
                                const GuiImageResolver* images, double now) {
    uint32_t n = 0;
    for (const HudHardPoint& hp : points) {
        float sx = 0, sy = 0;
        if (!project(view, hp.position, sx, sy)) continue;
        if (sx < 0 || sy < 0 || sx > static_cast<float>(view.width) || sy > static_cast<float>(view.height)) continue;
        const Vec2f c = canvas.pixel_to_canvas(sx, sy);
        const Vec2f size = reticle_size(hp, table, canvas, now);
        const Rgba8 col = reticle_tint(hp);
        const std::string tex = table.texture(hp);
        bool drawn = !tex.empty() && add_atlas_quad(dl, layer, HudList::Alpha, atlas, tex, c, size, 1.0f, col);
        if (!drawn && !tex.empty() && images && *images) {
            GuiImage img = (*images)(tex + ".tga");
            if (!img.valid) img = (*images)(tex);
            if (img.valid) {
                HudQuad q;
                q.min = {c.x - 0.5f * size.x, c.y - 0.5f * size.y};
                q.max = {c.x + 0.5f * size.x, c.y + 0.5f * size.y};
                q.uv0 = img.uv0;
                q.uv1 = img.uv1;
                q.page = img.page;
                q.color = col;
                dl.add(layer, HudList::Alpha, q);
                drawn = true;
            }
        }
        if (!drawn) {
            // [port] outline: four corner ticks.
            const float hx = 0.5f * size.x, hy = 0.5f * size.y, m = std::min(size.x, size.y);
            const float t = std::max(1.0f, m * 0.08f), l = m * 0.3f;
            const float sxs[2] = {-1.0f, 1.0f};
            for (float ux : sxs)
                for (float uy : sxs) {
                    HudQuad a, b2;
                    const float cx = c.x + ux * hx, cy = c.y + uy * hy;
                    a.min = {std::min(cx, cx - ux * l), std::min(cy, cy - uy * t)};
                    a.max = {std::max(cx, cx - ux * l), std::max(cy, cy - uy * t)};
                    b2.min = {std::min(cx, cx - ux * t), std::min(cy, cy - uy * l)};
                    b2.max = {std::max(cx, cx - ux * t), std::max(cy, cy - uy * l)};
                    a.color = b2.color = col;
                    dl.add(layer, HudList::Alpha, a);
                    dl.add(layer, HudList::Alpha, b2);
                }
        }
        ++n;
    }
    return n;
}

}  // namespace alamo::ui
