// test_gui_screens: tests split from test_gui_screens.cpp (PAF t08).
#include "test_gui_screens_common.hpp"

namespace test_gui_screens_t {

// ---- mouse cursor ---------------------------------------------------------------------------------------------------
void test_cursor() {
    const char* xml = R"(<MousePointers>
      <MousePointer Name="POINTER_NORMAL"><Base_Texture> DEFAULTPOINTER_00.TGA </Base_Texture><Hot_X>0</Hot_X><Hot_Y>0</Hot_Y><Anim_Frame_Delay>5</Anim_Frame_Delay></MousePointer>
      <MousePointer Name="POINTER_ATTACK"><Base_Texture>ATTACKPOINTER_00.TGA</Base_Texture><Hot_X>16</Hot_X><Hot_Y>16</Hot_Y><Anim_Frame_Delay>3</Anim_Frame_Delay></MousePointer>
      <MousePointer Name="POINTER_MOVIE_MODE"><Base_Texture>MOVIEPOINTER.TGA</Base_Texture><Hot_X>0</Hot_X><Hot_Y>0</Hot_Y></MousePointer>
    </MousePointers>)";
    const data::XmlDocument doc = data::XmlDocument::parse(std::string_view(xml), "MousePointers.xml");
    CursorTable t;
    CHECK(t.parse(doc.root()) == 3);
    TextureAtlas atlas;
    atlas.texture_width = atlas.texture_height = 256;
    for (const char* n : {"DEFAULTPOINTER_00.TGA", "ATTACKPOINTER_00.TGA", "ATTACKPOINTER_01.TGA", "ATTACKPOINTER_02.TGA"})
        atlas.add({n, 0, 0, 32, 32, true});
    CHECK(t.resolve_frames([&](std::string_view n) { return atlas.find(n) != nullptr; }) == 5);
    CHECK(t.find(Pointer::Attack)->frames.size() == 3 && t.find(Pointer::Attack)->frames[2] == "ATTACKPOINTER_02.TGA" &&
          t.find(Pointer::MovieMode)->frames.size() == 1);
    Pointer p;
    CHECK(pointer_from_name("pointer_repair_hardpoint", p) && p == Pointer::RepairHardpoint && kPointerCount == 53);
    // Context -> pointer.
    CursorContext c;
    CHECK(choose_pointer(c) == Pointer::Normal);
    c.hover = CursorContext::Hover::Enemy;
    CHECK(choose_pointer(c) == Pointer::Normal);  // nothing selected
    c.selection = true;
    CHECK(choose_pointer(c) == Pointer::Attack);
    c.in_range = false;
    CHECK(choose_pointer(c) == Pointer::AttackOutOfRange);
    c.hover = CursorContext::Hover::Ground;
    c.passable = false;
    CHECK(choose_pointer(c) == Pointer::CantMove);
    c.targeting = CursorContext::Targeting::AbilityEnemy;
    c.target_valid = false;
    CHECK(choose_pointer(c) == Pointer::AbilityEnemyInvalid);
    c.over_ui = true;
    c.over_draggable = true;
    CHECK(choose_pointer(c) == Pointer::CanGrab);
    c.fleet_drag = true;
    c.drop_on_planet = true;
    CHECK(choose_pointer(c) == Pointer::Drop);
    c.busy = true;
    CHECK(choose_pointer(c) == Pointer::Wait);
    // Animation: 3 frames, 3 ticks (0.1 s) each; restarts on change.
    MouseCursor mc;
    mc.init(t, &atlas, {});
    mc.update(Pointer::Attack, 10.0);
    CHECK(mc.frame(10.05) == 0 && mc.frame(10.15) == 1 && mc.frame(10.25) == 2 && mc.frame(10.35) == 0);
    CHECK(mc.texture(10.15) == "ATTACKPOINTER_01.TGA");
    const HudCanvas cv = HudCanvas::make(1024, 768);
    HudDrawList dl;
    CHECK(mc.build(dl, cv, {512, 384}, 10.0));
    const auto q = dl.items(kHudLayers - 1, HudList::Alpha);
    CHECK(q.size() == 1 && std::fabs(q[0].min.x + 16.0f) < 1e-3f && std::fabs(q[0].max.y - 16.0f) < 1e-3f &&
          std::fabs(q[0].max.x - 16.0f) < 1e-3f);  // hot spot 16,16 at the cursor
    mc.update(Pointer::CantGrab, 11.0);  // no entry -> falls back to POINTER_NORMAL
    dl.clear();
    CHECK(mc.build(dl, cv, {0, 0}, 11.0) && mc.texture(11.0) == "DEFAULTPOINTER_00.TGA");
}

// ---- Can_Animate, hack panel, fleet drag and drop -------------------------------------------------------------------
void test_anim_hack_drag() {
    PanelEnv env;
    for (const char* n : {"i_anim_static.tga", "i_anim_00.tga", "i_anim_01.tga", "i_anim_02.tga"})
        env.atlas.add({n, static_cast<uint32_t>(env.atlas.size()) * 8u, 0, 8, 8, true});
    const CommandBarComponent& ab = *env.table.get("anim_button");
    const CommandBarComponent& au = *env.table.get("anim_upper");
    CHECK(ab.can_animate && ab.anim_fps == 5.0f);
    // anim_button: 5 fps, 3 frames, one pass; anim_upper: 10 fps ping-pong 0 1 2 1 0 1 ..., looping.
    CHECK(anim_frame(ab, 1.0, 1.0, 3, false) == 0 && anim_frame(ab, 1.0, 1.21, 3, false) == 1 && anim_frame(ab, 1.0, 1.45, 3, false) == 2 &&
          anim_frame(ab, 1.0, 1.61, 3, false) == -1 && anim_frame(ab, 1.0, 1.61, 3, true) == 0);
    CHECK(anim_frame(au, 0.0, 0.25, 3, true) == 2 && anim_frame(au, 0.0, 0.35, 3, true) == 1 && anim_frame(au, 0.0, 0.45, 3, true) == 0 &&
          anim_frame(au, 0.0, 0.55, 3, true) == 1);
    HudButtons b;
    b.bind(env.table);
    const auto ci = static_cast<uint32_t>(env.table.find("anim_button"));
    const HudCanvas cv = HudCanvas::make(1024, 768);
    auto icon_u = [&](double now, HudList list) {
        HudDrawList dl;
        b.draw(dl, env.atlas, nullptr, cv, ci, {0, 0}, {}, 1, now);
        const auto q = dl.items(0, list);
        return q.empty() ? -1.0f : q.back().uv0.x * 512.0f;
    };
    const float idle = icon_u(0.0, HudList::Alpha);
    b.play_anim(ci, 5.0);
    const float f1 = icon_u(5.25, HudList::Alpha);
    CHECK(idle != f1 && f1 == static_cast<float>(env.atlas.find("i_anim_01.tga")->x));
    CHECK(icon_u(6.0, HudList::Alpha) == idle);  // one pass done
    const auto cu = static_cast<uint32_t>(env.table.find("anim_upper"));
    b.play_anim(cu, 0.0, true);
    HudDrawList dl;
    b.draw(dl, env.atlas, nullptr, cv, cu, {0, 0}, {}, 1, 0.05);
    // static icon, then the frame over it (later in the same PrimAlpha list)
    CHECK(dl.items(0, HudList::Alpha).size() == 2 && dl.items(0, HudList::Alpha)[1].uv0.x * 512.0f != dl.items(0, HudList::Alpha)[0].uv0.x * 512.0f);
    b.stop_anim(cu);
    dl.clear();
    b.draw(dl, env.atlas, nullptr, cv, cu, {0, 0}, {}, 1, 0.05);
    CHECK(dl.items(0, HudList::Alpha).size() == 1);

    // Hack panel.
    HackPanel hp;
    CHECK(hp.init(env.table, env.atlas, env.fonts));
    HackFrame hf;
    hf.open = true;
    hf.target = 9001;
    hf.planet = 600;
    hf.title = U"Hack";
    hf.lines = {U"Disable the shield generator"};
    hf.cost = U"500";
    hp.build(hf);
    std::vector<commands::EventData> cmds;
    std::vector<HudAction> acts;
    env.click(hp, "hack_hack", cmds, acts);
    env.click(hp, "hack_close", cmds, acts);
    CHECK(acts.size() == 2 && acts[0].kind == HudAction::Kind::HackConfirm && acts[0].unit == 9001 && acts[0].ids == std::vector<uint32_t>{600} &&
          acts[1].kind == HudAction::Kind::PanelClose && acts[1].name == "hack_shell" && cmds.empty());

    // Fleet drag and drop on the galactic HUD.
    ComponentTable ft;
    const data::XmlDocument fd = data::XmlDocument::parse(std::string_view(kFleetXml), "f.xml");
    ft.parse(fd.root());
    TextureAtlas fa;
    fa.texture_width = fa.texture_height = 64;
    fa.add({"i_big.tga", 0, 0, 30, 30, true});
    fa.add({"i_frame.tga", 0, 0, 40, 40, true});
    fa.add({"fleet_stacks_01.tga", 0, 0, 36, 45, true});
    fa.add({"fleet_empty.tga", 0, 0, 36, 45, true});
    GalacticHud hud;
    hud.init(ft, fa, env.fonts);
    hud.resize(1024, 768);
    GalacticCameraController cam;
    std::vector<GalacticPlanetInfo> planets(2);
    planets[0].planet = 100;
    planets[0].position = {200, 100, 0};
    planets[0].selected = true;
    planets[1].planet = 101;
    planets[1].position = {350, 200, 0};
    GalacticHudFrame gf;
    gf.view = make_view(cam.params(4.0f / 3.0f), 1024, 768);
    gf.planets = planets;
    gf.providers.fleets = [](uint32_t, std::vector<GalacticFleet>& out) {
        out.push_back({900, {{901, "i_frame.tga", 1, false}, {902, "i_frame.tga", 1, false}}});
        out.push_back({950, {{951, "i_frame.tga", 1, false}}});
    };
    gf.providers.land_forces = [](uint32_t, std::vector<GalacticFleetSlot>& out) { out.push_back({701, "i_frame.tga", 1, false}); };
    hud.build(gf);
    CHECK(hud.draggable(ft.find("q1_0000")) && hud.draggable(ft.find("q1_fleet")) && hud.draggable(ft.find("land_fleet_00")));
    auto center_px = [&](std::string_view name) {
        const int32_t c = ft.find(name);
        for (const HudHitBox& h : hud.hit_boxes())
            if (h.component == c) return hud.canvas().canvas_to_pixel(0.5f * (h.min.x + h.max.x), 0.5f * (h.min.y + h.max.y));
        return Vec2f{-1, -1};
    };
    auto drag = [&](std::string_view from, Vec2f to, int32_t to_component) {
        const int32_t fc = ft.find(from);
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
        r.world.mouse_x = static_cast<int32_t>(to.x);
        r.world.mouse_y = static_cast<int32_t>(to.y);
        r.hover_component = to_component;
        hud.update_input(r, false, false, 1.1, out);
        CHECK(hud.drag().active && hud.drag().component == fc);
        hud.build(gf);  // the dragged icon follows the cursor
        CHECK(!hud.draw_list().items(kHudLayers - 1, HudList::Alpha).empty());
        r.owner = {};
        r.hud = {{HudPointerEvent::Kind::Release, Button::Left, to_component, r.world.mouse_x, r.world.mouse_y}};
        hud.update_input(r, false, false, 1.2, out);
        CHECK(!hud.drag().active);
        hud.build(gf);
        return out;
    };
    // unit 902 of fleet 900 onto fleet 950's pad -> merge (op 3) {target 950, fleet 900, unit 902}
    std::vector<commands::EventData> o = drag("q1_0001", center_px("q2_0000"), ft.find("q2_0000"));
    CHECK(o.size() == 1 && o[0] == fleet_management_event(950, 900, 902, 3));
    // fleet 900 header onto the other planet -> MoveObjectToObject(900, 101)
    float sx = 0, sy = 0;
    CHECK(project(gf.view, planets[1].position, sx, sy));
    o = drag("q1_fleet", {sx, sy}, -1);
    CHECK(o.size() == 1 && o[0] == commands::ev::move_object_to_object(900, 101));
    // fleet header onto its own planet -> nothing; land unit onto a fleet -> launch (op 4)
    CHECK(project(gf.view, planets[0].position, sx, sy));
    CHECK(drag("q1_fleet", {sx, sy}, -1).empty());
    o = drag("land_fleet_00", center_px("q1_0000"), ft.find("q1_0000"));
    CHECK(o.size() == 1 && o[0] == fleet_management_event(900, commands::kNoObject, 701, 4));
    // unit into empty space -> detach (op 2) into a new fleet at the selected planet
    o = drag("q1_0000", {1000, 10}, -1);
    CHECK(o.size() == 1 && o[0] == fleet_management_event(100, commands::kNoObject, 901, 2));
    // a short press (no drag) still clicks: GalacticSelect
    std::vector<HudAction> ga;
    std::vector<commands::EventData> co;
    RoutedInput r;
    const int32_t fc = ft.find("q1_0000");
    r.hover_component = fc;
    r.hud = {{HudPointerEvent::Kind::Press, Button::Left, fc, 0, 0}, {HudPointerEvent::Kind::Release, Button::Left, fc, 0, 0},
             {HudPointerEvent::Kind::Click, Button::Left, fc, 0, 0}};
    hud.update_input(r, false, false, 2.0, co, &ga);
    CHECK(co.empty() && ga.size() == 1 && ga[0].kind == HudAction::Kind::GalacticSelect);

    // GALAXY_MAP_PRESENTATION Â§1 map fleet buttons (one g_planet_fleet component, one instance per slot): a left click
    // picks the instance under the cursor -> GalacticSelect {fleet}; right click nothing; an own fleet drags to another
    // planet -> MoveObjectToObject; an enemy fleet does not drag; a land-forces button is click-only.
    {
        planets[0].orbit_local = true;
        GalacticPlanetStack own;
        own.slot = 2;
        own.units = 3;
        own.fleet = 960;
        GalacticPlanetStack foe;
        foe.slot = 0;
        foe.units = 2;
        foe.enemy = true;
        foe.fleet = 990;
        GalacticPlanetStack none;  // no fleet id: drawn, not clickable
        none.slot = 1;
        none.units = 1;
        GalacticPlanetStack land;
        land.land = true;
        land.units = 4;
        land.fleet = 970;
        planets[0].stacks = {own, foe, none, land};
        hud.build(gf);
        const int32_t gpf = ft.find("g_planet_fleet"), gpl = ft.find("g_planet_land_forces");
        std::vector<Vec2f> fleet_px;  // hit centres (pixels) in draw order: slot 0 (foe), slot 2 (own)
        Vec2f land_px{-1, -1};
        for (const HudHitBox& h : hud.hit_boxes()) {
            const Vec2f c = hud.canvas().canvas_to_pixel(0.5f * (h.min.x + h.max.x), 0.5f * (h.min.y + h.max.y));
            if (h.component == gpf) fleet_px.push_back(c);
            if (h.component == gpl) land_px = c;
            if (h.component == gpf) CHECK(std::fabs(h.max.x - h.min.x - 36.0f) < 0.01f && std::fabs(h.max.y - h.min.y - 45.0f) < 0.01f);
        }
        CHECK(fleet_px.size() == 2 && land_px.x >= 0 && hud.draggable(gpf) && !hud.draggable(gpl));
        // Right presses on these buttons go to the world (planet picking / right-click move); left presses stay on the HUD.
        CHECK(!hud.takes_button(gpf, Button::Right) && !hud.takes_button(gpl, Button::Right) && hud.takes_button(gpf, Button::Left) &&
              hud.takes_button(ft.find("q1_0000"), Button::Right));
        {
            InputRouter router;
            RouteHooks hooks;
            hooks.hud_hit = [&](int32_t, int32_t) { return gpl; };
            hooks.hud_takes = [&](int32_t c, Button b) { return hud.takes_button(c, b); };
            InputFrame raw;
            raw.width = 1024;
            raw.height = 768;
            raw.buttons_down = button_bit(Button::Right);
            raw.buttons_pressed = button_bit(Button::Right);
            RoutedInput ro;
            router.route(raw, hooks, ro);
            CHECK(ro.owner[static_cast<size_t>(Button::Right)] == PointerTarget::World && (ro.world.buttons_pressed & button_bit(Button::Right)) &&
                  ro.hud.empty());
            raw.buttons_down = static_cast<uint8_t>(button_bit(Button::Right) | button_bit(Button::Left));
            raw.buttons_pressed = button_bit(Button::Left);
            router.route(raw, hooks, ro);
            CHECK(ro.owner[static_cast<size_t>(Button::Left)] == PointerTarget::Hud);
        }
        auto click_at = [&](int32_t comp, Vec2f px, Button b) {
            std::vector<HudAction> acts;
            std::vector<commands::EventData> cmds;
            RoutedInput ri;
            const auto x = static_cast<int32_t>(px.x), y = static_cast<int32_t>(px.y);
            ri.world.mouse_x = x;
            ri.world.mouse_y = y;
            ri.hover_component = comp;
            ri.hud = {{HudPointerEvent::Kind::Press, b, comp, x, y}, {HudPointerEvent::Kind::Release, b, comp, x, y},
                      {HudPointerEvent::Kind::Click, b, comp, x, y}};
            hud.update_input(ri, false, false, 3.0, cmds, &acts);
            CHECK(cmds.empty());
            return acts;
        };
        if (fleet_px.size() == 2) {
            std::vector<HudAction> a = click_at(gpf, fleet_px[1], Button::Left);
            CHECK(a.size() == 1 && a[0].kind == HudAction::Kind::GalacticSelect && a[0].ids == std::vector<uint32_t>{960} &&
                  a[0].unit == 100 && a[0].value == 2);
            a = click_at(gpf, fleet_px[0], Button::Left);
            CHECK(a.size() == 1 && a[0].ids == std::vector<uint32_t>{990} && a[0].value == 0);
            CHECK(click_at(gpf, fleet_px[1], Button::Right).empty());
            a = click_at(gpl, land_px, Button::Left);
            CHECK(a.size() == 1 && a[0].ids == std::vector<uint32_t>{970} && a[0].value == -1);
            // Drag the own fleet onto planet 101.
            auto drag_stack = [&](Vec2f from, Vec2f to) {
                std::vector<commands::EventData> out;
                RoutedInput rr;
                rr.world.mouse_x = static_cast<int32_t>(from.x);
                rr.world.mouse_y = static_cast<int32_t>(from.y);
                rr.hover_component = gpf;
                rr.owner[static_cast<size_t>(Button::Left)] = PointerTarget::Hud;
                rr.hud = {{HudPointerEvent::Kind::Press, Button::Left, gpf, rr.world.mouse_x, rr.world.mouse_y}};
                hud.update_input(rr, false, false, 4.0, out);
                rr.hud.clear();
                rr.world.mouse_x = static_cast<int32_t>(to.x);
                rr.world.mouse_y = static_cast<int32_t>(to.y);
                rr.hover_component = -1;
                hud.update_input(rr, false, false, 4.1, out);
                const bool active = hud.drag().active;
                const bool ok = active && hud.drag().fleet == 960 && hud.drag().fleet_index == kMapFleetIndex && hud.drag().planet == 100 &&
                                hud.drag().valid;
                rr.owner = {};
                rr.hud = {{HudPointerEvent::Kind::Release, Button::Left, -1, rr.world.mouse_x, rr.world.mouse_y}};
                hud.update_input(rr, false, false, 4.2, out);
                hud.build(gf);
                return std::make_pair(active && ok, out);
            };
            float tx = 0, ty = 0;
            CHECK(project(gf.view, planets[1].position, tx, ty));
            auto [ok, out] = drag_stack(fleet_px[1], {tx, ty});
            CHECK(ok && out.size() == 1 && out[0] == commands::ev::move_object_to_object(960, 101));
            auto [ok2, out2] = drag_stack(fleet_px[0], {tx, ty});  // enemy fleet: no drag
            CHECK(!ok2 && out2.empty());
        }
        planets[0].stacks.clear();
        planets[0].orbit_local = false;
        hud.build(gf);
    }

    // Playtest 3 fleet slots (base 40.jpg): the free slots of every planet show as fleet_empty ovals with a fleet selected
    // or while dragging; drops on a slot issue FleetManagement / MoveObjectToObject (galactic_hud.hpp "Fleet-slot drops").
    {
        constexpr uint32_t kNone = commands::kNoObject;
        const int32_t gpf = ft.find("g_planet_fleet");
        planets[0].orbit_local = planets[1].orbit_local = true;
        GalacticPlanetStack a;  // the host feeds the sim's slots (playtest 3 t07); a drop only issues commands
        a.units = 3;
        a.fleet = 960;
        a.slot = 0;
        GalacticPlanetStack b2 = a;
        b2.fleet = 961;
        b2.slot = 1;
        planets[0].stacks = {a, b2};
        planets[1].stacks.clear();
        cam.set_look_at({275.0f, 150.0f, 0.0f});  // both planets on screen
        gf.view = make_view(cam.params(4.0f / 3.0f), 1024, 768);
        gf.show_fleet_slots = false;
        hud.build(gf);
        CHECK(!hud.stats().fleet_slots_shown && hud.stats().empty_slots == 0);
        gf.show_fleet_slots = true;
        hud.build(gf);
        CHECK(hud.stats().fleet_slots_shown && hud.stats().empty_slots == 1 + 3);
        // R5 (base 40.jpg): the empty ovals take the frame's tint (host: the local player's faction colour) at full alpha.
        gf.empty_slot_color = Rgba8{54, 134, 242, 255};
        hud.build(gf);
        int tinted = 0;
        for (const HudQuad& q : hud.draw_list().items(0, HudList::Alpha))
            if (q.color.r == 54 && q.color.g == 134 && q.color.b == 242 && q.color.a == 255) ++tinted;
        CHECK(tinted == 1 + 3);
        gf.empty_slot_color = Rgba8{};
        hud.build(gf);
        auto slot_px = [&](size_t pi, int k) {
            float sx = 0, sy = 0;
            CHECK(project(gf.view, planets[pi].position, sx, sy));
            const Vec2f at = hud.canvas().pixel_to_canvas(sx, sy);
            const Vec2f off[3] = {{-36, 42}, {0, 47}, {36, 42}};
            return hud.canvas().canvas_to_pixel(at.x + off[k].x, at.y + off[k].y);
        };
        // The fleet a click at a slot selects (0 = none / empty oval).
        auto fleet_at = [&](size_t pi, int k) -> uint32_t {
            const Vec2f p = slot_px(pi, k);
            std::vector<HudAction> acts;
            std::vector<commands::EventData> cmds;
            RoutedInput ri;
            const auto x = static_cast<int32_t>(p.x), y = static_cast<int32_t>(p.y);
            ri.world.mouse_x = x;
            ri.world.mouse_y = y;
            ri.hover_component = hud.hit_test(x, y);
            ri.hud = {{HudPointerEvent::Kind::Press, Button::Left, ri.hover_component, x, y},
                      {HudPointerEvent::Kind::Release, Button::Left, ri.hover_component, x, y},
                      {HudPointerEvent::Kind::Click, Button::Left, ri.hover_component, x, y}};
            hud.update_input(ri, false, false, 5.0, cmds, &acts);
            CHECK(cmds.empty());
            return acts.size() == 1 && acts[0].ids.size() == 1 && acts[0].value == k ? acts[0].ids[0] : 0u;
        };
        CHECK(hud.hit_test(static_cast<int32_t>(slot_px(0, 2).x), static_cast<int32_t>(slot_px(0, 2).y)) == gpf);  // empty oval
        CHECK(fleet_at(0, 0) == 960 && fleet_at(0, 1) == 961 && fleet_at(0, 2) == 0);
        // Map drag: press on a slot's fleet, move to `to`, release there (hover from the HUD hit test, as the router does).
        auto drag_map = [&](Vec2f from, Vec2f to, bool* valid) {
            std::vector<commands::EventData> out;
            RoutedInput rr;
            rr.world.mouse_x = static_cast<int32_t>(from.x);
            rr.world.mouse_y = static_cast<int32_t>(from.y);
            rr.hover_component = gpf;
            rr.owner[static_cast<size_t>(Button::Left)] = PointerTarget::Hud;
            rr.hud = {{HudPointerEvent::Kind::Press, Button::Left, gpf, rr.world.mouse_x, rr.world.mouse_y}};
            hud.update_input(rr, false, false, 6.0, out);
            rr.hud.clear();
            rr.world.mouse_x = static_cast<int32_t>(to.x);
            rr.world.mouse_y = static_cast<int32_t>(to.y);
            rr.hover_component = -1;
            hud.update_input(rr, false, false, 6.1, out);  // drag starts
            hud.build(gf);                                   // slots shown while dragging, the source slot blank
            rr.hover_component = hud.hit_test(rr.world.mouse_x, rr.world.mouse_y);
            hud.update_input(rr, false, false, 6.2, out);
            if (valid) *valid = hud.drag().active && hud.drag().valid;
            rr.owner = {};
            rr.hud = {{HudPointerEvent::Kind::Release, Button::Left, rr.hover_component, rr.world.mouse_x, rr.world.mouse_y}};
            hud.update_input(rr, false, false, 6.3, out);
            hud.build(gf);
            return out;
        };
        // 1. Whole fleet onto an empty slot of its planet -> op 8 {planet, fleet, slot}; it is drawn there, the other stays.
        bool valid = false;
        std::vector<commands::EventData> so = drag_map(slot_px(0, 0), slot_px(0, 2), &valid);
        CHECK(valid && so.size() == 1 && so[0] == fleet_management_event(100, 960, kNone, 8, 2));
        CHECK(fleet_at(0, 0) == 960 && fleet_at(0, 2) == 0);  // the HUD keeps no slot state: nothing moved yet
        a.slot = 2;                                            // the sim applied op 8; the host feeds the new slot
        planets[0].stacks = {a, b2};
        hud.build(gf);
        CHECK(fleet_at(0, 2) == 960 && fleet_at(0, 1) == 961 && fleet_at(0, 0) == 0);
        // 2. Onto another own fleet's slot at the same planet -> merge (op 3) {target, source, all}.
        so = drag_map(slot_px(0, 2), slot_px(0, 1), &valid);
        CHECK(valid && so.size() == 1 && so[0] == fleet_management_event(961, 960, kNone, 3));
        // 3. Onto an empty slot of another planet -> op 8 {planet, fleet, slot, a = destination} (the slot it takes on
        //    arrival), then MoveObjectToObject {fleet, planet}.
        so = drag_map(slot_px(0, 2), slot_px(1, 1), &valid);
        CHECK(valid && so.size() == 2 && so[0] == fleet_management_event(100, 960, kNone, 8, 1, 101) &&
              so[1] == commands::ev::move_object_to_object(960, 101));
        // 4. An organiser unit onto an empty slot of its planet -> detach (op 2); the new fleet takes that slot (not the
        //    first free one).
        planets[0].stacks = {b2};  // 961 in slot 1; 0 and 2 free
        hud.build(gf);
        so = drag("q1_0001", slot_px(0, 2), gpf);
        CHECK(so.size() == 1 && so[0] == fleet_management_event(100, 900, 902, 2).set("b", 2u));  // the sim gives the slot
        GalacticPlanetStack c = a;
        c.fleet = 905;
        c.slot = 2;
        planets[0].stacks = {b2, c};
        hud.build(gf);
        CHECK(fleet_at(0, 2) == 905 && fleet_at(0, 1) == 961 && fleet_at(0, 0) == 0);
        // 5. A land unit onto the empty slot -> op 6 {planet, unit, a = planet} (launched as a new fleet).
        so = drag("land_fleet_00", slot_px(0, 0), gpf);
        CHECK(so.size() == 1 && so[0] == fleet_management_event(100, kNone, 701, 6, 0, 100).set("b", 0u));
        // 6. An enemy fleet's slot at the same planet: no drop.
        GalacticPlanetStack foe = a;
        foe.fleet = 990;
        foe.enemy = true;
        foe.slot = 0;
        planets[0].stacks = {b2, c, foe};
        hud.build(gf);
        CHECK(fleet_at(0, 0) == 990);
        so = drag_map(slot_px(0, 1), slot_px(0, 0), &valid);
        CHECK(!valid && so.empty());
        // 7. Hover highlight on the slot under the cursor while the slots are shown.
        gf.cursor_px = slot_px(1, 0);
        hud.build(gf);
        CHECK(hud.stats().slot_highlights == 1);
        gf.cursor_px = {};
        gf.show_fleet_slots = false;
        planets[0].stacks.clear();
        hud.build(gf);
        CHECK(!hud.stats().fleet_slots_shown && hud.stats().empty_slots == 0);
    }
}

void test_controls() {
    // 1. Pause: the port extension event round trip, the hotkey (Pause key) toggling, and both HUD buttons.
    bool pz = false;
    CHECK(read_pause_event(pause_event(true), pz) && pz && read_pause_event(pause_event(false), pz) && !pz);
    CHECK(!read_pause_event(commands::ev::stop(), pz));
    CHECK(pause_event(true).type() == commands::EventType::PortExtension && pause_event(true).u("subtype") == kPortPauseSubtype);
    {
        HotkeyTable table;
        CHECK(table.binding(hk::kPause).key == vk::kPause && kHotkeySlots == 162 && hotkey_slot("Pause") == hk::kPause);
        HotkeyDispatcher disp;
        InputState in;
        std::vector<commands::EventData> cmds;
        std::vector<HotkeyEvent> evs;
        in.update(key_frame({}, {vk::kPause}, 1.0));
        disp.update(in, table, {}, 1.0, cmds, evs);
        in.update(key_frame({}, {}, 1.1));
        in.update(key_frame({}, {vk::kPause}, 1.2));
        disp.update(in, table, {}, 1.2, cmds, evs);
        CHECK(cmds.size() == 2 && cmds[0] == pause_event(true) && cmds[1] == pause_event(false) && !disp.paused());
        Bindings b;
        table.apply_to_bindings(b);
        CHECK(b.binding(Action::Pause, 0).key == vk::kPause);
        Bindings d;  // P1 defaults have the action too
        CHECK(d.binding(Action::Pause, 0).key == vk::kPause && action_name(Action::Pause) == "Pause");
    }
    {
        ComponentTable ft;
        const data::XmlDocument fd = data::XmlDocument::parse(std::string_view(kFleetXml), "f.xml");
        ft.parse(fd.root());
        TextureAtlas fa;
        fa.texture_width = fa.texture_height = 64;
        fa.add({"i_big.tga", 0, 0, 30, 30, true});
        fa.add({"i_frame.tga", 0, 0, 40, 40, true});
        auto raster = make_box_rasterizer();
        FontCache fonts(*raster);
        auto click = [](auto& hud, int32_t ci, std::vector<commands::EventData>& out) {
            RoutedInput r;
            r.hover_component = ci;
            r.hud = {{HudPointerEvent::Kind::Press, Button::Left, ci, 0, 0}, {HudPointerEvent::Kind::Release, Button::Left, ci, 0, 0},
                     {HudPointerEvent::Kind::Click, Button::Left, ci, 0, 0}};
            hud.update_input(r, false, false, 1.0, out);
        };
        GalacticHud g;
        g.init(ft, fa, fonts);
        g.resize(1024, 768);
        GalacticHudFrame gf;
        gf.paused = true;
        g.build(gf);
        std::vector<commands::EventData> out;
        click(g, ft.find("b_play_pause"), out);
        CHECK(out.size() == 1 && out[0] == pause_event(false));  // paused -> resume
        TacticalHud t;
        t.init(ft, fa, fonts);
        t.resize(1024, 768);
        TacticalHudFrame tf;
        t.build(tf, {}, {});
        bool on_hud = false;
        for (const HudHitBox& h : t.hit_boxes()) on_hud = on_hud || h.component == ft.find("b_play_pause_t");
        CHECK(on_hud);
        out.clear();
        click(t, ft.find("b_play_pause_t"), out);
        CHECK(out.size() == 1 && out[0] == pause_event(true));
        // Queue markers through the tactical HUD frame.
        TacticalCameraController cam;
        tf.world.view = make_view(cam.params(4.0f / 3.0f), 1024, 768);
        HudOrderQueue q;
        q.unit = 7;
        q.position = {0, 0, 0};
        q.steps = {{HudQueuedStep::Kind::Attack, {200, 100, 0}, true},
                   {HudQueuedStep::Kind::Ability, {}, false},
                   {HudQueuedStep::Kind::Move, {-150, 250, 0}, true}};
        tf.order_queues = std::span<const HudOrderQueue>(&q, 1);
        t.build(tf, {}, {});
        CHECK(t.stats().queue_markers == 2 && t.stats().queue_dots > 10);
        // the markers are numbered 1 and 3 (the pointless ability step keeps its place in the count)
        HudDrawList dl;
        const QueueMarkerStats qs = add_queue_markers(dl, &fonts, t.canvas(), tf.world.view, tf.order_queues, 1);
        CHECK(qs.markers == 2 && dl.items(1, HudList::Text).size() >= 2);
        bool red = false;
        for (const HudQuad& qd : dl.items(1, HudList::Alpha)) red = red || qd.color == queue_step_color(HudQueuedStep::Kind::Attack);
        CHECK(red);
    }

    // 2. Field of view: tactical override, galactic setting, the options dialog.
    {
        TacticalCameraController tc;
        tc.set_fov_override(80.0f);
        CHECK(std::fabs(tc.params(4.0f / 3.0f).fov_x - 80.0f * kDegToRad) < 1e-5f && tc.fov_deg() == 80.0f);
        tc.set_fov_override(0.0f);
        CHECK(tc.fov_deg() == 55.0f);
        GalacticCameraController gc;
        CHECK(std::fabs(gc.fov_deg() - 26.565f) < 1e-2f);
        gc.set_fov_deg(40.0f);
        CHECK(std::fabs(gc.params(1.5f).fov_x - 40.0f * kDegToRad) < 1e-5f);
    }
    {
        Env env;
        CHECK(env.script.merge(kPortDialogsRc) && env.gui.has_dialog(kPortControlsDialog));
        FrontEnd fe;
        HotkeyTable keys;
        Bindings binds;
        MenuProviders p;
        p.hotkeys = &keys;
        p.bindings = &binds;
        CameraSettings cs;
        cs.tactical_fov_deg = 70.0f;
        p.camera_settings = [&] { return cs; };
        fe.init(env.gui, {}, p);
        std::vector<MenuAction> acts;
        fe.update(std::vector<GuiEvent>{event(GuiEvent::Kind::Clicked, "IDD_MAIN_OPTIONS_DIALOG", "IDC_MAIN_OPTIONS_MODS")}, InputFrame{}, acts);
        CHECK(fe.top() == kPortControlsDialog);
        GuiDialog* d = env.gui.top();
        CHECK(d && d->get("IDC_PORT_TACTICAL_FOV_SLIDER")->value == 70 && d->get("IDC_PORT_GALACTIC_FOV_SLIDER")->value == 27 &&
              d->get("IDC_PORT_TACTICAL_FOV_SLIDER")->range_max == 110 && d->get("IDC_EDIT_PORT_PAUSE")->text == U"Pause" &&
              d->get("IDC_EDIT_PORT_SCROLL_UP")->text == U"W" && d->get("IDC_PORT_QUEUE_MODIFIER")->selected == 0);
        // Slider moves update the value label; key capture rebinds Pause to F9; the queue modifier becomes Ctrl.
        d->set_value("IDC_PORT_GALACTIC_FOV_SLIDER", 35);
        fe.update(std::vector<GuiEvent>{event(GuiEvent::Kind::ValueChanged, kPortControlsDialog, "IDC_PORT_GALACTIC_FOV_SLIDER", 35)},
                  InputFrame{}, acts);
        CHECK(d->get("IDC_PORT_GALACTIC_FOV_VALUE")->text == U"35");
        d->focus("IDC_EDIT_PORT_PAUSE");
        InputFrame kf;
        kf.keys_pressed.set(0x78);  // F9
        kf.keys_down.set(0x78);
        fe.update({}, kf, acts);
        CHECK(d->get("IDC_EDIT_PORT_PAUSE")->text == U"F9");
        d->set_selected("IDC_PORT_QUEUE_MODIFIER", 1);
        acts.clear();
        fe.update(std::vector<GuiEvent>{event(GuiEvent::Kind::Clicked, kPortControlsDialog, "IDC_BUTTON_ACCEPT")}, InputFrame{}, acts);
        CHECK(acts.size() == 2 && acts[0].kind == MenuAction::Kind::ApplyCamera && acts[0].camera.galactic_fov_deg == 35.0f &&
              acts[0].camera.tactical_fov_deg == 70.0f && acts[0].camera.queue_mods == kModCtrl &&
              acts[1].kind == MenuAction::Kind::KeybindingsChanged);
        CHECK(keys.binding(hk::kPause).key == 0x78 && binds.binding(Action::Pause, 0).key == 0x78 && binds.queue_mods() == kModCtrl);
        CHECK(env.gui.find(kPortControlsDialog) == nullptr);
    }
    // t36 (QA V6): every way out of Camera and Controls lands on the Options page that opened it, not the pause menu. Real
    // mouse clicks and Esc through the GUI; the pause menu stays underneath.
    {
        Env env;
        CHECK(env.script.merge(kPortDialogsRc));
        CHECK(env.script.merge("IDD_GAME_OPTIONS_DIALOG DIALOGEX 0, 0, 321, 328\nFONT 8, \"Arial\", 0, 0, 0x0\nBEGIN\n"
                               "    PUSHBUTTON \"o\",IDC_INGAME_OPTIONS,10,100,100,20\nEND\n"
                               "IDD_IN_GAME_OPTIONS_DIALOG DIALOGEX 0, 0, 321, 328\nFONT 8, \"Arial\", 0, 0, 0x0\nBEGIN\n"
                               "    PUSHBUTTON \"n\",IDC_IN_GAME_OPTIONS_NETWORK,10,100,100,20\n"
                               "    PUSHBUTTON \"b\",IDC_OK_IN_GAME,10,200,100,20\nEND\n"));
        FrontEnd fe;
        HotkeyTable keys;
        Bindings binds;
        MenuProviders p;
        p.hotkeys = &keys;
        p.bindings = &binds;
        fe.init(env.gui, {}, p);
        std::vector<MenuAction> acts;
        fe.show_pause_menu();
        fe.update(env.click("IDD_GAME_OPTIONS_DIALOG", "IDC_INGAME_OPTIONS"), InputFrame{}, acts);
        CHECK(fe.top() == "IDD_IN_GAME_OPTIONS_DIALOG");
        const auto open_controls = [&] {
            fe.update(env.click("IDD_IN_GAME_OPTIONS_DIALOG", "IDC_IN_GAME_OPTIONS_NETWORK"), InputFrame{}, acts);
            CHECK(fe.top() == kPortControlsDialog);
        };
        open_controls();
        fe.update(env.click(kPortControlsDialog, "IDC_BUTTON_ACCEPT"), InputFrame{}, acts);
        CHECK(fe.top() == "IDD_IN_GAME_OPTIONS_DIALOG");
        fe.update(env.frame({1.0f, 1.0f}, false), InputFrame{}, acts);  // the frame after: nothing more closes
        CHECK(fe.top() == "IDD_IN_GAME_OPTIONS_DIALOG");
        open_controls();
        fe.update(env.click(kPortControlsDialog, "IDC_CANCEL_BUTTON"), InputFrame{}, acts);
        CHECK(fe.top() == "IDD_IN_GAME_OPTIONS_DIALOG");
        open_controls();
        fe.update(env.frame({1.0f, 1.0f}, false, {vk::kEscape}), InputFrame{}, acts);
        CHECK(fe.top() == "IDD_IN_GAME_OPTIONS_DIALOG" && fe.pause_menu_open());
        fe.update(env.click("IDD_IN_GAME_OPTIONS_DIALOG", "IDC_OK_IN_GAME"), InputFrame{}, acts);
        CHECK(fe.top() == "IDD_GAME_OPTIONS_DIALOG");
    }

    // 2b. Gameplay options defaults (P1_CAMERA_INPUT.md Â§3.1): absolute scrolling on, scroll speed 1.0 at the default
    //     slider position; the Defaults button restores them. GalacticCameraController::set_tuning keeps the pose.
    {
        const GameplaySettings g;
        CHECK(g.absolute_scroll && !g.right_button_scroll && !g.both_buttons_rotate && g.scroll_speed == kDefaultScrollSpeedSlider);
        CHECK(scroll_speed_option_from_slider(kDefaultScrollSpeedSlider) == 1.0f && scroll_speed_option_from_slider(0) == 0.0f &&
              scroll_speed_option_from_slider(100) == 3.0f && scroll_speed_option_from_slider(500) == 3.0f &&
              scroll_speed_option_from_slider(66) > 1.9f && scroll_speed_option_from_slider(66) < 2.1f);
        Env env;
        CHECK(env.script.merge(R"RC(
IDD_GAMEPLAY_OPTIONS_DIALOG DIALOGEX 0, 0, 400, 200
STYLE DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP
CAPTION "Dialog"
FONT 8, "MS Sans Serif", 0, 0, 0x0
BEGIN
    CONTROL         "",IDC_SCROLL_SPEED_SLIDER,"msctls_trackbar32",TBS_BOTH | TBS_NOTICKS | WS_TABSTOP,10,10,200,18
    CONTROL         "Check",IDC_ABSOLUTE_SCROLLING,"Button",BS_AUTOCHECKBOX | WS_TABSTOP,10,40,100,12
    PUSHBUTTON      "TEXT_BUTTON_DEFAULTS",IDC_DEFAULTS_BUTTON,10,160,160,24
END
)RC"));
        FrontEnd fe;
        MenuProviders p;
        GameplaySettings cur;
        cur.scroll_speed = 90;
        cur.absolute_scroll = false;
        p.gameplay = [&] { return cur; };
        fe.init(env.gui, {}, p);
        std::vector<MenuAction> acts;
        fe.update(std::vector<GuiEvent>{event(GuiEvent::Kind::Clicked, "IDD_MAIN_OPTIONS_DIALOG", "IDC_MAIN_OPTIONS_GAMEPLAY")}, InputFrame{}, acts);
        GuiDialog* d = env.gui.top();
        CHECK(d && d->name() == "IDD_GAMEPLAY_OPTIONS_DIALOG");
        if (d && d->name() == "IDD_GAMEPLAY_OPTIONS_DIALOG") {
            CHECK(!d->get("IDC_ABSOLUTE_SCROLLING")->checked && d->get("IDC_SCROLL_SPEED_SLIDER")->value == 90);
            fe.update(std::vector<GuiEvent>{event(GuiEvent::Kind::Clicked, "IDD_GAMEPLAY_OPTIONS_DIALOG", "IDC_DEFAULTS_BUTTON")},
                      InputFrame{}, acts);
            CHECK(d->get("IDC_ABSOLUTE_SCROLLING")->checked && d->get("IDC_SCROLL_SPEED_SLIDER")->value == kDefaultScrollSpeedSlider);
        }
        GalacticCameraController gc;
        gc.set_bounds({-100, -100, 100, 100});
        gc.set_look_at({30, 40, 0});
        gc.set_fov_deg(40.0f);
        CameraTuning tu;
        tu.scroll_speed_option = 2.0f;
        tu.absolute_scrolling = false;
        gc.set_tuning(tu);
        CHECK(gc.tuning().scroll_speed_option == 2.0f && !gc.tuning().absolute_scrolling && gc.look_at().x == 30.0f &&
              gc.look_at().y == 40.0f && gc.fov_deg() == 40.0f && gc.limits().max_x == 115.0f && gc.zoom_state() == 0);
    }

    // 3. Free tactical camera: no clamps on yaw / pitch, roll, keys on every axis, consistent picking view.
    {
        TacticalCameraController c;  // Space_Mode, free rotation by default
        c.rotate(0.0f, 100.0f);
        CHECK(std::fabs(c.pitch_deg() - 150.0f) < 1e-3f);  // past the vertical (data max 85)
        c.rotate(400.0f, 0.0f);
        CHECK(std::fabs(c.yaw_deg() - 40.0f) < 1e-3f);  // wrapped, not clamped at 1000
        c.roll(30.0f);
        CameraParams p = c.params(4.0f / 3.0f);
        CHECK(std::fabs(p.roll - 30.0f * kDegToRad) < 1e-5f && !(p.up.x == 0.0f && p.up.y == 0.0f && p.up.z == 1.0f));
        // up is perpendicular to the view direction and unit length
        const Vec3 v{p.target.x - p.eye.x, p.target.y - p.eye.y, p.target.z - p.eye.z};
        const float vl = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        CHECK(std::fabs((v.x * p.up.x + v.y * p.up.y + v.z * p.up.z) / vl) < 1e-4f &&
              std::fabs(p.up.x * p.up.x + p.up.y * p.up.y + p.up.z * p.up.z - 1.0f) < 1e-4f);
        // the target projects to the screen centre; a point along +up projects above it
        const View view = make_view(p, 1024, 768);
        float sx = 0, sy = 0, ux = 0, uy = 0;
        CHECK(project(view, p.target, sx, sy) && std::fabs(sx - 512.0f) < 0.01f && std::fabs(sy - 384.0f) < 0.01f);
        CHECK(project(view, {p.target.x + p.up.x * 50, p.target.y + p.up.y * 50, p.target.z + p.up.z * 50}, ux, uy) && uy < sy - 10.0f &&
              std::fabs(ux - sx) < 1.0f);
        c.reset();
        CHECK(c.roll_deg() == 0.0f && c.pitch_deg() == 50.0f);
        // Land_Mode locks the pitch in data; the free camera pitches with the keys / roll keys.
        TacticalCameraController land(land_mode_defaults());
        Bindings b;
        InputState in;
        in.update(key_frame({vk::kUp}, {}, 1.0));
        land.update(in, b, 0.5f);  // 60 deg/s * 0.5 s on top of the spline pitch 55 (fraction 1 from the start)
        CHECK(std::fabs(land.pitch_deg() - 85.0f) < 1e-3f);
        in.update(key_frame({vk::kNumpad6}, {}, 1.5));
        land.update(in, b, 0.5f);
        CHECK(std::fabs(land.roll_deg() - 30.0f) < 1e-3f);
        // the original clamps stay available
        CameraTuning clamped;
        clamped.free_rotation = false;
        TacticalCameraController old(space_mode_defaults(), UiConstants{}.tactical, clamped);
        old.rotate(0.0f, 100.0f);
        old.roll(10.0f);
        CHECK(old.pitch_deg() == 85.0f && old.roll_deg() == 0.0f);
    }

    // 4. W A S D: P1 defaults and the hotkey table; displaced hotkeys; modified combinations keep their meaning.
    {
        Bindings b;
        InputState in;
        TacticalCameraController c;
        in.update(key_frame({'W'}, {}, 1.0));
        c.update(in, b, 0.5f);
        CHECK(c.look_at().y > 100.0f && std::fabs(c.look_at().x) < 1e-3f);  // forward (yaw 0 = +Y)
        c.set_look_at({0, 0, 0});
        in.update(key_frame({vk::kShift, 'A'}, {}, 1.5));  // any modifier: Shift+A still scrolls left
        c.update(in, b, 0.5f);
        CHECK(c.look_at().x < -100.0f);
        CHECK(b.binding(Action::Stop, 0).key == 'H' && b.binding(Action::SelectAll, 0).key == 'Y' &&
              b.binding(Action::SelectAll, 0).mods == kModCtrl);
        // P1 Bindings: no action other than movement on W / A / S / D
        for (size_t a = 0; a < kActionCount; ++a) {
            const auto act = static_cast<Action>(a);
            const bool move = act == Action::ScrollUp || act == Action::ScrollDown || act == Action::ScrollLeft || act == Action::ScrollRight;
            for (size_t s = 0; s < Bindings::kSlots; ++s) {
                const uint16_t k = b.binding(act, s).key;
                CHECK(move || !(k == 'W' || k == 'A' || k == 'S' || k == 'D'));
            }
        }
        HotkeyTable t;
        CHECK(t.binding(hk::kScrollUp) == (HotkeyBinding{'W', HotkeyMod::None}) && t.binding(hk::kScrollLeft) == (HotkeyBinding{'A', HotkeyMod::None}) &&
              t.binding(hk::kScrollDown) == (HotkeyBinding{'S', HotkeyMod::None}) && t.binding(hk::kScrollRight) == (HotkeyBinding{'D', HotkeyMod::None}));
        CHECK(t.binding(hk::kAttack).key == 'X' && t.binding(hk::kStop).key == 'H' && t.binding(hk::kPrevUnit).key == 'V' &&
              t.binding(hk::kWaypoint).key == 'L');
        // no other slot uses a plain W / A / S / D; every default binding is unique
        bool unique = true, wasd_only_scroll = true;
        for (uint16_t i = 0; i < kHotkeySlots; ++i) {
            const HotkeyBinding& bi = t.binding(i);
            if (bi.key < 0) continue;
            for (uint16_t j = static_cast<uint16_t>(i + 1); j < kHotkeySlots; ++j) unique = unique && !(t.binding(j) == bi);
            const bool wasd = bi.key == 'W' || bi.key == 'A' || bi.key == 'S' || bi.key == 'D';  // any modifier
            if (wasd && i != hk::kScrollUp && i != hk::kScrollDown && i != hk::kScrollLeft && i != hk::kScrollRight) wasd_only_scroll = false;
        }
        CHECK(unique && wasd_only_scroll);
        // Keymap conflicts (P host open item): after the sync no key + modifier drives two Bindings actions, End / Home
        // only reset / home the camera (hotkey slots 73 / 75), zoom is on = / - (slots 160 / 161), defaults == synced.
        {
            Bindings synced;
            t.apply_to_bindings(synced);
            Bindings defaults;
            bool no_clash = true, same = true;
            for (size_t a = 0; a < kActionCount; ++a)
                for (size_t s = 0; s < Bindings::kSlots; ++s) {
                    const KeyBinding& ka = synced.binding(static_cast<Action>(a), s);
                    if (ka.key == 0) continue;
                    for (size_t a2 = a + 1; a2 < kActionCount; ++a2)
                        for (size_t s2 = 0; s2 < Bindings::kSlots; ++s2) {
                            const KeyBinding& kb2 = synced.binding(static_cast<Action>(a2), s2);
                            const bool any = (ka.mods & kModAny) != 0 || (kb2.mods & kModAny) != 0;
                            if (kb2.key == ka.key && (any || kb2.mods == ka.mods)) no_clash = false;
                        }
                }
            for (Action a : {Action::ZoomIn, Action::ZoomOut, Action::ResetCamera})
                same = same && synced.binding(a, 0) == defaults.binding(a, 0) && defaults.binding(a, 1).key == 0;
            CHECK(no_clash && same);
            CHECK(synced.binding(Action::ZoomIn, 0) == (KeyBinding{vk::kOemPlus, kModNone}) &&
                  synced.binding(Action::ZoomOut, 0) == (KeyBinding{vk::kOemMinus, kModNone}) &&
                  synced.binding(Action::ResetCamera, 0).key == vk::kEnd && t.binding(hk::kCameraHome).key == vk::kHome &&
                  t.binding(hk::kZoomIn).key == vk::kOemPlus && hotkey_slot("CameraZoomOut") == hk::kZoomOut);
            for (Action a : {Action::ZoomIn, Action::ZoomOut})
                for (size_t s = 0; s < Bindings::kSlots; ++s)
                    CHECK(synced.binding(a, s).key != vk::kHome && synced.binding(a, s).key != vk::kEnd);
        }
        // An original 153-slot profile with the original defaults: scroll arrows -> W A S D, every W / A / S / D use moves
        // to its port default (Attack A -> X, Ctrl+A -> Ctrl+Y, Shift+W -> Shift+Q, Alt+D -> Alt+\ ...), the arrows stay on
        // the port yaw / pitch slots.
        HotkeyProfile prof;
        {
            HotkeyTable orig;
            const std::pair<const char*, HotkeyBinding> original[] = {
                {"ScrollUp", {vk::kUp, HotkeyMod::None}},          {"ScrollDown", {vk::kDown, HotkeyMod::None}},
                {"ScrollLeft", {vk::kLeft, HotkeyMod::None}},      {"ScrollRight", {vk::kRight, HotkeyMod::None}},
                {"Attack", {'A', HotkeyMod::None}},                {"Stop", {'S', HotkeyMod::None}},
                {"PreviousUnit", {'D', HotkeyMod::None}},          {"Waypoint", {'W', HotkeyMod::None}},
                {"SelectAll", {'A', HotkeyMod::Ctrl}},             {"SuperWeapon", {'W', HotkeyMod::Ctrl}},
                {"Ability_Deploy", {'D', HotkeyMod::Ctrl}},        {"Ability_StimPack", {'S', HotkeyMod::Ctrl}},
                {"Ability_SpoilerLock", {'W', HotkeyMod::Shift}},  {"Ability_DestroyPlanet", {'D', HotkeyMod::Shift}},
                {"Ability_SuperLaser", {'A', HotkeyMod::Shift}},   {"Ability_SpaceSelfDestruct", {'S', HotkeyMod::Shift}},
                {"Ability_ReplenishWingmen", {'W', HotkeyMod::Alt}}, {"Ability_ForceSight", {'S', HotkeyMod::Alt}},
                {"Ability_DrainLife", {'D', HotkeyMod::Alt}},      {"Ability_FoCFullSalvo", {'A', HotkeyMod::Alt}}};
            for (const auto& [name, kb] : original) orig.bind(static_cast<uint16_t>(hotkey_slot(name)), kb);
            orig.save(prof);
        }
        prof["KeySettingsCount"] = kOriginalHotkeySlots;
        HotkeyTable loaded;
        CHECK(loaded.load(prof));
        bool migrated = true;
        for (uint16_t i = 0; i < kHotkeySlots; ++i) migrated = migrated && loaded.binding(i) == t.binding(i);
        CHECK(migrated);  // == the new defaults
        // a customised scroll key is kept; a port slot it collides with is unbound
        HotkeyTable custom;
        custom.bind(hk::kScrollUp, {vk::kNumpad4, HotkeyMod::None});
        HotkeyProfile cprof;
        custom.save(cprof);
        cprof["KeySettingsCount"] = kOriginalHotkeySlots;
        HotkeyTable cl;
        CHECK(cl.load(cprof) && cl.binding(hk::kScrollUp).key == vk::kNumpad4 && cl.binding(hk::kRollLeft).key == -1 &&
              cl.binding(hk::kYawLeft).key == vk::kLeft);
    }

    // 5. Queued orders: right click with the queue modifier (Shift; rebindable) sets the flag, a plain click clears it.
    {
        GalacticCameraController gcam;
        OrderContext ctx;
        ctx.mode = UiMode::Galactic;
        ctx.view = make_view(gcam.params(4.0f / 3.0f), 1024, 768);
        const PickSphere planet{77, {0, 0, 0}, 40.0f, -1, PickKind::Planet};
        ctx.galactic_items = std::span<const PickSphere>(&planet, 1);
        float px = 0, py = 0;
        CHECK(project(ctx.view, planet.center, px, py));
        OrderTranslator ot;
        ot.set_selection({5});
        Bindings b;
        InputState in;
        auto right_click = [&](std::initializer_list<uint16_t> mods, double t) {
            InputFrame f = key_frame(mods, {}, t);
            f.mouse_x = static_cast<int32_t>(px);
            f.mouse_y = static_cast<int32_t>(py);
            f.buttons_down = button_bit(Button::Right);
            f.buttons_pressed = button_bit(Button::Right);
            in.update(f);
            ot.update(in, b, ctx);
            f.buttons_down = 0;
            f.buttons_pressed = 0;
            f.buttons_released = button_bit(Button::Right);
            f.time = t + 0.05;
            in.update(f);
            ot.update(in, b, ctx);
        };
        right_click({vk::kShift}, 1.0);
        right_click({}, 3.0);
        CHECK(ot.pending().size() == 2 && ot.pending_queued() == (std::vector<uint8_t>{1, 0}));
        std::vector<UiOrder> orders = ot.take_orders();
        CHECK(orders.size() == 2 && orders[0].queued && !orders[1].queued &&
              orders[0].event == commands::ev::move_object_to_object(5, 77) && ot.pending().empty());
        b.set_queue_mods(kModCtrl);  // rebound
        right_click({vk::kShift}, 5.0);
        right_click({vk::kControl}, 7.0);
        orders = ot.take_orders();
        CHECK(orders.size() == 2 && !orders[0].queued && orders[1].queued);
        b.set_queue_mods(0);
        right_click({vk::kControl}, 9.0);
        CHECK(ot.take_orders().size() == 1 && !b.queue_held(in));
    }
}

// ---- camera hotkeys --------------------------------------------------------------------------------------------
void test_camera_hotkeys() {
    TacticalCameraController cam(space_mode_defaults());
    cam.set_bounds({-3000, -3000, 3000, 3000});
    std::vector<HotkeyEvent> ev(4);
    ev[0].slot = hk::kTacticalOverview;
    ev[0].kind = HotkeyKind::Camera;
    ev[1].slot = 2;  // second recall of group 3
    ev[1].kind = HotkeyKind::SelectGroup;
    ev[1].group = 3;
    ev[1].camera_jump = true;
    ev[2].slot = hk::kScreenshot;
    ev[2].kind = HotkeyKind::Interface;
    ev[3].slot = hk::kCameraCentre;
    ev[3].kind = HotkeyKind::Camera;
    CameraFocus focus;
    focus.group = [](int8_t g) -> std::optional<Vec3> {
        if (g != 3) return std::nullopt;
        const Vec3 pts[2] = {{100, 200, 0}, {300, 400, 0}};
        return focus_point(pts);
    };
    const std::vector<uint16_t> handled = apply_camera_hotkeys(ev, cam, focus);
    CHECK(handled.size() == 3 && handled[0] == hk::kTacticalOverview && handled[1] == 2 && handled[2] == hk::kCameraCentre);
    CHECK(cam.overview_level() == 1 && cam.look_at().x == 200.0f && cam.look_at().y == 300.0f);
    // HotkeyDispatcher marks the second recall within 1 s.
    HotkeyTable table;
    HotkeyDispatcher disp;
    InputState in;
    InputFrame f;
    std::vector<commands::EventData> cmds;
    std::vector<HotkeyEvent> out;
    for (int k = 0; k < 4; ++k) {
        f.time = 1.0 + 0.1 * k;
        f.keys_pressed = {};
        f.keys_down = {};
        if (k % 2 == 0) {
            f.keys_pressed.set('3');
            f.keys_down.set('3');
        }
        in.update(f);
        disp.update(in, table, {}, f.time, cmds, out);
    }
    CHECK(out.size() == 1 && out[0].camera_jump && out[0].group == 3 && cmds.size() == 2);
}

}  // namespace test_gui_screens_t
