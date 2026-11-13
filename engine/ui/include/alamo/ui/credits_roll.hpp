// ui1 t03: the front end's credits roll (MenuAction ShowCredits: IDC_MENU_CREDITS, UI.md §3.4 / §3.5).
// Data: Data\Text\CreditsText_<LANGUAGE>.DAT (docs/formats/SPEC.md §7: an unsorted DAT whose record order is the roll
// order; keys HEADER = a heading line, CENTER = a centred name line, value "[TBL]" = an empty line) and the GameConstants
// Credits_* entries: Credits_Font / Credits_Font_Size, Credits_Header_Top_Color / _Bottom_Color and Credits_Top_Color /
// _Bottom_Color (0..1 RGB gradients), Credits_Spacing, Credits_Margin and Credits_Scroll_Rate.
// [port] how the original uses them is not recovered (backlog, ui1 t03): spacing = the line pitch as a fraction of the
// screen height; margin = the top and bottom bands (fraction of the height) where the lines fade; scroll rate = screen
// heights per second, negative = upwards (the shipped file lists the tag twice, -0.05 then -0.10: the last one counts,
// as for every repeated GameConstants tag); the roll starts below the screen and ends when the last line has left the
// top; headers and names use the same font. Presentation only.
#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/ui/hud_canvas.hpp"
#include "alamo/ui/hud_draw.hpp"

namespace alamo::data {
struct GameConstants;
}

namespace alamo::ui {

class FontCache;

struct CreditsLine {
    enum class Kind : uint8_t { Blank, Header, Name };
    Kind kind = Kind::Blank;
    std::u32string text;
};

struct CreditsStyle {
    std::string font = "Arial";
    float font_size = 14.0f;
    Rgba8 header_top{0, 184, 204, 255}, header_bottom{0, 184, 204, 255};
    Rgba8 top{0, 184, 204, 255}, bottom{0, 184, 204, 255};
    float spacing = 0.04f;       // line pitch, fraction of the screen height
    float margin = 0.04f;        // fade bands, fraction of the screen height
    float scroll_rate = -0.10f;  // screen heights per second (negative: up)
};
CreditsStyle credits_style(const data::GameConstants& gc);

// The DAT records in file order (SPEC.md §7). False on a malformed file.
bool parse_credits(std::span<const uint8_t> bytes, std::vector<CreditsLine>& out);
// "Data\Text\CreditsText_<LANGUAGE>.DAT".
std::string credits_path(std::string_view language);

class CreditsRoll {
public:
    void start(std::vector<CreditsLine> lines, const CreditsStyle& style, double now);
    void stop() { active_ = false; }
    bool active() const { return active_; }
    // Seconds the whole roll takes on a screen `screen_height` pixels high.
    double duration() const;
    // Draws the visible lines (layer 12, Text) over a black backdrop; false (and inactive) once the roll has ended.
    bool build(HudDrawList& dl, FontCache& fonts, const HudCanvas& canvas, double now);
    size_t lines() const { return lines_.size(); }
    size_t drawn() const { return drawn_; }  // lines drawn by the last build

private:
    std::vector<CreditsLine> lines_;
    CreditsStyle style_;
    double t0_ = 0.0;
    bool active_ = false;
    size_t drawn_ = 0;
};

}  // namespace alamo::ui
