// alamo_data tests (no framework; non-zero exit on failure).
// Unit tests for the XML reader, value helpers and loaders on a synthetic tree, then an
// integration pass over the real game data if it is present (ALAMO_FOC_DATA / ALAMO_EAW_DATA
// override the default roots /home/claude/data/foc and /home/claude/data/eaw).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>

#include "alamo/core/file_provider.hpp"
#include "alamo/core/hash.hpp"
#include "alamo/core/log.hpp"
#include "alamo/data/game_database.hpp"
#include "alamo/data/values.hpp"
#include "alamo/data/xml.hpp"
#ifdef ALAMO_DATA_HAVE_VFS
#include "alamo/vfs/vfs.hpp"
#endif

using namespace alamo;
using namespace alamo::data;
namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)

static bool has_warning(const XmlDocument& d, uint32_t line, std::string_view needle) {
    for (const auto& w : d.diagnostics())
        if (w.line == line && w.message.find(needle) != std::string::npos) return true;
    return false;
}

static void test_xml_basic() {
    const std::string src =
        "\xEF\xBB\xBF<?xml version=\"1.0\" ?>\r\n"
        "<!-- header comment -->\r\n"
        "<Root Version='2' flag=\"a &amp; b\">\r\n"                 // line 3
        "  <Item Name=\"One\">  hello &lt;world&gt; &#65;&#x42; </Item>\r\n"  // line 4
        "  <item name=\"Two\"/>\r\n"                                // line 5
        "  <Empty></Empty>\r\n"
        "  <Data><![CDATA[x < y && z]]></Data>\r\n"
        "  <Latin>caf\xE9</Latin>\r\n"
        "  <Mixed> a <!-- c --> b </Mixed>\r\n"
        "  <Q>&quot;&apos;&bogus; &#xZZ; & tail</Q>\r\n"
        "  <Utf>&#233;</Utf>\r\n"
        "</Root>\r\n";
    XmlDocument d = XmlDocument::parse(src, "basic.xml");
    CHECK(d.ok());
    CHECK(d.diagnostics().empty());
    XmlNode r = d.root();
    CHECK(r.name() == "Root");
    CHECK(r.line() == 3);
    CHECK(r.attribute("version") == "2");
    CHECK(r.attribute("FLAG") == "a & b");
    CHECK(r.attribute("missing", "def") == "def");
    CHECK(r.child_count() == 8);
    XmlNode one = r.child("ITEM");
    CHECK(one && one.attribute("Name") == "One");
    CHECK(one.text_trimmed() == "hello <world> AB");
    CHECK(one.line() == 4);
    XmlNode two = one.next_sibling("Item");
    CHECK(two && two.attribute("NAME") == "Two" && two.text().empty() && two.line() == 5);
    int items = 0;
    for (XmlNode c : r.children("item")) {
        (void)c;
        ++items;
    }
    CHECK(items == 2);
    int all = 0;
    for (XmlNode c : r.children()) {
        (void)c;
        ++all;
    }
    CHECK(all == 8);
    CHECK(r.child("Empty") && r.child("Empty").text().empty());
    CHECK(r.child_text("Data") == "x < y && z");
    CHECK(r.child_text("Latin") == "caf\xE9");
    CHECK(r.child_text("Mixed") == "a  b");
    CHECK(r.child_text("Q") == "\"'&bogus; &#xZZ; & tail");
    CHECK(r.child_text("Utf") == "\xC3\xA9");
    CHECK(!r.child("Nope"));
    CHECK(!r.child("Nope").child("x"));  // null-safe chaining
    CHECK(r.child("Nope").text().empty());
    CHECK(two.parent() == r);
}

static void test_xml_recovery() {
    {  // <?xml ...> without '?', as in AI/Templates/*.xml
        XmlDocument d = XmlDocument::parse("<?xml version=\"1.0\">\r\n<AITemplates>\r\n<A>1</A>\r\n</AITemplates>\r\n", "t.xml");
        CHECK(d.ok());
        CHECK(d.root().name() == "AITemplates");
        CHECK(d.root().child_text("A") == "1");
        CHECK(has_warning(d, 1, "'?>'"));
    }
    {  // mismatched close tag closes the open element (EaW GUIDialogs.xml style)
        const char* src =
            "<R>\n"
            "  <C>\n"
            "    <Top_Color>1,2</Bottom_Color>\n"  // line 3
            "    <Bottom_Color>3,4</Bottom_Color>\n"
            "  </C>\n"
            "</R>\n";
        XmlDocument d = XmlDocument::parse(src, "m.xml");
        CHECK(d.ok());
        CHECK(d.diagnostics().size() == 1);
        CHECK(has_warning(d, 3, "</Bottom_Color> closes open element <Top_Color>"));
        XmlNode c = d.root().child("C");
        CHECK(c.child_count() == 2);
        CHECK(c.child_text("Top_Color") == "1,2");
        CHECK(c.child_text("Bottom_Color") == "3,4");
        CHECK(d.diagnostics()[0].to_string() == "m.xml(3): warning: close tag </Bottom_Color> closes open element <Top_Color>");
    }
    {  // unclosed elements at EOF, stray close tag, stray '<', unterminated comment
        const char* src =
            "</Stray>\n"   // line 1
            "<R>\n"        // line 2
            "  <A>x < 5</A>\n"  // line 3
            "  <B>\n"      // line 4
            "<!-- never closed";
        XmlDocument d = XmlDocument::parse(src, "u.xml");
        CHECK(d.ok());
        CHECK(has_warning(d, 1, "without an open element"));
        CHECK(has_warning(d, 3, "does not start markup"));
        CHECK(has_warning(d, 5, "unterminated comment"));
        CHECK(has_warning(d, 4, "<B> is not closed"));
        CHECK(has_warning(d, 2, "<R> is not closed"));
        CHECK(d.root().child_text("A") == "x < 5");
    }
    {  // element names may start with a digit (the original loads these unit files: docs/re/MODC_LOADER.md section 3)
        const char* src =
            "\xEF\xBB\xBF<?xml version=\"1.0\"?>\r\n"
            "<181st>\r\n"
            "\t<UniqueUnit Name=\"U1\"><Variant_Of_Existing_Type>TIE_Fighter</Variant_Of_Existing_Type></UniqueUnit>\r\n"
            "\t<80s_Visor_Man Name=\"U2\"/>\r\n"
            "\t<Text>a < 5</Text>\r\n"
            "</181st>\r\n";
        XmlDocument d = XmlDocument::parse(src, "digit.xml");
        CHECK(d.ok());
        CHECK(d.root().name() == "181st");
        CHECK(d.root().child_count() == 3);
        CHECK(d.root().child("UniqueUnit").attribute("Name") == "U1");
        CHECK(d.root().child("UniqueUnit").child_text("Variant_Of_Existing_Type") == "TIE_Fighter");
        CHECK(d.root().child("80s_Visor_Man").attribute("Name") == "U2");
        CHECK(d.root().child_text("Text") == "a < 5");
        CHECK(has_warning(d, 5, "does not start markup"));
        CHECK(d.diagnostics().size() == 1);
    }
    {  // unquoted attribute and self-closing variants
        XmlDocument d = XmlDocument::parse("<R a=1 b = \"2\" c=x/><S/>", "q.xml");
        CHECK(d.ok());
        CHECK(d.root().attribute("a") == "1");
        CHECK(d.root().attribute("b") == "2");
        CHECK(d.root().attribute("c") == "x");
        CHECK(d.root().child_count() == 0);
        CHECK(has_warning(d, 1, "more than one top-level element"));
    }
    {  // fatal: nothing but a declaration and a comment
        XmlDocument d = XmlDocument::parse("<?xml version=\"1.0\"?>\n<!-- x -->\n", "e.xml");
        CHECK(!d.ok());
        CHECK(!d.root());
        CHECK(!d.diagnostics().empty() && d.diagnostics().back().fatal);
        XmlDocument e = XmlDocument::parse(std::string_view(), "empty.xml");
        CHECK(!e.ok());
        XmlDocument u16 = XmlDocument::parse(std::string_view("\xFF\xFE<\0R\0", 6), "u16.xml");
        CHECK(!u16.ok());
    }
    {  // line numbers survive mixed CR/LF and long files
        std::string src = "<R>\n";
        for (int i = 0; i < 1000; ++i) src += "  <E i=\"" + std::to_string(i) + "\"/>\r\n";
        src += "</R>";
        XmlDocument d = XmlDocument::parse(src, "l.xml");
        CHECK(d.ok() && d.node_count() == 1001);
        uint32_t expect = 2;
        bool lines_ok = true;
        for (XmlNode e : d.root().children()) lines_ok &= (e.line() == expect++);
        CHECK(lines_ok);
    }
    {  // document survives a move (string_views stay valid)
        XmlDocument a = XmlDocument::parse("<R><A>v</A></R>", "mv.xml");
        XmlDocument b = std::move(a);
        CHECK(b.root().child_text("A") == "v");
    }
}


// Every prefix and a set of deterministic byte mutations of a tricky document must parse
// without crashing (run under ASan/UBSan in CI to catch out-of-bounds reads).
static void test_xml_robustness() {
    const std::string src =
        "\xEF\xBB\xBF<?xml version=\"1.0\"?><!DOCTYPE x><R a='1' b=\"&amp;&#x41;\" c=d/><R2><![CDATA[q]]>"
        "<!-- c --><A>x &lt; y</A><B><C/></X></B> < <D e=\"unterminated";
    size_t ok = 0;
    for (size_t n = 0; n <= src.size(); ++n) ok += XmlDocument::parse(std::string_view(src.data(), n), "p.xml").ok() ? 1 : 0;
    CHECK(ok > src.size() / 2);
    uint32_t rng = 12345;
    const char alphabet[] = "<>/!?&;#\"'=-[] \nAx\x80";
    for (int iter = 0; iter < 4000; ++iter) {
        std::string m = src;
        for (int k = 0; k < 4; ++k) {
            rng = rng * 1664525u + 1013904223u;
            const size_t pos = (rng >> 8) % m.size();
            rng = rng * 1664525u + 1013904223u;
            m[pos] = alphabet[(rng >> 8) % (sizeof(alphabet) - 1)];
        }
        XmlDocument d = XmlDocument::parse(m, "fuzz.xml");
        for (XmlNode c : d.root().children()) (void)c.text_trimmed();
    }
}

static void test_values() {
    CHECK(trim("  a b \r\n") == "a b");
    CHECK(parse_float(" 18 ") == 18.0f);
    CHECK(parse_float("-10.5") == -10.5f);
    CHECK(parse_float("+2.5") == 2.5f);
    CHECK(parse_float("5.0f") == 5.0f);
    CHECK(parse_float(".5") == 0.5f);
    CHECK(parse_float("1e3") == 1000.0f);
    CHECK(!parse_float("abc"));
    CHECK(!parse_float(""));
    CHECK(!parse_float("+-1"));
    CHECK(parse_float("0.1") == 0.1f);  // correctly rounded
    CHECK(parse_int(" 1000 ") == 1000);
    CHECK(parse_int("-7") == -7);
    CHECK(parse_int("+7") == 7);
    CHECK(parse_int("3.9") == 3);
    CHECK(!parse_int("x"));
    CHECK(!parse_int("99999999999"));
    // CU3 prefix / first-character variants.
    CHECK(parse_int_prefix(" 12abc") == 12);
    CHECK(parse_int_prefix("\v\f-7") == -7);
    CHECK(parse_int_prefix("+2.9") == 2);
    CHECK(parse_int_prefix("-0") == 0);
    CHECK(!parse_int_prefix(""));
    CHECK(!parse_int_prefix("x1"));
    CHECK(!parse_int_prefix(" -"));
    CHECK(!parse_int_prefix("+-1"));
    CHECK(parse_int_prefix("99999999999") == 2147483647);
    CHECK(parse_int_prefix("2147483647") == 2147483647);
    CHECK(parse_int_prefix("2147483648") == 2147483647);
    CHECK(parse_int_prefix("-2147483648") == -2147483647 - 1);
    CHECK(parse_int_prefix("-99999999999999999999") == -2147483647 - 1);
    CHECK(parse_type_bool("1") && parse_type_bool("yes") && parse_type_bool("True") && parse_type_bool("Y"));
    CHECK(!parse_type_bool("10") && !parse_type_bool(" Yes") && !parse_type_bool("") && !parse_type_bool("no") && !parse_type_bool("0"));
    CHECK(parse_flag_char("1") && parse_flag_char("10") && parse_flag_char("yes") && parse_flag_char("t"));
    CHECK(!parse_flag_char("") && !parse_flag_char(" Yes") && !parse_flag_char("false") && !parse_flag_char("0"));
    CHECK(parse_bool(" Yes ") == true);
    CHECK(parse_bool("TRUE") == true);
    CHECK(parse_bool("1") == true);
    CHECK(parse_bool("no") == false);
    CHECK(parse_bool("False") == false);
    CHECK(parse_bool("0") == false);
    CHECK(!parse_bool("maybe"));
    CHECK(parse_bool_or("", true) == true);
    const auto v = parse_vec3("-220.0, -210.0, 10.0");
    CHECK(v && v->x == -220.0f && v->y == -210.0f && v->z == 10.0f);
    const auto v2 = parse_vec3("1 2\t3");
    CHECK(v2 && v2->z == 3.0f);
    CHECK(!parse_vec3("1, 2"));
    float fl[4];
    CHECK(parse_floats("83,125,225,255", fl, 4) == 4 && fl[3] == 255.0f);
    const auto lst = split_list("\n  Galaxy_Core_Art_Model, \n\tAbregado_Rae,AlzocIII\r\n Hoth  ");
    CHECK(lst.size() == 4 && lst[0] == "Galaxy_Core_Art_Model" && lst[2] == "AlzocIII" && lst[3] == "Hoth");
    CHECK(split_list("  , ,").empty());
    const auto fields = split_fields(" Empire, Abregado_Rae, Ground_Cantina_E");
    CHECK(fields.size() == 3 && fields[1] == "Abregado_Rae");
    CHECK(split_fields("a,,b").size() == 3);
    const auto pr = parse_pair(" Rebel, Hoth ");
    CHECK(pr && pr->first == "Rebel" && pr->second == "Hoth");
    const auto pr2 = parse_pair("Empire Thyferra");
    CHECK(pr2 && pr2->first == "Empire" && pr2->second == "Thyferra");
    CHECK(!parse_pair("Rebel,"));
    CHECK(!parse_pair("Rebel"));
}

static void write_file(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}

// modc t06: planet columns through Variant_Of_Existing_Type (OBJECT_TYPES.md §3.3 / §3.4). The template is in a later
// file (any definition order), a two-step chain, a variant's own tags win, its <Behavior> replaces the list (last one
// wins), an empty tag is a no-op, a missing parent or a cycle keeps only the element's own tags.
static void test_planet_variants() {
    const fs::path tmp = fs::temp_directory_path() / "alamo_data_planet_variants";
    fs::remove_all(tmp);
    const fs::path x = tmp / "Data" / "XML";
    write_file(x / "GAMECONSTANTS.XML", "<GameConstants/>");
    write_file(x / "TRADEROUTEFILES.XML", "<Trade_Route_Files/>");
    write_file(x / "CAMPAIGNFILES.XML", "<Campaign_Files/>");
    write_file(x / "GAMEOBJECTFILES.XML",
               "<Game_Object_Files><File>Planets.xml</File><File>Templates.xml</File></Game_Object_Files>");
    write_file(x / "PLANETS.XML",
               "<Mod_Planets>"
               "<Planet Name=\"A\"><Variant_Of_Existing_Type>TEMPLATE_PLANET</Variant_Of_Existing_Type>"
               "<Galactic_Position>1,2,3</Galactic_Position><Max_Space_Base></Max_Space_Base></Planet>"
               "<Planet Name=\"B\"><Variant_Of_Existing_Type>Rich_Template</Variant_Of_Existing_Type>"
               "<Planet_Credit_Value>7</Planet_Credit_Value></Planet>"
               "<Planet Name=\"C\"><Variant_Of_Existing_Type>Template_Planet</Variant_Of_Existing_Type>"
               "<Behavior>PLANET</Behavior><Behavior>IDLE</Behavior></Planet>"
               "<Planet Name=\"D\"><Variant_Of_Existing_Type>No_Such_Type</Variant_Of_Existing_Type>"
               "<Behavior>PLANET</Behavior></Planet>"
               "<Planet Name=\"E\"><Variant_Of_Existing_Type>E</Variant_Of_Existing_Type><Planet_Credit_Value>3</Planet_Credit_Value></Planet>"
               "</Mod_Planets>");
    write_file(x / "TEMPLATES.XML",
               "<Planets>"
               "<Planet Name=\"Template_Planet\"><Behavior>SELECTABLE, PLANET, PRODUCTION</Behavior>"
               "<Planet_Credit_Value>40</Planet_Credit_Value><Max_Space_Base>5</Max_Space_Base><Max_Ground_Base>4</Max_Ground_Base>"
               "<Always_Instantiate_Galactic>Yes</Always_Instantiate_Galactic></Planet>"
               "<Planet Name=\"Rich_Template\"><Variant_Of_Existing_Type>Template_Planet</Variant_Of_Existing_Type>"
               "<Planet_Credit_Value>90</Planet_Credit_Value><Max_Ground_Base>2</Max_Ground_Base></Planet>"
               "</Planets>");
    DirectoryProvider dp(path_to_utf8(tmp));
    std::vector<std::string> diag;
    GameDatabase db;
    const bool loaded = db.load(dp, {}, &diag);
    if (!loaded)
        for (const std::string& s : diag) std::printf("  diag: %s\n", s.c_str());
    CHECK(loaded);
    const PlanetTypes& p = db.planets();
    const uint32_t a = p.find("A"), b = p.find("B"), c = p.find("C"), d = p.find("D"), e = p.find("E");
    CHECK(a != kInvalidPlanet && b != kInvalidPlanet && c != kInvalidPlanet && d != kInvalidPlanet && e != kInvalidPlanet);
    CHECK(p.flags[a] == (kPlanetAlwaysInstantiate | kPlanetHasPlanetBehavior) && p.credit_value[a] == 40);
    CHECK(p.max_space_base[a] == 5 && p.max_ground_base[a] == 4 && p.position[a].y == 2.0f);  // empty tag: no-op
    CHECK(p.behaviors[a] == (std::vector<std::string>{"SELECTABLE", "PLANET", "PRODUCTION"}));
    CHECK(p.credit_value[b] == 7 && p.max_ground_base[b] == 2 && p.max_space_base[b] == 5);  // two-step chain
    CHECK((p.flags[b] & kPlanetHasPlanetBehavior) && (p.flags[c] & kPlanetAlwaysInstantiate));
    CHECK(p.behaviors[c] == std::vector<std::string>{"IDLE"} && !(p.flags[c] & kPlanetHasPlanetBehavior));
    CHECK(p.flags[d] == kPlanetHasPlanetBehavior && p.credit_value[d] == 0);  // missing parent: own tags
    CHECK(p.flags[e] == 0 && p.credit_value[e] == 3);                         // cycle: own tags
    fs::remove_all(tmp);
}

static void test_database_synthetic() {
    // list_file_crc: standard CRC-32 over the name as written (trimmed), case kept.
    CHECK(list_file_crc("123456789") == 0xCBF43926u);
    CHECK(list_file_crc(" \tCampaigns_A.xml\r\n") == list_file_crc("Campaigns_A.xml"));
    CHECK(list_file_crc("campaigns_a.xml") != list_file_crc("Campaigns_A.xml"));

    const fs::path tmp = fs::temp_directory_path() / "alamo_data_test";
    fs::remove_all(tmp);
    const fs::path x = tmp / "Data" / "XML";
    write_file(x / "GAMECONSTANTS.XML",
               "<GameConstants><MaximumFleetMovementDistance> 120.5 </MaximumFleetMovementDistance>"
               "<Foo>1</Foo><Foo>2</Foo></GameConstants>");
    // Planets.xml is listed twice (different case): Q13 parses it twice, in list order.
    write_file(x / "GAMEOBJECTFILES.XML",
               "<Game_Object_Files><File>Planets.xml</File><File>Missing.xml</File><File>planets.XML</File>"
               "<File>Units.xml</File></Game_Object_Files>");
    write_file(x / "PLANETS.XML",
               "<Planets>\n"
               "<Planet Name=\"Alpha\"><Galactic_Position>1, 2, 3</Galactic_Position><Planet_Credit_Value>60</Planet_Credit_Value>"
               "<Max_Space_Base>5</Max_Space_Base><Always_Instantiate_Galactic>yes</Always_Instantiate_Galactic>"
               "<Behavior>SELECTABLE, PLANET, PRODUCTION</Behavior></Planet>\n"
               "<Planet Name=\"Beta\"><Galactic_Position>10,0,0</Galactic_Position><Behavior> selectable,\n planet </Behavior></Planet>\n"
               "<Planet Name=\"Gamma\"><Galactic_Position>0,10,0</Galactic_Position><Behavior>PLANET</Behavior>"
               "<Behavior>PRODUCTION</Behavior></Planet>\n"
               "<Planet Name=\"Core\"><In_Background>Yes</In_Background><Behavior>IDLE</Behavior></Planet>\n"
               "<Planet Name=\"ALPHA\"><Galactic_Position>9,9,9</Galactic_Position></Planet>\n"  // duplicate, line 6
               "</Planets>");
    write_file(x / "UNITS.XML", "<Units><SpaceUnit Name=\"X_Wing\"/></Units>");

    // Two route files whose CRC order differs from their list order (higher CRC listed first, and
    // the lower one listed twice): the higher-CRC file is parsed last and its Beta_Gamma wins.
    const bool a_hi = list_file_crc("RoutesA.xml") > list_file_crc("RoutesB.xml");
    const std::string hi = a_hi ? "RoutesA.xml" : "RoutesB.xml", lo = a_hi ? "RoutesB.xml" : "RoutesA.xml";
    write_file(x / "TRADEROUTEFILES.XML", "<Trade_Route_Files><File>" + hi + "</File><File>" + lo + "</File><File>" + lo +
                                              "</File></Trade_Route_Files>");
    write_file(x / lo,
               "<TradeRoutes>"
               "<TradeRoute Name=\"Alpha_Beta\"><Point_A>Alpha</Point_A><Point_B>Beta</Point_B><HS_Speed_Factor>5.0</HS_Speed_Factor>"
               "<Credit_Gain_Factor>0.5</Credit_Gain_Factor><Visible_Line_Name>DEFAULT</Visible_Line_Name></TradeRoute>"
               "<TradeRoute Name=\"Beta_Gamma\"><Point_A>Beta</Point_A><Point_B>Gamma</Point_B><HS_Speed_Factor>3.0</HS_Speed_Factor></TradeRoute>"
               // last wins within a file, whole object: Credit_Gain_Factor / Visible_Line_Name reset
               "<TradeRoute Name=\"alpha_beta\"><Point_A>Alpha</Point_A><Point_B>Beta</Point_B><HS_Speed_Factor>2.0</HS_Speed_Factor></TradeRoute>"
               "<TradeRoute Name=\"Alpha_Delta\"><Point_A>Alpha</Point_A><Point_B>Delta</Point_B></TradeRoute>"
               "<TradeRoute Name=\"Zero\"><Point_A>Alpha</Point_A><Point_B>Gamma</Point_B><HS_Speed_Factor>0</HS_Speed_Factor></TradeRoute>"
               "</TradeRoutes>");
    write_file(x / hi,
               "<TradeRoutes><TradeRoute Name=\"BETA_GAMMA\"><Point_A>Gamma</Point_A><Point_B>Beta</Point_B>"
               "<HS_Speed_Factor>4.0</HS_Speed_Factor></TradeRoute></TradeRoutes>");

    // Campaign files: Dup_GC is defined in A and D; the higher-CRC file wins.
    const bool d_hi = list_file_crc("Campaigns_D.xml") > list_file_crc("Campaigns_A.xml");
    write_file(x / "CAMPAIGNFILES.XML",
               "<Campaign_Files><File>Campaigns_D.xml</File><File>Campaigns_A.xml</File>"
               "<!-- <File>Campaigns_B.xml</File> --></Campaign_Files>");
    write_file(x / "CAMPAIGNS_A.XML",
               "<Campaigns><Campaign Name=\"Test_GC\">"
               "<Campaign_Set> Test_Set </Campaign_Set><Sort_Order>4</Sort_Order><Text_ID> TEXT_X </Text_ID>"
               "<Locations> Core, Alpha,\n Beta, Gamma, Nowhere, Alpha </Locations>"
               "<Trade_Routes> Alpha_Beta, Beta_Gamma, Alpha_Delta, Missing_Route, Zero </Trade_Routes>"
               "<Home_Location> Rebel, Alpha </Home_Location><Home_Location> Empire, Gamma </Home_Location>"
               "<Starting_Active_Player> Rebel </Starting_Active_Player>"
               "<Starting_Forces> Rebel, Alpha, X_Wing </Starting_Forces><Starting_Forces> Empire, Gamma, Tie </Starting_Forces>"
               "<AI_Player_Control> Empire, BasicEmpire </AI_Player_Control>"
               "</Campaign>"
               "<Campaign Name=\"Dup_GC\"><Sort_Order>9</Sort_Order><Locations>Alpha, Beta</Locations><Text_ID>FROM_A</Text_ID></Campaign>"
               "</Campaigns>");
    write_file(x / "CAMPAIGNS_D.XML",
               "<Campaigns><Campaign Name=\"dup_gc\"><Sort_Order>1</Sort_Order><Locations>Gamma</Locations></Campaign></Campaigns>");
    write_file(x / "CAMPAIGNS_B.XML", "<Campaigns><Campaign Name=\"B_GC\"><Locations>Alpha</Locations></Campaign></Campaigns>");

    DirectoryProvider dp(path_to_utf8(tmp));
    std::vector<std::string> diag;
    GameDatabase db;
    CHECK(db.load(dp, {}, &diag));
    CHECK(db.constants().max_fleet_move_distance == 120.5f);
    CHECK(db.constants().get("foo") == std::string_view("2"));  // last occurrence
    CHECK(db.constants().get_int("FOO", 0) == 2);
    CHECK(!db.constants().get("bar"));

    // Game objects: every definition kept (Planets.xml twice + ALPHA), lookup returns the first.
    const PlanetTypes& p = db.planets();
    CHECK(p.size() == 10);
    const uint32_t alpha = p.find("alpha");
    CHECK(alpha == 0 && p.position[alpha].x == 1.0f && p.position[alpha].z == 3.0f);  // first wins
    CHECK(p.duplicate[0] == 0 && p.duplicate[4] == 1 && p.duplicate[5] == 1 && p.duplicate[9] == 1);
    CHECK(p.position[4].x == 9.0f && p.name[4] == "ALPHA");
    CHECK(p.credit_value[alpha] == 60 && p.max_space_base[alpha] == 5);
    CHECK(p.flags[alpha] == (kPlanetAlwaysInstantiate | kPlanetHasPlanetBehavior));
    CHECK(p.behaviors[alpha] == (std::vector<std::string>{"SELECTABLE", "PLANET", "PRODUCTION"}));
    CHECK(p.flags[p.find("Beta")] == kPlanetHasPlanetBehavior);
    CHECK(p.flags[p.find("Gamma")] == kPlanetHasPlanetBehavior && p.behaviors[p.find("Gamma")].size() == 2);
    CHECK(p.flags[p.find("Core")] == kPlanetInBackground);  // IDLE only: not a graph planet
    CHECK(p.behaviors[p.find("Core")] == std::vector<std::string>{"IDLE"});
    CHECK(p.flags[4] == 0 && p.behaviors[4].empty());
    CHECK(p.element[alpha].child_text("Planet_Credit_Value") == "60");
    CHECK(db.find_game_object("x_wing") && db.find_game_object("x_wing")->type == "SpaceUnit");
    CHECK(db.game_objects().size() == 11);
    {
        const auto all = db.find_game_objects("ALPHA");
        // Alpha, ALPHA, then both again from the second listing of Planets.xml.
        CHECK(all.size() == 4 && all[0] == db.find_game_object("alpha") && !all[0]->duplicate);
        CHECK(all.size() == 4 && all[1]->duplicate && all[1]->name == "ALPHA" && all[2]->duplicate && all[3]->duplicate);
    }

    // Trade routes: last wins, whole object.
    CHECK(db.trade_routes().size() == 4);
    const TradeRouteDef* ab = db.find_trade_route("ALPHA_BETA");
    CHECK(ab && ab->name == "alpha_beta" && ab->hs_speed_factor == 2.0f && ab->point_a == "Alpha");
    CHECK(ab && ab->credit_gain_factor == 0.0f && ab->visible_line_name.empty() && ab->definitions == 2);  // no merge
    const TradeRouteDef* bg = db.find_trade_route("beta_gamma");
    CHECK(bg && bg->hs_speed_factor == 4.0f && bg->point_a == "Gamma" && bg->definitions == 2);  // higher-CRC file
    CHECK(bg && bg->file == "Data\\XML\\" + hi && bg->element.child_text("HS_Speed_Factor") == "4.0");
    // Slot order = first appearance in CRC parse order: lo's routes first, BETA_GAMMA kept in its slot.
    CHECK(db.trade_routes()[0].name == "alpha_beta" && db.trade_routes()[1].name == "BETA_GAMMA");

    // Campaigns: CRC order, last wins.
    CHECK(db.campaigns().size() == 2);
    const CampaignDef* dup = db.find_campaign("DUP_GC");
    CHECK(dup && dup->definitions == 2);
    if (dup) {
        if (d_hi) CHECK(dup->name == "dup_gc" && dup->sort_order == 1 && dup->locations.size() == 1 && dup->text_id.empty());
        else CHECK(dup->name == "Dup_GC" && dup->sort_order == 9 && dup->locations.size() == 2 && dup->text_id == "FROM_A");
    }
    // campaigns(): parse order (Campaigns_A first iff its CRC is lower); by_sort_order for UI.
    CHECK(db.campaigns()[d_hi ? 0 : 1].name == "Test_GC");
    {
        const auto ui = db.campaigns_by_sort_order();
        CHECK(ui.size() == 2 && ui[0]->sort_order <= ui[1]->sort_order);
    }
    const CampaignDef* c = db.find_campaign("TEST_GC");
    CHECK(c != nullptr);
    if (c) {
        CHECK(c->campaign_set == "Test_Set" && c->sort_order == 4 && c->text_id == "TEXT_X");
        CHECK(c->locations.size() == 6 && c->trade_routes.size() == 5);
        CHECK(c->home_locations.size() == 2 && c->home_locations[1].first == "Empire" && c->home_locations[1].second == "Gamma");
        CHECK(c->starting_active_player == "Rebel");
        CHECK(c->starting_forces.size() == 2 && c->ai_player_control.size() == 1);
        CHECK(c->raw_values("Home_Location").size() == 2);
        CHECK(c->raw_value("sort_order") == "4");
        CHECK(c->definitions == 1);
    }
    auto has = [&](std::string_view needle) {
        return std::any_of(diag.begin(), diag.end(), [&](const std::string& s) { return s.find(needle) != std::string::npos; });
    };
    CHECK(has("MISSING.XML") || has("Missing.xml"));
    CHECK(has("listed twice; parsed twice"));
    CHECK(has("collapses with"));  // lo listed twice in TradeRouteFiles.xml
    CHECK(has("duplicate object name 'ALPHA'"));
    CHECK(has("PLANETS.XML(6)") || has("Planets.xml(6)"));
    CHECK(has("duplicate trade route 'alpha_beta' replaces the earlier definition (CONTENT DIFFERS)"));
    CHECK(has("duplicate campaign '"));

    std::vector<std::string> gd;
    GalaxyMap g = build_galaxy_map(db, "test_gc", &gd);
    CHECK(g.campaign_name == "Test_GC");
    CHECK(g.planets.size() == 4);
    CHECK(g.planets.name[0] == "Core" && g.planets.name[1] == "Alpha");
    CHECK(!(g.planets.flags[0] & kPlanetHasPlanetBehavior) && (g.planets.flags[1] & kPlanetHasPlanetBehavior));
    CHECK(g.routes.size() == 2);
    CHECK(g.routes.name[0] == "alpha_beta" && g.routes.point_a[0] == 1 && g.routes.point_b[0] == 2);
    CHECK(g.routes.hs_speed_factor[0] == 2.0f && g.routes.credit_gain_factor[0] == 0.0f);
    CHECK(g.routes.point_a[1] == 3 && g.routes.point_b[1] == 2 && g.routes.hs_speed_factor[1] == 4.0f);
    CHECK(g.max_fleet_move_distance == 120.5f);
    CHECK(g.validate().empty());
    CHECK(gd.size() == 5);  // Nowhere, Alpha twice, Alpha_Delta endpoint, Missing_Route, Zero
    std::vector<std::string> nd;
    CHECK(build_galaxy_map(db, "nope", &nd).planets.size() == 0 && nd.size() == 1);

    // Opt-in campaign list merge: a second tree supplies another list.
    const fs::path tmp2 = fs::temp_directory_path() / "alamo_data_test2";
    fs::remove_all(tmp2);
    write_file(tmp2 / "Data" / "XML" / "CAMPAIGNFILES.XML",
               "<Campaign_Files><File>Campaigns_A.xml</File><File>Campaigns_B.xml</File><File>Campaigns_C.xml</File></Campaign_Files>");
    write_file(tmp2 / "Data" / "XML" / "CAMPAIGNS_C.XML",
               "<Campaigns><Campaign Name=\"C_GC\"><Locations>Beta</Locations></Campaign></Campaigns>");
    DirectoryProvider dp2(path_to_utf8(tmp2));
    LoadOptions lo_opt;
    lo_opt.merge_campaign_lists = true;
    lo_opt.extra_campaign_lists.push_back({"Data\\XML\\CampaignFiles.xml", &dp2});
    GameDatabase db2;
    CHECK(db2.load(dp, lo_opt, nullptr));
    CHECK(db2.campaigns().size() == 4);  // Test_GC, Dup_GC, B_GC (main tree, unlisted there), C_GC (second tree)
    CHECK(db2.find_campaign("B_GC") && db2.find_campaign("C_GC"));
    CHECK(build_galaxy_map(db2, "C_GC").planets.size() == 1);
    {  // union parsed in CRC order of the listed names
        uint32_t prev = 0;
        for (const CampaignDef& cd : db2.campaigns()) {
            if (cd.definitions != 1) continue;
            const uint32_t k = list_file_crc(std::string_view(cd.file).substr(9));  // strip "Data\XML\"
            CHECK(k >= prev);
            prev = k;
        }
    }

    // Missing list file -> load() false.
    fs::remove(x / "TRADEROUTEFILES.XML");
    DirectoryProvider dp3(path_to_utf8(tmp));
    GameDatabase db3;
    CHECK(!db3.load(dp3, {}, nullptr));
    CHECK(db3.campaigns().size() == 2);  // everything else still loaded
    fs::remove_all(tmp);
    fs::remove_all(tmp2);
}

// Regression: ALAMO_FOC_DATA pointed at a real install (Data\ holds MEGs + MegaFiles.xml, no loose
// XML) made the integration pass index empty tables (campaigns().front(), flags[kInvalidPlanet])
// and crash. The loader must yield a valid empty database for that layout and for junk where
// XML should be, and every accessor / build_galaxy_map must stay safe on it.
static void test_database_install_layout() {
    const fs::path tmp = fs::temp_directory_path() / "alamo_data_test_install";
    fs::remove_all(tmp);
    write_file(tmp / "Data" / "MegaFiles.xml", "<?xml version=\"1.0\"?>\r\n<Mega_Files><File>Data\\Config.meg</File></Mega_Files>");
    write_file(tmp / "Data" / "Config.meg", std::string("\xFF\xFF\xFF\xFF\x00\x01\x02\x03", 8));
    fs::create_directories(tmp / "Data" / "XML");  // the real install has an empty Data\XML
    auto has = [](const std::vector<std::string>& diag, std::string_view needle) {
        return std::any_of(diag.begin(), diag.end(), [&](const std::string& s) { return s.find(needle) != std::string::npos; });
    };
    {
        DirectoryProvider dp(path_to_utf8(tmp));
        std::vector<std::string> diag;
        GameDatabase db;
        CHECK(!db.load(dp, {}, &diag));
        CHECK(db.files().empty() && db.game_objects().empty() && db.planets().size() == 0);
        CHECK(db.trade_routes().empty() && db.campaigns().empty() && db.campaigns_by_sort_order().empty());
        CHECK(!db.constants().element && db.constants().fields.empty() && db.constants().max_fleet_move_distance == 100.0f);
        CHECK(!db.constants().get("MaximumFleetMovementDistance"));
        CHECK(db.planets().find("Galaxy_Core_Art_Model") == kInvalidPlanet);
        CHECK(!db.find_campaign("Sandbox_Gateways_Underworld") && !db.find_trade_route("Anaxes_Carida"));
        CHECK(!db.find_game_object("Hoth") && db.find_game_objects("Hoth").empty());
        CHECK(has(diag, "unable to open XML file 'Data\\XML\\CampaignFiles.xml'"));
        CHECK(has(diag, "hint: no XML found loose but Data\\MegaFiles.xml exists"));
        std::vector<std::string> gd;
        GalaxyMap g = build_galaxy_map(db, "Sandbox_Gateways_Underworld", &gd);
        CHECK(g.planets.size() == 0 && g.routes.size() == 0 && g.validate().empty() && gd.size() == 1);
        CHECK(g.planets.find("Hoth") == kInvalidPlanet);
    }
    {  // junk where XML should be: binary / UTF-16 / text-only list files, entries naming non-XML
        const fs::path x = tmp / "Data" / "XML";
        write_file(x / "GameConstants.xml", std::string("\x00\x01\x02<\x00", 5));
        write_file(x / "GameObjectFiles.xml",
                   "<L>text<File>..\\Config.meg</File><File>Bin.xml</File><File>  </File><File>Sub</File>"
                   "<File>Objs.xml</File></L>");
        write_file(x / "Bin.xml", std::string("\xFF\xFE<\0R\0>\0", 8));
        fs::create_directories(x / "Sub");
        write_file(x / "Objs.xml", "<Objs><Planet/><Planet Name=\"\"/><Planet Name=\"P\"><Galactic_Position>x</Galactic_Position>"
                                   "<Behavior>,,</Behavior></Planet><Unit Name=\"P\"/></Objs>");
        write_file(x / "TradeRouteFiles.xml", "just text, no element");
        write_file(x / "CampaignFiles.xml", "<C><File>Camp.xml</File><File>Camp.xml</File></C>");
        write_file(x / "Camp.xml",
                   "<Campaigns><Campaign Name=\"G\"><Locations>,,P,,P</Locations><Trade_Routes>R,,R</Trade_Routes>"
                   "<Home_Location>,</Home_Location><Sort_Order>zz</Sort_Order></Campaign><Campaign/>"
                   "<Campaign Name=\"  \"/><Campaign Name=\"g\"><Locations>P</Locations></Campaign>");  // unclosed root
        DirectoryProvider dp(path_to_utf8(tmp));
        std::vector<std::string> diag;
        GameDatabase db;
        CHECK(!db.load(dp, {}, &diag));  // GameConstants.xml and TradeRouteFiles.xml unparsable
        CHECK(!db.constants().element && db.constants().max_fleet_move_distance == 100.0f);
        CHECK(db.trade_routes().empty());
        CHECK(db.planets().size() == 1 && db.planets().find("p") == 0 && db.planets().position[0].x == 0.0f);
        CHECK(db.game_objects().size() == 2 && db.game_objects()[1].duplicate);
        CHECK(db.campaigns().size() == 1 && db.campaigns()[0].definitions == 2 && db.campaigns()[0].name == "g");
        CHECK(!has(diag, "hint:"));  // loose XML exists, so no MEG hint
        std::vector<std::string> gd;
        GalaxyMap g = build_galaxy_map(db, "G", &gd);
        CHECK(g.planets.size() == 1 && g.routes.size() == 0 && g.validate().empty());
    }
    fs::remove_all(tmp);
}

// ------------------------------------------------------------------------------------------
// Integration over the real data

static std::string env_or(const char* name, const char* fallback) {
#ifdef _MSC_VER
    char* v = nullptr;
    size_t len = 0;
    std::string out = fallback;
    if (_dupenv_s(&v, &len, name) == 0 && v) {
        if (*v) out = v;
        std::free(v);
    }
    return out;
#else
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : std::string(fallback);
#endif
}

struct TreeStats {
    size_t files = 0, bytes = 0, nodes = 0, recovered = 0, fatal = 0;
    double ms = 0;
};

// Where a data root's XML comes from: a loose folder with Data\XML (e.g. extracted/*.zip), or a
// game install whose XML lives in MEGs, read through the VFS exactly as alamo_headless does.
struct DataSource {
    std::unique_ptr<FileProvider> provider;
    std::vector<std::string> xml_paths;  // logical "Data\..." path of every XML file, sorted
    std::string kind;
};

static bool has_xml_ext(std::string_view p) {
    return p.size() >= 4 && iequals(p.substr(p.size() - 4), ".xml");
}

// Returns false (with a printed reason) when the root has no usable game XML: the caller skips.
static bool open_source(const std::string& root, bool foc, DataSource& out) {
    auto dp = std::make_unique<DirectoryProvider>(root);
    if (dp->exists("Data\\XML\\GameConstants.xml")) {
        std::error_code ec;
        const fs::path base = path_from_utf8(root);
        for (auto it = fs::recursive_directory_iterator(base / "Data", ec); !ec && it != fs::recursive_directory_iterator();
             it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            std::string rel = path_to_utf8(it->path().lexically_relative(base));
            if (!has_xml_ext(rel)) continue;
            std::replace(rel.begin(), rel.end(), '/', '\\');
            out.xml_paths.push_back(std::move(rel));
        }
        std::sort(out.xml_paths.begin(), out.xml_paths.end());
        out.provider = std::move(dp);
        out.kind = "loose folder";
        return true;
    }
#ifdef ALAMO_DATA_HAVE_VFS
    vfs::VfsConfig cfg;
    cfg.foc_root = root;
    if (!foc) cfg.eaw_root = "";  // an EaW root is itself the base; no second EaW side
    std::string err;
    auto v = vfs::Vfs::create(cfg, &err);
    if (!v) {
        std::printf("  no loose Data\\XML and the VFS failed (%s)\n", err.c_str());
        return false;
    }
    if (!v->exists("Data\\XML\\GameConstants.xml")) {
        std::printf("  no Data\\XML\\GameConstants.xml loose or in %zu mounted MEG(s)\n", v->mounts().size());
        return false;
    }
    for (std::string& p : v->list("Data\\**"))
        if (has_xml_ext(p)) out.xml_paths.push_back(std::move(p));
    out.kind = "VFS, " + std::to_string(v->mounts().size()) + " MEG(s) mounted";
    out.provider = std::move(v);
    return true;
#else
    (void)foc;
    std::printf("  no loose Data\\XML (a game install keeps it in MEGs; this build has no vfs module)\n");
    return false;
#endif
}

static TreeStats parse_tree(const DataSource& src, bool verbose) {
    TreeStats st;
    std::vector<std::pair<std::string, std::vector<uint8_t>>> blobs;
    for (const std::string& p : src.xml_paths) {
        std::vector<uint8_t> b;
        if (src.provider->read(p, b)) blobs.emplace_back(p, std::move(b));
        else std::printf("  unreadable %s\n", p.c_str());
    }
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<XmlDocument> docs;
    docs.reserve(blobs.size());
    for (const auto& [path, b] : blobs) docs.push_back(XmlDocument::parse(b, path));
    const auto t1 = std::chrono::steady_clock::now();
    st.ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    for (size_t i = 0; i < docs.size(); ++i) {
        const XmlDocument& d = docs[i];
        ++st.files;
        st.bytes += blobs[i].second.size();
        st.nodes += d.node_count();
        if (!d.ok()) {
            ++st.fatal;
            for (const auto& w : d.diagnostics()) std::printf("  FATAL %s\n", w.to_string().c_str());
        } else if (!d.diagnostics().empty()) {
            ++st.recovered;
            if (verbose) {
                std::printf("  recovered %s (%zu warning(s))\n", d.file().c_str(), d.diagnostics().size());
                for (size_t k = 0; k < d.diagnostics().size() && k < 3; ++k)
                    std::printf("    %s\n", d.diagnostics()[k].to_string().c_str());
            }
        }
    }
    return st;
}

static void integration(const std::string& root, const DataSource& src, const char* label, bool foc) {
    std::printf("== %s data: %s (%s)\n", label, root.c_str(), src.kind.c_str());
    TreeStats st = parse_tree(src, true);
    std::printf("  parsed %zu XML files, %.1f MB, %zu elements in %.1f ms (%.0f MB/s); recovered %zu, fatal %zu\n", st.files,
                static_cast<double>(st.bytes) / 1048576.0, st.nodes, st.ms,
                st.ms > 0 ? static_cast<double>(st.bytes) / 1048576.0 / (st.ms / 1000.0) : 0.0, st.recovered, st.fatal);
    CHECK(st.fatal == 0);
    CHECK(st.files > 100);

    std::vector<std::string> diag;
    GameDatabase db;
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = db.load(*src.provider, {}, &diag);
    const auto t1 = std::chrono::steady_clock::now();
    CHECK(ok);
    std::printf("  database: %zu files, %zu game objects, %zu planets, %zu trade routes, %zu campaigns, %zu load warnings, %.1f ms\n",
                db.files().size(), db.game_objects().size(), db.planets().size(), db.trade_routes().size(),
                db.campaigns().size(), diag.size(), std::chrono::duration<double, std::milli>(t1 - t0).count());
    for (const auto& w : diag) std::printf("    load: %s\n", w.c_str());
    CHECK(db.constants().max_fleet_move_distance == 100.0f);
    CHECK(db.planets().size() > 40);
    CHECK(db.trade_routes().size() > 100);
    if (foc) CHECK(db.campaigns().size() == 33);
    // Q13 last-wins: vanilla duplicates (routes Anaxes_Carida, Mandalore_Utapau) are identical.
    for (const auto& w : diag) CHECK(w.find("CONTENT DIFFERS") == std::string::npos);
    for (const CampaignDef& c : db.campaigns()) CHECK(c.definitions == 1);
    if (const TradeRouteDef* r = db.find_trade_route("Anaxes_Carida")) CHECK(r->definitions == 2);
    else CHECK(false);
    CHECK(db.campaigns_by_sort_order().size() == db.campaigns().size());
    // Q1: <Behavior> PLANET.
    size_t with_planet_behavior = 0;
    for (size_t i = 0; i < db.planets().size(); ++i)
        if (db.planets().flags[i] & kPlanetHasPlanetBehavior) ++with_planet_behavior;
    std::printf("  planets with PLANET behaviour: %zu / %zu; campaign parse order:", with_planet_behavior, db.planets().size());
    for (const CampaignDef& c : db.campaigns())
        if (&c == &db.campaigns().front() || c.file != (&c)[-1].file) std::printf(" %s", c.file.c_str() + 9);
    std::printf("\n");
    if (foc) {
        CHECK(with_planet_behavior == 64 && db.planets().size() == 65);
        const uint32_t core = db.planets().find("Galaxy_Core_Art_Model");
        CHECK(core != kInvalidPlanet && !(db.planets().flags[core] & kPlanetHasPlanetBehavior));
        CHECK(core != kInvalidPlanet && db.planets().behaviors[core] == std::vector<std::string>{"IDLE"});
        // CampaignFiles.xml is parsed in ascending CRC order of the listed names.
        CHECK(!db.campaigns().empty() && db.campaigns().front().name == "Tutorial_Underworld");
    }

    size_t clean = 0;
    std::map<std::string, size_t> unresolved;  // message -> count
    for (const CampaignDef& c : db.campaigns()) {
        std::vector<std::string> gd;
        GalaxyMap g = build_galaxy_map(db, c.name, &gd);
        const auto errs = g.validate();
        CHECK(errs.empty());
        CHECK(g.planets.size() > 0);
        if (gd.empty()) ++clean;
        std::printf("  %-48s planets %3zu routes %3zu issues %zu\n", c.name.c_str(), g.planets.size(), g.routes.size(), gd.size());
        for (const auto& s : gd) ++unresolved[s];
    }
    std::printf("  campaigns building cleanly: %zu / %zu\n", clean, db.campaigns().size());
    for (const auto& [msg, n] : unresolved) std::printf("    unresolved: %s\n", msg.c_str());

    if (foc) {
        GalaxyMap g = build_galaxy_map(db, "Sandbox_Gateways_Underworld");
        CHECK(g.planets.size() == 28 && g.routes.size() == 23);
        const PlanetIndex hoth = g.planets.find("Hoth");
        CHECK(hoth != kInvalidPlanet && g.planets.position[hoth].x == -220.0f && g.planets.position[hoth].y == -210.0f);
        const PlanetIndex core = g.planets.find("Galaxy_Core_Art_Model");
        CHECK(core != kInvalidPlanet && (g.planets.flags[core] & kPlanetInBackground));
        CHECK(core != kInvalidPlanet && !(g.planets.flags[core] & kPlanetHasPlanetBehavior));
        CHECK(hoth != kInvalidPlanet && (g.planets.flags[hoth] & kPlanetHasPlanetBehavior));
        const CampaignDef* c = db.find_campaign("Sandbox_Gateways_Underworld");
        CHECK(c && c->home_locations.size() == 3 && c->home_locations[2].second == "Wayland");
    }
}

static void integration_merge(const DataSource& foc, const DataSource& eaw) {
    LoadOptions lo;
    lo.merge_campaign_lists = true;
    lo.extra_campaign_lists.push_back({"Data\\XML\\CampaignFiles.xml", eaw.provider.get()});
    GameDatabase db;
    std::vector<std::string> diag;
    CHECK(db.load(*foc.provider, lo, &diag));
    size_t clean = 0;
    for (const CampaignDef& c : db.campaigns()) {
        std::vector<std::string> gd;
        GalaxyMap g = build_galaxy_map(db, c.name, &gd);
        CHECK(g.validate().empty());
        if (gd.empty()) ++clean;
    }
    std::printf("== FoC + EaW campaign lists merged (opt-in): %zu campaigns, %zu build cleanly\n", db.campaigns().size(), clean);
    CHECK(db.find_campaign("Full_Story_Campaign_Empire") != nullptr);
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // keep output if a check aborts the process
    log::set_min_level(log::Level::Error);  // loaders warn a lot by design; tests read diagnostics
    test_xml_basic();
    test_xml_recovery();
    test_xml_robustness();
    test_values();
    test_database_synthetic();
    test_planet_variants();
    test_database_install_layout();

    const std::string foc = env_or("ALAMO_FOC_DATA", "/home/claude/data/foc");
    const std::string eaw = env_or("ALAMO_EAW_DATA", "/home/claude/data/eaw");
    // A root counts only if its XML can be read (loose Data\XML, or through the VFS); anything
    // else is skipped with the reason printed, never run against empty tables.
    auto open = [](const std::string& root, const char* label, bool is_foc, DataSource& src) {
        std::error_code ec;
        if (!fs::is_directory(path_from_utf8(root) / "Data", ec)) {
            std::printf("%s data not found at %s: integration test skipped\n", label, root.c_str());
            return false;
        }
        if (open_source(root, is_foc, src)) return true;
        std::printf("%s data at %s has no readable game XML: integration test skipped\n", label, root.c_str());
        return false;
    };
    DataSource foc_src, eaw_src;
    const bool have_foc = open(foc, "FoC", true, foc_src);
    const bool have_eaw = open(eaw, "EaW", false, eaw_src);
    if (have_foc) integration(foc, foc_src, "FoC", true);
    if (have_eaw) integration(eaw, eaw_src, "EaW", false);
    if (have_foc && have_eaw) integration_merge(foc_src, eaw_src);

    std::printf(g_fail ? "alamo_data_tests: %d failure(s)\n" : "alamo_data_tests: OK\n", g_fail);
    return g_fail ? 1 : 0;
}
