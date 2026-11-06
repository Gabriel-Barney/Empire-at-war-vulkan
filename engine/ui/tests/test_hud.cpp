// alamo_ui_hud tests (P2, CPU only; no framework, non-zero exit on failure): HUD canvas + ortho cameras at
// 1024x768 / 1920x1080 / 1280x1024 / 800x600, font pixel rule, CommandBarComponents parsing, MTD atlas and
// MasterTextFile DAT readers (+ English fallback), shell-bone layout with Hor+ anchoring, radial wipe geometry,
// draw-list batching and frame order, text layout, projection of brackets / bars / control-group numbers
// (constant size, off-screen, size classes), selection circles, input routing and the selection panel.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "alamo/core/file_provider.hpp"
#include "alamo/core/hash.hpp"
#include "alamo/data/xml.hpp"
#include "alamo/ui/camera.hpp"
#include "alamo/ui/hud_abilities.hpp"
#include "alamo/ui/hud_canvas.hpp"
#include "alamo/ui/hud_components.hpp"
#include "alamo/ui/hud_draw.hpp"
#include "alamo/ui/hud_font.hpp"
#include "alamo/ui/hud_input.hpp"
#include "alamo/ui/hud_world.hpp"
#include "alamo/ui/input.hpp"
#include "alamo/ui/picking.hpp"
#include "alamo/ui/tactical_hud.hpp"
#include "hud_test_data.hpp"
#ifdef ALAMO_UI_HAVE_VFS
#include "alamo/vfs/vfs.hpp"
#endif

using namespace alamo;
using namespace alamo::ui;

static int g_fail = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)

namespace {

bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

class MemProvider final : public FileProvider {
public:
    void put(std::string path, std::vector<uint8_t> bytes) { files_[normalize_path(path)] = std::move(bytes); }
    bool read(std::string_view path, std::vector<uint8_t>& out) const override {
        const auto it = files_.find(normalize_path(path));
        if (it == files_.end()) return false;
        out = it->second;
        return true;
    }
    bool exists(std::string_view path) const override { return files_.count(normalize_path(path)) != 0; }
    std::string origin(std::string_view path) const override { return exists(path) ? "mem" : ""; }

private:
    std::map<std::string, std::vector<uint8_t>> files_;
};

void put32(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

std::vector<uint8_t> make_mtd(const std::vector<AtlasEntry>& es) {
    std::vector<uint8_t> b;
    put32(b, static_cast<uint32_t>(es.size()));
    for (const AtlasEntry& e : es) {
        char name[64] = {};
        std::memcpy(name, e.name.data(), std::min<size_t>(63, e.name.size()));
        b.insert(b.end(), name, name + 64);
        put32(b, e.x);
        put32(b, e.y);
        put32(b, e.width);
        put32(b, e.height);
        b.push_back(e.alpha ? 1 : 0);
    }
    return b;
}

std::vector<uint8_t> make_dat(const std::vector<std::pair<std::string, std::u16string>>& kv) {
    std::vector<uint8_t> b;
    put32(b, static_cast<uint32_t>(kv.size()));
    for (const auto& [k, v] : kv) {
        put32(b, crc32(k.data(), k.size()));
        put32(b, static_cast<uint32_t>(v.size()));
        put32(b, static_cast<uint32_t>(k.size()));
    }
    for (const auto& kvp : kv)
        for (char16_t c : kvp.second) {
            b.push_back(static_cast<uint8_t>(c & 0xFF));
            b.push_back(static_cast<uint8_t>(c >> 8));
        }
    for (const auto& kvp : kv) b.insert(b.end(), kvp.first.begin(), kvp.first.end());
    return b;
}

// Camera straight above the origin looking down (-Z), north = up on screen.
View top_view(uint32_t w, uint32_t h, float height = 1000.0f) {
    CameraParams p;
    p.eye = {0, -0.001f, height};
    p.target = {0, 0, 0};
    p.fov_x = 0.9599311f;
    p.znear = 10.0f;
    p.zfar = 7000.0f;
    return make_view(p, w, h);
}

// ---------------------------------------------------------------------------------------------- tests
void test_canvas() {
    const HudCanvas a = HudCanvas::make(1024, 768);
    CHECK(near(a.width, 1024) && near(a.height, 768) && near(a.units_per_pixel_x, 1) && near(a.scale_x, 1));
    const HudCanvas b = HudCanvas::make(1920, 1080);
    CHECK(near(b.width, 1920.0f * 768.0f / 1080.0f) && near(b.height, 768) && near(b.scale_x, b.width / 1024.0f) &&
          near(b.scale_y, 1.0f) && near(b.units_per_pixel_x, b.width / 1920.0f) && near(b.units_per_pixel_y, 768.0f / 1080.0f));
    const HudCanvas c = HudCanvas::make(1280, 1024);
    CHECK(near(c.width, 1024) && near(c.height, 1024.0f * 1024.0f / 1280.0f) && near(c.scale_y, c.height / 768.0f));
    const HudCanvas d = HudCanvas::make(800, 600);
    CHECK(near(d.width, 1024) && near(d.height, 768) && near(d.units_per_pixel_x, 1.28f) && d.text_wrap_factor() == 0.95f);
    CHECK(a.text_wrap_factor() == 1.0f);
    for (const HudCanvas* cv : {&a, &b, &c, &d}) {
        // Corners and round trips.
        const Vec2f tl = cv->pixel_to_canvas(0, 0), br = cv->pixel_to_canvas(static_cast<float>(cv->screen_width),
                                                                             static_cast<float>(cv->screen_height));
        CHECK(near(tl.x, -0.5f * cv->width) && near(tl.y, 0.5f * cv->height) && near(br.x, 0.5f * cv->width) &&
              near(br.y, -0.5f * cv->height));
        CHECK(near(cv->corners[0].x, tl.x) && near(cv->corners[2].y, br.y));
        const Vec2f p = cv->canvas_to_pixel(123.0f, -45.0f), q = cv->pixel_to_canvas(p.x, p.y);
        CHECK(near(q.x, 123.0f, 1e-2f) && near(q.y, -45.0f, 1e-2f));
        // Three ortho cameras spanning the canvas; near/far per UI.md 2.4.
        CHECK(near(cv->cameras[kHudCameraComponents].half_width, 0.5f * cv->width) &&
              near(cv->cameras[kHudCameraShells].half_height, 0.5f * cv->height));
        CHECK(cv->cameras[0].znear == 0.1f && cv->cameras[0].zfar == 40.0f && cv->cameras[1].znear == 0.01f &&
              cv->cameras[1].zfar == 500.0f && cv->cameras[2].zfar == 40.0f);
        const auto m = cv->cameras[kHudCameraShells].view_proj();
        const float z = cv->cameras[1].eye_z() - cv->cameras[1].znear;
        const float x = 0.5f * cv->width, y = 0.5f * cv->height;
        CHECK(near(m[0][0] * x + m[0][3], 1.0f) && near(m[1][1] * y + m[1][3], 1.0f) && near(m[2][2] * z + m[2][3], 0.0f, 1e-5f));
        const float zf = cv->cameras[1].eye_z() - cv->cameras[1].zfar;
        CHECK(near(m[2][2] * zf + m[2][3], 1.0f, 1e-5f));
        // Snapping lands on whole pixels.
        const Vec2f s = cv->snap_to_pixel({10.3f, 7.7f});
        const Vec2f sp = cv->canvas_to_pixel(s.x, s.y);
        CHECK(near(sp.x, std::round(sp.x), 1e-3f) && near(sp.y, std::round(sp.y), 1e-3f));
    }
    // Font pixel rule: point * (screenH * 96 / 600) / 72.
    CHECK(near(font_pixel_height(9, 768), 15.36f) && font_pixel_height_int(9, 768) == 15);
    CHECK(near(font_pixel_height(9, 1080), 21.6f) && font_pixel_height_int(9, 1080) == 22);
    CHECK(near(font_pixel_height(9, 1024), 20.48f) && font_pixel_height_int(9, 1024) == 20);
    CHECK(font_pixel_height_int(7, 600) == 9 && font_pixel_height_int(0.01f, 100) == 1);
}

void test_components() {
    const ComponentTable t = hud_test::load_table();
    CHECK(t.size() == 16);  // duplicate st_health replaced in place
    const CommandBarComponent* b = t.get("ST_BRACKET_SMALL");
    CHECK(b && b->type == ComponentType::TextButton && b->no_shell && b->has_color && b->color == (Rgba8{0, 255, 0, 255}));
    CHECK(b && b->icon_alternate_texture.size() == 2 && b->icon_alternate_texture[1] == "i_icon_select_small_r.tga");
    const CommandBarComponent* h = t.get("st_health");
    CHECK(h && t.find("st_health") == 4 && h->bar_overlay.size() == 2 && h->smooth_bar && h->max_bar_level == 10);
    const CommandBarComponent* s = t.get("i_main_skirmish");
    CHECK(s && s->type == ComponentType::Shell && s->model_name == "i_tactical_controls.alo" && s->hidden &&
          s->model_offset_x && s->model_offset_y);
    const CommandBarComponent* g = t.get("st_control_group");
    CHECK(g && g->font_point_size == 9.0f && near(g->text_offset.y, -32) && g->text_outline && g->has_text_color);
    const CommandBarComponent* r = t.get("radar");
    CHECK(r && r->has_size && near(r->size.x, 171) && near(r->size.y, 175));
    const CommandBarComponent* tb = t.get("tooltip_back");  // tag-named form
    CHECK(tb && tb->base_layer == 10);
    const CommandBarComponent* ss = t.get("s_select_00");
    CHECK(ss && ss->tooltip_text.size() == 2 && ss->blank_texture.size() == 2 && ss->raw_value("group") == "Tactical_Selection");
    CHECK(t.find("missing") == -1);

    // Real data: ALAMO_FOC_DATA with Data\XML\CommandBarComponent*.xml.
    if (const char* root = std::getenv("ALAMO_FOC_DATA")) {
        std::string err;
#ifdef ALAMO_UI_HAVE_VFS
        vfs::VfsConfig cfg;  // the XML lives in config.meg in a retail install
        cfg.foc_root = root;
        auto vfs_files = vfs::Vfs::create(cfg, &err);
        if (!vfs_files) return;
        const FileProvider& files = *vfs_files;
#else
        DirectoryProvider files(root);
#endif
        ComponentTable real;
        if (real.load(files, &err)) {
            uint32_t counts[6] = {};
            for (const CommandBarComponent& c : real.items()) ++counts[static_cast<size_t>(c.type)];
            std::printf("real CommandBarComponents: %zu components (Shell %u, Button %u, TextButton %u, Bar %u, Icon %u)\n",
                        real.size(), counts[1], counts[2], counts[3], counts[4], counts[5]);
            CHECK(real.size() >= 800 && counts[1] >= 18 && counts[4] >= 80);
            CHECK(real.get("st_bracket_large") && real.get("generic_flytext") && real.get("s_select_23"));  // 00..23 in FoC
            // P2 data check (2026-10-29, docs/re/UI.md §1.4 / §6): what the shipped files settle.
            // (1) Layer identity: Base_Layer spans exactly 0..10 and the tooltips (drawn on HUD layer 10) carry 10.
            int32_t max_layer = -1;
            for (const CommandBarComponent& c : real.items()) max_layer = std::max(max_layer, c.base_layer);
            CHECK(max_layer == 10);
            for (const char* n : {"tooltip_back", "tooltip_name", "tooltip_icon"}) {
                const CommandBarComponent* c = real.get(n);
                CHECK(c && c->base_layer == 10);
            }
            for (const char* n : {"st_bracket_small", "st_health", "st_shields_large", "st_power", "st_control_group"}) {
                const CommandBarComponent* c = real.get(n);
                CHECK(c && c->base_layer == 0);
            }
            CHECK(real.get("generic_flytext") && real.get("generic_flytext")->base_layer == 1);
            // (2) Bar geometry comes from the atlas: frame and every fill level share one size per class (34 / 68 / 102 x 2
            // canvas units); GameConstants Health_Bar_Scale and friends have no reader (STRATEGIC_GAPS §7.2).
            // (3) The bracket art (i_icon_select_*) is in no atlas: vanilla has nothing to draw for st_bracket_*.
            TextureAtlas real_atlas;
            std::vector<uint8_t> mtd;
            if (files.read("Data\\Art\\Textures\\MT_CommandBar.mtd", mtd) && real_atlas.parse(mtd, &err)) {
                const struct {
                    const char* comp;
                    uint32_t width;
                } bars[] = {{"st_health", 34}, {"st_health_medium", 68}, {"st_health_large", 102},
                            {"st_shields", 34}, {"st_shields_medium", 68}, {"st_shields_large", 102}};
                for (const auto& bar : bars) {
                    const CommandBarComponent* c = real.get(bar.comp);
                    CHECK(c && c->bar_texture.size() == 11 && c->bar_overlay.size() == 11 && c->max_bar_level == 10 && c->smooth_bar);
                    if (!c || c->bar_texture.empty() || c->bar_overlay.empty()) continue;
                    CHECK(near(c->scale, 1.0f));
                    for (const std::string& tex : c->bar_texture) {
                        const AtlasEntry* e = real_atlas.find(tex);
                        CHECK(e && e->width == bar.width && e->height == 2);
                    }
                    for (const std::string& tex : c->bar_overlay) {
                        const AtlasEntry* e = real_atlas.find(tex);
                        CHECK(e && e->width == bar.width && e->height == 2);
                    }
                }
                for (const char* n : {"st_bracket_small", "st_bracket_medium", "st_bracket_large"}) {
                    const CommandBarComponent* c = real.get(n);
                    CHECK(c && c->icon_alternate_texture.size() == 2);
                    if (!c) continue;
                    for (const std::string& tex : c->icon_alternate_texture) CHECK(real_atlas.find(tex) == nullptr);
                }
#ifdef ALAMO_UI_HAVE_ABILITIES
                // QA D5: every ability type's button icon exists (a missing one leaves the lavender blank of special_button_NN).
                for (uint32_t ty = 1; ty <= 75; ++ty) {
                    const std::string icon = ability_icon(static_cast<uint8_t>(ty));
                    if (!icon.empty() && !real_atlas.find(icon)) std::printf("ability %u: %s not in the atlas\n", ty, icon.c_str());
                    CHECK(icon.empty() || real_atlas.find(icon));
                }
#endif
            } else {
                std::printf("real data: MT_CommandBar.mtd: %s\n", err.c_str());
            }
        } else {
            std::printf("real data: %s\n", err.c_str());
        }
    }
}

void test_atlas_text() {
    std::vector<AtlasEntry> es = {{"i_bar_control_wide", 1, 2, 3, 4, true}, {"I_Radar_Map_Grid.TGA", 10, 20, 30, 40, false}};
    TextureAtlas a;
    CHECK(a.parse(make_mtd(es)));
    CHECK(a.size() == 2);
    const AtlasEntry* e = a.find("I_BAR_CONTROL_WIDE.tga");
    CHECK(e && e->x == 1 && e->height == 4 && e->alpha);
    e = a.find("Data\\Art\\Textures\\i_radar_map_grid.dds");
    CHECK(e && e->width == 30 && !e->alpha);
    std::vector<uint8_t> trunc = make_mtd(es);
    trunc.resize(trunc.size() - 1);
    TextureAtlas b;
    CHECK(!b.parse(trunc) && b.size() == 0);

    const std::vector<std::pair<std::string, std::u16string>> kv = {{"TEXT_BUTTON_BEGIN", u"Begin"},
                                                                   {"TEXT_TOOLTIP_GALACTIC_RESERVE", u"Credits \u00e9"}};
    TextDatabase d;
    CHECK(d.parse(make_dat(kv)) && d.size() == 2);
    CHECK(d.find("TEXT_BUTTON_BEGIN") && *d.find("TEXT_BUTTON_BEGIN") == u"Begin");
    // q6 t14 (GALACTIC_DIALOGS §2.7): case-sensitive CRC-32 lookup; a missing id shows as itself and is remembered (the
    // runtime-addition map; find() keeps reporting the file's table only); [MISSING] for an add = 0 lookup, a null key or a
    // table that never loaded.
    CHECK(!d.find("text_button_begin") && !d.find("TEXT_NOPE"));
    CHECK(d.text("text_button_begin") == u"text_button_begin" && d.text("TEXT_BUTTON_BEGIN") == u"Begin");
    CHECK(d.added() == 1 && d.text("TEXT_NOPE") == u"TEXT_NOPE" && d.added() == 2 && d.text("TEXT_NOPE") == u"TEXT_NOPE" && d.added() == 2);
    CHECK(d.text("TEXT_OTHER", false) == u"[MISSING]" && d.added() == 2 && d.text("TEXT_NOPE", false) == u"TEXT_NOPE");
    CHECK(d.text(std::string_view()) == u"[MISSING]" && d.text("x\xc3\xa9") == u"xé");  // UTF-8 -> UTF-16
    CHECK(TextDatabase{}.text("TEXT_BUTTON_BEGIN") == u"[MISSING]" && !TextDatabase{}.loaded() && d.loaded());
    TextDatabase esc;  // "\n" stays two characters in the table (the wrapped-text splitter breaks the line)
    esc.add("TEXT_ESC", u"a\\nb");
    CHECK(*esc.find("TEXT_ESC") == u"a\\nb" && esc.text("TEXT_ESC") == u"a\\nb");
    std::vector<uint8_t> bad = make_dat(kv);
    bad.resize(bad.size() - 3);
    TextDatabase e2;
    CHECK(!e2.parse(bad) && e2.size() == 0);
    // Language fallback to English.
    MemProvider files;
    files.put("Data\\Text\\MasterTextFile_ENGLISH.DAT", make_dat(kv));
    TextDatabase g;
    std::string log;
    CHECK(load_master_text(files, "german", g, &log) && g.language == "ENGLISH" && log.find("GERMAN") != std::string::npos);
    files.put("Data\\Text\\MasterTextFile_GERMAN.DAT", make_dat({{"TEXT_BUTTON_BEGIN", u"Beginnen"}}));
    TextDatabase g2;
    CHECK(load_master_text(files, "German", g2) && g2.language == "GERMAN" && g2.text("TEXT_BUTTON_BEGIN") == u"Beginnen");
    CHECK(utf16_to_utf32(u"a\U0001F600") == U"a\U0001F600" && utf8_to_utf32("x\xc3\xa9") == U"x\u00e9");
}

// [port] pseudo-localization (ALAMO_PSEUDO_LOC, off unless asked for).
void test_pseudo_localize() {
    // 18 units -> 6 more (brackets + 4 doubled letters, spread over the letters); the space stays, so the text can wrap.
    const std::u16string c = pseudo_localize(u"Corellian Corvette");
    CHECK(c.size() == 24 && c.front() == u'[' && c.back() == u']');
    CHECK(c.find(u' ') != std::u16string::npos && c.find(u'\u00c7') == 1);
    CHECK(pseudo_localize(u"Corellian Corvette") == c);
    // 30-40 % from 10 units on, whatever the mix of letters and other characters.
    for (size_t n = 10; n < 400; ++n) {
        std::u16string s;
        for (size_t i = 0; i < n; ++i) s.push_back(i % 5 == 4 ? u' ' : (i % 7 == 3 ? u'7' : u'x'));
        const size_t out = pseudo_localize(s).size();
        CHECK(out * 10 >= n * 13 && out * 10 <= n * 14);
    }
    CHECK(pseudo_localize(u"OK").size() == 4);  // short strings: at least the brackets
    // Inserts and escapes survive unchanged and in order; "%" followed by text is no spec.
    const std::u16string f = pseudo_localize(u"Salvage: $ %d of %.0f%% by %ls\\n##1 at ##2 %+d");
    for (std::u16string_view t : {u"%d", u"%.0f", u"%%", u"%ls", u"\\n", u"##1", u"##2", u"%+d"}) CHECK(f.find(t) != std::u16string::npos);
    CHECK(f.find(u"%d") < f.find(u"%.0f") && f.find(u"##1") < f.find(u"##2"));
    const std::u16string p = pseudo_localize(u"reduced by 50% due to ash");
    CHECK(p.find(u"% d") != std::u16string::npos && p.find(u"due") == std::u16string::npos);
    // Nothing to accent: as written (empty, blanks, numbers, a bare insert).
    for (std::u16string_view t : {u"", u"   ", u"0", u"____", u"%d", u"##1"}) CHECK(pseudo_localize(t) == t);
    // The whole table, seen through the lookups; a missing key still shows the key.
    TextDatabase d;
    d.add("TEXT_A", u"Good against");
    d.pseudo_localize_all();
    CHECK(d.find("TEXT_A") && *d.find("TEXT_A") == pseudo_localize(u"Good against") && d.text("TEXT_NOPE") == u"TEXT_NOPE");
    // t47: strings the port adds follow the table.
    TextDatabase plain;
    CHECK(plain.port_text(U"V-Sync Mode") == U"V-Sync Mode");
    CHECK(d.port_text(U"V-Sync Mode") == utf16_to_utf32(pseudo_localize(u"V-Sync Mode")) && d.port_text(U"60") == U"60");
    CHECK(fit_width_scale(100.0f, 110.0f) == 1.0f && fit_width_scale(100.0f, 75.0f) == 0.75f && fit_width_scale(100.0f, 10.0f) == 0.5f);
}

void test_layout() {
    const ComponentTable t = hud_test::load_table();
    const auto shell = static_cast<uint32_t>(t.find("i_main_skirmish"));
    HudLayout l;
    // Shell model space (stage 2, verified on i_tactical_controls.alo): origin = the anchored corner, bones extend
    // right / up (radar at (102, 98)) -> origin on the bottom-left canvas corner at every resolution.
    l.set_shell_bones(shell, {{"radar", 102, 98, 0}, {"s_select_00", 380, 93, 4}, {"st_health", 0, 0, 0}});
    for (auto [w, h] : {std::pair{1024u, 768u}, std::pair{1920u, 1080u}, std::pair{1280u, 1024u}}) {
        const HudCanvas cv = HudCanvas::make(w, h);
        l.update(cv, t);
        const Vec2f off = l.shell_offset(shell);
        CHECK(near(off.x, -0.5f * cv.width) && near(off.y, -0.5f * cv.height));
        const ComponentPlacement& r = l.placement(static_cast<uint32_t>(t.find("radar")));
        CHECK(r.placed && r.from_bone && r.shell == static_cast<int32_t>(shell) && near(r.center.x, 102 + off.x) &&
              near(r.center.y, 98 + off.y));
        // No_Shell components never take a bone.
        CHECK(!l.placement(static_cast<uint32_t>(t.find("st_health"))).placed);
        // The radar keeps its distance to the left / bottom canvas edge at every resolution.
        CHECK(near(r.center.x - (-0.5f * cv.width), 102.0f, 1e-2f) && near(r.center.y - (-0.5f * cv.height), 98.0f, 1e-2f));
        CHECK(near(l.placement(static_cast<uint32_t>(t.find("s_select_00"))).z, 4.0f));
    }
    // Bones extending left / down (mean < -1) anchor to the right / top edge; an inactive shell places nothing.
    l.set_shell_bones(shell, {{"radar", -102, -98, 0}});
    l.update(HudCanvas::make(1920, 1080), t);
    CHECK(near(l.shell_offset(shell).x, 0.5f * HudCanvas::make(1920, 1080).width) && near(l.shell_offset(shell).y, 384.0f));
    l.set_shell_active(shell, false);
    l.update(HudCanvas::make(1920, 1080), t);
    CHECK(!l.placement(static_cast<uint32_t>(t.find("radar"))).placed && !l.shell_active(shell) && l.shells().size() == 1);
    // Fallback positions when no bone exists.
    HudLayout f;
    const HudCanvas cv = HudCanvas::make(1920, 1080);
    apply_tactical_fallback_layout(t, cv, f);
    f.update(cv, t);
    const ComponentPlacement& p = f.placement(static_cast<uint32_t>(t.find("s_select_00")));
    CHECK(p.placed && !p.from_bone);
    CHECK(f.placement(static_cast<uint32_t>(t.find("radar"))).placed);
}

void test_draw_list() {
    HudQuad q;
    q.min = {0, 0};
    q.max = {10, 10};
    HudVertex v[11];
    const float fills[] = {0.0f, 5.0f, 12.5f, 13.0f, 25.0f, 50.0f, 99.0f, 100.0f};
    const uint32_t want[] = {0, 3, 3, 4, 4, 6, 10, 4};
    for (size_t i = 0; i < 8; ++i) CHECK(radial_wipe(q, fills[i], v) == want[i]);
    // 50 %: fan centre, top-centre start, corners/midpoints, ends at bottom-centre.
    CHECK(radial_wipe(q, 50.0f, v) == 6 && near(v[0].x, 5) && near(v[0].y, 5) && near(v[1].x, 5) && near(v[1].y, 10) &&
          near(v[5].x, 5) && near(v[5].y, 0));
    // Batching by page, order kept; the list gives the effect (STRATEGIC_GAPS §7.2).
    HudDrawList dl;
    HudQuad a = q, b = q, c = q;
    b.page = kHudPageAtlas;
    c.page = kHudPageAtlas;
    dl.add(3, HudList::Opaque, a);
    dl.add(3, HudList::Opaque, b);
    dl.add(3, HudList::Opaque, b);
    dl.add(3, HudList::Additive, c);
    HudQuad d = q;
    d.fill = 25.0f;
    dl.add(3, HudList::Alpha, d);
    dl.add(99, HudList::Text, a);  // clamped to layer 12
    CHECK(dl.total() == 6 && dl.items(12, HudList::Text).size() == 1);
    std::vector<HudVertex> vs;
    std::vector<uint32_t> is;
    std::vector<HudDrawList::Batch> bs;
    dl.build_geometry(3, HudList::Opaque, vs, is, bs);
    CHECK(vs.size() == 12 && is.size() == 18 && bs.size() == 2 && bs[1].index_count == 12 && bs[0].effect == HudEffect::PrimOpaque);
    dl.build_geometry(3, HudList::Additive, vs, is, bs);
    CHECK(bs.size() == 3 && bs[2].effect == HudEffect::PrimAdditive && bs[2].page == kHudPageAtlas);  // same page, new effect
    dl.build_geometry(3, HudList::Alpha, vs, is, bs);
    CHECK(vs.size() == 16 + 4 && is.size() == 24 + 6 && bs.back().effect == HudEffect::PrimAlpha);  // 25 %: 4 vertices
    // QA D2: a layer never merges into the previous layer's batch (a draw step such as the shell models may run between them).
    {
        HudDrawList ld;
        HudQuad lq = q;
        lq.page = kHudPageAtlas;
        ld.add(1, HudList::Alpha, lq);
        ld.add(1, HudList::Alpha, lq);
        ld.add(2, HudList::Alpha, lq);
        ld.add(2, HudList::Text, lq);  // same effect and page: merges inside its own layer
        ld.add(3, HudList::Alpha, lq);
        std::vector<HudVertex> lv;
        std::vector<uint32_t> li;
        std::vector<HudDrawList::Batch> lb;
        HudDrawList::LayerBatchRanges lr{};
        ld.build_layers(lv, li, lb, &lr);
        CHECK(lb.size() == 3 && lb[0].index_count == 12 && lb[1].index_count == 12 && lb[2].index_count == 6);
        CHECK(lr[0].first == lr[0].second && lr[1].first == 0 && lr[1].second == 1 && lr[2].first == 1 && lr[2].second == 2 &&
              lr[3].first == 2 && lr[3].second == 3);
        ld.build_layers(lv, li, lb);  // a second list (dialogs, cursor) never joins the first one's last batch
        CHECK(lb.size() == 6);
    }
    // The selector table (0..13) and the list -> effect map.
    CHECK(hud_list_effect(HudList::Quads) == HudEffect::PrimOpaque && hud_list_effect(HudList::Dial) == HudEffect::PrimAlpha &&
          hud_list_effect(HudList::DialAlt) == HudEffect::PrimAdditive && hud_list_effect(HudList::Extra) == HudEffect::PrimModulate &&
          hud_list_effect(HudList::Text) == HudEffect::PrimAlpha && hud_list_effect(HudList::ModelText) == HudEffect::PrimAlpha);
    CHECK(std::string_view(hud_effect_name(HudEffect::PrimOpaque)) == "Engine\\PrimOpaque" &&
          std::string_view(hud_effect_name(HudEffect::PrimAlpha)) == "Engine\\PrimAlpha" &&
          std::string_view(hud_effect_name(HudEffect::PrimAlphaScanlines)) == "Engine\\PrimAlphaScanlines" &&
          std::string_view(hud_effect_name(static_cast<HudEffect>(kHudEffectCount))).empty());
    // Movie quads: alpha movies on PrimAlpha (straight alpha), others PrimOpaque (STRATEGIC_GAPS §7.3).
    HudDrawList mv;
    add_movie_quad(mv, 0, {0, 0}, {200, 200}, 20, true);
    add_movie_quad(mv, 0, {0, 0}, {800, 600}, 21, false);
    CHECK(mv.items(0, HudList::Alpha).size() == 1 && mv.items(0, HudList::Opaque).size() == 1 &&
          mv.items(0, HudList::Alpha)[0].page == 20 && mv.items(0, HudList::Opaque)[0].color == (Rgba8{255, 255, 255, 255}));
    // Frame order: every layer exactly once, ascending, shells / dialogs / cursor in between.
    int next = 0;
    size_t shells = 0;
    for (const HudFrameStep& s : hud_frame_steps()) {
        if (s.kind == HudStepKind::Layers) {
            CHECK(s.first == next);
            next = s.last + 1;
        }
        if (s.kind == HudStepKind::ShellModels) ++shells;
    }
    CHECK(next == 13 && shells == 2 && hud_frame_steps().back().kind == HudStepKind::Cursor);
    CHECK(hud_frame_steps()[1].kind == HudStepKind::ShellModels && hud_frame_steps()[10].kind == HudStepKind::Dialogs);
}

void test_text() {
    auto box = make_box_rasterizer();
    FontCache fonts(*box, 128);
    const HudCanvas cv = HudCanvas::make(1920, 1080);
    const int32_t f = fonts.font("EmpireAtWar-Medium", 9, cv.screen_height);
    CHECK(f >= 0 && fonts.metrics(f).pixel_height == 22 && fonts.font("empireatwar-medium", 9, 1080) == f);
    CHECK(fonts.font("EmpireAtWar-Medium", 9, 768) != f);
    CHECK(face_is_bold("EmpireAtWar-Bold") && !face_is_bold("Arial"));
    HudDrawList dl;
    HudTextStyle st;
    st.font = f;
    st.align = TextAlign::Center;
    const float w = add_text(dl, 1, HudList::Text, fonts, cv, U"12 3", {0, 0}, st);
    // 4 advances of round(22 * 0.6) = 13 px -> 52 px; 3 visible glyphs.
    CHECK(near(w, 52.0f * cv.units_per_pixel_x) && dl.items(1, HudList::Text).size() == 3);
    const HudQuad& g0 = dl.items(1, HudList::Text)[0];
    const Vec2f px = cv.canvas_to_pixel(g0.min.x, g0.max.y);
    CHECK(near(px.x, std::round(px.x), 1e-2f) && near(px.y, std::round(px.y), 1e-2f));  // snapped
    CHECK(g0.page >= kHudPageFontBase && g0.uv1.x > g0.uv0.x);
    // Outline adds four copies, emboss one.
    HudDrawList dl2;
    st.outline = st.emboss = true;
    add_text(dl2, 0, HudList::Text, fonts, cv, U"7", {0, 0}, st);
    CHECK(dl2.items(0, HudList::Text).size() == 6);
    CHECK(fonts.page_count() == 1 && fonts.measure(f, U"") == 0);
    // The system rasteriser opens a face (GDI on Windows; substituted when EmpireAtWar is not installed).
    auto sys = make_system_rasterizer();
    FontCache sf(*sys);
    const int32_t sfid = sf.font("EmpireAtWar-Medium", 9, 768);
    CHECK(sfid >= 0);
    if (sfid >= 0) {
        CHECK(sf.metrics(sfid).pixel_height == 15 && sf.measure(sfid, U"88") > 0);
        std::printf("system rasteriser %s: face '%s', %d px, ascent %d\n", sys->name(), sf.metrics(sfid).actual_face.c_str(),
                    sf.metrics(sfid).pixel_height, sf.metrics(sfid).ascent);
    }
}

// ---- game fonts (find_truetype_fonts / register_game_fonts) ----

void put_be16(std::vector<uint8_t>& v, size_t at, uint32_t x) {
    v[at] = static_cast<uint8_t>(x >> 8);
    v[at + 1] = static_cast<uint8_t>(x);
}
void put_be32(std::vector<uint8_t>& v, size_t at, uint32_t x) {
    put_be16(v, at, x >> 16);
    put_be16(v, at + 2, x & 0xFFFFu);
}

// Minimal sfnt: cmap / glyf / head / loca / name (family + full name, Windows Unicode English).
std::vector<uint8_t> make_test_sfnt(const std::u16string& family, const std::u16string& full) {
    std::vector<uint8_t> name(6 + 24);
    put_be16(name, 2, 2);
    put_be16(name, 4, 30);
    const std::u16string* strs[2] = {&family, &full};
    const uint32_t ids[2] = {1, 4};
    uint32_t off = 0;
    for (int i = 0; i < 2; ++i) {
        const size_t r = 6 + static_cast<size_t>(i) * 12;
        put_be16(name, r, 3);
        put_be16(name, r + 2, 1);
        put_be16(name, r + 4, 0x409);
        put_be16(name, r + 6, ids[i]);
        put_be16(name, r + 8, static_cast<uint32_t>(strs[i]->size() * 2));
        put_be16(name, r + 10, off);
        off += static_cast<uint32_t>(strs[i]->size() * 2);
    }
    for (int i = 0; i < 2; ++i)
        for (char16_t c : *strs[i]) {
            name.push_back(static_cast<uint8_t>(c >> 8));
            name.push_back(static_cast<uint8_t>(c));
        }
    std::vector<uint8_t> head(54, 0);
    put_be32(head, 12, 0x5F0F3CF5u);
    const char* tags[5] = {"cmap", "glyf", "head", "loca", "name"};
    const std::vector<uint8_t> four(4, 0);
    const std::vector<uint8_t>* data[5] = {&four, &four, &head, &four, &name};
    std::vector<uint8_t> f(12 + 5 * 16, 0);
    put_be32(f, 0, 0x00010000u);
    put_be16(f, 4, 5);
    put_be16(f, 6, 64);
    put_be16(f, 8, 2);
    put_be16(f, 10, 5 * 16 - 64);
    for (int i = 0; i < 5; ++i) {
        const size_t r = 12 + static_cast<size_t>(i) * 16;
        std::memcpy(f.data() + r, tags[i], 4);
        put_be32(f, r + 8, static_cast<uint32_t>(f.size()));
        put_be32(f, r + 12, static_cast<uint32_t>(data[i]->size()));
        f.insert(f.end(), data[i]->begin(), data[i]->end());
        while (f.size() % 4) f.push_back(0);
    }
    return f;
}

// Uncompressed (stored-deflate) RGBA PNG; test output only.
bool write_test_png(const std::string& path, uint32_t w, uint32_t h, const std::vector<uint8_t>& rgba) {
    std::vector<uint8_t> raw;
    for (uint32_t y = 0; y < h; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rgba.begin() + static_cast<std::ptrdiff_t>(y) * w * 4,
                   rgba.begin() + static_cast<std::ptrdiff_t>(y + 1) * w * 4);
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    for (size_t i = 0; i < raw.size(); i += 65535) {
        const size_t n = std::min<size_t>(65535, raw.size() - i);
        z.push_back(static_cast<uint8_t>(i + n == raw.size() ? 1 : 0));
        z.push_back(static_cast<uint8_t>(n));
        z.push_back(static_cast<uint8_t>(n >> 8));
        z.push_back(static_cast<uint8_t>(~n));
        z.push_back(static_cast<uint8_t>(~n >> 8));
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(i), raw.begin() + static_cast<std::ptrdiff_t>(i + n));
    }
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) {
        a = (a + c) % 65521u;
        b = (b + a) % 65521u;
    }
    for (int s = 24; s >= 0; s -= 8) z.push_back(static_cast<uint8_t>(((b << 16) | a) >> s));
    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    const auto chunk = [&out](const char* type, const std::vector<uint8_t>& d) {
        std::vector<uint8_t> c(8, 0);
        put_be32(c, 0, static_cast<uint32_t>(d.size()));
        std::memcpy(c.data() + 4, type, 4);
        c.insert(c.end(), d.begin(), d.end());
        const uint32_t crc = alamo::crc32(c.data() + 4, c.size() - 4);
        c.resize(c.size() + 4);
        put_be32(c, c.size() - 4, crc);
        out.insert(out.end(), c.begin(), c.end());
    };
    std::vector<uint8_t> ihdr(13, 0);
    put_be32(ihdr, 0, w);
    put_be32(ihdr, 4, h);
    ihdr[8] = 8;
    ihdr[9] = 6;
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) return false;
    const bool ok = std::fwrite(out.data(), 1, out.size(), fp) == out.size();
    std::fclose(fp);
    return ok;
}

// Draws `lines` (face, point size, text) with the system rasteriser at 768 lines into a dark RGBA image.
void render_font_sample(const std::string& path, const std::vector<std::tuple<std::string, float, std::u32string>>& lines) {
    auto sys = make_system_rasterizer();
    FontCache fc(*sys);
    const uint32_t W = 560, H = 24 * static_cast<uint32_t>(lines.size()) + 8;
    std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4, 0);
    for (size_t i = 3; i < px.size(); i += 4) px[i - 3] = 16, px[i - 2] = 20, px[i - 1] = 28, px[i] = 255;
    int32_t baseline = 22;
    for (const auto& [face, pt, text] : lines) {
        const int32_t f = fc.font(face, pt, 768);
        int32_t pen = 8;
        for (char32_t c : text) {
            const FontCache::Glyph* g = fc.glyph(f, c);
            if (!g) continue;
            const std::vector<uint8_t>& page = fc.page_rgba(g->page);
            for (uint32_t y = 0; y < g->height; ++y)
                for (uint32_t x = 0; x < g->width; ++x) {
                    const int32_t dx = pen + g->origin_x + static_cast<int32_t>(x);
                    const int32_t dy = baseline - g->origin_y + static_cast<int32_t>(y);
                    if (dx < 0 || dy < 0 || dx >= static_cast<int32_t>(W) || dy >= static_cast<int32_t>(H)) continue;
                    const uint32_t cov = page[((static_cast<size_t>(g->y + y) * fc.page_size()) + g->x + x) * 4 + 3];
                    uint8_t* d = &px[(static_cast<size_t>(dy) * W + static_cast<size_t>(dx)) * 4];
                    const uint32_t col[3] = {232, 196, 64};  // menu gold
                    for (int k = 0; k < 3; ++k) d[k] = static_cast<uint8_t>((col[k] * cov + d[k] * (255u - cov)) / 255u);
                }
            pen += g->advance;
        }
        baseline += 24;
    }
    CHECK(write_test_png(path, W, H, px));
    std::printf("font sample: %s\n", path.c_str());
}

void test_game_fonts() {
    // Embedded sfnt blobs are found between arbitrary bytes; a bogus 00 01 00 00 header is rejected.
    const std::vector<uint8_t> font = make_test_sfnt(u"Test Family Bold", u"TestFamily-Bold");
    std::vector<uint8_t> blob(37, 0xCC);
    blob.insert(blob.end(), {0, 1, 0, 0, 0, 5, 0, 64, 0, 2, 0, 16});  // header only, no table directory
    blob.insert(blob.end(), 64, 0x11);
    const size_t at = blob.size();
    blob.insert(blob.end(), font.begin(), font.end());
    blob.insert(blob.end(), 29, 0xEE);
    const std::vector<TrueTypeFontInfo> found = find_truetype_fonts(blob.data(), blob.size());
    CHECK(found.size() == 1);
    if (found.size() == 1) {
        CHECK(found[0].offset == at && found[0].size == font.size());
        CHECK(found[0].family == "Test Family Bold" && found[0].full_name == "TestFamily-Bold" && found[0].weight == 400);
    }
    CHECK(find_truetype_fonts(font.data(), font.size() - 8).empty());  // truncated table -> rejected
    CHECK(!find_game_font("TestFamily-Bold"));

    const char* foc = std::getenv("ALAMO_FOC_DATA");
    if (!foc) {
        std::printf("game fonts: skipped (ALAMO_FOC_DATA not set)\n");
        return;
    }
#ifdef _WIN32
    const std::string logs = std::string(ALAMO_UI_SOURCE_DIR) + "/logs/";
    const std::vector<std::tuple<std::string, float, std::u32string>> sample = {
        {"EmpireAtWar-Bold", 10.0f, U"Main Menu  Tutorial  Single Player"},
        {"EmpireAtWar-Bold", 8.0f, U"Load Game  Options  Exit Game"},
        {"EmpireAtWar-Medium", 8.0f, U"$ 13,000  $ 668  $ 420  Kuat  Coruscant"},  // '$' = credits sign
        {"EmpireAtWar-light", 8.0f, U"EmpireAtWar-light 0123456789"},
        {"EmpireAtWar-Medium", 9.0f, U"$ ¢ £ ¤ ¥ € ƒ ∙ © ® ° §"},
    };
    // Before: nothing registered -> Windows substitutes the EmpireAtWar faces (unless they are installed system-wide).
    render_font_sample(logs + "ui_font_before.png", sample);
    const char* eaw = std::getenv("ALAMO_EAW_DATA");
    const std::vector<GameFont> added = register_game_fonts({foc, eaw ? eaw : ""});
    for (const GameFont& g : added) std::printf("game font %s (%s, %u) <- %s\n", g.full_name.c_str(), g.family.c_str(), static_cast<unsigned>(g.weight),
                                               g.source.c_str());
    for (const char* face : {"EmpireAtWar-Medium", "EmpireAtWar-Bold", "EmpireAtWar-light", "empireatwar-stencil"}) {
        GameFont g;
        CHECK(find_game_font(face, &g));
        CHECK(g.family.rfind("Empire At War", 0) == 0);
    }
    CHECK(register_game_fonts({foc}).empty());  // idempotent
    CHECK(registered_game_fonts().size() >= 4);
    // The GDI rasteriser now opens the real faces: no substitution, Bold at its own weight.
    auto sys = make_system_rasterizer();
    FontCache fc(*sys);
    for (const char* face : {"EmpireAtWar-Medium", "EmpireAtWar-Bold", "EmpireAtWar-light"}) {
        const int32_t f = fc.font(face, 8.0f, 768);
        CHECK(f >= 0);
        if (f < 0) continue;
        std::printf("game font '%s' -> actual '%s', %d px\n", face, fc.metrics(f).actual_face.c_str(), fc.metrics(f).pixel_height);
        CHECK(fc.metrics(f).actual_face == face && fc.measure(f, U"Main Menu") > 0);
    }
    CHECK(fc.substitutions() == 0);
    // The faces differ from Arial (small caps: lower-case 'a' is a capital-height glyph).
    const int32_t eaw_f = fc.font("EmpireAtWar-Bold", 20.0f, 768), arial = fc.font("Arial", 20.0f, 768);
    const FontCache::Glyph* ga = fc.glyph(eaw_f, U'a');
    const FontCache::Glyph* gA = fc.glyph(eaw_f, U'A');
    const FontCache::Glyph* ra = fc.glyph(arial, U'a');
    const FontCache::Glyph* rA = fc.glyph(arial, U'A');
    CHECK(ga && gA && ra && rA);
    if (ga && gA && ra && rA) {
        std::printf("glyph heights: EaW a=%u A=%u, Arial a=%u A=%u\n", ga->height, gA->height, ra->height, rA->height);
        CHECK(ga->height * rA->height > ra->height * gA->height);  // small caps: taller lower case than Arial's
    }
    render_font_sample(logs + "ui_font_after.png", sample);
#endif
}

void test_world() {
    const ComponentTable t = hud_test::load_table();
    const TextureAtlas atlas = hud_test::make_atlas();
    auto box = make_box_rasterizer();
    FontCache fonts(*box);
    std::vector<HudTypeInfo> types(3);
    types[0].gui_bracket_size = 0;  // small
    types[1].gui_bracket_size = 2;  // large
    types[1].select_box_scale = 30.0f;
    types[1].scale = 2.0f;
    types[2].gui_bracket_size = -1;
    types[2].space_class = BracketSize::Large;
    types[2].land_class = BracketSize::Small;
    CHECK(bracket_size(types[0], false) == BracketSize::Small && bracket_size(types[1], true) == BracketSize::Large &&
          bracket_size(types[2], false) == BracketSize::Large && bracket_size(types[2], true) == BracketSize::Small);
    // Default class for GUI_Bracket_Size -1 (P2 data check): the category sets that the shipped files leave at -1.
    {
        using S = std::vector<std::string_view>;
        auto cls = [](const S& cats) { return derive_bracket_classes(cats); };
        // Space: fighters / bombers small; corvettes, frigates medium; capitals, supers, starbases, structures large.
        CHECK(cls(S{"AntiBomber", "Fighter"}).space == BracketSize::Small && cls(S{"AntiCapital", "Bomber"}).space == BracketSize::Small);
        CHECK(cls(S{"Fighter", "SpaceHero"}).space == BracketSize::Small && cls(S{"fighter"}).space == BracketSize::Small);
        CHECK(cls(S{"AntiBomber", "AntiFighter", "Corvette"}).space == BracketSize::Medium);
        CHECK(cls(S{"AntiCorvette", "Frigate"}).space == BracketSize::Medium);
        CHECK(cls(S{"AntiFrigate", "Capital"}).space == BracketSize::Large && cls(S{"Capital"}).space == BracketSize::Large);
        CHECK(cls(S{"Super"}).space == BracketSize::Large && cls(S{" Structure "}).space == BracketSize::Large);
        // Land: infantry small; vehicles and air medium; structures large.
        CHECK(cls(S{"AntiVehicle", "Infantry"}).land == BracketSize::Small && cls(S{"Wall"}).land == BracketSize::Medium);
        CHECK(cls(S{"AntiInfantry", "Vehicle"}).land == BracketSize::Medium && cls(S{"Air"}).land == BracketSize::Medium);
        CHECK(cls(S{"LandHero"}).land == BracketSize::Medium && cls(S{}).land == BracketSize::Medium);
        CHECK(cls(S{"AntiInfantry", "Structure"}).land == BracketSize::Large);
        // The names the earlier rule looked for (SpaceStructure / SuperWeapon) are not GameObjectCategoryType names.
        CHECK(cls(S{"SuperWeapon"}).space == BracketSize::Medium && cls(S{"SpaceStructure"}).space == BracketSize::Medium);
    }
    WorldHud wh;
    wh.bind(t);
    for (auto [w, h] : {std::pair{1024u, 768u}, std::pair{1920u, 1080u}, std::pair{1280u, 1024u}}) {
        const HudCanvas cv = HudCanvas::make(w, h);
        WorldHudContext ctx;
        ctx.canvas = &cv;
        ctx.view = top_view(w, h);
        HudUnit u;
        u.id = 7;
        u.type = 0;
        u.owner = 0;
        u.position = {100, 50, 0};
        u.radius = 20.0f;
        u.health = 0.5f;
        u.selected = true;
        u.control_group = 3;
        HudDrawList dl;
        std::vector<SelectionBlob> blobs;
        wh.build(ctx, std::span<const HudUnit>(&u, 1), types, atlas, fonts, dl, &blobs);
        CHECK(wh.stats().brackets == 1 && wh.stats().bars == 1 && wh.stats().group_numbers == 1 && blobs.empty());
        {  // R5 stealth (ABILITY_VISUALS 1.4): a hostile stealthed unit shows no health bars; an own stealthed one keeps them
            HudUnit hs = u;
            hs.owner = 1;
            hs.stealthed = true;
            HudDrawList dl2;
            wh.build(ctx, std::span<const HudUnit>(&hs, 1), types, atlas, fonts, dl2, nullptr);
            CHECK(wh.stats().bars == 0);
            hs.stealthed = false;
            wh.build(ctx, std::span<const HudUnit>(&hs, 1), types, atlas, fonts, dl2, nullptr);
            CHECK(wh.stats().bars == 1);
            hs.owner = 0;
            hs.stealthed = true;
            wh.build(ctx, std::span<const HudUnit>(&hs, 1), types, atlas, fonts, dl2, nullptr);
            CHECK(wh.stats().bars == 1);
            HudDrawList dl3;  // restore the stats for the checks below (the first list keeps its quads)
            std::vector<SelectionBlob> blobs3;
            wh.build(ctx, std::span<const HudUnit>(&u, 1), types, atlas, fonts, dl3, &blobs3);
        }
        // Bracket pieces symmetric about the projected centre, at the left / right bound, 6 x 16 units each.
        float sx = 0, sy = 0;
        CHECK(project(ctx.view, u.position, sx, sy));
        const auto quads = dl.items(0, HudList::Alpha);
        CHECK(quads.size() == 4);  // 2 bracket pieces + bar frame + bar fill
        if (quads.size() == 4) {
            const Vec2f c = cv.pixel_to_canvas(sx, sy);
            const float l = 0.5f * (quads[0].min.x + quads[0].max.x), r = 0.5f * (quads[1].min.x + quads[1].max.x);
            CHECK(near(0.5f * (l + r), c.x, 1e-2f) && near(0.5f * (quads[0].min.y + quads[0].max.y), c.y, 1e-2f));
            CHECK(near(quads[0].max.x - quads[0].min.x, 6.0f) && near(quads[0].max.y - quads[0].min.y, 16.0f));
            CHECK(quads[0].page == kHudPageAtlas && quads[0].color == (Rgba8{0, 255, 0, 255}));
            // Expected separation: the sphere radius projected (top view: 20 world units at depth 1000).
            const UnitScreenBounds b = unit_screen_bounds(ctx.view, u, types[0]);
            CHECK(near((r - l) / cv.units_per_pixel_x, 2.0f * b.radius_px, 1e-2f));
            // Smooth health bar at 50 %: the fill is half the frame's width, left-aligned, above the bracket.
            const HudQuad& frame = quads[2];
            const HudQuad& fill = quads[3];
            CHECK(near(frame.max.x - frame.min.x, 34.0f) && near(fill.max.x - fill.min.x, 17.0f) && near(fill.min.x, frame.min.x));
            CHECK(frame.min.y > quads[0].min.y);
            CHECK(near(fill.uv1.x - fill.uv0.x, 0.5f * 34.0f / 256.0f, 1e-5f));
        }
        // Control-group digit: centred, 32 canvas units above (+Y up) the bracket centre.
        CHECK(!dl.items(0, HudList::Text).empty());
        // Constant size: farther away the pieces keep their size, the separation shrinks.
        HudUnit far_u = u;
        far_u.position = {100, 50, -3000};
        HudDrawList dl2;
        wh.build(ctx, std::span<const HudUnit>(&far_u, 1), types, atlas, fonts, dl2, nullptr);
        const auto q2 = dl2.items(0, HudList::Alpha);
        CHECK(q2.size() == 4);
        if (q2.size() == 4 && quads.size() == 4) {
            CHECK(near(q2[0].max.x - q2[0].min.x, 6.0f) && near(q2[0].max.y - q2[0].min.y, 16.0f));
            CHECK((q2[1].min.x - q2[0].min.x) < (quads[1].min.x - quads[0].min.x));
        }
        // Off-screen and behind the camera: nothing drawn, counted.
        HudUnit off = u;
        off.position = {5000, 0, 0};
        HudUnit behind = u;
        behind.position = {0, 0, 2000};
        const HudUnit two[2] = {off, behind};
        HudDrawList dl3;
        wh.build(ctx, two, types, atlas, fonts, dl3, nullptr);
        CHECK(wh.stats().brackets == 0 && wh.stats().offscreen == 2 && dl3.total() == 0);
        // Large class picks st_bracket_large; selection blob for an own selected unit with Select_Box_Scale.
        HudUnit big = u;
        big.type = 1;
        big.control_group = -1;
        big.shield = 1.0f;
        HudDrawList dl4;
        blobs.clear();
        wh.build(ctx, std::span<const HudUnit>(&big, 1), types, atlas, fonts, dl4, &blobs);
        CHECK(dl4.items(0, HudList::Alpha).size() >= 2 && near(dl4.items(0, HudList::Alpha)[0].max.y - dl4.items(0, HudList::Alpha)[0].min.y, 32.0f));
        CHECK(wh.stats().bars == 2);
        CHECK(blobs.size() == 1 && blobs[0].radius == 60.0f && blobs[0].material == "Selection_Empire_Space" && !blobs[0].land);
        ctx.land = true;
        ctx.selection_material = [](int32_t, bool land, bool) { return std::string(land ? "Selection_Rebel_Land" : "x"); };
        blobs.clear();
        wh.build(ctx, std::span<const HudUnit>(&big, 1), types, atlas, fonts, dl4, &blobs);
        CHECK(blobs.size() == 1 && blobs[0].land && blobs[0].material == "Selection_Rebel_Land");
        ctx.targeting_active = true;
        blobs.clear();
        wh.build(ctx, std::span<const HudUnit>(&big, 1), types, atlas, fonts, dl4, &blobs);
        CHECK(blobs.empty());
        // Enemy hover: red bracket, no blob.
        ctx.targeting_active = false;
        HudUnit enemy = u;
        enemy.owner = 2;
        enemy.selected = false;
        enemy.hovered = true;
        HudDrawList dl5;
        blobs.clear();
        wh.build(ctx, std::span<const HudUnit>(&enemy, 1), types, atlas, fonts, dl5, &blobs);
        CHECK(blobs.empty() && !dl5.items(0, HudList::Alpha).empty() && dl5.items(0, HudList::Alpha)[0].color.r == 255 &&
              dl5.items(0, HudList::Alpha)[0].color.g == 64);
        // Hidden interface: nothing.
        ctx.interface_hidden = true;
        HudDrawList dl6;
        wh.build(ctx, std::span<const HudUnit>(&u, 1), types, atlas, fonts, dl6, nullptr);
        CHECK(dl6.total() == 0);
    }
    // [port: Gabriel playtest] vanilla bracket textures are in no atlas: no stand-in boxes (they read as a thick "II");
    // without a host radius the bounds come from Select_Box_Scale x Scale_Factor, not 10 units.
    {
        const HudCanvas cv = HudCanvas::make(1024, 768);
        WorldHudContext ctx;
        ctx.canvas = &cv;
        ctx.view = top_view(1024, 768);
        TextureAtlas empty_atlas;
        empty_atlas.texture_width = empty_atlas.texture_height = 256;
        HudUnit u;
        u.type = 1;  // large, Select_Box_Scale 30 x Scale_Factor 2
        u.owner = 0;
        u.position = {100, 50, 0};
        u.selected = true;
        HudDrawList dl;
        wh.build(ctx, std::span<const HudUnit>(&u, 1), types, empty_atlas, fonts, dl, nullptr);
        CHECK(wh.stats().brackets == 1 && wh.stats().bars == 1);
        for (const HudQuad& q : dl.items(0, HudList::Alpha))  // only the bar (frame + fill) remains, both flat and thin
            CHECK(q.max.y - q.min.y < 10.0f);
        HudUnit with_r = u;
        with_r.radius = 60.0f;
        CHECK(near(unit_screen_bounds(ctx.view, u, types[1]).radius_px, unit_screen_bounds(ctx.view, with_r, types[1]).radius_px,
                   1e-3f));
        HudUnit plain = u;
        plain.type = 0;  // no Select_Box_Scale: 10 units
        HudUnit ten = plain;
        ten.radius = 10.0f;
        CHECK(near(unit_screen_bounds(ctx.view, plain, types[0]).radius_px, unit_screen_bounds(ctx.view, ten, types[0]).radius_px,
                   1e-3f));
        // Unit icon (host-flagged, e.g. one per squadron): drawn unselected, above the unit, gold own / blue hostile frame.
        std::vector<HudTypeInfo> icon_types = types;
        icon_types[0].icon_name = "i_button_ni_starviper.tga";
        HudUnit sq = plain;
        sq.selected = false;
        sq.unit_icon = true;
        sq.health = 0.5f;
        HudDrawList dl2;
        wh.build(ctx, std::span<const HudUnit>(&sq, 1), icon_types, empty_atlas, fonts, dl2, nullptr);
        CHECK(wh.stats().unit_icons == 1 && wh.stats().brackets == 0);
        float isx = 0, isy = 0;
        CHECK(project(ctx.view, sq.position, isx, isy));
        const Vec2f icon_at = cv.pixel_to_canvas(isx, isy);
        bool gold = false;
        for (const HudQuad& q : dl2.items(0, HudList::Alpha)) {
            CHECK(q.min.y > icon_at.y);  // everything above the unit (+Y up)
            gold = gold || q.color == ctx.unit_icon_own;
        }
        CHECK(gold);
        sq.owner = 5;
        HudDrawList dl3;
        wh.build(ctx, std::span<const HudUnit>(&sq, 1), icon_types, empty_atlas, fonts, dl3, nullptr);
        bool blue = false;
        for (const HudQuad& q : dl3.items(0, HudList::Alpha)) blue = blue || q.color == ctx.unit_icon_hostile;
        CHECK(blue);
        // QA U7: a squadron of four (a hangar launch: one participant per fighter) + a lone ship = two markers, the
        // squadron's at the centre of its members with their mean health; a unit without a link has none.
        std::vector<HudUnit> fleet(6, plain);
        const float xs[6] = {100.0f, 110.0f, 120.0f, 130.0f, 300.0f, 400.0f};
        const float hs[6] = {1.0f, 0.5f, 1.0f, 0.5f, 1.0f, 1.0f};
        for (uint32_t i = 0; i < 6; ++i) {
            fleet[i].id = 10u + i;
            fleet[i].selected = false;
            fleet[i].position.x = xs[i];
            fleet[i].health = hs[i];
            fleet[i].unit_icon = true;  // stale flag: cleared
        }
        const std::vector<UnitIconLink> links = {{10, 10}, {11, 10}, {12, 10}, {13, 10}, {14, 14}, {99, 99}};
        assign_unit_icons(fleet, links);
        CHECK(fleet[0].unit_icon && fleet[0].icon_anchored && near(fleet[0].icon_anchor.x, 115.0f, 1e-3f) &&
              near(fleet[0].icon_health, 0.75f, 1e-5f));
        CHECK(!fleet[1].unit_icon && !fleet[2].unit_icon && !fleet[3].unit_icon);
        CHECK(fleet[4].unit_icon && near(fleet[4].icon_anchor.x, 300.0f, 1e-3f));
        CHECK(!fleet[5].unit_icon);
        HudDrawList dl4;
        wh.build(ctx, fleet, icon_types, empty_atlas, fonts, dl4, nullptr);
        CHECK(wh.stats().unit_icons == 2);
        // The first member gone: the next one carries the marker, still at the centre of those left.
        fleet.erase(fleet.begin());
        assign_unit_icons(fleet, links);
        CHECK(fleet[0].id == 11u && fleet[0].unit_icon && near(fleet[0].icon_anchor.x, 120.0f, 1e-3f));
    }
    // Fly-text: projected, rising, expiring.
    const HudCanvas cv = HudCanvas::make(1024, 768);
    WorldHudContext ctx;
    ctx.canvas = &cv;
    ctx.view = top_view(1024, 768);
    FlyTextList ft;
    FlyText a;
    a.anchor = {0, 0, 0};
    a.text = U"+100";
    ft.add(a);
    HudDrawList dl;
    wh.build(ctx, {}, types, atlas, fonts, dl, nullptr, &ft);
    CHECK(wh.stats().flytexts == 1 && !dl.items(1, HudList::Text).empty());
    const float y0 = dl.items(1, HudList::Text)[0].max.y;
    ft.update(1.0f);
    HudDrawList dl7;
    wh.build(ctx, {}, types, atlas, fonts, dl7, nullptr, &ft);
    CHECK(!dl7.items(1, HudList::Text).empty() && dl7.items(1, HudList::Text)[0].max.y > y0 + 15.0f);
    ft.update(1.5f);
    CHECK(ft.items().empty());
}

void test_routing() {
    InputRouter router;
    RoutedInput out;
    RouteHooks hooks;
    hooks.hud_hit = [](int32_t x, int32_t y) { return (x >= 100 && x < 200 && y >= 500) ? 42 : -1; };
    InputFrame f;
    f.width = 1024;
    f.height = 768;
    InputState world;
    // 1. Left click on the HUD: HUD press / release / click; the world never sees the button.
    f.mouse_x = 150;
    f.mouse_y = 600;
    f.buttons_down = button_bit(Button::Left);
    f.buttons_pressed = button_bit(Button::Left);
    router.route(f, hooks, out);
    world.update(out.world);
    CHECK(out.hud.size() == 1 && out.hud[0].kind == HudPointerEvent::Kind::Press && out.hud[0].component == 42);
    CHECK(out.world.buttons_down == 0 && out.owner[0] == PointerTarget::Hud && out.hover_component == 42);
    f.buttons_down = 0;
    f.buttons_pressed = 0;
    f.buttons_released = button_bit(Button::Left);
    router.route(f, hooks, out);
    world.update(out.world);
    CHECK(out.hud.size() == 2 && out.hud[1].kind == HudPointerEvent::Kind::Click && out.world.buttons_released == 0);
    CHECK(!world.button(Button::Left).click && !world.button(Button::Left).released);
    // 2. World drag-select ending over the HUD: the world gets press, hold and release (drag ends there).
    f.mouse_x = 600;
    f.mouse_y = 200;
    f.buttons_released = 0;
    f.buttons_down = f.buttons_pressed = button_bit(Button::Left);
    router.route(f, hooks, out);
    world.update(out.world);
    CHECK(out.hud.empty() && world.button(Button::Left).pressed && out.owner[0] == PointerTarget::World);
    f.buttons_pressed = 0;
    f.mouse_x = 150;
    f.mouse_y = 600;
    router.route(f, hooks, out);
    world.update(out.world);
    CHECK(world.button(Button::Left).down && world.button(Button::Left).dragging);
    f.buttons_down = 0;
    f.buttons_released = button_bit(Button::Left);
    router.route(f, hooks, out);
    world.update(out.world);
    CHECK(world.button(Button::Left).drag_ended && out.world.buttons_released == button_bit(Button::Left));
    CHECK(out.hud.size() == 1 && out.hud[0].kind == HudPointerEvent::Kind::Release);  // heard, no click
    // 3. Right press over the HUD then release over the world: HUD owns it, no click, world untouched.
    f.buttons_released = 0;
    f.buttons_down = f.buttons_pressed = button_bit(Button::Right);
    router.route(f, hooks, out);
    f.buttons_pressed = 0;
    f.buttons_down = 0;
    f.buttons_released = button_bit(Button::Right);
    f.mouse_x = 600;
    f.mouse_y = 100;
    router.route(f, hooks, out);
    CHECK(out.hud.size() == 1 && out.hud[0].kind == HudPointerEvent::Kind::Release && out.world.buttons_released == 0);
    // 4. Middle press over the HUD goes to the world (HUD takes no middle presses).
    f.mouse_x = 150;
    f.mouse_y = 600;
    f.buttons_released = 0;
    f.buttons_down = f.buttons_pressed = button_bit(Button::Middle);
    router.route(f, hooks, out);
    CHECK(out.world.buttons_pressed == button_bit(Button::Middle) && out.owner[2] == PointerTarget::World);
    f.buttons_down = 0;
    f.buttons_pressed = 0;
    f.buttons_released = button_bit(Button::Middle);
    router.route(f, hooks, out);
    // 5. Order: dialogs before HUD; overlay before everything; targeting blocks hover.
    bool dialog = true;
    hooks.dialog_hit = [&](int32_t, int32_t) { return dialog; };
    f.buttons_released = 0;
    f.buttons_down = f.buttons_pressed = button_bit(Button::Left);
    router.route(f, hooks, out);
    CHECK(out.owner[0] == PointerTarget::Dialog && out.hud.empty() && out.world.buttons_down == 0);
    f.buttons_down = f.buttons_pressed = 0;
    f.buttons_released = button_bit(Button::Left);
    router.route(f, hooks, out);
    CHECK(out.hud.empty() && out.world.buttons_released == 0);
    dialog = false;
    hooks.overlay_active = [] { return true; };
    f.buttons_released = 0;
    f.buttons_down = f.buttons_pressed = button_bit(Button::Left);
    router.route(f, hooks, out);
    CHECK(out.owner[0] == PointerTarget::Overlay && out.hover_component == -1 && out.world.buttons_down == 0);
    f.buttons_down = f.buttons_pressed = 0;
    f.buttons_released = button_bit(Button::Left);
    router.route(f, hooks, out);
    hooks.overlay_active = nullptr;
    hooks.targeting_active = [] { return true; };
    f.buttons_released = 0;
    router.route(f, hooks, out);
    CHECK(out.hover_component == -1);
    hooks.targeting_active = nullptr;
    // 6. Wheel and keys: world unless a dialog has focus.
    f.wheel = 1.0f;
    f.keys_down.set(vk::kLeft);
    router.route(f, hooks, out);
    CHECK(out.wheel_target == PointerTarget::World && out.world.wheel == 1.0f && out.keys_to_world && out.world.keys_down.test(vk::kLeft));
    hooks.dialog_focus = [] { return true; };
    router.route(f, hooks, out);
    CHECK(out.wheel_target == PointerTarget::Dialog && out.world.wheel == 0.0f && !out.keys_to_world &&
          !out.world.keys_down.test(vk::kLeft));
    // 7. A one-frame tap on the HUD (pressed + released in one frame): press, release, click.
    hooks.dialog_focus = nullptr;
    InputRouter r2;
    InputFrame tap = f;
    tap.wheel = 0;
    tap.mouse_x = 120;
    tap.mouse_y = 700;
    tap.buttons_down = 0;
    tap.buttons_pressed = tap.buttons_released = button_bit(Button::Left);
    r2.route(tap, hooks, out);
    CHECK(out.hud.size() == 3 && out.hud[2].kind == HudPointerEvent::Kind::Click && out.world.buttons_pressed == 0 &&
          r2.owner(Button::Left) == PointerTarget::None);
}

void test_tactical_hud() {
    const ComponentTable t = hud_test::load_table();
    const TextureAtlas atlas = hud_test::make_atlas();
    auto box = make_box_rasterizer();
    FontCache fonts(*box);
    TacticalHud hud;
    hud.init(t, atlas, fonts);
    std::vector<HudTypeInfo> types(2);
    std::vector<HudUnit> units(4);
    for (uint32_t i = 0; i < 4; ++i) {
        units[i].id = 10 + i;
        units[i].type = i < 3 ? 1 : 0;
        units[i].owner = 0;
        units[i].position = {static_cast<float>(i) * 50.0f, 0, 0};
        units[i].selected = i != 2;
        units[i].health = 0.25f * static_cast<float>(i + 1);
    }
    uint64_t digest = 0;
    for (auto [w, h] : {std::pair{1024u, 768u}, std::pair{1920u, 1080u}, std::pair{1280u, 1024u}}) {
        hud.resize(w, h);
        TacticalHudFrame fr;
        fr.world.view = top_view(w, h);
        fr.credits = 1500;
        hud.build(fr, units, types);
        const auto& g = hud.panel_groups();
        CHECK(g.size() == 2 && g[0].type == 1 && g[0].ids == (std::vector<uint32_t>{10, 11}) && g[1].ids == (std::vector<uint32_t>{13}));
        CHECK(g.size() == 2 && near(g[0].health, 0.375f));
        CHECK(hud.stats().panel_slots == 2 && hud.stats().world.brackets == 3 && hud.stats().radar_blips == 4);
        // Hit test at the first portrait -> s_select_00; a click there selects that group.
        const ComponentPlacement& p = hud.layout().placement(static_cast<uint32_t>(t.find("s_select_00")));
        CHECK(p.placed);
        const Vec2f px = hud.canvas().canvas_to_pixel(p.center.x, p.center.y);
        const int32_t hit = hud.hit_test(static_cast<int32_t>(px.x), static_cast<int32_t>(px.y));
        CHECK(hit == t.find("s_select_00"));
        CHECK(hud.hit_test(static_cast<int32_t>(w / 2), 10) == -1);
        std::vector<commands::EventData> cmds;
        const HudPointerEvent click{HudPointerEvent::Kind::Click, Button::Left, hit, 0, 0};
        hud.on_pointer(std::span<const HudPointerEvent>(&click, 1), cmds);
        const HudPointerEvent rclick{HudPointerEvent::Kind::Click, Button::Right, hit, 0, 0};
        hud.on_pointer(std::span<const HudPointerEvent>(&rclick, 1), cmds);
        CHECK(cmds.size() == 1 && cmds[0] == commands::ev::select({10, 11}));
        // Determinism: the same inputs give the same draw list.
        const uint64_t d1 = hud.draw_list().digest();
        hud.build(fr, units, types);
        CHECK(hud.draw_list().digest() == d1);
        if (w == 1024) digest = d1;
        // q6 t19 (OBJECT_SCRIPTS §10.3): a Lua-hidden unit has no radar blip unless its type is
        // Visible_On_Radar_When_Fogged (own units too).
        units[3].script_hidden = true;
        hud.build(fr, units, types);
        CHECK(hud.stats().radar_blips == 3);
        types[0].visible_on_radar_when_fogged = true;
        hud.build(fr, units, types);
        CHECK(hud.stats().radar_blips == 4);
        types[0].visible_on_radar_when_fogged = false;
        units[3].script_hidden = false;
    }
    CHECK(digest != 0);
}


// OI 2026-10-28: planet panel texts, credits separator, Game Paused banner (Resume -> HudAction::ResumeGame) and the
// Mission Objectives box.
const char* kTacticalPanelsXml = R"(<CommandBarComponents>
  <CommandBarComponent Name="Text_Planet_tactical"><Type>TextButton</Type><Font_Name>Arial</Font_Name></CommandBarComponent>
  <CommandBarComponent Name="Text_Tactical_Tech"><Type>TextButton</Type><Font_Name>Arial</Font_Name></CommandBarComponent>
  <CommandBarComponent Name="Text_Planetary_Pop"><Type>TextButton</Type><Font_Name>Arial</Font_Name></CommandBarComponent>
  <CommandBarComponent Name="Text_Pop_Cap"><Type>TextButton</Type><Font_Name>Arial</Font_Name></CommandBarComponent>
  <CommandBarComponent Name="Movie_tactical"><Type>Icon</Type><Size>200 200</Size></CommandBarComponent>
  <CommandBarComponent Name="Text_Credits_tactical"><Type>TextButton</Type><Font_Name>Arial</Font_Name></CommandBarComponent>
  <CommandBarComponent Name="text_attack"><Type>TextButton</Type><Font_Name>Arial</Font_Name><Font_Point_Size>7</Font_Point_Size></CommandBarComponent>
  <CommandBarComponent Name="attack_button"><Type>TextButton</Type><Font_Name>Arial</Font_Name><Font_Point_Size>7</Font_Point_Size>
    <Text_Color>255 64 64 255</Text_Color></CommandBarComponent>
  <CommandBarComponent Name="objective_back"><Type>TextButton</Type><Blank_Texture_Name>i_tooltip_back.tga</Blank_Texture_Name>
    <Base_Layer>3</Base_Layer><Size>266 14</Size><Offset>5 2</Offset></CommandBarComponent>
  <CommandBarComponent Name="objective_header_text"><Type>TextButton</Type><Font_Name>Arial</Font_Name><Font_Point_Size>8</Font_Point_Size>
    <Text_Color>255 255 255 255</Text_Color><Text_Color2>255 0 0 255</Text_Color2><Base_Layer>4</Base_Layer><Size>5 18</Size></CommandBarComponent>
  <CommandBarComponent Name="objective_text"><Type>TextButton</Type><Font_Name>Arial</Font_Name><Font_Point_Size>7</Font_Point_Size>
    <Text_Color>255 255 255 255</Text_Color><Text_Color2>128 128 128 255</Text_Color2><Base_Layer>4</Base_Layer><Size>40 16</Size></CommandBarComponent>
</CommandBarComponents>)";

void test_tactical_panels() {
    ComponentTable t;
    const data::XmlDocument doc = data::XmlDocument::parse(std::string_view(kTacticalPanelsXml), "tp.xml");
    t.parse(doc.root());
    TextureAtlas atlas;
    atlas.texture_width = atlas.texture_height = 64;
    auto box = make_box_rasterizer();
    FontCache fonts(*box);
    TextDatabase text;
    text.add("TEXT_TECH", u"Tech:");
    text.add("TEXT_GAME_PAUSED", u"Game Paused");
    text.add("TEXT_BUTTON_RESUME_GAME", u"Resume Game");
    text.add("TEXT_TEST_OBJECTIVE_HEADER", u"Mission Objectives");
    text.add("TEXT_OBJ_1", u"Destroy all enemy space forces to win. Keep the space station alive until the end.");
    TacticalHud hud;
    hud.init(t, atlas, fonts, &text);
    hud.resize(1920, 1080);
    // Thousands separator.
    CHECK(hud_thousands(13000) == U"13,000" && hud_thousands(668) == U"668" && hud_thousands(-1250) == U"-1,250" &&
          hud_thousands(1234567) == U"1,234,567" && hud_thousands(0) == U"0");
    TacticalHudFrame fr;
    fr.credits = 13000;
    fr.planet_name = U"Nal Hutta";
    fr.tech = 3;
    fr.pop = 13;
    fr.pop_cap = 20;
    hud.build(fr, {}, {});
    auto text_of = [&](const char* n) {
        const ButtonRuntime* r = hud.buttons().find_state(static_cast<uint32_t>(t.find(n)));
        return r ? r->text : std::u32string();
    };
    CHECK(text_of("Text_Planet_tactical") == U"Nal Hutta");
    // t45 (capture C2): the name's line is centred on the bone, not hung half a line below it.
    {
        const Vec2f bone = hud.layout().placement(static_cast<uint32_t>(t.find("Text_Planet_tactical"))).center;
        float top = -1e9f;
        for (uint32_t layer = 0; layer < 16; ++layer)
            for (const HudQuad& q : hud.draw_list().items(layer, HudList::Text))
                if (std::fabs(0.5f * (q.min.x + q.max.x) - bone.x) < 60.0f && std::fabs(q.max.y - bone.y) < 20.0f) top = std::max(top, q.max.y);
        CHECK(top > bone.y + 1.0f);
        // t47: a name wider than Max_Text_Width stays on one line centred on the bone, inside it.
        const float limit = 0.5f * t[static_cast<size_t>(t.find("Text_Planet_tactical"))].max_text_width;
        fr.planet_name = U"Nal Hutta Nal Hutta Nal Hutta Nal Hutta";
        hud.build(fr, {}, {});
        float low = 1e9f, left = 1e9f, right = -1e9f;
        for (uint32_t layer = 0; layer < 16; ++layer)
            for (const HudQuad& q : hud.draw_list().items(layer, HudList::Text))
                if (std::fabs(0.5f * (q.min.x + q.max.x) - bone.x) < 150.0f && std::fabs(0.5f * (q.min.y + q.max.y) - bone.y) < 30.0f) {
                    low = std::min(low, q.min.y);
                    left = std::min(left, q.min.x);
                    right = std::max(right, q.max.x);
                }
        CHECK(low > bone.y - 15.0f && (limit <= 0.0f || (left >= bone.x - limit - 2.0f && right <= bone.x + limit + 2.0f)));
        fr.planet_name = U"Nal Hutta";
        hud.build(fr, {}, {});
    }
    CHECK(text_of("Text_Tactical_Tech") == U"Tech: 3");
    CHECK(text_of("Text_Planetary_Pop") == U"13/20" && text_of("Text_Pop_Cap").empty());  // QA D7: the tactical shell's bone
    fr.pop_cap = -1;
    fr.tech = -1;
    fr.planet_name.clear();
    hud.build(fr, {}, {});
    CHECK(text_of("Text_Planetary_Pop") == U"13" && text_of("Text_Tactical_Tech").empty() && text_of("Text_Planet_tactical").empty());
    // FIXPASS_GALACTIC §4.2, capture C2: the advisor loop in Movie_tactical, 200 x 200 with the bone (50, 288 from the bottom
    // left) at its bottom-left corner, straight alpha; nothing without a loop. 1920 x 1080: pixels x 70..351, y 394..675.
    CHECK(!hud.stats().advisor_movie);
    {
        const size_t before = hud.draw_list().total();
        fr.advisor_page = 30;
        hud.build(fr, {}, {});
        CHECK(hud.stats().advisor_movie && hud.draw_list().total() == before + 1);
        bool found = false;
        for (uint32_t l = 0; l < kHudLayers; ++l)
            for (const HudQuad& q : hud.draw_list().items(l, HudList::Alpha))
                if (q.page == 30) {
                    found = std::fabs(q.max.x - q.min.x - 200.0f) < 1e-3f && std::fabs(q.max.y - q.min.y - 200.0f) < 1e-3f &&
                            std::fabs(q.min.x - (-0.5f * hud.canvas().width + 50.0f)) < 1e-3f &&
                            std::fabs(q.min.y - (-0.5f * hud.canvas().height + 288.0f)) < 1e-3f;
                }
        CHECK(found);
        fr.advisor_page = -1;
        hud.build(fr, {}, {});
        CHECK(!hud.stats().advisor_movie && hud.draw_list().total() == before);
    }
    CHECK(!hud.stats().pause_banner && hud.stats().objectives == 0);
    const int32_t resume = t.find("attack_button");
    auto resume_hit = [&]() {
        for (const HudHitBox& h : hud.hit_boxes())
            if (h.component == resume) return true;
        return false;
    };
    CHECK(!resume_hit());

    // Game Paused banner: top centre, Resume button under the bar; a click emits ResumeGame (no command).
    fr.paused = true;
    hud.build(fr, {}, {});
    CHECK(hud.stats().pause_banner && resume_hit());
    for (const HudHitBox& h : hud.hit_boxes())
        if (h.component == resume)
            CHECK(h.min.y > 0.3f * hud.canvas().height && std::fabs(h.min.x + h.max.x) < 20.0f);
    auto click = [&](int32_t ci, std::vector<commands::EventData>& out, std::vector<HudAction>& acts) {
        RoutedInput r;
        r.hover_component = ci;
        r.hud = {{HudPointerEvent::Kind::Press, Button::Left, ci, 0, 0}, {HudPointerEvent::Kind::Release, Button::Left, ci, 0, 0},
                 {HudPointerEvent::Kind::Click, Button::Left, ci, 0, 0}};
        hud.update_input(r, false, false, 1.0, out, &acts);
    };
    std::vector<commands::EventData> cmds;
    std::vector<HudAction> acts;
    click(resume, cmds, acts);
    CHECK(cmds.empty() && acts.size() == 1 && acts[0].kind == HudAction::Kind::ResumeGame && acts[0].name == "attack_button");
    fr.pause_banner = false;
    hud.build(fr, {}, {});
    CHECK(!hud.stats().pause_banner && !resume_hit());
    fr.pause_banner = true;
    fr.paused = false;
    hud.build(fr, {}, {});
    acts.clear();
    click(resume, cmds, acts);  // stale click after the unpause: nothing
    CHECK(acts.empty() && cmds.empty());

    // Mission Objectives: bottom right, wrapped, numbered.
    std::vector<HudObjective> objs(2);
    objs[0].text_key = "TEXT_OBJ_1";
    objs[1].text = U"Protect the mining facility.";
    objs[1].state = HudObjective::State::Complete;
    fr.objectives = objs;
    hud.build(fr, {}, {});
    CHECK(hud.stats().objectives == 2 && hud.stats().objective_lines >= 3);
    const float right = 0.5f * hud.canvas().width - 24.0f;
    bool backing = false, rule = false, grey = false;
    for (const HudQuad& q : hud.draw_list().items(3, HudList::Alpha))
        backing = backing || (std::fabs(q.max.x - right) < 0.01f && q.min.y < -0.4f * hud.canvas().height && q.max.x - q.min.x > 200.0f);
    for (const HudQuad& q : hud.draw_list().items(4, HudList::Alpha))
        rule = rule || (q.max.y - q.min.y < 1.5f && q.max.x - q.min.x > 200.0f);
    float min_x = 1e9f, max_x = -1e9f;
    for (const HudQuad& q : hud.draw_list().items(4, HudList::Text)) {
        grey = grey || q.color == Rgba8{128, 128, 128, 255};
        min_x = std::min(min_x, q.min.x);
        max_x = std::max(max_x, q.max.x);
    }
    CHECK(backing && rule && grey);
    CHECK(min_x > right - 266.0f && max_x <= right + 0.5f);  // all glyphs inside the box
}

}  // namespace

int run_stage2_tests();  // test_hud_stage2.cpp

int main() {
    test_canvas();
    test_components();
    test_atlas_text();
    test_pseudo_localize();
    test_layout();
    test_draw_list();
    test_text();
    test_game_fonts();
    test_world();
    test_routing();
    test_tactical_hud();
    test_tactical_panels();
    g_fail += run_stage2_tests();
    if (g_fail) {
        std::printf("alamo_ui_hud_tests: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("alamo_ui_hud_tests: all passed\n");
    return 0;
}
