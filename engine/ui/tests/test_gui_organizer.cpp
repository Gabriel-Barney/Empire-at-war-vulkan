// alamo_ui_organizer_tests (p4ui t04, CPU, synthetic data): which fleet fills which pane of the fleet organiser
// (docs/re/UI.md §7). The three panes q1 / q2 / q3 always exist; the HUD fills them by the fleets' orbit slots and the
// orbit owner, not by provider order; foreign units are drawn but inert. No game data needed.
#include "test_gui_screens_common.hpp"

namespace {

using namespace test_gui_screens_t;

constexpr int kPad = 3;  // unit pads per pane in the synthetic table (q{n}_0001 .. q{n}_0003; q{n}_0000 is the header pad, UI.md §7.4)

std::string organizer_xml() {
    std::string x = "<CommandBarComponents>\n";
    auto add = [&](const std::string& name) {
        x += "<CommandBarComponent Name=\"" + name +
             "\"><Type>TextButton</Type><Blank_Texture_Name>i_frame.tga</Blank_Texture_Name><Drag_And_Drop>True</Drag_And_Drop></CommandBarComponent>\n";
    };
    for (int n = 1; n <= 3; ++n) {
        add("q" + std::to_string(n) + "_fleet");
        for (int c = 0; c <= kPad; ++c) add("q" + std::to_string(n) + "_000" + std::to_string(c));
    }
    return x + "</CommandBarComponents>";
}

// A fleet of `units` units whose icons are "i_<fleet>_<n>.tga" (so a pad says which fleet it shows).
GalacticFleet make_fleet(uint32_t id, int32_t slot, bool enemy, uint32_t units) {
    GalacticFleet f;
    f.fleet = id;
    f.slot = slot;
    f.enemy = enemy;
    for (uint32_t u = 0; u < units; ++u) f.units.push_back({id * 10 + u + 1, "i_" + std::to_string(id) + "_" + std::to_string(u) + ".tga", 1, false});
    return f;
}

struct Env {
    ComponentTable table;
    TextureAtlas atlas;
    std::unique_ptr<GlyphRasterizer> raster = make_box_rasterizer();
    FontCache fonts{*raster};
    GalacticHud hud;
    GalacticHudFrame frame;
    std::vector<GalacticPlanetInfo> planets;
    GalacticCameraController cam;

    Env() {
        const std::string xml = organizer_xml();
        const data::XmlDocument doc = data::XmlDocument::parse(std::string_view(xml), "organizer.xml");
        table.parse(doc.root());
        atlas.texture_width = atlas.texture_height = 128;
        atlas.add({"i_frame.tga", 0, 0, 40, 40, true});
        for (uint32_t id = 900; id < 912; ++id)
            for (uint32_t u = 0; u < 4; ++u) atlas.add({"i_" + std::to_string(id) + "_" + std::to_string(u) + ".tga", 0, 0, 30, 30, true});
        hud.init(table, atlas, fonts);
        hud.resize(1024, 768);
        planets.resize(2);
        planets[0].planet = 100;
        planets[0].position = {200, 100, 0};
        planets[0].selected = true;
        planets[1].planet = 101;
        planets[1].position = {350, 200, 0};
        frame.view = make_view(cam.params(4.0f / 3.0f), 1024, 768);
    }
    // Feed `fleets` (in this order) for the selected planet, whose orbit is the local player's or not, and build.
    void build(std::vector<GalacticFleet> fleets, bool orbit_local) {
        planets[0].orbit_local = orbit_local;
        frame.planets = planets;
        frame.providers.fleets = [fleets = std::move(fleets)](uint32_t, std::vector<GalacticFleet>& out) { out.insert(out.end(), fleets.begin(), fleets.end()); };
        hud.build(frame);
    }
    int32_t id(const std::string& name) const { return table.find(name); }
    static std::string pad_name(int pane, int pad) { return "q" + std::to_string(pane + 1) + "_000" + std::to_string(pad + 1); }
    bool hidden(const std::string& name) {
        const ButtonRuntime* r = hud.buttons().find_state(static_cast<uint32_t>(id(name)));
        return r == nullptr || r->hidden;
    }
    // The icon of a shown unit pad, "" when hidden.
    std::string icon(int pane, int pad) {
        const std::string n = pad_name(pane, pad);
        if (hidden(n)) return {};
        return hud.buttons().find_state(static_cast<uint32_t>(id(n)))->icon_override;
    }
    int filled(int pane) {
        int n = 0;
        for (int p = 0; p < kPad; ++p) n += hidden(pad_name(pane, p)) ? 0 : 1;
        return n;
    }
    bool header_shown(int pane) { return !hidden("q" + std::to_string(pane + 1) + "_fleet"); }
    Vec2f center_px(const std::string& name) const {
        const int32_t c = id(name);
        for (const HudHitBox& h : hud.hit_boxes())
            if (h.component == c) return hud.canvas().canvas_to_pixel(0.5f * (h.min.x + h.max.x), 0.5f * (h.min.y + h.max.y));
        return Vec2f{-1, -1};
    }
    // Press `from`, start the drag, rebuild (the empty pads appear, UI.md §7.3), move onto `to_name`'s pad, release; the
    // commands the drop issued.
    std::vector<commands::EventData> drag(const std::string& from, const std::string& to_name, bool* valid = nullptr) {
        const int32_t fc = id(from), tc = id(to_name);
        const Vec2f p = center_px(from);
        CHECK(p.x >= 0);
        std::vector<commands::EventData> out;
        RoutedInput r;
        r.world.mouse_x = static_cast<int32_t>(p.x);
        r.world.mouse_y = static_cast<int32_t>(p.y);
        r.hover_component = fc;
        r.owner[static_cast<size_t>(Button::Left)] = PointerTarget::Hud;
        r.hud = {{HudPointerEvent::Kind::Press, Button::Left, fc, r.world.mouse_x, r.world.mouse_y}};
        hud.update_input(r, false, false, 1.0, out);
        r.hud.clear();
        r.world.mouse_x += 10;
        hud.update_input(r, false, false, 1.05, out);
        CHECK(hud.drag().active);
        hud.build(frame);
        const Vec2f to = center_px(to_name);
        CHECK(to.x >= 0);
        r.world.mouse_x = static_cast<int32_t>(to.x);
        r.world.mouse_y = static_cast<int32_t>(to.y);
        r.hover_component = tc;
        hud.update_input(r, false, false, 1.1, out);
        CHECK(hud.drag().active);
        if (valid) *valid = hud.drag().valid;
        r.owner = {};
        r.hud = {{HudPointerEvent::Kind::Release, Button::Left, tc, r.world.mouse_x, r.world.mouse_y}};
        hud.update_input(r, false, false, 1.2, out);
        CHECK(!hud.drag().active);
        hud.build(frame);
        return out;
    }
    std::vector<HudAction> click(const std::string& name, double now = 2.0) {
        const int32_t c = id(name);
        std::vector<commands::EventData> cmds;
        std::vector<HudAction> acts;
        RoutedInput r;
        r.hover_component = c;
        r.hud = {{HudPointerEvent::Kind::Press, Button::Left, c, 0, 0}, {HudPointerEvent::Kind::Release, Button::Left, c, 0, 0},
                 {HudPointerEvent::Kind::Click, Button::Left, c, 0, 0}};
        hud.update_input(r, false, false, now, cmds, &acts);
        CHECK(cmds.empty());
        return acts;
    }
};

// UI.md §7.1: three panes whatever the fleet count; one fleet in slot 1 fills pane 1 only.
void test_one_fleet() {
    Env e;
    e.build({make_fleet(900, 1, false, 2)}, true);
    CHECK(e.hud.stats().organize_shown);
    CHECK(e.filled(0) == 0 && e.filled(1) == 2 && e.filled(2) == 0);
    CHECK(!e.header_shown(0) && e.header_shown(1) && !e.header_shown(2));
    CHECK(e.icon(1, 0) == "i_900_0.tga" && e.icon(1, 1) == "i_900_1.tga");
    CHECK(e.hud.stats().fleet_slots == 2);
    // No fleet at all: all three panes empty, the organiser still shows.
    e.build({}, true);
    CHECK(e.hud.stats().organize_shown && e.filled(0) + e.filled(1) + e.filled(2) == 0 && e.hud.stats().fleet_slots == 0);
}

// UI.md §7.2: the local player's orbit: slot k -> pane k, whatever the provider order.
void test_slots_to_panes() {
    Env e;
    e.build({make_fleet(902, 2, false, 1), make_fleet(900, 0, false, 1), make_fleet(901, 1, false, 1)}, true);
    CHECK(e.icon(0, 0) == "i_900_0.tga" && e.icon(1, 0) == "i_901_0.tga" && e.icon(2, 0) == "i_902_0.tga");
    CHECK(e.header_shown(0) && e.header_shown(1) && e.header_shown(2) && e.hud.stats().fleet_slots == 3);
    CHECK(e.hud.draggable(e.id("q1_fleet")) && e.hud.draggable(e.id("q2_0001")) && e.hud.draggable(e.id("q3_0001")));
}

// UI.md §7.2: every own slot-0 fleet merges into pane 0, one pad run after the other from the first pad.
void test_slot0_merge() {
    Env e;
    e.build({make_fleet(900, 0, false, 2), make_fleet(901, 1, false, 1), make_fleet(902, 0, false, 1)}, true);
    CHECK(e.filled(0) == 3 && e.icon(0, 0) == "i_900_0.tga" && e.icon(0, 1) == "i_900_1.tga" && e.icon(0, 2) == "i_902_0.tga");
    CHECK(e.filled(1) == 1 && e.icon(1, 0) == "i_901_0.tga" && e.filled(2) == 0);
    // The header stands for the pane's first fleet: dragging it onto the other planet moves fleet 900.
    float sx = 0, sy = 0;
    CHECK(project(e.frame.view, e.planets[1].position, sx, sy));
    const Vec2f p = e.center_px("q1_fleet");
    std::vector<commands::EventData> out;
    RoutedInput r;
    const int32_t hc = e.id("q1_fleet");
    r.world.mouse_x = static_cast<int32_t>(p.x);
    r.world.mouse_y = static_cast<int32_t>(p.y);
    r.hover_component = hc;
    r.owner[static_cast<size_t>(Button::Left)] = PointerTarget::Hud;
    r.hud = {{HudPointerEvent::Kind::Press, Button::Left, hc, r.world.mouse_x, r.world.mouse_y}};
    e.hud.update_input(r, false, false, 1.0, out);
    r.hud.clear();
    r.world.mouse_x = static_cast<int32_t>(sx);
    r.world.mouse_y = static_cast<int32_t>(sy);
    r.hover_component = -1;
    e.hud.update_input(r, false, false, 1.1, out);
    r.owner = {};
    r.hud = {{HudPointerEvent::Kind::Release, Button::Left, -1, r.world.mouse_x, r.world.mouse_y}};
    e.hud.update_input(r, false, false, 1.2, out);
    CHECK(out.size() == 1 && out[0] == commands::ev::move_object_to_object(900, 101));
}

// UI.md §7.2: a pane shows at most its pads; the rest of the stacks are not drawn.
void test_pane_overflow() {
    Env e;
    e.build({make_fleet(900, 0, false, 4)}, true);
    CHECK(e.filled(0) == kPad && e.hud.stats().fleet_slots == static_cast<uint32_t>(kPad));
}

// UI.md §7.2, second row: the orbit is not the local player's. Foreign fleets (merged) -> pane 0, own slot 1 -> pane 1,
// own slots 0 and 2 -> pane 2 (merged); foreign units are drawn but inert.
void test_foreign_orbit() {
    Env e;
    e.build({make_fleet(903, 2, false, 1), make_fleet(901, 1, false, 1), make_fleet(905, 1, true, 1), make_fleet(902, 0, false, 2),
             make_fleet(904, 0, true, 1)},
            false);
    CHECK(e.filled(0) == 2 && e.icon(0, 0) == "i_905_0.tga" && e.icon(0, 1) == "i_904_0.tga");  // both foreign fleets, feed order
    CHECK(e.filled(1) == 1 && e.icon(1, 0) == "i_901_0.tga");
    CHECK(e.filled(2) == kPad && e.icon(2, 0) == "i_903_0.tga" && e.icon(2, 1) == "i_902_0.tga" && e.icon(2, 2) == "i_902_1.tga");
    CHECK(e.header_shown(0) && e.header_shown(1) && e.header_shown(2));
    // Pane 0 is foreign: not draggable (header and pads), clicks do nothing; the own panes are draggable and click-selectable.
    CHECK(!e.hud.draggable(e.id("q1_fleet")) && !e.hud.draggable(e.id("q1_0001")) && !e.hud.draggable(e.id("q1_0002")));
    CHECK(e.hud.draggable(e.id("q2_fleet")) && e.hud.draggable(e.id("q2_0001")) && e.hud.draggable(e.id("q3_0001")));
    CHECK(e.click("q1_0001").empty() && e.click("q1_fleet").empty());
    const std::vector<HudAction> own = e.click("q2_0001");
    CHECK(own.size() == 1 && own[0].kind == HudAction::Kind::GalacticSelect && own[0].ids == std::vector<uint32_t>{9011});
    // Dropping an own unit on a foreign pad merges nothing; on an own fleet's pad it merges (op 3).
    bool valid = true;
    CHECK(e.drag("q2_0001", "q1_0001", &valid).empty() && !valid);
    CHECK(e.drag("q2_0001", "q1_fleet", &valid).empty() && !valid);
    const std::vector<commands::EventData> o = e.drag("q2_0001", "q3_0001", &valid);
    CHECK(valid && o.size() == 1 && o[0] == fleet_management_event(commands::kFleetMerge, 903, 901, 9011));
    // A lone foreign fleet: pane 0 only, the other panes empty.
    e.build({make_fleet(906, 0, true, 1)}, false);
    CHECK(e.filled(0) == 1 && e.filled(1) == 0 && e.filled(2) == 0 && !e.hud.draggable(e.id("q1_0001")));
}

// [port] fleets without a slot, or on a taken pane, take the first empty pane (as the map's build_planet_stacks).
void test_unknown_slots() {
    Env e;
    e.build({make_fleet(900, -1, false, 1), make_fleet(901, 2, false, 1), make_fleet(902, -1, false, 1)}, true);
    CHECK(e.icon(2, 0) == "i_901_0.tga" && e.icon(0, 0) == "i_900_0.tga" && e.icon(1, 0) == "i_902_0.tga");
    // Two fleets claim slot 1: the second takes the first empty pane (0).
    e.build({make_fleet(900, 1, false, 1), make_fleet(901, 1, false, 1)}, true);
    CHECK(e.icon(1, 0) == "i_900_0.tga" && e.icon(0, 0) == "i_901_0.tga" && e.filled(2) == 0);
    // Foreign orbit: a slot-less own fleet takes the first empty pane (pane 0 holds no foreign fleet here).
    e.build({make_fleet(900, -1, false, 1), make_fleet(901, 1, false, 1)}, false);
    CHECK(e.icon(1, 0) == "i_901_0.tga" && e.icon(0, 0) == "i_900_0.tga" && e.filled(2) == 0);
    // UI.md §7.2 first row: in the local player's orbit a non-local (e.g. allied) fleet fills no pane, even a free one.
    e.build({make_fleet(904, 2, true, 1), make_fleet(901, 1, false, 1)}, true);
    CHECK(e.filled(0) == 0 && e.icon(1, 0) == "i_901_0.tga" && e.filled(2) == 0 && e.hud.stats().fleet_slots == 1);
    // More fleets than panes: the surplus is not shown.
    e.build({make_fleet(900, -1, false, 1), make_fleet(901, -1, false, 1), make_fleet(902, -1, false, 1), make_fleet(903, -1, false, 1)}, true);
    CHECK(e.filled(0) == 1 && e.filled(1) == 1 && e.filled(2) == 1 && e.hud.stats().fleet_slots == 3);
}

// UI.md §7.4: q{n}_0000 is the header pad, never a unit pad: units start at q{n}_0001.
void test_header_pad() {
    Env e;
    e.build({make_fleet(900, 0, false, 2)}, true);
    CHECK(!e.hidden("q1_0001") && !e.hidden("q1_0002") && e.hidden("q1_0003"));
    // The header pad shows in every pane, empty and inert: no drag, no click, no drop target.
    CHECK(!e.hidden("q1_0000") && !e.hidden("q2_0000") && !e.hidden("q3_0000") && e.icon(0, -1).empty());
    CHECK(!e.hud.draggable(e.id("q1_0000")) && e.click("q1_0000").empty());
    e.build({make_fleet(900, 0, false, 2), make_fleet(901, 1, false, 1)}, true);
    bool valid = true;
    CHECK(e.drag("q2_0001", "q1_0000", &valid).empty() && !valid);
}

// UI.md §7.2: units of one fleet with the same type share a pad with the count; other types and kNoSlotType units do not.
void test_stacks() {
    Env e;
    GalacticFleet f = make_fleet(900, 0, false, 0);
    auto unit = [&](uint32_t obj, const char* icon, uint32_t type) { f.units.push_back({obj, icon, 1, false, type}); };
    unit(9001, "i_900_0.tga", 7);
    unit(9002, "i_900_1.tga", 8);
    unit(9003, "i_900_0.tga", 7);
    unit(9004, "i_900_0.tga", 7);
    unit(9005, "i_900_1.tga", kNoSlotType);
    unit(9006, "i_900_1.tga", kNoSlotType);
    e.build({f}, true);
    CHECK(e.filled(0) == 3 && e.icon(0, 0) == "i_900_0.tga" && e.icon(0, 1) == "i_900_1.tga" && e.icon(0, 2) == "i_900_1.tga");
    CHECK(e.hud.stats().fleet_slots == 3);
    auto text = [&](int pad) { return e.hud.buttons().find_state(static_cast<uint32_t>(e.id(e.pad_name(0, pad))))->text; };
    CHECK(text(0) == U"3" && text(1).empty() && text(2).empty());
    // Clicking a stack selects all its members; dragging it takes its first unit only.
    const std::vector<HudAction> a = e.click("q1_0001");
    CHECK(a.size() == 1 && a[0].ids == std::vector<uint32_t>({9001, 9003, 9004}));
    // Two fleets do not share stacks even with equal types (a drag names one fleet).
    GalacticFleet g = make_fleet(901, 0, false, 0);
    g.units.push_back({9101, "i_901_0.tga", 7, false, 7});
    e.build({f, g}, true);
    CHECK(e.filled(0) == kPad);  // 3 stacks of f fill the 3 pads; fleet 901's stack has no pad left
    bool valid = false;
    e.build({make_fleet(902, 1, false, 1), g}, true);
    const std::vector<commands::EventData> o = e.drag("q1_0001", "q2_0001", &valid);
    CHECK(valid && o.size() == 1 && o[0] == fleet_management_event(commands::kFleetMerge, 902, 901, 9101));
}

// UI.md §7.3: empty pads show only while dragging; a drop on an empty pane makes a new fleet in that slot, on a pad of an
// occupied pane it merges.
void test_empty_pads() {
    Env e;
    e.build({make_fleet(900, 0, false, 2), make_fleet(901, 1, false, 1)}, true);
    CHECK(e.hidden("q1_0003") && e.hidden("q2_0002") && e.hidden("q3_0001"));
    // Drag unit 9011 (fleet 901, pane 1) onto the empty pane 2: detach into a new fleet in slot 2.
    bool valid = false;
    std::vector<commands::EventData> o = e.drag("q2_0001", "q3_0001", &valid);
    commands::EventData want = fleet_management_event(commands::kFleetDetach, 100, 901, 9011);
    want.set("b", 2u);
    CHECK(valid && o.size() == 1 && o[0] == want);
    // The empty pads are gone again after the drop.
    CHECK(e.hidden("q3_0001") && e.hidden("q1_0003"));
    // A drop on an empty pad of an occupied pane merges into that pane's fleet; of its own fleet it does nothing.
    o = e.drag("q2_0001", "q1_0003", &valid);
    CHECK(valid && o.size() == 1 && o[0] == fleet_management_event(commands::kFleetMerge, 900, 901, 9011));
    o = e.drag("q1_0001", "q1_0003", &valid);
    CHECK(!valid && o.empty());
    // The whole fleet (its header) dropped on an empty pane changes its slot (op 8).
    o = e.drag("q2_fleet", "q3_0002", &valid);
    CHECK(valid && o.size() == 1 && o[0] == fleet_management_event(commands::kFleetSlot, 100, 901, commands::kNoObject, commands::kNoObject, 2));
    // A foreign pane is no target, empty pads included.
    e.build({make_fleet(906, 0, true, 1), make_fleet(901, 1, false, 1)}, false);
    o = e.drag("q2_0001", "q1_0002", &valid);
    CHECK(!valid && o.empty());
}

// The organiser feeds nothing while it is closed.
void test_closed() {
    Env e;
    e.frame.organize_open = false;
    e.build({make_fleet(900, 0, false, 1)}, true);
    CHECK(!e.hud.stats().organize_shown && e.hud.stats().fleet_slots == 0 && e.filled(0) == 0);
}

}  // namespace

namespace {

// [port: Gabriel, 2026-11-13] fleet / ship names: the header's and a unit pad's tooltip text; a named unit is not merged
// into its type's stack; a double click on an own pad asks for a rename (HudAction::Rename), a foreign pad does not.
void test_names() {
    Env e;
    GalacticFleet f = make_fleet(900, 1, false, 3);
    for (GalacticFleetSlot& u : f.units) u.type = 7;  // one type: an unnamed fleet would be a single stack of 3
    f.name = U"Home Fleet";
    f.units[1].name = U"Avenger";
    e.build({f}, true);
    const auto state = [&](const std::string& n) { return e.hud.buttons().find_state(static_cast<uint32_t>(e.id(n))); };
    CHECK(state("q2_fleet") && state("q2_fleet")->tooltip_text == U"Home Fleet");
    CHECK(e.filled(1) == 2);  // the stack of the two unnamed units, then the named one on its own pad
    CHECK(state(Env::pad_name(1, 0))->text == U"2" && state(Env::pad_name(1, 0))->tooltip_text.empty());
    CHECK(state(Env::pad_name(1, 1))->text.empty() && state(Env::pad_name(1, 1))->tooltip_text == U"Avenger");
    CHECK(e.icon(1, 1) == "i_900_1.tga");

    const auto renames = [](const std::vector<HudAction>& acts) {
        std::vector<HudAction> out;
        for (const HudAction& a : acts)
            if (a.kind == HudAction::Kind::Rename) out.push_back(a);
        return out;
    };
    // A double click on the named unit's pad: one Rename for that unit (value 0), after the usual select.
    CHECK(renames(e.click(Env::pad_name(1, 1), 3.0)).empty());
    std::vector<HudAction> r = renames(e.click(Env::pad_name(1, 1), 3.3));
    CHECK(r.size() == 1 && r[0].unit == 9002 && r[0].value == 0 && r[0].ids == std::vector<uint32_t>{900});
    CHECK(renames(e.click(Env::pad_name(1, 1), 3.5)).empty());  // a third click starts over
    // The header: the fleet (value 1). Too slow a second click is no double click.
    CHECK(renames(e.click("q2_fleet", 5.0)).empty());
    CHECK(renames(e.click("q2_fleet", 5.7)).empty());
    r = renames(e.click("q2_fleet", 5.9));
    CHECK(r.size() == 1 && r[0].unit == 900 && r[0].value == 1);
    // Two different pads are no double click.
    CHECK(renames(e.click(Env::pad_name(1, 0), 7.0)).empty());
    CHECK(renames(e.click(Env::pad_name(1, 1), 7.1)).empty());

    // A foreign fleet's pads are inert: no rename.
    Env g;
    GalacticFleet x = make_fleet(901, 0, true, 1);
    x.name = U"Pirates";
    g.build({x}, false);
    CHECK(renames(g.click(Env::pad_name(0, 0), 1.0)).empty() && renames(g.click(Env::pad_name(0, 0), 1.1)).empty());
}

// [port] the rename prompt: typing, the 32-character cap, Backspace, Enter / Esc, UTF-8 out, drawing only while open.
void test_rename_prompt() {
    using R = RenamePrompt::Result;
    RenamePrompt p;
    CHECK(!p.active() && p.update(U"x", false, true, false) == R::None);
    p.open(9002, false, "Avenger");
    CHECK(p.active() && p.object() == 9002 && !p.fleet() && p.text() == U"Avenger");
    CHECK(p.update(U"", true, false, false) == R::None && p.text() == U"Avenge");
    CHECK(p.update(U"r É", false, false, false) == R::None && p.text() == U"Avenger É");
    CHECK(p.utf8() == std::string("Avenger \xC3\x89"));
    CHECK(p.update(U"\x01", false, false, false) == R::None && p.text() == U"Avenger É");  // controls dropped
    CHECK(p.update(U"", false, true, false) == R::Submit && !p.active());
    p.open(900, true, "");
    CHECK(p.fleet() && p.text().empty());
    CHECK(p.update(std::u32string(40, U'a'), false, false, false) == R::None && p.text().size() == commands::kMaxObjectNameChars);
    CHECK(p.update(U"", false, false, true) == R::Cancel && !p.active());
    // Drawn only while open.
    std::unique_ptr<GlyphRasterizer> raster = make_box_rasterizer();
    FontCache fonts{*raster};
    HudCanvas canvas;
    resize_canvas(canvas, &fonts, 1024, 768);
    HudDrawList closed, open;
    p.build(closed, fonts, canvas, 0.0);
    p.open(900, true, "Home Fleet");
    p.build(open, fonts, canvas, 0.0);
    CHECK(closed.total() == 0 && open.total() > 0);
}

}  // namespace

int main() {
    test_one_fleet();
    test_names();
    test_rename_prompt();
    test_slots_to_panes();
    test_slot0_merge();
    test_pane_overflow();
    test_foreign_orbit();
    test_unknown_slots();
    test_header_pad();
    test_stacks();
    test_empty_pads();
    test_closed();
    if (g_fail) {
        std::printf("alamo_ui_organizer_tests: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("alamo_ui_organizer_tests: all passed\n");
    return 0;
}
