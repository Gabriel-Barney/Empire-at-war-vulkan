#include "alamo/game/attract_mode.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string_view>

#include "alamo/core/ascii.hpp"
#include "alamo/data/game_database.hpp"

namespace alamo::game {

namespace {
// Comma / whitespace separated tokens.
std::vector<std::string> tokens(std::string_view s) {
    std::vector<std::string> out;
    std::string cur;
    for (const char c : s) {
        if (c == ',' || c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (!cur.empty()) out.push_back(std::move(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(std::move(cur));
    return out;
}
}  // namespace

AttractConfig load_attract_config(const data::GameConstants& gc) {
    AttractConfig c;
    c.enabled = gc.get_bool("Main_Menu_Demo_Attract_Mode", c.enabled);
    c.start_timeout_s = static_cast<double>(gc.get_int("Demo_Attract_Start_Timeout_Seconds", static_cast<int32_t>(c.start_timeout_s)));
    if (const auto maps = gc.get("Demo_Attract_Maps"))
        for (std::string m : tokens(*maps)) {
            if (m.size() > 4) {
                const std::string ext = m.substr(m.size() - 4);
                if (ext == ".ted" || ext == ".TED" || ext == ".Ted") m.resize(m.size() - 4);
            }
            c.maps.push_back(std::move(m));
        }
    if (const auto delays = gc.get("Demo_Attract_Map_Cycle_Delay_Seconds"))
        for (const std::string& d : tokens(*delays)) c.cycle_delay_s.push_back(std::strtod(d.c_str(), nullptr));
    return c;
}

namespace {
// Position of `needle` in `s` (case-insensitive), npos when absent.
size_t ifind(const std::string& s, std::string_view needle) {
    for (size_t at = 0; at + needle.size() <= s.size(); ++at) {
        size_t i = 0;
        while (i < needle.size() && ascii_to_lower(s[at + i]) == ascii_to_lower(needle[i])) ++i;
        if (i == needle.size()) return at;
    }
    return std::string::npos;
}
}  // namespace

bool is_attract_recording(const std::string& entry) { return ascii_iends_with(entry, ".rec"); }

std::string attract_rec_map(std::span<const uint8_t> bytes) {
    constexpr std::string_view kPrefix = ".\\Data\\Art\\Maps\\";
    const size_t n = std::min<size_t>(bytes.size(), 8192);
    std::string cur;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t b = bytes[i];
        if (b == 0) {
            // The EaW file has printable header bytes right before the path ("d+" in front_end_demo_attract.rec), so the
            // path may start inside the run.
            const size_t at = ifind(cur, kPrefix);
            if (at != std::string::npos && ascii_iends_with(cur, ".ted") && cur.size() > at + kPrefix.size() + 4)
                return cur.substr(at + kPrefix.size(), cur.size() - at - kPrefix.size() - 4);
            cur.clear();
        } else if (b >= 0x20 && b < 0x7F) {
            cur.push_back(static_cast<char>(b));
        } else {
            cur.clear();  // not ASCII text: no string runs through here
        }
    }
    return {};
}

const std::string& AttractController::map() const {
    static const std::string kNone;
    return index_ < cfg_.maps.size() ? cfg_.maps[index_] : kNone;
}

double AttractController::cycle_delay() const {
    // 0x22c50: Demo_Attract_Map_Cycle_Delay_Seconds[index], 120 s when the list is shorter than the map list.
    return index_ < cfg_.cycle_delay_s.size() ? cfg_.cycle_delay_s[index_] : 120.0;
}

AttractController::Request AttractController::start(double now) {
    // 0x22f10: index advances before the load (first run included); map start time = now.
    if (cfg_.maps.empty()) return Request::None;
    index_ = (index_ + 1) % cfg_.maps.size();
    running_ = true;
    paused_ = false;
    started_at_ = now;
    ++starts_;
    return Request::Start;
}

AttractController::Request AttractController::menu_opened(double now, bool force) {
    if ((!cfg_.enabled && !force) || cfg_.maps.empty()) return Request::None;
    if (!running_) {  // 0x22bc0: arm (start = last input = now), then the first map at once
        armed_ = true;
        last_input_ = now;
        return start(now);
    }
    if (cycle_due(now, Dialogs::MainMenu, true)) {  // 0x235f0
        const Request r = start(now);
        return r == Request::Start ? Request::Next : r;
    }
    return Request::None;
}

bool AttractController::cycle_due(double now, Dialogs dialogs, bool window_active) const {
    if (!running_ || paused_ || now - started_at_ <= cycle_delay()) return false;
    if (dialogs == Dialogs::None) return true;
    const double idle = now - last_input_;
    return window_active && idle > (dialogs == Dialogs::MainMenu ? 8.0 : 20.0);
}

AttractController::Request AttractController::update(double now, bool input, Dialogs dialogs, bool window_active) {
    if (!armed_) return Request::None;
    if (input) last_input_ = now;
    paused_ = now - last_input_ > cfg_.start_timeout_s;  // the idle pause (fast mode cleared by the host)
    if (cycle_due(now, dialogs, window_active)) {
        const Request r = start(now);
        return r == Request::Start ? Request::Next : r;
    }
    return Request::None;
}

float demo_wrap_deg(float deg) {
    deg = std::fmod(deg, 360.0f);
    if (deg > 180.0f) deg -= 360.0f;
    if (deg <= -180.0f) deg += 360.0f;
    return deg;
}

float demo_view_alignment(float yaw_deg, float facing_x, float facing_y) {
    const float fl = std::sqrt(facing_x * facing_x + facing_y * facing_y);
    if (fl < 1e-4f) return 0.0f;
    const float yaw = yaw_deg * (3.14159265358979323846f / 180.0f);
    return std::fabs((std::sin(yaw) * facing_x + std::cos(yaw) * facing_y) / fl);
}

float demo_side_yaw_deg(float facing_x, float facing_y, float from_yaw_deg) {
    if (std::sqrt(facing_x * facing_x + facing_y * facing_y) < 1e-4f) return from_yaw_deg;
    const float rad = 180.0f / 3.14159265358979323846f;
    const float a = std::atan2(facing_y, -facing_x) * rad;  // view direction (facing_y, -facing_x)
    const float b = demo_wrap_deg(a + 180.0f);               // the other broadside
    return std::fabs(demo_wrap_deg(a - from_yaw_deg)) <= std::fabs(demo_wrap_deg(b - from_yaw_deg)) ? a : b;
}

float demo_shot_yaw_deg(float current_yaw_deg, float facing_x, float facing_y, float shot_t, float dt, const DemoCameraTuning& t) {
    const float side = demo_side_yaw_deg(facing_x, facing_y, current_yaw_deg);
    const float swing = t.swing_deg * std::sin(shot_t * 6.28318530717958647692f / std::max(t.swing_period_s, 0.1f));
    const float delta = demo_wrap_deg(side + swing - current_yaw_deg);
    const float step = std::max(t.max_turn_deg_per_s, 0.0f) * std::max(dt, 0.0f);
    return demo_wrap_deg(current_yaw_deg + std::clamp(delta, -step, step));
}

void AttractController::stop() {
    armed_ = running_ = paused_ = false;
}

}  // namespace alamo::game
