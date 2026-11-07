// test_gui_screens: tests split from test_gui_screens.cpp (PAF t08).
#include "test_gui_screens_common.hpp"

namespace test_gui_screens_t {

// HUD_GAPS Â§2.2: Encyclopedia_Delay 750 ms (Long_Encyclopedia_Delay 4000 ms with IDC_DELAY_TOOLTIPS), fade in / out at
// Encyclopedia_Fade_Rate 4.0 / s, Encyclopedia_Min_Display_Time 1 s, anchored on the hint bone (not the cursor).
void test_encyclopedia_fade_anchor() {
    PanelEnv env;
    auto near = [](float a, float b) { return std::fabs(a - b) < 1e-4f; };
    // Delay.
    {
        EncyclopediaPanel enc;
        enc.init(env.table, env.atlas, env.fonts);
        enc.update(-1, 42, 0.0);
        enc.update(-1, 42, 0.74);
        CHECK(enc.stage() == EncyclopediaPanel::Stage::Hidden);
        enc.update(-1, 42, 0.75);
        CHECK(enc.stage() == EncyclopediaPanel::Stage::Shown && enc.shown_object() == 42 && near(enc.alpha(), 0.0f));
        TooltipConfig slow;
        slow.delay_tooltips = true;
        CHECK(slow.encyclopedia_wait() == 4.0 && TooltipConfig{}.encyclopedia_wait() == 0.75);
        EncyclopediaPanel e2;
        e2.init(env.table, env.atlas, env.fonts);
        e2.set_config(slow);
        e2.update(-1, 42, 0.0);
        e2.update(-1, 42, 3.99);
        CHECK(e2.stage() == EncyclopediaPanel::Stage::Hidden);
        e2.update(-1, 42, 4.0);
        CHECK(e2.stage() == EncyclopediaPanel::Stage::Shown);
        // The countdown restarts when the subject changes or is lost.
        e2.update(-1, 0, 4.1);
        e2.update(-1, 43, 4.2);
        e2.update(-1, 43, 8.1);
        CHECK(e2.shown_object() != 43);
        e2.update(-1, 43, 8.2);
        CHECK(e2.shown_object() == 43 && e2.stage() == EncyclopediaPanel::Stage::Shown);
    }
    // Fade in at 4 / s from the expiry instant, independent of the frame steps.
    {
        EncyclopediaPanel a, b;
        for (EncyclopediaPanel* p : {&a, &b}) {
            p->init(env.table, env.atlas, env.fonts);
            p->update(-1, 7, 0.0);
        }
        a.update(-1, 7, 0.8);
        CHECK(near(a.alpha(), 0.2f));
        a.update(-1, 7, 0.875);
        CHECK(near(a.alpha(), 0.5f));
        b.update(-1, 7, 0.875);  // one step over the expiry
        CHECK(near(b.alpha(), 0.5f));
        a.update(-1, 7, 1.0);
        CHECK(near(a.alpha(), 1.0f));
        a.update(-1, 7, 3.0);
        CHECK(near(a.alpha(), 1.0f));
    }
    // Minimum display time 1 s, then fade out at 4 / s and destroy.
    {
        EncyclopediaPanel a, b;
        for (EncyclopediaPanel* p : {&a, &b}) {
            p->init(env.table, env.atlas, env.fonts);
            p->update(-1, 7, 0.0);
            p->update(-1, 7, 0.9);  // shown at 0.75, alpha 0.6
        }
        CHECK(near(a.alpha(), 0.6f));
        a.update(-1, 0, 1.0);  // lost: still rising until 1.75
        CHECK(near(a.alpha(), 1.0f) && a.shown_object() == 7);
        a.update(-1, 0, 1.75);
        CHECK(near(a.alpha(), 1.0f) && a.stage() == EncyclopediaPanel::Stage::Shown);
        a.update(-1, 0, 1.875);
        CHECK(near(a.alpha(), 0.5f) && a.shown_object() == 7);
        a.update(-1, 0, 2.0);
        CHECK(a.stage() == EncyclopediaPanel::Stage::Hidden && a.alpha() == 0.0f && a.shown_object() == 0);
        b.update(-1, 0, 1.875);  // one step: up to the cap, hold, then down
        CHECK(near(b.alpha(), 0.5f));
        // Lost long after the minimum time: fades out at once.
        EncyclopediaPanel c;
        c.init(env.table, env.atlas, env.fonts);
        c.update(-1, 7, 0.0);
        c.update(-1, 7, 5.0);
        c.update(-1, 0, 5.0625);
        CHECK(near(c.alpha(), 0.75f));
        c.update(-1, 0, 5.25);
        CHECK(c.stage() == EncyclopediaPanel::Stage::Hidden);
    }
    // GameConstants Encyclopedia_Fade_Rate / Encyclopedia_Min_Display_Time.
    {
        const data::XmlDocument doc = data::XmlDocument::parse(
            std::string_view("<GameConstants><Encyclopedia_Delay>750</Encyclopedia_Delay><Long_Encyclopedia_Delay>4000</Long_Encyclopedia_Delay>"
                             "<Encyclopedia_Fade_Rate>2.0</Encyclopedia_Fade_Rate><Encyclopedia_Min_Display_Time>0.5</Encyclopedia_Min_Display_Time>"
                             "</GameConstants>"),
            "gc.xml");
        const TooltipConfig c = tooltip_config_from(doc.root());
        CHECK(near(c.encyclopedia_fade_rate, 2.0f) && std::fabs(c.encyclopedia_min_display - 0.5) < 1e-9 &&
              std::fabs(c.encyclopedia_wait() - 0.75) < 1e-9);
    }
    // Alpha scales every colour; anchor = hint bone (bottom-left corner), not the cursor.
    {
        EncyclopediaEntry e;
        e.header = U"X-wing";
        e.icon = "i_button_vader.tga";
        e.cost = U"350";
        const HudCanvas cv = HudCanvas::make(1024, 768);
        EncyclopediaPanel enc;
        enc.init(env.table, env.atlas, env.fonts);
        enc.update(-1, 9, 0.0);
        enc.update(-1, 9, 0.875);  // alpha 0.5
        HudDrawList dl;
        enc.build(dl, cv, e, {500, 400});
        CHECK(!dl.items(9, HudList::Alpha).empty() && dl.items(9, HudList::Alpha)[0].color.a == 128);  // 255 x 0.5
        enc.update(-1, 9, 2.0);  // alpha 1
        HudLayout layout;
        const auto good = static_cast<uint32_t>(env.table.find("good_hero_frame"));
        const auto evil = static_cast<uint32_t>(env.table.find("evil_hero_frame"));
        ShellBone hb;
        hb.name = "Text_galactic_help";
        hb.x = 65.0f;
        hb.y = 309.0f;
        layout.set_shell_bones(good, {hb});
        ShellBone ab = hb;
        ab.name = "text_adv_help";  // case-insensitive
        ab.x = 300.0f;
        ab.y = 100.0f;
        layout.set_shell_bones(evil, {ab});
        layout.set_shell_active(evil, false);  // advanced map closed
        layout.update(cv, env.table);
        const Vec2f g = layout.shell_offset(good);
        const Vec2f at = enc.hint_anchor(layout, cv);
        CHECK(near(at.x, 65.0f + g.x) && near(at.y, 309.0f + g.y));
        dl.clear();
        const Vec2f sz = enc.build(dl, cv, e, layout);
        CHECK(!dl.items(9, HudList::Alpha).empty());
        if (!dl.items(9, HudList::Alpha).empty()) {
            CHECK(dl.items(9, HudList::Alpha).size() >= 2);
            const HudQuad& frame = dl.items(9, HudList::Alpha)[0];
            const HudQuad& back = dl.items(9, HudList::Alpha)[1];  // over the larger faction frame (only its rim shows)
            CHECK(near(frame.max.x - frame.min.x, 262.0f + 2.0f * kTooltipFrameMargin) && frame.min.x < back.min.x && frame.max.y > back.max.y);
            std::printf("encyclopedia hint anchor (%.1f, %.1f), panel %.0f x %.0f\n", static_cast<double>(at.x),
                        static_cast<double>(at.y), static_cast<double>(sz.x), static_cast<double>(sz.y));
            CHECK(near(back.min.x, at.x) && near(back.min.y, at.y) && near(back.max.x - back.min.x, 262.0f) && back.color.a == 255);  // opaque: nothing of the world shows through
        }
        // The cursor plays no part: the same frame twice is identical.
        HudDrawList dl2;
        enc.build(dl2, cv, e, layout);
        CHECK(dl2.digest() == dl.digest());
        // Advanced map open: its hint bone wins.
        layout.set_shell_active(evil, true);
        layout.update(cv, env.table);
        const Vec2f m = layout.shell_offset(evil);
        const Vec2f at2 = enc.hint_anchor(layout, cv);
        CHECK(near(at2.x, 300.0f + m.x) && near(at2.y, 100.0f + m.y));
        // No hint bone: [port] the galactic bone's base-canvas place.
        const Vec2f at3 = enc.hint_anchor(HudLayout{}, cv);
        CHECK(near(at3.x, -512.0f + 65.0f) && near(at3.y, -384.0f + 309.0f));
    }
}

void test_panels() {
    PanelEnv env;
    // Encyclopedia: hidden until Encyclopedia_Delay (0.75 s), then shown; subject change restarts (fade / anchor details:
    // test_encyclopedia_fade_anchor).
    EncyclopediaPanel enc;
    enc.init(env.table, env.atlas, env.fonts);
    enc.update(5, 0, 1.0);
    CHECK(enc.stage() == EncyclopediaPanel::Stage::Hidden);
    enc.update(5, 0, 1.76);
    CHECK(enc.stage() == EncyclopediaPanel::Stage::Shown && enc.shown_component() == 5);
    enc.update(-1, 77, 1.9);
    CHECK(enc.object() == 77 && enc.shown_component() == 5);  // the old pop-up stays / fades out, 77's countdown runs
    enc.update(-1, 77, 2.64);
    CHECK(enc.shown_component() == 5);  // min display until 2.75
    enc.update(-1, 0, 5.1);
    enc.update(-1, 0, 9.0);
    CHECK(enc.stage() == EncyclopediaPanel::Stage::Hidden);
    EncyclopediaEntry e;
    e.header = U"Darth Vader";
    e.icon = "i_button_vader.tga";
    e.cost = U"2500";
    e.rows = {{U"Health", U"3000", {}}, {U"Shields", U"0", {}}, {{}, {}, U"Hero"}};
    e.description = U"Dark Lord of the Sith. Force Crush, Force Push and Saber Throw make him deadly against infantry.";
    const HudCanvas cv = HudCanvas::make(1024, 768);
    HudDrawList dl;
    enc.update(3, 0, 10.0);
    CHECK(enc.build(dl, cv, e, {500, 400}).x == 0.0f && dl.total() == 0);  // hidden
    enc.update(3, 0, 10.8);
    const Vec2f s1 = enc.build(dl, cv, e, {500, 400});
    const size_t items1 = dl.total();
    CHECK(s1.x == 262.0f && s1.y > 3 * 14.0f && dl.items(9, HudList::Alpha).size() >= 3 && dl.items(9, HudList::Text).size() > 10);
    dl.clear();
    enc.update(3, 0, 14.5);
    const Vec2f s2 = enc.build(dl, cv, e, {1000, 10});  // near the top-right corner: kept inside the canvas
    CHECK(s2.y == s1.y && dl.total() == items1);
    for (const HudQuad& q : dl.items(9, HudList::Alpha)) CHECK(q.max.x <= 512.0f + 1e-3f && q.max.y <= 384.0f + 1e-3f);
    dl.clear();
    enc.build(dl, cv, e, {500, 400}, std::nullopt, 0, true);  // zoomed variant on layer 4
    CHECK(!dl.items(4, HudList::Alpha).empty() && dl.items(9, HudList::Alpha).empty());

    // QA U4 (HUD_GAPS Â§2.1): a long "Good against" value wraps in its own column, right of its label, and every glyph stays
    // inside the box; a long name wraps left of the cost; the box grows with the lines. Also pseudo-localised (+40 %).
    {
        const HudCanvas wcv = HudCanvas::make(1024, 768);
        EncyclopediaPanel wenc;
        wenc.init(env.table, env.atlas, env.fonts);
        wenc.update(-1, 9, 0.0);
        wenc.update(-1, 9, 2.0);
        const std::u32string name = U"The Accuser - Captain Piett of the Imperial Fleet and Other Very Long Titles";
        const std::u32string value = U"Corellian Corvette, Corellian Gunship, Interceptor IV Frigate, Carrack Cruiser, Marauder Missile Cruiser";
        const std::u32string word(120, U'W');  // a lone word wider than the box
        EncyclopediaEntry shrt, lng;
        shrt.header = U"X";
        shrt.cost = U"5000";
        EncyclopediaRow r;
        r.left = U"Good against";
        r.right = U"X-wing";
        shrt.rows.push_back(r);
        lng = shrt;
        lng.header = name;
        lng.rows[0].right = value;
        EncyclopediaRow c;
        c.center = word;
        lng.rows.push_back(c);
        const Vec2f at{-200.0f, 200.0f};
        HudDrawList ds, wdl;
        const Vec2f one_line = wenc.build(ds, wcv, shrt, {0.0f, 0.0f}, at);
        const Vec2f big = wenc.build(wdl, wcv, lng, {0.0f, 0.0f}, at);
        CHECK(std::fabs(big.x - one_line.x) < 1e-4f && big.y > one_line.y + 14.0f);
        const float left = at.x, right = at.x + big.x;
        float label_right = -1e9f, value_left = 1e9f;
        const std::span<const HudQuad> text = wdl.items(9, HudList::Text);
        CHECK(!text.empty());
        for (const HudQuad& q : text) {
            CHECK(q.min.x >= left && q.max.x <= right);
            CHECK(q.max.y <= at.y && q.min.y >= at.y - big.y);
        }
        // The label's row: the glyphs left of the box centre sit clear of the value's, so the label stays readable.
        HudDrawList label_only;
        EncyclopediaEntry lb = lng;
        lb.rows[0].right.clear();
        lb.rows.pop_back();
        wenc.build(label_only, wcv, lb, {0.0f, 0.0f}, at);
        HudDrawList with_label;
        EncyclopediaEntry wl = lng;
        wl.rows.pop_back();
        wenc.build(with_label, wcv, wl, {0.0f, 0.0f}, at);
        const size_t header_glyphs = label_only.items(9, HudList::Text).size();
        const std::span<const HudQuad> lt = label_only.items(9, HudList::Text);
        size_t label_n = 0;
        for (char32_t ch : r.left)
            if (ch != U' ') ++label_n;
        CHECK(header_glyphs >= label_n);
        for (size_t i = header_glyphs - label_n; i < header_glyphs; ++i) label_right = std::max(label_right, lt[i].max.x);
        const std::span<const HudQuad> wt = with_label.items(9, HudList::Text);
        for (size_t i = header_glyphs; i < wt.size(); ++i) value_left = std::min(value_left, wt[i].min.x);
        CHECK(wt.size() > header_glyphs && label_right < value_left);
    }

    // t40 (HUD_GAPS sec. 2.2, captures C3_*tooltip*): class + ability icons + Strong / Weak rows. The name and class start at
    // Encyclopedia_Name_Offset, the frames sit flush on the panel's two sides, the box grows by them, the text stays inside.
    {
        const HudCanvas ucv = HudCanvas::make(1024, 768);
        EncyclopediaPanel uenc;
        uenc.init(env.table, env.atlas, env.fonts);
        uenc.update(-1, 9, 0.0);
        uenc.update(-1, 9, 2.0);
        EncyclopediaEntry plain;
        plain.header = U"The Accuser";
        plain.icon = "i_button_vader.tga";
        plain.unit_class = U"Class: Fleet Commander";
        plain.description = U"Imperial fleet commander. Commands enhanced Star Destroyer with improved weaponry.";
        EncyclopediaEntry full = plain;
        full.population = 4;
        full.abilities = {"i_button_vader.tga", "i_button_vader.tga"};
        full.strong_label = U"Strong Against : ";
        full.weak_label = U"Weak Against :";
        full.strong = {"i_button_vader.tga", "i_button_vader.tga", "i_button_vader.tga", "i_button_vader.tga"};  // 3 cells
        full.weak = {"i_button_vader.tga"};
        const Vec2f at{-200.0f, 200.0f};
        HudDrawList pdl, fdl;
        const Vec2f ps = uenc.build(pdl, ucv, plain, {0.0f, 0.0f}, at);
        const Vec2f fs = uenc.build(fdl, ucv, full, {0.0f, 0.0f}, at);
        CHECK(ps.x == 262.0f && fs.x == 262.0f && fs.y >= ps.y + 43.0f && ps.y >= 47.0f);
        CHECK(pdl.items(9, HudList::Alpha).size() + 6 == fdl.items(9, HudList::Alpha).size());  // 2 abilities + 3 + 1 cells
        const std::span<const HudQuad> ft = fdl.items(9, HudList::Text);
        CHECK(!ft.empty());
        for (const HudQuad& q : ft) CHECK(q.min.x >= at.x && q.max.x <= at.x + fs.x && q.max.y <= at.y && q.min.y >= at.y - fs.y);
        float name_left = 1e9f;
        for (const HudQuad& q : pdl.items(9, HudList::Text)) name_left = std::min(name_left, q.min.x);
        CHECK(name_left >= at.x + 5.0f);  // the description starts at the padding
        const std::span<const HudQuad> fa = fdl.items(9, HudList::Alpha);
        float cell_right = -1e9f, cell_bottom = 1e9f;
        for (size_t i = fa.size() - 4; i < fa.size(); ++i) cell_bottom = std::min(cell_bottom, fa[i].min.y);
        for (size_t i = fa.size() - 4; i < fa.size() - 1; ++i) cell_right = std::max(cell_right, fa[i].max.x);
        CHECK(cell_bottom >= at.y - fs.y);
        CHECK(cell_right <= at.x + 129.0f + 1e-3f);  // the strong row's third cell ends with its frame
    }

    // FIXPASS_GALACTIC Â§2.4 / capture C3 (FoC 1.121, Clusters GC): rank ascending, ties by CRC-32 of the upper-case bar type
    // name, unsigned; ranking <= 0 compares equal (gathering order kept). Slot 00 = first entry = rightmost.
    {
        CHECK(hero_tie_key("Boba_Fett") == 0x31FC4F9Au && hero_tie_key("General_Veers") == 0x4B2003E7u &&
              hero_tie_key("Grand_Moff_Tarkin") == 0x8B291F16u && hero_tie_key("Accuser_Star_Destroyer") == 0x9E58A04Bu);
        CHECK(hero_tie_key("boba_fett") == hero_tie_key("BOBA_FETT"));
        struct Hero {
            const char* name;
            int32_t rank;
        };
        auto sorted = [](std::initializer_list<Hero> in) {
            std::vector<HeroEntry> v;
            for (const Hero& h : in) {
                HeroEntry e;
                e.type_name = h.name;
                e.ranking = h.rank;
                v.push_back(std::move(e));
            }
            sort_hero_entries(v);
            std::string out;
            for (const HeroEntry& e : v) out += e.type_name + " ";
            return out;
        };
        // The three bars seen in the captures, gathered in a scrambled order.
        CHECK(sorted({{"Grand_Moff_Tarkin", 3}, {"General_Veers", 3}, {"Emperor_Palpatine", 1}, {"Accuser_Star_Destroyer", 3}, {"Boba_Fett", 3}}) ==
              "Emperor_Palpatine Boba_Fett General_Veers Grand_Moff_Tarkin Accuser_Star_Destroyer ");
        CHECK(sorted({{"Droid_R2D2", 5}, {"Sundered_Heart", 3}, {"Millennium_Falcon", 2}, {"Obi_Wan_Kenobi", 2}, {"Mon_Mothma", 1}, {"Rogue_Squadron_Space", 0}}) ==
              "Mon_Mothma Obi_Wan_Kenobi Millennium_Falcon Sundered_Heart Droid_R2D2 Rogue_Squadron_Space ");
        CHECK(sorted({{"IG-2000", 3}, {"Silri", 3}, {"HoundsTooth", 3}, {"Urai_Fen", 2}, {"The_Peacebringer", 1}}) ==
              "The_Peacebringer Urai_Fen HoundsTooth Silri IG-2000 ");
        std::vector<HeroEntry> z(3);
        z[0].object = 1;
        z[1].object = 2;
        z[1].ranking = 1;
        z[2].object = 3;
        sort_hero_entries(z);
        CHECK(z[0].object == 1 && z[1].object == 2 && z[2].object == 3);  // ranking 0 -> unchanged
    }
    // Hero roster: two heroes on the good frame; click -> HeroSelect; bar hidden while respawning.
    HeroPanel heroes;
    CHECK(heroes.init(env.table, env.atlas, env.fonts));
    heroes.resize(1024, 768);
    CHECK(heroes.shells().size() == 2 && heroes.shells()[0] == env.table.find("good_hero_frame"));
    HeroEntry hs[2];
    hs[0].object = 4001;
    hs[0].icon = "i_button_vader.tga";
    hs[0].health = 0.5f;
    hs[0].planet = 7001;
    hs[1].object = 4002;
    hs[1].icon = "i_button_vader.tga";
    hs[1].health = 0.0f;
    hs[1].respawn = 0.3f;
    hs[1].selected = true;
    hs[1].planet = 7002;
    hs[1].container = 5002;
    HeroPanelFrame hf;
    hf.open = true;
    hf.good = true;
    hf.heroes = hs;
    heroes.build(hf);
    CHECK(heroes.open() && heroes.active_shell() == env.table.find("good_hero_frame") && heroes.hit_boxes().size() == 2 &&
          heroes.slots().size() == 4);
    std::vector<commands::EventData> cmds;
    std::vector<HudAction> acts;
    // FIXPASS_GALACTIC Â§2.2 galactic: left -> HeroFocusPlanet {hero, {planet}}; double -> + HeroZoomPlanet; right: nothing.
    env.click(heroes, "hero_slot_good01", cmds, acts, Button::Left, 1.0);
    CHECK(acts.size() == 1 && acts[0].kind == HudAction::Kind::HeroFocusPlanet && acts[0].unit == 4002 &&
          acts[0].ids == std::vector<uint32_t>{7002} && cmds.empty());
    acts.clear();
    env.click(heroes, "hero_slot_good01", cmds, acts, Button::Left, 1.3);
    CHECK(acts.size() == 2 && acts[0].kind == HudAction::Kind::HeroFocusPlanet &&
          acts[1].kind == HudAction::Kind::HeroZoomPlanet && acts[1].ids == std::vector<uint32_t>{7002});
    acts.clear();
    env.click(heroes, "hero_slot_good01", cmds, acts, Button::Left, 1.5);  // third click: a new single
    CHECK(acts.size() == 1 && acts[0].kind == HudAction::Kind::HeroFocusPlanet);
    acts.clear();
    env.click(heroes, "hero_slot_good00", cmds, acts, Button::Left, 3.0);
    env.click(heroes, "hero_slot_good00", cmds, acts, Button::Left, 3.7);  // too slow: two singles
    CHECK(acts.size() == 2 && acts[0].kind == HudAction::Kind::HeroFocusPlanet && acts[1].kind == HudAction::Kind::HeroFocusPlanet &&
          acts[1].unit == 4001);
    acts.clear();
    env.click(heroes, "hero_slot_good00", cmds, acts, Button::Right, 5.0);
    CHECK(acts.empty());
    hs[0].planet = commands::kNoObject;  // hyperspace
    hs[1].object = 0;                    // building / respawning
    heroes.build(hf);
    env.click(heroes, "hero_slot_good00", cmds, acts, Button::Left, 7.0);
    env.click(heroes, "hero_slot_good01", cmds, acts, Button::Left, 9.0);
    CHECK(acts.empty());
    // Â§2.3 tactical: left -> HeroSelect; double -> + HeroSelectGroup {container} / {} (by type).
    hs[0].planet = 7001;
    hs[1].object = 4002;
    hf.tactical = true;
    heroes.build(hf);
    env.click(heroes, "hero_slot_good01", cmds, acts, Button::Left, 11.0);
    CHECK(acts.size() == 1 && acts[0].kind == HudAction::Kind::HeroSelect && acts[0].unit == 4002 &&
          acts[0].ids == std::vector<uint32_t>{4002});
    acts.clear();
    env.click(heroes, "hero_slot_good01", cmds, acts, Button::Left, 11.2);
    CHECK(acts.size() == 2 && acts[1].kind == HudAction::Kind::HeroSelectGroup && acts[1].unit == 4002 &&
          acts[1].ids == std::vector<uint32_t>{5002});
    acts.clear();
    env.click(heroes, "hero_slot_good00", cmds, acts, Button::Left, 13.0);
    env.click(heroes, "hero_slot_good00", cmds, acts, Button::Left, 13.2);
    CHECK(acts.size() == 3 && acts[2].kind == HudAction::Kind::HeroSelectGroup && acts[2].unit == 4001 && acts[2].ids.empty());
    acts.clear();
    // Hover: the good slot's hero for the encyclopedia.
    {
        RoutedInput ri;
        ri.hover_component = env.table.find("hero_slot_good00");
        heroes.update_input(ri, false, false, 14.0, cmds, &acts);
        CHECK(heroes.hover_component() == ri.hover_component && heroes.hover_hero() == 4001 && acts.empty());
    }
    hf.tactical = false;
    hf.good = false;
    heroes.build(hf);
    CHECK(heroes.active_shell() == env.table.find("evil_hero_frame") && heroes.hit_boxes().size() == 1);  // only evil00 exists
    env.click(heroes, "hero_slot_evil00", cmds, acts, Button::Left, 20.0);
    env.click(heroes, "hero_slot_evil00", cmds, acts, Button::Left, 20.1);
    CHECK(acts.empty() && heroes.hover_hero() == 0);  // Â§2.1: enemy slots have no callbacks
    hf.open = false;
    heroes.build(hf);
    CHECK(!heroes.open() && heroes.hit_boxes().empty() && heroes.draw_list().total() == 0 && heroes.hit_test(512, 384) == -1);

    // Black market: item click -> BlackMarketBuy{item, planet}; disabled item -> nothing; close / right click -> PanelClose.
    BlackMarketPanel bm;
    CHECK(bm.init(env.table, env.atlas, env.fonts));
    bm.resize(1920, 1080);
    const BlackMarketItem items[2] = {{301, "i_button_vader.tga", U"Ion cannon upgrade", U"Hits harder.", 1200, true, 1},
                                      {302, "i_button_vader.tga", U"Shield booster", U"", 900, false, -1}};
    BlackMarketFrame bf;
    bf.open = true;
    bf.planet = 555;
    bf.title = U"Black Market";
    bf.items = items;
    bm.build(bf);
    acts.clear();
    env.click(bm, "market_01", cmds, acts);  // disabled: swallowed
    env.click(bm, "market_00", cmds, acts);
    CHECK(cmds.empty() && acts.size() == 1 && acts[0].kind == HudAction::Kind::BlackMarketBuy && acts[0].unit == 301 &&
          acts[0].value == 0 && acts[0].ids == std::vector<uint32_t>{555});
    acts.clear();
    // hovering item 0 shows its description / cost on market_title02 / 03 at the next build
    bm.build(bf);
    bool desc = false;
    for (const PanelSlot& s : bm.slots())
        if (s.component == env.table.find("market_title02")) desc = s.text == U"Hits harder.";
    CHECK(desc);
    env.click(bm, "market_close", cmds, acts);
    env.click(bm, "market_00", cmds, acts, Button::Right);
    CHECK(acts.size() == 2 && acts[0].kind == HudAction::Kind::PanelClose && acts[0].name == "market_frame" &&
          acts[1].kind == HudAction::Kind::PanelClose && cmds.empty());
    // Slice dialog (market_frame stand-in): 7 rows, 5 visible; row click -> SliceSelect{type, {planet, slicer}, row};
    // unaffordable row swallowed; the wheel over the panel scrolls; hovered row shows "Chance n%"; close -> PanelClose.
    {
        SlicePanel sp;
        CHECK(sp.init(env.table, env.atlas, env.fonts));
        CHECK(sp.shells().size() == 1 && sp.shells()[0] == env.table.find("market_frame"));
        std::vector<SliceRow> rows;
        for (uint32_t k = 0; k < 7; ++k)
            rows.push_back({500 + k, "i_button_vader.tga", U"Type", U"", 1000 + static_cast<int32_t>(k), 1.0f, k != 1});
        SliceFrame sf2;
        sf2.open = true;
        sf2.planet = 557;
        sf2.slicer = 9001;
        sf2.title = U"Slice";
        sf2.rows = rows;
        sp.build(sf2);
        CHECK(sp.open() && sp.active_shell() == env.table.find("market_frame") && sp.first_row() == 0);
        acts.clear();
        env.click(sp, "market_01", cmds, acts);  // unaffordable: swallowed
        env.click(sp, "market_00", cmds, acts);
        CHECK(cmds.empty() && acts.size() == 1 && acts[0].kind == HudAction::Kind::SliceSelect && acts[0].unit == 500 &&
              acts[0].value == 0 && acts[0].ids == (std::vector<uint32_t>{557, 9001}));
        sp.build(sf2);  // market_00 hovered: its chance on market_title02
        bool chance = false;
        for (const PanelSlot& s : sp.slots())
            if (s.component == env.table.find("market_title02")) chance = s.text == U"Chance 100%";
        CHECK(chance);
        RoutedInput wheel;
        wheel.wheel_target = PointerTarget::Hud;
        wheel.hover_component = env.table.find("market_00");
        sp.update_input(wheel, false, false, 2.0, cmds, &acts, -5.0f);  // towards the user: down, clamped to 7 - 5
        sp.build(sf2);
        CHECK(sp.first_row() == 2);
        acts.clear();
        env.click(sp, "market_00", cmds, acts);
        CHECK(acts.size() == 1 && acts[0].unit == 502 && acts[0].value == 2);
        wheel.hover_component = -1;  // not over the panel: ignored
        sp.update_input(wheel, false, false, 2.1, cmds, &acts, 1.0f);
        CHECK(sp.first_row() == 2);
        acts.clear();
        env.click(sp, "market_close", cmds, acts);
        CHECK(acts.size() == 1 && acts[0].kind == HudAction::Kind::PanelClose && acts[0].name == "market_frame" && cmds.empty());
        sf2.open = false;  // reopening starts at the first row
        sp.build(sf2);
        sf2.open = true;
        sp.build(sf2);
        CHECK(sp.first_row() == 0);
    }
    // Sabotage and corruption.
    SabotagePanel sab;
    CHECK(sab.init(env.table, env.atlas, env.fonts));
    const SabotageTarget tg[2] = {{8001, "i_button_vader.tga", U"Barracks", 300, true}, {8002, "i_button_vader.tga", U"Turbolaser", 500, true}};
    SabotageFrame sf;
    sf.open = true;
    sf.planet = 556;
    sf.targets = tg;
    sab.build(sf);
    acts.clear();
    env.click(sab, "bm_0001", cmds, acts);
    CHECK(acts.size() == 1 && acts[0].kind == HudAction::Kind::SabotageTarget && acts[0].unit == 8002 && acts[0].ids == std::vector<uint32_t>{556});
    CorruptionPanel cor;
    CHECK(cor.init(env.table, env.atlas, env.fonts));
    CorruptionOption opts[2];
    opts[0].corruption_type = 1;
    opts[0].icon = "i_button_vader.tga";
    opts[0].title = U"Piracy";
    opts[0].requirements = {"i_button_vader.tga"};
    opts[1].corruption_type = 6;
    opts[1].icon = "i_button_vader.tga";
    opts[1].enabled = false;
    CorruptionFrame cf;
    cf.open = true;
    cf.planet = 557;
    cf.options = opts;
    cor.build(cf);
    acts.clear();
    env.click(cor, "corrupt_button_icon_a", cmds, acts);
    env.click(cor, "corrupt_button_icon_b", cmds, acts);
    CHECK(acts.size() == 1 && acts[0].kind == HudAction::Kind::CorruptionChoice && acts[0].unit == 557 && acts[0].value == 1);
    // A component of another panel is not owned.
    CHECK(!cor.owns(env.table.find("market_00")) && cmds.empty());
}

// ---- P4 completion 2: remaining screens ---------------------------------------------------------------------------------
void test_info_screens() {
    Env env;
    InfoScreens is;
    is.init(env.gui);
    std::vector<ScreenAction> acts;
    // Skirmish buy units.
    BuyUnitsState bs;
    bs.credits = 1000;
    bs.units = {{"Rebel_Infantry_Squad", U"Infantry", 200, 0, "i_unit.tga", true}, {"T2B_Company", U"T2-B", 1200, 2, "i_tank.tga", true}};
    CHECK(is.show_buy_units(bs));
    GuiDialog* d = env.gui.find(dlg::kBuyUnits);
    CHECK(d && d->get("IDC_SKIRMISH_BUY_FORCES_LISTBOX")->items.size() == 2 &&
          d->get("IDC_SKIRMISH_BUY_FORCES_LISTBOX")->items[1] == U"T2-B  x2  1200" &&
          d->get("IDC_SKIRMISH_BUY_FORCES_LISTBOX")->items[0] == U"Infantry  200" && d->get("IDC_BUY_BUTTON")->enabled &&
          !d->get("IDC_SELL_BUTTON")->enabled && d->get("IDC_SELL_ALL_BUTTON")->enabled &&
          d->get("IDC_SKIRMISH_BUY_MENU_UNIT_IMAGE")->image == "i_unit.tga" &&
          d->get("IDC_SKIRMISH_BUY_UNITS_CREDITS_TEXT")->text == U"Credits: 1000");
    is.update(env.click(dlg::kBuyUnits, "IDC_BUY_BUTTON"), acts);
    CHECK(acts.size() == 1 && acts[0].kind == ScreenAction::Kind::BuyUnit && acts[0].map == "Rebel_Infantry_Squad");
    d->set_selected("IDC_SKIRMISH_BUY_FORCES_LISTBOX", 1);
    is.update(std::vector<GuiEvent>{event(GuiEvent::Kind::SelectionChanged, dlg::kBuyUnits, "IDC_SKIRMISH_BUY_FORCES_LISTBOX", 1)}, acts);
    CHECK(!d->get("IDC_BUY_BUTTON")->enabled && d->get("IDC_SELL_BUTTON")->enabled);  // 1200 > 1000 credits
    is.update(env.click(dlg::kBuyUnits, "IDC_SELL_BUTTON"), acts);
    is.update(env.click(dlg::kBuyUnits, "IDC_NEXT_BUTTON"), acts);
    CHECK(acts.size() == 3 && acts[1].kind == ScreenAction::Kind::SellUnit && acts[1].map == "T2B_Company" &&
          acts[2].kind == ScreenAction::Kind::BuyUnitsDone && env.gui.find(dlg::kBuyUnits) == nullptr);
    // Story dialog.
    acts.clear();
    StoryDialogInfo st;
    st.tag = "intro";
    st.title = U"A new hope";
    st.lines = {U"Line 1", U"Line 2"};
    st.cancel = true;
    CHECK(is.show_story(st));
    d = env.gui.find(dlg::kStory);
    CHECK(d && d->get("IDC_STORY_TEXT")->items.size() == 2 && d->get("IDC_STORY_DIALOG_CANCEL")->visible && !d->get("IDC_IMAGE")->visible);
    is.update(env.click(dlg::kStory, "IDC_STORY_DIALOG_ACCEPT"), acts);
    CHECK(acts.size() == 1 && acts[0].kind == ScreenAction::Kind::StoryAccepted && acts[0].tag == "intro");
    // Story dialog script timing: coloured lines appear at their WAIT times, SFX / speech cues fire once each.
    {
        StoryDialogInfo sc;
        sc.tag = "timed";
        sc.lines = {U"Title", U"", U"Body", U"Objective"};
        sc.line_colors = {{128, 55, 136, 255}, {0, 0, 0, 0}, {255, 255, 255, 176}, {109, 247, 255, 255}};
        sc.line_at = {0.0, 0.0, 0.5, 1.5};
        sc.cues = {{0.5, StoryDialogCue::Kind::Sfx, "Button_Press"}, {0.5, StoryDialogCue::Kind::Speech, "MON_01"},
                   {1.5, StoryDialogCue::Kind::Sfx, "Button_Press"}};
        CHECK(is.show_story(sc));
        const GuiControl* tx = env.gui.find(dlg::kStory) ? env.gui.find(dlg::kStory)->get("IDC_STORY_TEXT") : nullptr;
        CHECK(tx && tx->items.size() == 2 && tx->item_text_colors.size() == 2 && tx->item_text_colors[0].r == 128);
        std::vector<StoryDialogCue> due;
        CHECK(is.update_story(10.0, &due) && due.empty() && is.story_lines_shown() == 2);  // the clock starts here
        CHECK(is.update_story(10.6, &due) && is.story_lines_shown() == 3 && due.size() == 2 && due[1].kind == StoryDialogCue::Kind::Speech &&
              due[1].event == "MON_01");
        CHECK(!is.update_story(11.6, &due) && is.story_lines_shown() == 4 && due.size() == 3);
        CHECK(!is.update_story(12.0, &due) && due.size() == 3);  // cues fire once
        CHECK(tx && tx->items.size() == 4 && tx->item_text_colors.size() == 4 && tx->item_text_colors[3].g == 247);
        acts.clear();
        is.update(env.click(dlg::kStory, "IDC_STORY_DIALOG_ACCEPT"), acts);
        CHECK(acts.size() == 1 && acts[0].tag == "timed" && !is.update_story(13.0, &due));
    }
    // GALACTIC_DIALOGS Â§9 IDD_PLANET_INFO (GameScreens): name line in the owner colour, overlay, wrapped lists (tactical in
    // 50/200/255), icon-name rewrites, map icon off when missing; single instance; Close / Esc -> PlanetInfoClosed; host close.
    {
        CHECK(planet_info_weather_icon("i_pa_weather_rain.tga") == "load_weather_rain.tga" && planet_info_weather_icon("").empty());
        CHECK(planet_info_map_icon("_Space_Planet_Kuat.TED") == "_Space_Planet_Kuat.dds" && planet_info_map_icon("Kuat") == "Kuat.dds" &&
              planet_info_map_icon("").empty());
        GameScreens gs2;
        gs2.init(env.gui);
        PlanetInfoData pi;
        pi.planet = 88;
        pi.name = U"Kuat";
        pi.name_color = Rgba8{200, 30, 30, 255};
        pi.planet_icon = "planet_render_kuat";
        std::u32string longline;
        for (int k = 0; k < 40; ++k) longline += U"shipyards ";
        pi.tactical = {longline + U"\nSecond paragraph."};
        pi.advantages = {U"+25% space unit production"};
        pi.natives = U"Human";
        pi.terrain = U"Urban";
        pi.weather = U"Clear";
        pi.weather_icon = planet_info_weather_icon("i_pa_weather_clear.tga");
        pi.native_icon = "load_urban.tga";
        pi.map_icon = "";  // the .dds is missing: control off
        CHECK(!gs2.planet_info_open() && gs2.show_planet_info(pi) && gs2.planet_info_open());
        const GuiDialog* pd2 = env.gui.find(dlg::kPlanetInfo);
        CHECK(pd2 != nullptr);
        if (pd2) {
            const GuiControl* nm = pd2->get("IDC_STATIC_PLANET_NAME");
            CHECK(nm && nm->text.size() > 5 && nm->text.substr(nm->text.size() - 5) == U" Kuat" && nm->tint.r == 200 && nm->tint.g == 30);
            const GuiControl* tac = pd2->get("IDC_LIST_TACTICAL");
            CHECK(tac && tac->items.size() >= 3 && tac->top == 0 && tac->item_text_colors.size() == tac->items.size() &&
                  tac->item_text_colors[0].g == 200 && tac->item_text_colors[0].b == 255 && tac->items.back() == U"Second paragraph.");
            const GuiControl* adv = pd2->get("IDC_LIST_ADVANTAGE");
            CHECK(adv && adv->items.size() == 1 && adv->item_text_colors.size() == 1 && adv->item_text_colors[0].a == 0);
            CHECK(pd2->get("IDC_LIST_REFERENCE")->items.empty());
            CHECK(pd2->get("IDC_PLANET_ICON")->image == "planet_render_kuat" && pd2->get("IDC_PLANET_OVERLAY")->image == "load_overlay.tga" &&
                  pd2->get("IDC_PLANET_OVERLAY")->visible);
            CHECK(pd2->get("IDC_WEATHER_ICON")->image == "load_weather_clear.tga" && pd2->get("IDC_NATIVE_ICON")->image == "load_urban.tga" &&
                  !pd2->get("IDC_MAP_ICON")->visible);
            CHECK(pd2->get("IDC_STATIC_NATIVE")->text == U"Human" && pd2->get("IDC_STATIC_TERRAIN")->text == U"Urban" &&
                  pd2->get("IDC_STATIC_WEATHER")->text == U"Clear");
        }
        PlanetInfoData other = pi;
        other.planet = 99;
        other.natives = U"Wookiee";
        CHECK(!gs2.show_planet_info(other));  // single instance: ignored, first planet kept
        CHECK(env.gui.find(dlg::kPlanetInfo)->get("IDC_STATIC_NATIVE")->text == U"Human");
        std::vector<ScreenAction> pa;
        std::vector<commands::EventData> pc;
        gs2.update(env.click(dlg::kPlanetInfo, "IDC_CLOSE_BUTTON"), pa, pc);
        CHECK(pa.size() == 1 && pa[0].kind == ScreenAction::Kind::PlanetInfoClosed && pa[0].object == 88 && !gs2.planet_info_open());
        other.map_icon = planet_info_map_icon("Kuat.ted");
        CHECK(gs2.show_planet_info(other) && env.gui.find(dlg::kPlanetInfo)->get("IDC_MAP_ICON")->image == "Kuat.dds" &&
              env.gui.find(dlg::kPlanetInfo)->get("IDC_MAP_ICON")->visible);
        pa.clear();
        gs2.update(env.frame({5, 5}, false, {vk::kEscape}), pa, pc);
        CHECK(pa.size() == 1 && pa[0].kind == ScreenAction::Kind::PlanetInfoClosed && pa[0].object == 99 && !gs2.planet_info_open());
        CHECK(gs2.show_planet_info(pi));
        gs2.close_planet_info();  // host: a battle load screen starts
        CHECK(!gs2.planet_info_open() && pc.empty());
    }
    // Planet dialog (IDD_PLANET_DIALOG: dead in the original, kept).
    PlanetDialogInfo pd;
    pd.planet = 77;
    pd.name = U"Kuat";
    pd.credits = 120;
    pd.political = {"i_pol.tga"};
    pd.producing = U"Star Destroyer";
    pd.progress = 0.25f;
    CHECK(is.show_planet(pd));
    d = env.gui.find(dlg::kPlanetDialog);
    CHECK(d && d->get("IDC_TEXT_PLANET_CREDITS")->text == U"+120" && d->get("IDC_POLITICAL_ICON_0")->visible &&
          !d->get("IDC_POLITICAL_ICON_1")->visible && d->get("IDC_PRODUCTION_MENU_PROGRESS")->value == 25);
    // REMAINING_GAPS Â§6.3 gates: hidden starbase / ground-base info blanks those icon rows.
    {
        PlanetDialogInfo hidden = pd;
        hidden.starbase = {"i_sb.tga", "i_sb.tga"};
        hidden.ground_base = {"i_gb.tga"};
        CHECK(is.show_planet(hidden));
        const GuiDialog* hd = env.gui.find(dlg::kPlanetDialog);
        const GuiControl* sb = hd ? hd->get("IDC_STARBASE_ICON_0") : nullptr;
        const GuiControl* gb = hd ? hd->get("IDC_GROUND_BASE_ICON_0") : nullptr;
        CHECK(sb && gb && sb->visible && gb->visible);
        hidden.starbase_info = false;
        hidden.groundbase_info = false;
        CHECK(is.show_planet(hidden));
        CHECK(sb && gb && !sb->visible && !gb->visible && hd->get("IDC_POLITICAL_ICON_0")->visible);
        hidden.groundbase_info = true;
        CHECK(is.show_planet(hidden));
        CHECK(sb && gb && !sb->visible && gb->visible);
        CHECK(is.show_planet(pd));
    }
    is.update(env.click(dlg::kPlanetDialog, "IDC_PLANET_PRODUCTION_BUTTON"), acts);
    is.update(env.frame({5, 5}, false, {vk::kEscape}), acts);
    CHECK(acts.size() == 3 && acts[1].kind == ScreenAction::Kind::PlanetProduction && acts[1].object == 77 &&
          acts[2].kind == ScreenAction::Kind::PlanetDialogClosed);
    // Holocron: data chart -> tech tree -> space variant -> summary (scroll) -> close.
    acts.clear();
    DataChartInfo dc;
    dc.economy = {{U"Empire", {200, 30, 30, 255}, {100, 300, 900, 1200}}, {U"Rebels", {40, 90, 220, 255}, {150, 200, 250, 400}}};
    dc.military = {{U"Empire", {200, 30, 30, 255}, {5, 9}}};
    dc.first_cycle = 3;
    is.set_data_chart(dc);
    TechTreeInfo tt;
    tt.dialog = "IDD_TECH_TREE_EMPIRE";
    tt.item = [](std::string_view type, TechTreeItem& out) {
        if (type == "Dark_Trooper_PhaseI") return false;
        out.icon = "i_tech.tga";
        out.state = type == "E_Ground_Barracks" ? TechState::Researched : TechState::Locked;
        return true;
    };
    is.set_tech_tree(tt);
    SummaryInfo sm;
    for (int i = 0; i < 9; ++i) sm.production.push_back({"i_planet.tga", U"P" + std::u32string(1, static_cast<char32_t>(U'0' + static_cast<char32_t>(i))), "", "", U"", U"", {"i_hero.tga"}, U"5"});
    is.set_summary(sm);
    CHECK(is.show_info(InfoPage::DataChart) && is.info_dialog() == dlg::kDataChart);
    d = env.gui.find(dlg::kDataChart);
    CHECK(d->get("IDC_Y_MAX")->text == U"1200" && d->get("IDC_STATIC_END")->text == U"6" && d->get("IDC_DATA_CHART_RADIO")->checked);
    HudDrawList dl;
    env.gui.build(dl, env.now);
    const size_t dots = is.build_overlay(dl);
    CHECK(dots > 100);
    is.update(std::vector<GuiEvent>{event(GuiEvent::Kind::Checked, dlg::kDataChart, "IDC_RADIO_MILITARY", 1)}, acts);
    CHECK(is.chart_mode() == 1 && d->get("IDC_Y_MAX")->text == U"9");
    is.update(env.click(dlg::kDataChart, "IDC_TECH_TREE_RADIO"), acts);
    CHECK(is.info_page() == InfoPage::TechTree && is.info_dialog() == "IDD_TECH_TREE_EMPIRE" && !acts.empty() &&
          acts.back().kind == ScreenAction::Kind::InfoPageChanged && acts.back().button == static_cast<int32_t>(InfoPage::TechTree));
    d = env.gui.find("IDD_TECH_TREE_EMPIRE");
    CHECK(d && env.gui.find(dlg::kDataChart) == nullptr && d->get("IDC_TECHTREE_E_Ground_Barracks")->tint == (Rgba8{255, 255, 255, 255}) &&
          d->get("IDC_TECHTREE_Imperial_Stormtrooper_Squad")->tint == (Rgba8{90, 90, 90, 255}) &&
          !d->get("IDC_TECHTREE_Dark_Trooper_PhaseI")->visible && d->get("IDC_TECH_TREE_LAND")->checked);
    is.update(std::vector<GuiEvent>{event(GuiEvent::Kind::Checked, "IDD_TECH_TREE_EMPIRE", "IDC_TECH_TREE_SPACE", 1)}, acts);
    CHECK(is.info_dialog() == "IDD_TECH_TREE_EMPIRE_SPACE" && env.gui.find("IDD_TECH_TREE_EMPIRE_SPACE")->get("IDC_TECHTREE_Star_Destroyer")->visible);
    CHECK(is.show_info(InfoPage::SummaryProduction));
    d = env.gui.find(dlg::kSummaryProduction);
    CHECK(d && d->get("IDC_PLANET_NAME0")->text == U"P0" && d->get("IDC_HERO0_ICON0")->visible && d->get("IDC_SUMMARY_SCREEN_RADIO")->checked);
    is.update(std::vector<GuiEvent>{event(GuiEvent::Kind::ValueChanged, dlg::kSummaryProduction, "IDC_SCROLLBAR_SLIDER", 100)}, acts);
    CHECK(is.summary_offset() == 3 && d->get("IDC_PLANET_NAME0")->text == U"P3");
    acts.clear();
    is.update(env.click(dlg::kSummaryProduction, "IDC_BUTTON_POP_CAP"), acts);
    is.update(env.click(dlg::kSummaryProduction, "IDC_BUTTON_EXIT"), acts);
    CHECK(acts.size() == 2 && acts[0].kind == ScreenAction::Kind::SummarySort && acts[0].tag == "IDC_BUTTON_POP_CAP" &&
          acts[1].kind == ScreenAction::Kind::InfoClosed && is.info_dialog().empty() && env.gui.open_count() == 0);
    CHECK(tech_tree_dialog("IDD_TECH_TREE_BLACKMARKET", true) == "IDD_TECH_TREE_BLACKMARKET_SPACE");
    CHECK(tech_tree_dialog("", true).empty());  // modc t20: a faction without Tech_Tree_Dialog_Name has no tech tree
}

}  // namespace test_gui_screens_t
