// P2 HUD draw list (docs/re/UI.md §2.5, §1.1): 13 layers (0..12), each with six sub-lists drawn in a fixed
// order - plain quads, radial-wipe quads, radial-wipe quads (alternate state), extra quads, model text, text -
// each sub-list one dynamic batch, cleared after drawing. Everything is in HUD canvas units (+Y up) and drawn
// with an identity world and the components ortho camera (hud_canvas.hpp), after the 3D world's post chain
// and a depth clear (§1.1 step 4); nothing reads world depth.
//
// The frame interleaves layer pairs with other passes (§1.1 steps 5i..9): HudFrameStep lists them in order;
// the renderer (alamo_ui_draw) records the layer steps and calls back for the others (shell models, 2D
// scenes, cursor).
//
// Texture pages: 0 = white, 1 = the MT_CommandBar atlas, 2 = the radar image (terrain + fog of war, a dynamic
// page the host refreshes), 3..15 = FontCache pages (kHudPageFontBase + page), 16..31 = image pages (P4: stand-alone
// GUI textures such as MenuBack_overlay_panel / loadmenuback / planet icons that are not in the atlas).
// Blend (docs/re/STRATEGIC_GAPS.md §7.2 [H]): the layer draw submits each sub-list with a 2D-effect selector, so the
// LIST decides the blend, not the item: plain quads -> 0 PrimOpaque (no blend), fill dials -> 2 PrimAlpha
// (SRCALPHA / INVSRCALPHA, tex x diffuse), alternate dials -> 1 PrimAdditive (ONE / ONE, tex x diffuse), extra quads ->
// 3 PrimModulate (ZERO / SRCCOLOR, colour = tex + diffuse); z write off, z-func always. HudList's Opaque / Alpha /
// Additive / Modulate names say which effect a list gets. [port] the two text lists use PrimAlpha (glyph coverage in
// alpha; their selector is not in the doc). [port] emitters: everything that needs transparency goes to the Alpha list
// (fill 100 = a plain quad), additive art (Lower_Effect_Additive) to Additive, only opaque content (non-alpha movies)
// to Opaque; nothing is drawn darkening yet (Modulate stays free for it).
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "alamo/ui/hud_canvas.hpp"
#include "alamo/ui/hud_components.hpp"

namespace alamo::ui {

class FontCache;

// Sub-lists in draw order. The first four are named by their effect (above); Quads / Dial / DialAlt / Extra are the
// UI.md §2.5 names of the same lists. Every list draws fill < 100 as a radial wipe (radial_wipe), 100 as a plain quad.
enum class HudList : uint8_t {
    Opaque = 0, Alpha = 1, Additive = 2, Modulate = 3, ModelText = 4, Text = 5,
    Quads = Opaque, Dial = Alpha, DialAlt = Additive, Extra = Modulate,
};
inline constexpr uint32_t kHudSubLists = 6;

// The 2D effect selector table of the engine's prim draw (STRATEGIC_GAPS §7.2 [H], effects loaded at start-up).
// Only 0..3 are HUD-list effects; the rest are listed for completeness (depth sprites, stencil darkening, heat,
// bump-mapped particles / decals, scanlines) and are drawn by other passes.
enum class HudEffect : uint8_t {
    PrimOpaque = 0, PrimAdditive = 1, PrimAlpha = 2, PrimModulate = 3, PrimDepthSpriteAdditive = 4,
    PrimDepthSpriteAlpha = 5, PrimDepthSpriteModulate = 6, PrimDiffuseAlpha = 7, StencilDarken = 8,
    StencilDarkenFinalBlur = 9, PrimHeat = 10, PrimParticleBumpAlpha = 11, PrimDecalBumpAlpha = 12,
    PrimAlphaScanlines = 13,
};
inline constexpr uint32_t kHudEffectCount = 14;
// "Engine\\PrimOpaque", ..., "Engine\\StencilDarken" (8 also has the StencilDarkenToAlpha variant); "" out of range.
const char* hud_effect_name(HudEffect e);
// Effect of a sub-list: Opaque 0, Alpha 2, Additive 1, Modulate 3, ModelText / Text PrimAlpha [port].
HudEffect hud_list_effect(HudList list);

inline constexpr uint16_t kHudPageWhite = 0, kHudPageAtlas = 1, kHudPageRadar = 2, kHudPageFontBase = 3;
inline constexpr uint16_t kHudPageImageBase = 16;  // P4 image pages (HudRenderer::set_image_page)
inline constexpr uint16_t kHudPageCount = 32;      // texture pages bound per HUD pass
inline constexpr uint16_t kHudFontPages = kHudPageImageBase - kHudPageFontBase;
inline constexpr uint16_t kHudImagePages = kHudPageCount - kHudPageImageBase;

struct HudQuad {
    Vec2f min{}, max{};              // canvas units, +Y up (min = bottom-left corner)
    Vec2f uv0{0, 0}, uv1{1, 1};      // texture coordinates of the top-left / bottom-right corners
    Rgba8 color{};
    uint16_t page = kHudPageWhite;
    float fill = 100.0f;             // 0..100 radial wipe (100 = plain quad)
    // P4: vertical gradient (GUIDialogs.xml font Top_Color / Bottom_Color): `color` at max.y, `color_bottom` at min.y.
    bool gradient = false;
    Rgba8 color_bottom{};
};

struct HudVertex {
    float x = 0, y = 0;  // canvas units
    float u = 0, v = 0;
    uint32_t color = 0;  // R | G << 8 | B << 16 | A << 24 (VK_FORMAT_R8G8B8A8_UNORM)
    uint32_t page = 0;
};
static_assert(sizeof(HudVertex) == 24);
inline uint32_t pack_rgba(Rgba8 c) {
    return static_cast<uint32_t>(c.r) | (static_cast<uint32_t>(c.g) << 8) | (static_cast<uint32_t>(c.b) << 16) |
           (static_cast<uint32_t>(c.a) << 24);
}

// Radial "clock wipe" (§2.5 list 2/3): fill 100 (or more) -> the plain quad (4 vertices); otherwise a fan over the
// last `fill` % of the clock: from (100 - fill) % clockwise from 12 o'clock round to 12 o'clock (cu9h t03: the retail
// bombing-run dial at value v shows the first v % clockwise from 12 o'clock uncovered and the rest under the texture, and
// UI.md §2.5 says the fan loses one vertex per 12.5 % of the dial value; the dial callers pass 100 - value). One
// perimeter vertex per 45-degree boundary passed (the corners and edge midpoints of the quad) plus the start and end
// points: 3 + (45-degree boundaries strictly inside the sweep) vertices for 0 < fill < 100, i.e. 3 at 0..12.5 % up to
// 10 near 100 %; fill <= 0 returns 0 (nothing drawn). Writes up to 10 vertices (centre first) and returns the count;
// triangles are (0, i, i + 1), clockwise.
uint32_t radial_wipe(const HudQuad& q, float fill, HudVertex (&out)[11]);

class HudDrawList {
public:
    void clear();
    void add(uint32_t layer, HudList list, const HudQuad& q);
    std::span<const HudQuad> items(uint32_t layer, HudList list) const {
        return lists_[layer < kHudLayers ? layer : kHudLayers - 1][static_cast<size_t>(list)];
    }
    size_t total() const;

    // One (layer, list) as indexed triangles; batches split where the page changes (submission order). The batch's
    // effect is the list's (hud_list_effect).
    struct Batch {
        uint32_t first_index = 0, index_count = 0;
        uint16_t page = 0;
        HudEffect effect = HudEffect::PrimAlpha;
    };
    // Batches below index `merge_floor` are closed: a quad never joins them (they belong to an earlier draw step).
    void build_geometry(uint32_t layer, HudList list, std::vector<HudVertex>& vertices, std::vector<uint32_t>& indices,
                        std::vector<Batch>& batches, size_t merge_floor = 0) const;
    // Every layer's lists, in layer / list order. Each layer starts a new batch, because hud_frame_steps() draws other
    // passes between layers: a layer 2 quad merged into layer 1's last batch would draw before the shell models and
    // disappear behind them. `ranges` (optional) receives each layer's [first, last) batch range.
    using LayerBatchRanges = std::array<std::pair<size_t, size_t>, kHudLayers>;
    void build_layers(std::vector<HudVertex>& vertices, std::vector<uint32_t>& indices, std::vector<Batch>& batches,
                      LayerBatchRanges* ranges = nullptr) const;
    // Ordered digest of every item (determinism tests).
    uint64_t digest() const;

private:
    std::array<std::array<std::vector<HudQuad>, kHudSubLists>, kHudLayers> lists_;
};

struct HudHitBox {
    int32_t component = -1;
    uint32_t layer = 0;
    Vec2f min{}, max{};  // canvas units
    uint32_t slot = 0;   // panel slot (s_select_NN) or 0
};

// Hit box of a component drawn at `center` with `size` (canvas units, +Y up).
HudHitBox make_hit_box(int32_t component, uint32_t layer, Vec2f center, Vec2f size, uint32_t slot = 0);
// The topmost box under pixel (px, py): highest layer, then the latest added on that layer; its component, or -1.
int32_t topmost_hit(std::span<const HudHitBox> hits, const HudCanvas& canvas, int32_t px, int32_t py);
// A rectangle outline of 2-unit lines plus a translucent fill ([port] stand-in plates).
void frame_rect(HudDrawList& dl, uint32_t layer, Vec2f center, Vec2f size, Rgba8 back, Rgba8 edge);
// Window resize: `canvas` becomes HudCanvas::make(width, height, canvas.ui_scale); the font cache is cleared when the text
// height changed (pixel sizes follow it).
void resize_canvas(HudCanvas& canvas, FontCache* fonts, uint32_t width, uint32_t height);

// ---- Emitters -------------------------------------------------------------------------------------
// A texture as the GUI draws it: an atlas entry (page 1 + UVs) or an image page (HudRenderer::set_image_page).
struct GuiImage {
    bool valid = false;
    uint16_t page = kHudPageWhite;
    Vec2f uv0{0, 0}, uv1{1, 1};
    Vec2f size{};  // texels (1 texel = 1 canvas unit [port])
};
// Host hook for textures that are not in the MT_CommandBar atlas (stand-alone DDS such as MenuBack_overlay_panel,
// loadmenuback, planet icons): return an image page, or {} when unavailable.
using GuiImageResolver = std::function<GuiImage(std::string_view texture)>;

// Atlas-textured quad of `texture` centred at `center` (canvas). size: canvas units; {0,0} = the atlas entry's
// pixel size x scale [port: 1 atlas texel = 1 canvas unit]. A texture missing from the atlas draws white
// (page 0) at `fallback_size` x scale (nothing when that is 0). Returns false when nothing was drawn. The blend is the
// list's (Alpha for ordinary art, Additive for additive art).
bool add_atlas_quad(HudDrawList& dl, uint32_t layer, HudList list, const TextureAtlas& atlas, std::string_view texture,
                    Vec2f center, Vec2f size, float scale, Rgba8 color, Vec2f fallback_size = {}, float fill = 100.0f);
// A movie frame on a HUD / 2D-scene list (STRATEGIC_GAPS §7.3 [H]): colour (1,1,1,1), PrimAlpha (straight alpha) for an
// alpha movie (Movies.xml Alpha), PrimOpaque otherwise. `min` / `max` canvas corners (+Y up), `page` the texture page.
void add_movie_quad(HudDrawList& dl, uint32_t layer, Vec2f min, Vec2f max, uint16_t page, bool alpha);
// Same, but clipped on the right to `fraction` of its width (smooth bars): the quad and its UVs shrink.
bool add_atlas_quad_clipped(HudDrawList& dl, uint32_t layer, HudList list, const TextureAtlas& atlas,
                            std::string_view texture, Vec2f center, Vec2f size, float scale, float fraction, Rgba8 color,
                            Vec2f fallback_size = {});

enum class TextAlign : uint8_t { Left, Center, Right };
struct HudTextStyle {
    int32_t font = -1;  // FontCache id
    Rgba8 color{};
    bool outline = false;  // [port] four 1-pixel offset copies in outline_color under the text
    bool emboss = false;   // [port] one copy offset (+1, +1) pixel in emboss_color under the text
    Rgba8 outline_color{0, 0, 0, 255}, emboss_color{0, 0, 0, 200};
    float scale = 1.0f;    // item scale (glyphs are drawn at units-per-pixel x scale)
    float width_scale = 1.0f;  // [port] t47: extra factor on widths and advances only (fit_width_scale); heights stay
    TextAlign align = TextAlign::Left;
    bool snap = true;      // left / top edge rounded to whole pixels (§1.2)
    // P4 GUI fonts: Top_Color (= color) / Bottom_Color gradient [port: per glyph box, not per line], and
    // Character_Padding (extra pixels after every glyph advance).
    bool gradient = false;
    Rgba8 color_bottom{};
    float char_padding_px = 0.0f;
};
// [port] t47: the HudTextStyle::width_scale that narrows a line `width` wide into `max_width` (same units), at most to half.
inline float fit_width_scale(float width, float max_width) {
    return width > max_width && max_width > 0.0f ? std::max(0.5f, max_width / width) : 1.0f;
}
// Lays out one line: `anchor` (canvas, +Y up) is the left / centre / right end (align) of the line's vertical
// centre [port]. Glyph quads go to `list` (Text or ModelText) on `layer`. Returns the line width in canvas units.
float add_text(HudDrawList& dl, uint32_t layer, HudList list, FontCache& fonts, const HudCanvas& canvas,
               std::u32string_view text, Vec2f anchor, const HudTextStyle& style);

// Max_Text_Width wrapping (UI.md §2.1, §2.4): greedy word wrap of `text` into lines no wider than
// `max_width_px` pixels (FontCache advances); '\n' forces a break, a word wider than the limit is split between
// code points, runs of spaces at a break are dropped. max_width_px <= 0: one line per '\n' only.
std::vector<std::u32string> wrap_text(FontCache& fonts, int32_t font, std::u32string_view text, float max_width_px);
// Wrap width in pixels for a component's Max_Text_Width (canvas units) at a text scale: units / units-per-pixel
// / scale x the 95 % factor at exactly 800 pixels wide (HudCanvas::text_wrap_factor).
float wrap_width_px(const HudCanvas& canvas, float max_text_width_units, float scale = 1.0f);
struct TextBlock {
    float width = 0, height = 0;  // canvas units
    uint32_t lines = 0;
};
// Wrapped block: `anchor` is the top edge's left / centre / right point (style.align); lines are line_height
// pixels apart. Returns the block extent.
TextBlock add_text_block(HudDrawList& dl, uint32_t layer, HudList list, FontCache& fonts, const HudCanvas& canvas,
                         std::u32string_view text, Vec2f anchor, float max_width_units, const HudTextStyle& style);
// Extent of the same block without drawing.
TextBlock measure_text_block(FontCache& fonts, const HudCanvas& canvas, std::u32string_view text, float max_width_units,
                             const HudTextStyle& style);

// ---- Frame order (§1.1) ---------------------------------------------------------------------------
enum class HudStepKind : uint8_t {
    Layers,        // draw layers [first, last]
    ShellModels,   // ortho 3D shell models (camera kHudCameraShells); half 0 = first half, 1 = second
    HudScene2d,    // HUD-owned Scene2d
    Models2,       // second ortho model set (kHudCameraModels2)
    Teletype,      // tutorial / teletype text
    Dialogs,       // 2D scene #1: GUI dialogs (P4)
    Cursor,        // 2D scene #2: mouse cursor (topmost)
};
struct HudFrameStep {
    HudStepKind kind;
    uint8_t first = 0, last = 0;  // Layers range; ShellModels: half
};
// The fixed order: L0-1, shells(1st half), L2-4, scene2d, L5-6, models2, L7-8, shells(2nd half), teletype, L9-10,
// dialogs, L11-12, cursor.
std::span<const HudFrameStep> hud_frame_steps();

}  // namespace alamo::ui
