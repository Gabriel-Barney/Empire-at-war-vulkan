// alamo_objects tests (no framework; non-zero exit on failure).
// Schema + value classification, the type loader on a synthetic tree (list order, listed twice,
// missing file, unnamed / duplicate / variant types, nested sub-objects, nothing dropped,
// deterministic reload), the coverage report, then the real game data if present
// (ALAMO_FOC_DATA / ALAMO_EAW_DATA: a loose folder with Data\XML, or an install read through the
// VFS). Real-data coverage reports are written to <build dir>/logs/objects_coverage_<game>.txt.
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>

#include "alamo/core/file_provider.hpp"
#include "alamo/core/hash.hpp"
#include "alamo/core/log.hpp"
#include "alamo/data/game_database.hpp"
#include "alamo/objects/behaviors.hpp"
#include "alamo/objects/coverage.hpp"
#include "alamo/objects/enums.hpp"
#include "alamo/objects/field_schema.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/object_store.hpp"
#include "alamo/objects/type_fields.hpp"
#include "alamo/objects/object_types.hpp"
#ifdef ALAMO_OBJECTS_HAVE_VFS
#include "alamo/vfs/vfs.hpp"
#endif
#ifdef ALAMO_OBJECTS_HAVE_ANIM
#include "alamo/anim/anim_types.hpp"  // test only: the library never links engine/anim
#endif

using namespace alamo;
using namespace alamo::objects;
namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)

static void test_schema() {
    const auto rows = field_schema();
    CHECK(rows.size() == 2918);  // 1675 + 1224 + 14 + 5 rows in docs/re/xml_field_tables.md
    const uint32_t ms = find_field_schema("max_speed");
    CHECK(ms != kNoSchema && rows[ms].code == 0x8 && rows[ms].kind() == FieldKind::Float && rows[ms].offset == 0x37c);
    const uint32_t dummy = find_field_schema("Is_Dummy");
    CHECK(dummy != kNoSchema && rows[dummy].kind() == FieldKind::Bool && rows[dummy].owner == "GameObjectType");
    const uint32_t sc = find_field_schema("Stacking_Category", "Combat_Bonus_Ability");
    CHECK(sc != kNoSchema && rows[sc].owner == "Combat_Bonus_Ability" && rows[sc].kind() == FieldKind::Int);
    // "Type" has no GameObjectType row: first row in document order unless the owner matches.
    const uint32_t ty = find_field_schema("Type");
    CHECK(ty != kNoSchema && rows[ty].owner == "CommandBarComponent");
    const uint32_t ty2 = find_field_schema("Type", "Phase");
    CHECK(ty2 != kNoSchema && rows[ty2].owner == "Phase");
    CHECK(field_schema_ambiguous("Type"));
    CHECK(!field_schema_ambiguous("Max_Speed"));
    CHECK(find_field_schema("No_Such_Tag_Anywhere") == kNoSchema);
    CHECK(find_field_schema("Variant_Of_Existing_Type") != kNoSchema);
    // Segment-owner corrections (gen_field_schema.py OWNER_FIXES / ROW_OWNER_FIXES): table 0xa39ab0's
    // "GameObjectType" runs are HardPoint rows; the Faction tail is the special-ability base.
    const uint32_t tro = find_field_schema("Turret_Rotation_Offset");
    CHECK(tro != kNoSchema && rows[tro].owner == "HardPoint" && rows[tro].table == 1);
    const uint32_t trs = find_field_schema("Turret_Rotate_Speed", "HardPoint");  // type row 0x5a8 vs HardPoint 0x4c0
    CHECK(trs != kNoSchema && rows[trs].owner == "HardPoint" && rows[trs].offset == 0x4c0 &&
          rows[find_field_schema("Turret_Rotate_Speed")].offset == 0x5a8);
    const uint32_t tt = find_field_schema("Tooltip_Text");
    CHECK(tt != kNoSchema && rows[tt].owner != "GameObjectType" && find_type_field("Tooltip_Text") == 0xFFFFFFFFu);
    const uint32_t pd = find_field_schema("Projectile_Damage");  // the GameObjectType row, not HardPoint's 0x478
    CHECK(pd != kNoSchema && rows[pd].owner == "GameObjectType" && rows[pd].offset == 0x474);
    const uint32_t as = find_field_schema("Activation_Style");
    CHECK(as != kNoSchema && rows[as].owner == "Ability_Base" && rows[as].offset == 0x3c);
    for (const FieldSchema& r : rows) CHECK(!(r.table == 1 && r.owner == "GameObjectType"));
    CHECK(behavior_classes().size() == 121);  // RTTI classes deriving from BehaviorClass
    CHECK(behavior_name_count() == 116);
    CHECK(behavior_type_id("squash") == 0x26 && behavior_type_id("WALK_LOCOMOTOR") == beh::Locomotor &&
          behavior_type_id("FIGHTER_LOCOMOTOR") == beh::Locomotor && behavior_type_id("TEAM_TARGETING") == 0x09 &&
          behavior_type_id("EXPLOSION") == 0x20 && behavior_type_id("DUMMY_GROUND_BASE") == 0x5B &&
          behavior_type_id("DUMMY_UPGRADE") == beh::DummyUpgrade &&
          behavior_type_id("DUMMY_DESTROY_AFTER_PLANETARY_BOMBARD") == 0x69 && behavior_type_id("Selectable") == 0x02);
    CHECK(behavior_type_id("NOT_A_BEHAVIOR") == kNoBehavior && behavior_type_name(0x1F).empty() &&
          behavior_type_name(0x2C) == "TACTICAL_SUPER_WEAPON");
    CHECK(is_behavior_list_tag("spacebehavior") && !is_behavior_list_tag("Behaviors"));
    CHECK(kind_of_code(0x2a) == FieldKind::Raw && kind_of_code(0x17) == FieldKind::Name &&
          kind_of_code(0x16) == FieldKind::NumberList && kind_of_code(0x5) == FieldKind::Int);
}

static void test_classify() {
    CHECK(classify_value(FieldKind::Bool, " Yes ") == ParseStatus::Ok);
    CHECK(classify_value(FieldKind::Bool, "maybe") == ParseStatus::Failed);
    CHECK(classify_value(FieldKind::Bool, "  ") == ParseStatus::Empty);
    CHECK(classify_value(FieldKind::Int, "-12") == ParseStatus::Ok);
    CHECK(classify_value(FieldKind::Int, "3.0") == ParseStatus::Lossy);
    CHECK(classify_value(FieldKind::Int, "abc") == ParseStatus::Failed);
    CHECK(classify_value(FieldKind::Float, "2.5") == ParseStatus::Ok);
    CHECK(classify_value(FieldKind::Float, "1e-3") == ParseStatus::Ok);
    CHECK(classify_value(FieldKind::Float, "9f") == ParseStatus::Lossy);
    CHECK(classify_value(FieldKind::Float, "1, 2") == ParseStatus::Lossy);
    CHECK(classify_value(FieldKind::Float, "x") == ParseStatus::Failed);
    CHECK(classify_value(FieldKind::NumberList, "1, 2.5 3") == ParseStatus::Ok);
    CHECK(classify_value(FieldKind::NumberList, "1, a") == ParseStatus::Lossy);
    CHECK(classify_value(FieldKind::NumberList, "a b") == ParseStatus::Failed);
    CHECK(classify_value(FieldKind::Name, "Foo") == ParseStatus::Ok);
    CHECK(classify_value(FieldKind::Raw, "") == ParseStatus::Untyped);
    CHECK(shape_of("Yes") == ValueShape::BoolWord && shape_of("12") == ValueShape::Int &&
          shape_of("1.5") == ValueShape::Float && shape_of("1, 2, 3") == ValueShape::NumberList &&
          shape_of("A_B") == ValueShape::Name && shape_of("A, B") == ValueShape::NameList &&
          shape_of("") == ValueShape::Empty && shape_of("a = b") == ValueShape::Text);
}

static void write_file(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}

// Every element below a file root, once per walk: must equal types + fields (nothing dropped).
static size_t elements_below_roots(const ObjectTypeDatabase& db) {
    size_t n = 0;
    for (const SourceFile& f : db.files())
        if (f.doc->ok()) n += static_cast<size_t>(f.walks) * (f.doc->node_count() - 1);
    return n;
}

static bool same_columns(const ObjectTypeDatabase& a, const ObjectTypeDatabase& b) {
    if (a.type_count() != b.type_count() || a.fields().size() != b.fields().size() || a.tags().size() != b.tags().size())
        return false;
    for (TagId i = 0; i < a.tags().size(); ++i)
        if (a.tags().name[i] != b.tags().name[i]) return false;
    for (TypeIndex t = 0; t < a.type_count(); ++t)
        if (a.types().name[t] != b.types().name[t] || a.types().element[t] != b.types().element[t] ||
            a.types().first_definition[t] != b.types().first_definition[t] ||
            a.types().variant_parent[t] != b.types().variant_parent[t] ||
            a.types().field_begin[t] != b.types().field_begin[t])
            return false;
    const FieldColumns &x = a.fields(), &y = b.fields();
    for (FieldIndex f = 0; f < x.size(); ++f)
        if (x.tag[f] != y.tag[f] || x.type[f] != y.type[f] || x.parent[f] != y.parent[f] || x.text[f] != y.text[f] ||
            x.line[f] != y.line[f] || x.schema[f] != y.schema[f] || x.status[f] != y.status[f])
            return false;
    return true;
}

static void test_loader_synthetic() {
    const fs::path root = fs::temp_directory_path() / "alamo_objects_test";
    std::error_code ec;
    remove_all_long(root, ec);
    write_file(root / "Data/XML/GameObjectFiles.xml",
               "<?xml version=\"1.0\"?>\n<Game_Object_Files>\n"
               "  <File>A.xml</File>\n  <File> B.xml </File>\n  <File>Missing.xml</File>\n  <File>a.XML</File>\n"
               "</Game_Object_Files>\n");
    write_file(root / "Data/XML/A.xml",
               "<Root>\n"
               "  <SpaceUnit Name=\"Ship_A\" Extra=\"1\">\n"                         // line 2
               "    <Max_Speed> 2.5 </Max_Speed>\n"                                  // 3
               "    <Is_Dummy>maybe</Is_Dummy>\n"                                    // 4
               "    <Behavior>A, B</Behavior>\n"
               "    <Behavior>C</Behavior>\n"
               "    <My_Unknown_Tag flag=\"x\">hello</My_Unknown_Tag>\n"             // 7
               "    <Max_Speed>3.5</Max_Speed>\n"
               "    <Abilities SubObjectList=\"Yes\">\n"                             // 9
               "      <Combat_Bonus_Ability Name=\"Bonus\">\n"
               "        <Stacking_Category>2</Stacking_Category>\n"                  // 11
               "      </Combat_Bonus_Ability>\n"
               "    </Abilities>\n"
               "  </SpaceUnit>\n"
               "  <Marker/>\n"                                                       // 15: unnamed
               "</Root>\n");
    write_file(root / "Data/XML/B.xml",
               "<Root>\n"
               "  <SpaceUnit Name=\"Ship_B\"><Variant_Of_Existing_Type>Ship_A</Variant_Of_Existing_Type></SpaceUnit>\n"
               "  <SpaceUnit Name=\"ship_a\"><Max_Speed>9f</Max_Speed></SpaceUnit>\n"
               "  <GroundInfantry Name=\"Fwd\"><Variant_Of_Existing_Type>Later</Variant_Of_Existing_Type></GroundInfantry>\n"
               "  <GroundInfantry Name=\"Orphan\"><Variant_Of_Existing_Type>Nope</Variant_Of_Existing_Type></GroundInfantry>\n"
               "  <Prop Name=\"Later\"/>\n"
               "</Root>\n");

    DirectoryProvider dp(root.string());
    ObjectTypeDatabase db;
    std::vector<std::string> diag;
    CHECK(db.load(dp, &diag));
    const LoadStats& s = db.stats();
    CHECK(s.list_entries == 4 && s.files_missing == 1 && s.files_walked == 3 && db.files().size() == 2);
    CHECK(db.files()[0].walks == 2 && db.files()[1].walks == 1);
    // A: 2 types, walked twice; B: 5 types. Order: A, B, A.
    CHECK(db.type_count() == 9 && s.unnamed == 2);
    CHECK(elements_below_roots(db) == db.type_count() + db.fields().size());
    const TypeColumns& ty = db.types();
    CHECK(ty.name[0] == "Ship_A" && ty.name[1].empty() && ty.name[2] == "Ship_B" && ty.name[7] == "Ship_A");
    CHECK(db.element_name(1) == "Marker" && db.element_name(6) == "Prop");
    // Duplicates: "ship_a" (B) and the second walk of A refer to the first definition.
    CHECK(s.duplicates == 2);  // ship_a, Ship_A (2nd walk of A); unnamed types are never duplicates
    CHECK(ty.first_definition[3] == 0 && ty.first_definition[7] == 0 && ty.first_definition[0] == 0);
    CHECK(db.find("SHIP_A") == 0 && db.find_all("ship_a") == (std::vector<TypeIndex>{0, 3, 7}));
    CHECK(db.find("") == kNone && db.find("nothing") == kNone);
    // Variants: back reference, forward reference, unresolved.
    CHECK(s.variants == 3 && s.variants_unresolved == 1);
    CHECK(ty.variant_parent[2] == 0 && ty.variant_parent[4] == db.find("Later") && ty.variant_parent[5] == kNone);
    // Field access (direct children, last occurrence wins for scalars).
    CHECK(db.get_float(0, "max_speed") == 3.5f);
    CHECK(db.fields_of(0, "Max_Speed").size() == 2);
    CHECK(db.get_list(0, "Behavior") == (std::vector<std::string_view>{"A", "B", "C"}));
    CHECK(!db.get_bool(0, "Is_Dummy").has_value());
    CHECK(db.get_text(0, "My_Unknown_Tag") == std::string_view("hello"));
    CHECK(!db.get_text(0, "Stacking_Category").has_value());  // nested, not a direct child
    // Nested sub-object: schema chosen by the enclosing element, attributes kept.
    const FieldColumns& fc = db.fields();
    bool saw_nested = false, saw_attr = false;
    for (FieldIndex f = ty.field_begin[0]; f < ty.field_end[0]; ++f) {
        if (db.tag_name(f) == "Stacking_Category") {
            saw_nested = fc.depth[f] == 3 && fc.line[f] == 11 && fc.schema[f] != kNoSchema &&
                         field_schema()[fc.schema[f]].owner == "Combat_Bonus_Ability" && fc.status[f] == ParseStatus::Ok &&
                         db.tag_name(fc.parent[f]) == "Combat_Bonus_Ability";
        }
        if (db.tag_name(f) == "My_Unknown_Tag")
            saw_attr = fc.attr_count[f] == 1 && db.attributes()[fc.attr_begin[f]].name == "flag" &&
                       fc.schema[f] == kNoSchema && fc.status[f] == ParseStatus::Untyped && fc.line[f] == 7;
        if (db.tag_name(f) == "Is_Dummy") CHECK(fc.status[f] == ParseStatus::Failed);
        if (db.tag_name(f) == "Abilities") CHECK(fc.child_count[f] == 1 && fc.status[f] == ParseStatus::Untyped);
    }
    CHECK(saw_nested && saw_attr);
    CHECK(ty.attr_count[0] == 2);

    // Determinism: a second load gives identical columns.
    ObjectTypeDatabase db2;
    CHECK(db2.load(dp));
    CHECK(same_columns(db, db2));
    ObjectTypeDatabase moved = std::move(db2);
    CHECK(same_columns(db, moved));

    // Coverage.
    const CoverageReport r = build_coverage(db);
    CHECK(r.tags_unknown == 1 && r.tags_sub_object == 1 && r.occ_failed == 2 && r.occ_lossy == 1);
    CHECK(is_sub_object_element("combat_bonus_ability") && is_sub_object_element("Unit_Ability") &&
          !is_sub_object_element("My_Unknown_Tag"));  // Is_Dummy x2 walks, 9f
    CHECK(r.behaviors.size() == 3 && r.behaviors_unknown == 3 && r.behaviors[0].occurrences == 2 &&
          r.behaviors[0].types == 2 && r.behavior_tags.size() == 1);
    bool unknown_listed = false;
    for (const TagCoverage& c : r.tags)
        if (c.name == "My_Unknown_Tag") unknown_listed = !c.known() && c.occurrences == 2 && c.types == 2 && c.files.size() == 1;
    CHECK(unknown_listed);
    const std::string text = format_coverage(r, db, "synthetic");
    CHECK(text.find("My_Unknown_Tag") != std::string::npos && text.find("Is_Dummy") != std::string::npos);
    CHECK(text == format_coverage(build_coverage(moved), moved, "synthetic"));

    // Missing list file.
    write_file(root / "Empty/Data/XML/Other.xml", "<R/>");
    DirectoryProvider empty((root / "Empty").string());
    ObjectTypeDatabase none;
    CHECK(!none.load(empty));
    CHECK(none.type_count() == 0 && none.find("x") == kNone && none.get_text(0, "x") == std::nullopt);
    remove_all_long(root, ec);
}


static void test_converters() {
    CHECK(c_atoi("  -12abc") == -12 && c_atoi("+7") == 7 && c_atoi("x") == 0 && c_atoi("") == 0);
    CHECK(c_atoi("99999999999") == 2147483647 && c_atoi("-99999999999") == -2147483647 - 1);
    CHECK(c_atoi("3.9") == 3 && c_atoi("\t\n 42") == 42);
    CHECK(c_atof("3.0f") == 3.0 && c_atof("1e3") == 1000.0 && c_atof("1e") == 1.0 && c_atof(".5") == 0.5);
    CHECK(c_atof("-.5e1") == -5.0 && c_atof("abc") == 0.0 && c_atof("0.1") == 0.1 && c_atof("  +2.5x") == 2.5);
    CHECK(std::isinf(c_atof("1e400")) && c_atof("1e400") > 0 && c_atof("1e-400") == 0.0 && c_atof("0x1p4") == 16.0);
    CHECK(std::isinf(c_atof("-Infinity")) && c_atof("-Infinity") < 0 && std::isnan(c_atof("nan")));
    CHECK(static_cast<float>(c_atof("0.995")) == 0.995f);
    CHECK(parse_type_bool("1") && parse_type_bool("yes") && parse_type_bool("True") && parse_type_bool("t"));
    CHECK(!parse_type_bool("10") && !parse_type_bool("No") && !parse_type_bool("") && !parse_type_bool("Nos") &&
          !parse_type_bool("0"));
    CHECK(split_type_list("a, b\tc\r\nd,,") == (std::vector<std::string_view>{"a", "b", "c", "d"}));
    CHECK(split_type_list("A | B C", true) == (std::vector<std::string_view>{"A", "|", "B", "C"}));
    CHECK(split_type_list("A|B, C", false, true) == (std::vector<std::string_view>{"A", "B", "C"}));
    CHECK(type_fields().size() > 850 && type_fields().size() < 900);
    const uint32_t hp = find_type_field("hardpoints");
    CHECK(hp != 0xFFFFFFFFu && type_fields()[hp].merge == Merge::ClearList);
    const uint32_t de = find_type_field("Death_Explosions");
    CHECK(de != 0xFFFFFFFFu && type_fields()[de].merge == Merge::TypeRefList);
    const uint32_t ab = find_type_field("Abilities");
    CHECK(ab != 0xFFFFFFFFu && type_fields()[ab].conv == Conv::SubObjects);
    CHECK(find_type_field("Variant_Of_Existing_Type") != 0xFFFFFFFFu &&
          type_fields()[find_type_field("Variant_Of_Existing_Type")].conv == Conv::TypeRef);
    CHECK(find_type_field("Stacking_Category") == 0xFFFFFFFFu);  // an ability field, not a type field
    for (const auto& [tag, text] : type_field_defaults()) CHECK(find_type_field(tag) != 0xFFFFFFFFu);
}

static void test_types_synthetic() {
    const fs::path root = fs::temp_directory_path() / "alamo_objects_types_test";
    std::error_code ec;
    remove_all_long(root, ec);
    write_file(root / "Data/XML/GameObjectFiles.xml", "<L><File>T1.xml</File><File>T2.xml</File></L>");
    write_file(root / "Data/XML/T1.xml",
               "<R>\n"
               " <SpaceUnit Name=\"Base_Ship\">\n"
               "  <Max_Speed>3.0f</Max_Speed><Tactical_Health> TBD </Tactical_Health><Is_Dummy>Yes</Is_Dummy>\n"
               "  <Hyperspace>10</Hyperspace><HardPoints>HP_A, HP_B</HardPoints>\n"
               "  <Behavior>A</Behavior><Behavior>B</Behavior><Death_Explosions>X</Death_Explosions>\n"
               "  <Nest><Mass>2.0</Mass></Nest><Unknown_X>5</Unknown_X><Build_Cost_Credits>100</Build_Cost_Credits>\n"
               "  <Abilities SubObjectList=\"Yes\"><Combat_Bonus_Ability Name=\"a\"><Max_Speed>99</Max_Speed></Combat_Bonus_Ability></Abilities>\n"
               " </SpaceUnit>\n"
               " <SpaceUnit Name=\"Child\"><Variant_Of_Existing_Type>base_ship</Variant_Of_Existing_Type>\n"
               "  <HardPoints>HP_C</HardPoints><HardPoints>HP_D</HardPoints>\n"
               "  <Death_Explosions>Y</Death_Explosions><Death_Explosions>Z</Death_Explosions><Behavior></Behavior>\n"
               " </SpaceUnit>\n"
               " <SpaceUnit Name=\"Fwd\"><Variant_Of_Existing_Type>Later</Variant_Of_Existing_Type><Max_Speed>7</Max_Speed></SpaceUnit>\n"
               " <SpaceUnit Name=\"Cyc1\"><Variant_Of_Existing_Type>Cyc2</Variant_Of_Existing_Type></SpaceUnit>\n"
               " <SpaceUnit Name=\"Cyc2\"><Variant_Of_Existing_Type>Cyc1</Variant_Of_Existing_Type></SpaceUnit>\n"
               " <SpaceUnit Name=\"Self\"><Variant_Of_Existing_Type>Self</Variant_Of_Existing_Type></SpaceUnit>\n"
               " <SpaceUnit Name=\"Orphan\"><Variant_Of_Existing_Type>Nope</Variant_Of_Existing_Type><Max_Speed>4</Max_Speed></SpaceUnit>\n"
               " <Squadron Name=\"Sq\"><Squadron_Units>F1, F2</Squadron_Units><Build_Cost_Credits>-1</Build_Cost_Credits>\n"
               "  <CategoryMask>Corvette</CategoryMask></Squadron>\n"
               " <SpaceUnit Name=\"F1\"><Max_Speed>5</Max_Speed><Build_Cost_Credits>10</Build_Cost_Credits>\n"
               "  <CategoryMask>Fighter</CategoryMask><AI_Combat_Power>1.5</AI_Combat_Power></SpaceUnit>\n"
               " <SpaceUnit Name=\"F2\"><Max_Speed>3.5</Max_Speed><Build_Cost_Credits>20</Build_Cost_Credits>\n"
               "  <CategoryMask>Fighter | Bomber</CategoryMask><AI_Combat_Power>2</AI_Combat_Power></SpaceUnit>\n"
               "</R>\n");
    write_file(root / "Data/XML/T2.xml",
               "<R><SpaceUnit Name=\"Later\"><Variant_Of_Existing_Type>Base_Ship</Variant_Of_Existing_Type></SpaceUnit>\n"
               "<SpaceUnit Name=\"Later2\"><Variant_Of_Existing_Type>Fwd</Variant_Of_Existing_Type></SpaceUnit></R>\n");
    write_file(root / "Data/XML/HardPointDataFiles.xml", "<L><File>HP.xml</File></L>");
    write_file(root / "Data/XML/HP.xml",
               "<H><HardPoint Name=\"HP_A\"><Fire_Min_Range_Distance>10</Fire_Min_Range_Distance></HardPoint>\n"
               "<HardPoint Name=\"HP_C\"><Fire_Min_Range_Distance>50</Fire_Min_Range_Distance></HardPoint>\n"
               "<HardPoint Name=\"HP_D\"><Fire_Min_Range_Distance>20</Fire_Min_Range_Distance></HardPoint></H>");

    DirectoryProvider dp(root.string());
    ObjectTypeDatabase raw;
    CHECK(raw.load(dp));
    GameObjectTypes ty;
    std::vector<std::string> diag;
    CHECK(ty.build(raw, &dp, &diag));
    auto T = [&](const char* n) { return raw.find(n); };
    const TypeIndex base = T("Base_Ship"), child = T("Child"), fwd = T("Fwd"), sq = T("Sq");
    CHECK(ty.get_float(base, "Max_Speed") == 3.0f && ty.get_float(base, "Tactical_Health") == 100.0f);
    CHECK(ty.get_bool(base, "Is_Dummy") && !ty.get_bool(base, "Hyperspace") && ty.get_float(base, "Mass") == 2.0f);
    CHECK(ty.get_list(base, "Behavior").size() == 3 && ty.get_list(base, "Behavior")[2] == "STARSHIP_COMBATANT");
    CHECK(ty.get_int(base, "Required_Ground_Base_Level") == 1);  // §6.1 normalisation
    CHECK(ty.get_sub_objects(base, "Abilities").size() == 1);
    CHECK(ty.stats().tbd == 1 && ty.stats().tags_ignored >= 1 && ty.stats().subobject_lists == 1);
    // Variant: clearing list (last occurrence), type-name list (all), empty tag no-op, scalars inherited.
    CHECK(ty.resolved(child) && ty.resolve_pass(child) == 0 && ty.variant_parent(child) == base);
    CHECK(std::vector<std::string_view>(ty.get_list(child, "HardPoints").begin(), ty.get_list(child, "HardPoints").end()) ==
          (std::vector<std::string_view>{"HP_D"}));
    CHECK(std::vector<std::string_view>(ty.get_list(child, "Death_Explosions").begin(),
                                        ty.get_list(child, "Death_Explosions").end()) ==
          (std::vector<std::string_view>{"Y", "Z"}));
    // No own Behavior tag (the empty one is a no-op): inherited list + a second STARSHIP_COMBATANT.
    CHECK(ty.get_list(child, "Behavior").size() == 4 && ty.behavior_count(child, "starship_combatant") == 2 &&
          ty.get_float(child, "Max_Speed") == 3.0f &&
          ty.get_bool(child, "Is_Dummy") && ty.get_float(child, "Mass") == 2.0f);
    CHECK(ty.get_list(base, "Death_Explosions").size() == 1);  // parent untouched
    // Forward references resolve in pass 1, cycles / self / missing stay unresolved.
    CHECK(ty.resolved(fwd) && ty.resolve_pass(fwd) == 1 && ty.get_float(fwd, "Max_Speed") == 7.0f &&
          ty.get_float(fwd, "Mass") == 2.0f);
    CHECK(ty.resolved(T("Later")) && ty.resolve_pass(T("Later")) == 0);
    CHECK(ty.resolved(T("Later2")) && ty.resolve_pass(T("Later2")) == 1 && ty.get_float(T("Later2"), "Max_Speed") == 7.0f);
    CHECK(!ty.resolved(T("Cyc1")) && !ty.resolved(T("Cyc2")) && !ty.resolved(T("Self")) && !ty.resolved(T("Orphan")));
    CHECK(ty.get_float(T("Orphan"), "Max_Speed") == 4.0f && ty.get_float(T("Orphan"), "Mass") == 0.995f);
    // S7 t14 (MOVEMENT §3.1.1, type ctor 0x367250): the jetpack's alternate speed / turn rate defaults.
    CHECK(ty.get_float(T("Orphan"), "Alternate_Max_Speed") == 1.0f && ty.get_float(T("Orphan"), "Alternate_Max_Rate_Of_Turn") == 2.0f);
    CHECK(ty.stats().variants == 8 && ty.stats().unresolved == 4 && ty.stats().resolved_pass0 == 2 &&
          ty.stats().resolved_later == 2 && ty.stats().passes == 10);
    // Squadron derivation.
    CHECK(ty.get_int(sq, "Build_Cost_Credits") == 30 && ty.get_float(sq, "Max_Speed") == 3.5f);
    CHECK(ty.get_float(sq, "AI_Combat_Power") == 3.5f && ty.squadron_member(T("F1")) && !ty.squadron_member(base));
    CHECK(std::vector<std::string_view>(ty.get_list(sq, "CategoryMask").begin(), ty.get_list(sq, "CategoryMask").end()) ==
          (std::vector<std::string_view>{"Fighter", "Bomber"}));
    CHECK(ty.get_mask(sq, "CategoryMask") == 0x3 && ty.get_mask(T("F2"), "CategoryMask") == 0x3);  // bit OR, own Corvette replaced
    // Hardpoints: unknown dropped; min attack distance raised (no WEAPON behaviour); child re-derived.
    CHECK(ty.hardpoints(base).size() == 1 && ty.get_float(base, "Targeting_Min_Attack_Distance") == 10.0f);
    CHECK(ty.hardpoints(child).size() == 1 && ty.get_float(child, "Targeting_Min_Attack_Distance") == 20.0f);
    CHECK(ty.columns().max_speed[sq] == 3.5f && ty.columns().tactical_health[base] == 100.0f);
    bool cycle_reported = false;
    for (const std::string& d : diag)
        if (d.find("'Cyc1' unresolved") != std::string::npos) cycle_reported = true;
    CHECK(cycle_reported);
    // Without registries: names kept, nothing resolved or raised.
    GameObjectTypes bare;
    CHECK(bare.build(raw));
    CHECK(bare.hardpoints(base).empty() && bare.get_float(base, "Targeting_Min_Attack_Distance") == 0.0f);
    remove_all_long(root, ec);
}

static bool same_types(const GameObjectTypes& a, const GameObjectTypes& b) {
    if (a.size() != b.size()) return false;
    for (TypeIndex t = 0; t < a.size(); ++t) {
        if (a.resolved(t) != b.resolved(t) || a.resolve_pass(t) != b.resolve_pass(t)) return false;
        const auto va = a.values(t), vb = b.values(t);
        if (va.size() != vb.size()) return false;
        for (size_t k = 0; k < va.size(); ++k) {
            const FieldValue &x = va[k], &y = vb[k];
            if (x.field != y.field || x.b != y.b || x.i != y.i || std::memcmp(&x.f, &y.f, sizeof x.f) != 0 ||
                x.text != y.text || x.items != y.items || x.sub != y.sub || x.v != y.v || x.mask != y.mask)
                return false;
        }
        for (uint8_t s = 0; s < 5; ++s)
            if (!(a.behavior_bits(t, static_cast<BehaviorSet>(s)) == b.behavior_bits(t, static_cast<BehaviorSet>(s))))
                return false;
    }
    return true;
}

// ------------------------------------------------------------------------------------------
// OBJECT_TYPES.md §9 / OBJECT_RUNTIME.md §9 rules

static void test_enum_converters() {
    EnumRegistry reg;  // built-in tables
    const EnumConverter& cat = reg.get(EnumId::GameObjectCategory);
    CHECK(cat.valid && cat.bitfield && !cat.from_file);
    CHECK(cat.convert("Fighter | Bomber") == 0x3 && cat.convert("fighter|BOMBER") == 0x3 &&
          cat.convert("Fighter, Bomber") == 0x3 && cat.convert(" Fighter\tBomber\n") == 0x3);
    CHECK(cat.convert("Fighter | Nope") == std::nullopt);  // one unknown token rejects all
    CHECK(cat.convert(" | , ") == 0 && cat.convert("All") == -1 && cat.convert("Hero") == 0x6400000);
    const EnumConverter& ucc = reg.get(EnumId::UnitCollisionClass);
    CHECK(ucc.convert("  large vehicle ") == 8 && ucc.convert("Large  Vehicle") == std::nullopt &&
          ucc.convert("None") == -1);
    CHECK(reg.get(EnumId::SpaceLayer).convert("Capital | Frigate") == 3);
    CHECK(reg.get(EnumId::MovementClass).convert("space") == 13 && !reg.get(EnumId::MovementClass).convert("A B"));
    CHECK(!reg.get(EnumId::SurfaceFXTrigger).valid);  // no built-in table
    CHECK(reg.get(EnumId::DamageType).convert("damage_wampa") == 18 && !reg.get(EnumId::DamageType).convert("x"));
    CHECK(scan_hex_u64(" 0x10 ") == 16 && scan_hex_u64("10") == 16 && scan_hex_u64("FFFFFFFFFFFFFFFF") == ~0ull &&
          scan_hex_u64("zz") == 0);
    CHECK(ship_class_value("Tank") == 4 && ship_class_value("CAPITAL") == 5 && !ship_class_value("boat"));
    CHECK(projectile_category_value("laser") == 7 && projectile_category_value("Cluster_Bomb") == 9 &&
          projectile_category_value("foo") == -1);
    CHECK(enum_of_type_field("projectile_stun_victims_category_mask") == EnumId::GameObjectCategory &&
          enum_of_type_field("Corruption_2_Tactical_Game_Mode") == EnumId::SubGameMode &&
          enum_of_type_field("Corruption_3_Required_Mission_Type") == EnumId::CorruptionType &&
          !enum_of_type_field("Targeting_Priority_Set") && !enum_of_type_field("Shield_On_Anim"));
    // Dynamic converters: duplicate names / values discard the converter.
    EnumConverter d;
    d.dynamic = true;
    d.valid = true;
    CHECK(d.add("A", 1) && !d.add("B", 1) && !d.valid && !d.lookup("A"));
    EnumConverter st;
    st.valid = true;
    CHECK(st.add("A", 1) && st.add("B", 1) && st.valid);  // static tables: no value check
    BehaviorService s = behavior_service("CAPTURE_POINT");
    CHECK(s.serviced && s.interval == 4 && !behavior_service("MARKER").serviced &&
          !behavior_service("starship_combatant").serviced && !behavior_service("DUMMY_GROUND_BASE").serviced &&
          behavior_service("EXPLOSION").interval == 0 && behavior_service("INDIGENOUS_UNIT").computed &&
          behavior_service("GRAVITY_CONTROL_FIELD").interval == 60 && behavior_service("BURNING").interval == 30 &&
          behavior_service("WALK_LOCOMOTOR").interval == 1 && !behavior_service("NOPE").serviced);
}

// Stand-in for anim::find_anim_type (objects cannot link engine/anim): a few ModelAnimType names.
static int32_t fake_anim_type(std::string_view name) {
    if (iequals(name, "Idle")) return 0;
    if (iequals(name, "Die")) return 7;
    if (iequals(name, "FL_Die")) return 41;
    if (iequals(name, "Space Idle")) return 1;
    return -1;
}

// Targeting_Priority_Set registry and ModelAnimType conversion (OBJECT_TYPES.md §9.1.3).
static void test_s1_registries() {
    const fs::path root = fs::temp_directory_path() / "alamo_objects_s1r_test";
    std::error_code ec;
    remove_all_long(root, ec);
    write_file(root / "Data/XML/GameObjectFiles.xml", "<L><File>U.xml</File></L>");
    write_file(root / "Data/XML/TargetingPrioritySetFiles.xml",
               "<Targeting_Priority_Set_Files><File>TP1.xml</File><File>Missing.xml</File><File>TP2.xml</File>"
               "</Targeting_Priority_Set_Files>");
    write_file(root / "Data/XML/TP1.xml",
               "<Targeting_Priority_Sets><Priority_Set Name=\"Fighter\"><Attack_Priorities>Fighter, 1.0</Attack_Priorities>"
               "</Priority_Set><Priority_Set Name=\"AntiInfantry\"/></Targeting_Priority_Sets>");
    write_file(root / "Data/XML/TP2.xml",
               "<Targeting_Priority_Sets><Priority_Set Name=\"fighter\"/><Priority_Set Name=\"Capital\"/>"
               "<Priority_Set/></Targeting_Priority_Sets>");
    write_file(root / "Data/XML/U.xml",
               "<R>\n"
               " <SpaceUnit Name=\"A\"><Targeting_Priority_Set> FIGHTER </Targeting_Priority_Set>"
               "<Specific_Death_Anim_Type>fl_die</Specific_Death_Anim_Type><Shield_On_Anim>Space Idle</Shield_On_Anim></SpaceUnit>\n"
               " <SpaceUnit Name=\"B\"><Targeting_Priority_Set>Capital</Targeting_Priority_Set>"
               "<Targeting_Priority_Set>Nope</Targeting_Priority_Set><Specific_Death_Anim_Type>Bogus</Specific_Death_Anim_Type></SpaceUnit>\n"
               " <SpaceUnit Name=\"AV\"><Variant_Of_Existing_Type>A</Variant_Of_Existing_Type>"
               "<Targeting_Priority_Set>Nope</Targeting_Priority_Set><Shield_Off_Anim>DIE</Shield_Off_Anim></SpaceUnit>\n"
               " <SpaceUnit Name=\"N\"/>\n"
               "</R>\n");
    DirectoryProvider dp(path_to_utf8(root));
    ObjectTypeDatabase raw;
    CHECK(raw.load(dp));
    const TypeIndex a = raw.find("A"), b = raw.find("B"), av = raw.find("AV"), n = raw.find("N");
    {
        GameObjectTypes ty;
        std::vector<std::string> diag;
        CHECK(ty.build(raw, &dp, &diag, &fake_anim_type));
        auto has_diag = [&](const char* s) {
            return std::any_of(diag.begin(), diag.end(), [&](const std::string& d) { return d.find(s) != std::string::npos; });
        };
        // Registry: file order, duplicates kept, unnamed skipped, missing file skipped (logged).
        CHECK(ty.have_priority_sets() && ty.targeting_priority_set_names().size() == 4 &&
              ty.targeting_priority_set_names()[2] == "fighter" && ty.targeting_priority_set_names()[3] == "Capital");
        CHECK(has_diag("Data\\XML\\Missing.xml"));
        // First definition, case-insensitive; text kept as written.
        CHECK(ty.targeting_priority_set(a) == 0 && ty.get_int(a, "Targeting_Priority_Set") == 0 &&
              ty.get_string(a, "Targeting_Priority_Set") == "FIGHTER");
        // Unknown name: logged, previous value kept.
        CHECK(ty.targeting_priority_set(b) == 3 && ty.get_string(b, "Targeting_Priority_Set") == "Capital" &&
              has_diag("<Targeting_Priority_Set>: Failed to convert value (Nope)!"));
        CHECK(ty.targeting_priority_set(av) == 0);  // variant: rejected -> parent's set kept
        CHECK(ty.targeting_priority_set(n) == -1 && ty.get_string(n, "Targeting_Priority_Set").empty());
        CHECK(ty.stats().priority_set_converted == 2 && ty.stats().priority_set_failed == 2);
        // ModelAnimType through the injected lookup.
        CHECK(ty.anim_types_converted());
        CHECK(ty.get_enum(a, "Specific_Death_Anim_Type") == 41 && ty.get_string(a, "Specific_Death_Anim_Type") == "fl_die" &&
              ty.get_enum(a, "Shield_On_Anim") == 1);
        CHECK(ty.get_enum(b, "Specific_Death_Anim_Type") == 0 && ty.get_string(b, "Specific_Death_Anim_Type").empty() &&
              has_diag("<Specific_Death_Anim_Type>: Failed to convert value (Bogus)!"));
        CHECK(ty.get_enum(av, "Specific_Death_Anim_Type") == 41 && ty.get_enum(av, "Shield_Off_Anim") == 7);
        CHECK(ty.stats().anim_converted == 3 && ty.stats().anim_failed == 1);
        CHECK(is_anim_type_field("specific_death_anim_type") && is_anim_type_field("Shield_Off_Anim") &&
              !is_anim_type_field("Anim_Type") && !is_anim_type_field("Land_Model_Anim_Override_Name"));
    }
    {   // No lookup, no provider: names only, no conversion, no log (the pre-registry behaviour).
        GameObjectTypes ty;
        std::vector<std::string> diag;
        CHECK(ty.build(raw, nullptr, &diag));
        CHECK(!ty.have_priority_sets() && !ty.anim_types_converted() && ty.targeting_priority_set(a) == -1);
        CHECK(ty.get_string(b, "Targeting_Priority_Set") == "Nope" && ty.get_string(b, "Specific_Death_Anim_Type") == "Bogus" &&
              ty.get_enum(a, "Specific_Death_Anim_Type") == 0);
        CHECK(ty.stats().anim_failed == 0 && ty.stats().priority_set_failed == 0);
        for (const std::string& d : diag) CHECK(d.find("Failed to convert") == std::string::npos);
    }
#ifdef ALAMO_OBJECTS_HAVE_ANIM
    {   // The real table: anim::find_anim_type (DIE 7, FL_DIE 41, "Space Idle" 1).
        GameObjectTypes ty;
        CHECK(ty.build(raw, &dp, nullptr, &anim::find_anim_type));
        CHECK(ty.get_enum(a, "Specific_Death_Anim_Type") == 41 && ty.get_enum(a, "Shield_On_Anim") == 1 &&
              ty.get_enum(av, "Shield_Off_Anim") == 7 && ty.stats().anim_failed == 1);
    }
#endif
    remove_all_long(root, ec);
}

static void test_s1_rules() {
    const fs::path root = fs::temp_directory_path() / "alamo_objects_s1c_test";
    std::error_code ec;
    remove_all_long(root, ec);
    write_file(root / "Data/XML/GameObjectFiles.xml", "<L><File>U.xml</File></L>");
    // Own category table (hex: 10 = 16) and a properties table with a duplicate name (discarded).
    write_file(root / "Data/XML/Enum/GameObjectCategoryType.xml",
               "<EnumDefinition><!-- c --><Fighter> 1 </Fighter><Bomber>0x2</Bomber><Big>10</Big></EnumDefinition>");
    write_file(root / "Data/XML/Enum/GameObjectPropertiesType.xml",
               "<EnumDefinition><HealsInfantry>1</HealsInfantry><healsinfantry>2</healsinfantry></EnumDefinition>");
    write_file(root / "Data/XML/U.xml",
               "<R>\n"
               " <SpaceUnit Name=\"M1\"><CategoryMask>fighter | BIG</CategoryMask><MovementClass> space </MovementClass>\n"
               "  <Space_Layer>Capital | Frigate</Space_Layer><UnitCollisionClass> large vehicle </UnitCollisionClass>\n"
               "  <Ship_Class>Tank</Ship_Class><Projectile_Category>laser</Projectile_Category>\n"
               "  <Property_Flags>HealsInfantry</Property_Flags><Affiliation>rebel, Unknown</Affiliation>\n"
               "  <Faction_Anim_Subindex>Rebel, 1</Faction_Anim_Subindex><HardPoints>HP_A</HardPoints></SpaceUnit>\n"
               " <SpaceUnit Name=\"M2\"><CategoryMask>Fighter</CategoryMask><CategoryMask>Fighter, Nope</CategoryMask>\n"
               "  <MovementClass>Bogus</MovementClass><Ship_Class>boat</Ship_Class><Projectile_Category>foo</Projectile_Category></SpaceUnit>\n"
               " <SpaceUnit Name=\"M3\"><CategoryMask> | , </CategoryMask></SpaceUnit>\n"
               " <SpaceUnit Name=\"MV\"><Variant_Of_Existing_Type>M1</Variant_Of_Existing_Type><CategoryMask>Nope</CategoryMask>\n"
               "  <Faction_Anim_Subindex>Empire, 2</Faction_Anim_Subindex><Faction_Anim_Subindex>Pirates 3</Faction_Anim_Subindex></SpaceUnit>\n"
               // §9.3 implicit STARSHIP_COMBATANT.
               " <Projectile Name=\"P\"><Behavior>PROJECTILE</Behavior></Projectile>\n"
               " <Projectile Name=\"PV\"><Variant_Of_Existing_Type>P</Variant_Of_Existing_Type></Projectile>\n"
               " <SpaceUnit Name=\"U\"><Behavior>SELECTABLE</Behavior></SpaceUnit>\n"
               " <SpaceUnit Name=\"UV\"><Variant_Of_Existing_Type>U</Variant_Of_Existing_Type></SpaceUnit>\n"
               " <SpaceUnit Name=\"UW\"><Variant_Of_Existing_Type>U</Variant_Of_Existing_Type><Behavior>TEAM</Behavior></SpaceUnit>\n"
               " <Prop Name=\"D\"><Is_Decoration>Yes</Is_Decoration></Prop>\n"
               " <Prop Name=\"Z\"><Tactical_Health>0</Tactical_Health></Prop>\n"
               " <Prop Name=\"ZP\"><Tactical_Health>0</Tactical_Health><AI_Combat_Power>0.5</AI_Combat_Power></Prop>\n"
               // §9.4 population override.
               " <SpaceUnit Name=\"TSW\"><SpaceBehavior>TACTICAL_SUPER_WEAPON</SpaceBehavior>\n"
               "  <Place_Other_Type_At_Every_Specific_Marker_Position>D</Place_Other_Type_At_Every_Specific_Marker_Position></SpaceUnit>\n"
               " <SpaceUnit Name=\"TSWG\"><Behavior>TACTICAL_SUPER_WEAPON</Behavior><Land_Override_Population_Value>3</Land_Override_Population_Value></SpaceUnit>\n"
               // §9.5 nested company clamp: Inf has Create_Team; IC (4 x Inf, Max_Squad_Size 2) is nested in OC.
               " <GroundInfantry Name=\"Inf\"><Create_Team>Yes</Create_Team><AI_Combat_Power>1</AI_Combat_Power></GroundInfantry>\n"
               " <GroundCompany Name=\"IC\"><Company_Units>Inf, Inf, Inf, Inf</Company_Units><Max_Squad_Size>2</Max_Squad_Size></GroundCompany>\n"
               " <GroundCompany Name=\"OC\"><Company_Units>IC, U</Company_Units></GroundCompany>\n"
               "</R>\n");
    write_file(root / "Data/XML/HardPointDataFiles.xml", "<L><File>HP1.xml</File><File>HP2.xml</File></L>");
    write_file(root / "Data/XML/HP1.xml",
               "<H><HardPoint Name=\"HP_A\"><Fire_Min_Range_Distance>10</Fire_Min_Range_Distance></HardPoint></H>");
    write_file(root / "Data/XML/HP2.xml",
               "<H><HardPoint Name=\"hp_a\"><Fire_Min_Range_Distance>30</Fire_Min_Range_Distance></HardPoint></H>");
    write_file(root / "Data/XML/FactionFiles.xml", "<L><File>F.xml</File></L>");
    write_file(root / "Data/XML/F.xml",
               "<R><Faction Name=\"Rebel\"><Is_Neutral>No</Is_Neutral></Faction>\n"
               "<Faction Name=\"Wild\"><Is_Neutral> Yes </Is_Neutral></Faction>\n"
               "<Faction Name=\"Neutral\"><Is_Neutral>Yes</Is_Neutral></Faction>\n"
               "<Faction Name=\"REBEL\"><Is_Neutral>Yes</Is_Neutral></Faction></R>");

    DirectoryProvider dp(root.string());
    ObjectTypeDatabase raw;
    CHECK(raw.load(dp));
    GameObjectTypes ty;
    std::vector<std::string> diag;
    CHECK(ty.build(raw, &dp, &diag));
    auto T = [&](const char* n) { return raw.find(n); };
    auto has_diag = [&](const char* s) {
        for (const std::string& d : diag)
            if (d.find(s) != std::string::npos) return true;
        return false;
    };
    // Enum / mask values.
    const TypeIndex m1 = T("M1"), m2 = T("M2"), mv = T("MV");
    CHECK(ty.enums().get(EnumId::GameObjectCategory).from_file && !ty.enums().get(EnumId::GameObjectProperties).valid);
    CHECK(ty.get_mask(m1, "CategoryMask") == 0x11 && ty.get_enum(m1, "MovementClass") == 13);
    CHECK(ty.get_int(m1, "Space_Layer") == 3 && ty.get_mask(m1, "Space_Layer") == 3 &&
          ty.get_int(m1, "UnitCollisionClass") == 8 && ty.get_int(m1, "Ship_Class") == 4 &&
          ty.get_int(m1, "Projectile_Category") == 7);
    CHECK(ty.get_mask(m1, "Property_Flags") == 0 && has_diag("<Property_Flags>: Failed to convert value (HealsInfantry)!"));
    CHECK(has_diag("couldn't add enum entry 'healsinfantry'") && has_diag("MovementClassType.xml' not found"));
    // Rejected values keep the previous value (here: the first CategoryMask line).
    CHECK(ty.get_mask(m2, "CategoryMask") == 0x1 && ty.get_list(m2, "CategoryMask").size() == 1 &&
          has_diag("<CategoryMask>: Failed to convert value (Fighter, Nope)!"));
    CHECK(ty.get_int(m2, "MovementClass") == -1 && ty.get_int(m2, "Ship_Class") == -1 &&
          ty.get_int(m2, "Projectile_Category") == -1 && ty.get_int(m2, "Weather_Category") == 0);
    CHECK(ty.value(T("M3"), find_type_field("CategoryMask")) != nullptr && ty.get_mask(T("M3"), "CategoryMask") == 0);
    CHECK(ty.get_mask(mv, "CategoryMask") == 0x11);  // variant: rejected -> parent's value kept
    CHECK(ty.stats().mask_failed == 3 && ty.stats().enum_failed == 1);  // variant re-parse not counted twice
    // 0x3c appends in variants (pairs; never cleared).
    CHECK(std::vector<std::string_view>(ty.get_list(mv, "Faction_Anim_Subindex").begin(),
                                        ty.get_list(mv, "Faction_Anim_Subindex").end()) ==
          (std::vector<std::string_view>{"Rebel", "1", "Empire", "2", "Pirates", "3"}));
    // Hardpoints: last definition wins.
    CHECK(ty.hardpoint_names().size() == 1 && ty.hardpoint_min_range()[0] == 30.0f &&
          ty.stats().hardpoints_redefined == 1 && ty.get_float(m1, "Targeting_Min_Attack_Distance") == 30.0f);
    // Factions: all definitions kept, first match wins; default = first Is_Neutral.
    CHECK(ty.faction_names().size() == 4 && ty.default_faction() == 1 && ty.default_faction_name() == "WILD");
    CHECK(ty.factions(m1).size() == 1 && ty.factions(m1)[0] == 0);  // "rebel" -> first definition, Unknown dropped
    CHECK(ty.get_list(m2, "Affiliation").size() == 1 && ty.get_list(m2, "Affiliation")[0] == "WILD" &&
          ty.factions(m2).size() == 1 && ty.factions(m2)[0] == 1);
    CHECK(ty.factions(mv).size() == 1 && ty.factions(mv)[0] == 0);  // inherited "rebel, Unknown"
    // STARSHIP_COMBATANT quirk.
    CHECK(ty.behavior_count(T("P"), "STARSHIP_COMBATANT") == 1);   // projectile: first parse, empty bitset
    CHECK(ty.behavior_count(T("PV"), "STARSHIP_COMBATANT") == 1);  // parent bitset has PROJECTILE
    CHECK(ty.behavior_count(T("U"), "STARSHIP_COMBATANT") == 1 && ty.behavior_count(T("UV"), "STARSHIP_COMBATANT") == 2);
    CHECK(ty.behavior_count(T("UW"), "STARSHIP_COMBATANT") == 1 && ty.get_list(T("UW"), "Behavior").size() == 2);
    CHECK(ty.behavior_count(T("D"), "STARSHIP_COMBATANT") == 0 && ty.behavior_count(T("Z"), "STARSHIP_COMBATANT") == 0 &&
          ty.behavior_count(T("ZP"), "STARSHIP_COMBATANT") == 1);
    CHECK(ty.has_behavior(T("UV"), BehaviorSet::Generic, beh::StarshipCombatant) &&
          ty.has_behavior(T("UV"), BehaviorSet::Union, beh::Selectable) &&
          !ty.has_behavior(T("UV"), BehaviorSet::Space, beh::Selectable));
    CHECK(ty.has_behavior(T("PV"), BehaviorSet::Generic, beh::Projectile) &&
          ty.has_behavior(T("P"), BehaviorSet::Generic, beh::StarshipCombatant));
    // Population override: Space/Land sets only; -1 forced to 0 too.
    CHECK(ty.get_int(T("TSW"), "Space_Override_Population_Value") == 0 &&
          ty.get_int(T("TSW"), "Land_Override_Population_Value") == -1);
    CHECK(ty.get_int(T("TSWG"), "Space_Override_Population_Value") == -1 &&
          ty.get_int(T("TSWG"), "Land_Override_Population_Value") == 3);
    CHECK(ty.placed_at_markers(T("D")) && !ty.placed_at_markers(T("TSW")));
    // Company power: IC = 4 x 1 (plain); OC = IC x clamp(2/4) + U (0) = 2.
    CHECK(ty.get_float(T("IC"), "AI_Combat_Power") == 4.0f && ty.get_float(T("OC"), "AI_Combat_Power") == 2.0f);

    // Store: duplicates attach twice, per-behaviour service intervals.
    write_file(root / "Data/XML/U.xml",
               "<R><SpaceUnit Name=\"CP\"><Behavior>CAPTURE_POINT, MARKER, CAPTURE_POINT, SELECTABLE</Behavior></SpaceUnit>\n"
               "<SpaceUnit Name=\"MK\"><Behavior>MARKER, DUMMY_STARSHIP</Behavior></SpaceUnit></R>");
    ObjectTypeDatabase raw2;
    CHECK(raw2.load(dp));
    GameObjectTypes ty2;
    CHECK(ty2.build(raw2, &dp));
    IdCounter ids;
    ObjectStore st(ModeKind::Space, StoreKind::Primary, ty2, &ids);
    SpawnParams sp;
    sp.type = raw2.find("CP");
    const ObjectHandle cp = st.create(sp);
    sp.type = raw2.find("MK");
    const ObjectHandle mk = st.create(sp);
    CHECK(std::vector<BehaviorTypeId>(st.behaviors(cp).begin(), st.behaviors(cp).end()) ==
          (std::vector<BehaviorTypeId>{0x3D, beh::Marker, 0x3D, beh::Selectable, beh::StarshipCombatant}));
    CHECK(st.behavior_index(cp, 0x3D) == 2 && st.serviced_behavior_count(cp) == 3 && st.behavior_interval(cp, 0) == 4 &&
          !st.behavior_serviced(cp, 1) && !st.behavior_serviced(cp, 4));
    CHECK(st.serviced().size() == 1 && st.serviced()[0] == cp && st.behaviors(mk).size() == 3);
    CHECK(st.behavior_table(0x3D).members.size() == 1);
    std::vector<std::pair<uint32_t, uint32_t>> calls;  // (frame, index)
    for (uint32_t frame = 0; frame <= 8; ++frame)
        st.update(frame, [&](ObjectStore& s, ObjectHandle h) {
            s.service_behaviors(h, frame, [&](ObjectStore&, ObjectHandle, BehaviorTypeId, uint32_t k) { calls.push_back({frame, k}); });
        });
    size_t cp0 = 0, cp2 = 0, sel = 0;
    for (const auto& [f, k] : calls) {
        if (k == 0) cp0 += (f % 4 == 0) ? 1 : 100;
        if (k == 2) cp2 += (f % 4 == 0) ? 1 : 100;
        if (k == 3) ++sel;
        CHECK(k != 1 && k != 4);
    }
    CHECK(cp0 == 3 && cp2 == 3 && sel == 9 && calls.front() == (std::pair<uint32_t, uint32_t>{0, 0}) &&
          calls[1] == (std::pair<uint32_t, uint32_t>{0, 2}));
    st.destroy(cp);
    st.flush();
    CHECK(st.behavior_table(0x3D).members.empty() && st.alive(mk));
    remove_all_long(root, ec);
}
// ------------------------------------------------------------------------------------------
// OI objects (2026-10-28): behaviour scheduling (TACTICAL_GAPS §6.4 / §6.6) and code 0x52 (§6.5).

static void test_behavior_scheduling() {
    // Pure helpers.
    const BehaviorIntervalConfig cfg;
    CHECK(computed_behavior_interval(beh::Shielded, cfg) == 90 && computed_behavior_interval(beh::Powered, cfg) == 150);
    CHECK(computed_behavior_interval(beh::BaseShield, cfg) == 15 && computed_behavior_interval(beh::LandObstacle, cfg) == 1);
    CHECK(computed_behavior_interval(beh::Hunt, cfg) == 30 && computed_behavior_interval(beh::Hunt, cfg, 1.0f, true) == 1 &&
          computed_behavior_interval(beh::IndigenousUnit, cfg) == 30 && computed_behavior_interval(beh::GuardSpawnHouse, cfg) == 30);
    CHECK(computed_behavior_interval(beh::Shielded, cfg, 0.5f) == 45 && computed_behavior_interval(beh::Shielded, cfg, -1.0f) == 0 &&
          computed_behavior_interval(beh::Shielded, cfg, std::nanf("")) == 0);
    {
        BehaviorIntervalConfig c2;
        c2.shield_recharge_interval_secs = 0.51f;  // 30 x 0.51 + 0.5 = 15.8 -> 15
        CHECK(computed_behavior_interval(beh::Shielded, c2) == 15);
    }
    for (BehaviorTypeId id : {beh::Hunt, beh::Lure, beh::SelfDestruct, beh::DeployTroopers, beh::Barrage, beh::Landing})
        CHECK(behavior_starts_disabled(id));
    CHECK(!behavior_starts_disabled(beh::Selectable) && !behavior_starts_disabled(beh::Shielded));
    // Code 0x52 table.
    CHECK(combat_mod_value("DEFENSE") == 1 && combat_mod_value("speed") == 6 && combat_mod_value(" Fire_Rate, Damage") == 7 &&
          combat_mod_value("TACTICAL_BUILD_TIME") == 11 && combat_mod_value("ABILITY_RECHARGE_TIME") == 10 &&
          combat_mod_value("COMBAT_MOD_SPEED") == 6 && combat_mod_value("combat_mod_Defense") == 1);
    CHECK(!combat_mod_value("COMBAT_MOD_") && !combat_mod_value("COMBAT_MOD_BOGUS"));
    CHECK(!combat_mod_value("bogus") && !combat_mod_value("") && !combat_mod_value(" , ") && !combat_mod_value("SPEEDY"));

    const fs::path root = fs::temp_directory_path() / "alamo_objects_sched_test";
    std::error_code ec;
    remove_all_long(root, ec);
    write_file(root / "Data/XML/GameObjectFiles.xml", "<L><File>U.xml</File></L>");
    write_file(root / "Data/XML/U.xml",
               "<R><SpaceUnit Name=\"S\"><Behavior>SELECTABLE, SHIELDED, CAPTURE_POINT, HUNT, POWERED, EXPLOSION</Behavior>\n"
               "  <SpaceBehavior>BASE_SHIELD, LAND_OBSTACLE, AMBIENT_SFX, MARKER</SpaceBehavior>\n"
               "  <Projectile_Combat_Mod>Speed</Projectile_Combat_Mod>\n"
               "  <Projectile_Blast_Area_Immune_Faction>Rebel</Projectile_Blast_Area_Immune_Faction></SpaceUnit>\n"
               "<SpaceUnit Name=\"V\"><Variant_Of_Existing_Type>S</Variant_Of_Existing_Type>\n"
               "  <Projectile_Combat_Mod>Nope</Projectile_Combat_Mod></SpaceUnit>\n"
               "<SpaceUnit Name=\"W\"><Projectile_Combat_Mod>fire_rate damage</Projectile_Combat_Mod></SpaceUnit></R>");
    DirectoryProvider dp(root.string());
    ObjectTypeDatabase raw;
    CHECK(raw.load(dp));
    GameObjectTypes ty;
    CHECK(ty.build(raw, &dp));
    const TypeIndex s = raw.find("S");
    CHECK(ty.get_int(s, "Projectile_Combat_Mod") == 6 && ty.get_string(s, "Projectile_Combat_Mod") == "Speed");
    CHECK(ty.get_int(raw.find("V"), "Projectile_Combat_Mod") == 6);  // unknown token: inherited value kept
    CHECK(ty.get_int(raw.find("W"), "Projectile_Combat_Mod") == 7);
    CHECK(ty.get_string(s, "Projectile_Blast_Area_Immune_Faction") == "Rebel");
    CHECK(ty.stats().combat_mod_converted == 2 && ty.stats().combat_mod_unknown == 1);

    // Attach: one synced Get_Int(0, interval - 1) per interval >= 2 behaviour, in attach order.
    // Order: SELECTABLE 1, SHIELDED 90, CAPTURE_POINT 4, HUNT 30 (disabled), POWERED 150, EXPLOSION 0,
    // the implicit STARSHIP_COMBATANT (end of the Behavior list, not serviced), BASE_SHIELD 15, LAND_OBSTACLE 1,
    // AMBIENT_SFX 30, MARKER (not serviced).
    const std::vector<uint32_t> intervals = {1, 90, 4, 30, 150, 0, 0, 15, 1, 30, 0};
    IdCounter ids;
    ObjectStore st(ModeKind::Space, StoreKind::Primary, ty, &ids);
    SimRng rng(0x1234u);
    uint64_t mode_frame = 7;
    st.bind_sync_rng(&rng);
    st.bind_mode_frame(&mode_frame);
    SimRng ref(0x1234u);
    SpawnParams sp;
    sp.type = s;
    const ObjectHandle a = st.create(sp);
    if (st.behaviors(a).size() != intervals.size()) {
        std::printf("  behaviours of S:");
        for (uint32_t k = 0; k < st.behaviors(a).size(); ++k) std::printf(" %02x/%u", st.behaviors(a)[k], st.behavior_interval(a, k));
        std::printf("\n");
    }
    CHECK(st.behaviors(a).size() == intervals.size());
    for (uint32_t k = 0; k < intervals.size(); ++k) {
        CHECK(st.behavior_interval(a, k) == intervals[k]);
        const uint32_t want =
            intervals[k] >= 2 ? 7u + static_cast<uint32_t>(ref.range_int(0, static_cast<int32_t>(intervals[k]) - 1)) : 0u;
        CHECK(st.behavior_next_service(a, k) == want);
    }
    CHECK(rng == ref && st.attach_draws() == 6 && rng.draws() >= 6);
    CHECK(st.behavior_disable_count(a, 3) == 1 && !st.behavior_enabled_by_type(a, beh::Hunt) &&
          st.behavior_enabled_by_type(a, beh::Shielded) && st.behavior_enabled(a, 0));
    // A second object continues the stream; a runtime add_behavior draws too (here: interval 30).
    mode_frame = 9;
    const ObjectHandle b = st.create(sp);
    for (uint32_t k = 0; k < intervals.size(); ++k)
        if (intervals[k] >= 2) CHECK(st.behavior_next_service(b, k) == 9u + static_cast<uint32_t>(ref.range_int(0, static_cast<int32_t>(intervals[k]) - 1)));
    CHECK(st.add_behavior(b, 0x1B));  // SPAWN_INDIGENOUS_UNITS, interval 30
    CHECK(st.behavior_next_service(b, 11) == 9u + static_cast<uint32_t>(ref.range_int(0, 29)) && rng == ref && st.attach_draws() == 13);

    // Service: due when next <= frame and the counter is 0; then next = frame + interval.
    std::vector<std::pair<uint32_t, uint32_t>> calls;  // (frame, index) of object a
    const uint32_t shield_first = st.behavior_next_service(a, 1);
    auto run = [&](uint32_t frame) {
        mode_frame = frame;
        st.update(frame, [&](ObjectStore& so, ObjectHandle h) {
            so.service_behaviors(h, frame, [&](ObjectStore&, ObjectHandle hh, BehaviorTypeId, uint32_t k) {
                if (hh == a) calls.push_back({frame, k});
            });
        });
    };
    for (uint32_t f = 10; f < 200; ++f) run(f);
    auto count = [&](uint32_t k) {
        uint32_t n = 0;
        for (const auto& c : calls) n += c.second == k ? 1u : 0u;
        return n;
    };
    CHECK(count(0) == 190 && count(5) == 190 && count(8) == 190);  // interval 1, 0 (EXPLOSION), 1
    CHECK(count(3) == 0 && count(6) == 0 && count(10) == 0);       // HUNT disabled, combatant / MARKER never
    uint32_t first_shield = 0xFFFFFFFFu, shield_calls = 0;
    for (const auto& c : calls)
        if (c.second == 1) {
            if (first_shield == 0xFFFFFFFFu) first_shield = c.first;
            CHECK((c.first - first_shield) % 90 == 0);
            ++shield_calls;
        }
    CHECK(first_shield == std::max<uint32_t>(10, shield_first) && shield_calls >= 2);
    // Disable counter: Enable = max(0, c - 1), Disable = c + 1.
    CHECK(st.enable_behavior(a, 3) == 0 && st.enable_behavior(a, 3) == 0 && st.behavior_enabled_by_type(a, beh::Hunt));
    calls.clear();
    run(200);
    CHECK(count(3) == 1);
    CHECK(st.disable_behavior(a, 0) == 1 && st.disable_behavior(a, 0) == 2 && st.enable_behavior(a, 0) == 1);
    calls.clear();
    run(201);
    CHECK(count(0) == 0 && count(5) == 1);
    st.disable_all_behaviors(a);
    calls.clear();
    run(202);
    CHECK(calls.empty() && st.behavior_disable_count(a, 0) == 2 && st.behavior_disable_count(a, 5) == 1);
    // Overrides: STUNNABLE-style end frame, interval re-cache.
    st.enable_behavior(a, 5);
    st.set_behavior_next_service(a, 5, 210);
    st.set_behavior_interval(a, 5, 3);
    calls.clear();
    for (uint32_t f = 203; f < 220; ++f) run(f);
    CHECK(count(5) == 4 && calls.front() == (std::pair<uint32_t, uint32_t>{210, 5}));  // 210, 213, 216, 219
    CHECK(st.behavior_disable_count({}, 0) == 0 && st.enable_behavior({}, 0) == 0 && st.disable_behavior(a, 99) == 0);

    // Config: computed intervals follow GameConstants; an unbound store draws nothing.
    {
        ObjectStore u(ModeKind::Space, StoreKind::Secondary, ty);
        BehaviorIntervalConfig c3;
        c3.shield_recharge_interval_secs = 5.0f;
        u.set_interval_config(c3);
        u.update(42, {});
        const ObjectHandle h = u.create(sp);
        CHECK(u.behavior_interval(h, 1) == 150 && u.behavior_next_service(h, 1) == 42 && u.attach_draws() == 0 &&
              u.mode_frame() == 42 && u.behavior_next_service(h, 0) == 0);
    }
    // Determinism: same seed, same schedule.
    {
        IdCounter ids2;
        ObjectStore t2(ModeKind::Space, StoreKind::Primary, ty, &ids2);
        SimRng r2(0x1234u);
        t2.bind_sync_rng(&r2);
        uint64_t f2 = 7;
        t2.bind_mode_frame(&f2);
        const ObjectHandle a2 = t2.create(sp);
        SimRng r3(0x1234u);
        IdCounter ids3;
        ObjectStore t3(ModeKind::Space, StoreKind::Primary, ty, &ids3);
        t3.bind_sync_rng(&r3);
        t3.bind_mode_frame(&f2);
        const ObjectHandle a3 = t3.create(sp);
        for (uint32_t k = 0; k < intervals.size(); ++k) CHECK(t2.behavior_next_service(a2, k) == t3.behavior_next_service(a3, k));
        CHECK(r2 == r3);
    }
    remove_all_long(root, ec);
}

// ------------------------------------------------------------------------------------------
// Stage 3: object store

static void test_store() {
    const fs::path root = fs::temp_directory_path() / "alamo_objects_store_test";
    std::error_code ec;
    remove_all_long(root, ec);
    write_file(root / "Data/XML/GameObjectFiles.xml", "<L><File>S.xml</File></L>");
    write_file(root / "Data/XML/S.xml",
               "<R><GroundInfantry Name=\"Trooper\"><Tactical_Health>40</Tactical_Health>\n"
               "  <Behavior>SELECTABLE, WALK_LOCOMOTOR, FIGHTER_LOCOMOTOR, BOGUS</Behavior>\n"
               "  <LandBehavior>TEAM</LandBehavior><SpaceBehavior>SHIELDED</SpaceBehavior></GroundInfantry>\n"
               "<Prop Name=\"Rock\"><Behavior>DUMMY_TOOLTIP</Behavior></Prop>\n"
               "<HardPoint_Object Name=\"Turret\"><Behavior>TURRET</Behavior></HardPoint_Object>\n"
               "<Projectile Name=\"Bolt\"><Behavior>PROJECTILE</Behavior></Projectile></R>");
    DirectoryProvider dp(root.string());
    ObjectTypeDatabase raw;
    CHECK(raw.load(dp));
    GameObjectTypes types;
    CHECK(types.build(raw));
    const TypeIndex trooper = raw.find("Trooper"), rock = raw.find("Rock"), turret = raw.find("Turret");

    // Shared id counter across primary stores; secondary has its own.
    IdCounter ids;
    ObjectStore land(ModeKind::Land, StoreKind::Primary, types, &ids);
    ObjectStore space(ModeKind::Space, StoreKind::Primary, types, &ids);
    ObjectStore local(ModeKind::Land, StoreKind::Secondary, types);
    SpawnParams sp;
    sp.type = trooper;
    sp.owner = 1;
    const ObjectHandle a = land.create(sp);
    const ObjectHandle b = space.create(sp);
    const ObjectHandle c = local.create(sp);
    CHECK(land.id(a) == 1 && space.id(b) == 2 && local.id(c) == 1 && ids.next() == 3);
    CHECK(land.health(a) == 40.0f && land.owner(a) == 1 && land.parent_mode_id(a) == kNoObjectId);
    // Behaviours: Behavior list (+ implicit STARSHIP_COMBATANT) then the mode list; two locomotor
    // names = two instances of id 1 (no duplicate check, OBJECT_RUNTIME §9.2); unknown dropped.
    CHECK(std::vector<BehaviorTypeId>(land.behaviors(a).begin(), land.behaviors(a).end()) ==
          (std::vector<BehaviorTypeId>{beh::Selectable, beh::Locomotor, beh::Locomotor, beh::StarshipCombatant, beh::Team}));
    CHECK(std::vector<BehaviorTypeId>(space.behaviors(b).begin(), space.behaviors(b).end()) ==
          (std::vector<BehaviorTypeId>{beh::Selectable, beh::Locomotor, beh::Locomotor, beh::StarshipCombatant, 0x10}));
    CHECK(land.behavior_index(a, beh::Locomotor) == 2 && land.serviced_behavior_count(a) == 4);
    CHECK(land.has_behavior(a, beh::Team) && !land.has_behavior(a, 0x10));
    CHECK(land.add_behavior(a, 0x45) && !land.add_behavior(a, 0x45) && land.behaviors(a).back() == 0x45);
    // S8 t09: Remove_Behavior (the death-clone strip): one instance leaves, later ones move down with their state; the
    // object leaves the per-type table and category list with the last instance. Saved and reloaded unchanged.
    {
        ObjectStore st(ModeKind::Space, StoreKind::Primary, types);
        const ObjectHandle o = st.create(sp);  // Selectable, Locomotor, Locomotor, StarshipCombatant, SHIELDED
        const ObjectHandle o2 = st.create(sp);
        CHECK(!st.remove_behavior(o, 5) && !st.remove_behavior(ObjectHandle{}, 0));
        CHECK(st.remove_behavior(o, 1) && st.behaviors(o).size() == 4 && st.behavior_index(o, beh::Locomotor) == 1 &&
              st.behavior_table(beh::Locomotor).members.size() == 2);
        CHECK(st.remove_behavior(o, 1) && !st.has_behavior(o, beh::Locomotor) && st.behavior_index(o, 0x10) == 2 &&
              !st.behavior_serviced(o, 1) && st.behavior_serviced(o, 2));
        CHECK(st.behavior_table(beh::Locomotor).members.size() == 1 && st.behavior_table(beh::Locomotor).insertion.size() == 1 &&
              st.behavior_table(beh::Locomotor).insertion[0] == o2 && st.has_behavior(o2, beh::Locomotor));
        ByteWriter w;
        st.write(w);
        ObjectStore back(ModeKind::Space, StoreKind::Primary, types);
        const std::vector<uint8_t> saved = w.bytes();
        ByteReader rd(saved);
        CHECK(back.read(rd));
        StateHasher h1, h2;
        st.hash_state(h1);
        back.hash_state(h2);
        CHECK(h1.digest() == h2.digest() && !back.has_behavior(back.find(st.id(o)), beh::Locomotor));
    }
    // Rock has only a dummy behaviour: not serviced.
    sp.type = rock;
    const ObjectHandle r = land.create(sp);
    CHECK(land.serviced().size() == 1 && land.all_objects().size() == 2);

    // Handles: 64-bit value round trip, deferred delete, stale detection, id never reused.
    const ObjectHandle a_copy{a.value};
    CHECK(a_copy == a && land.find(1) == a);
    CHECK(land.destroy(a) && !land.destroy(a));
    CHECK(land.exists(a) && !land.alive(a) && land.find(1) == a && land.all_objects().size() == 2);
    {
        CrcSum only_rock;
        const float p0[3] = {0, 0, 0}, f0[3] = {0, 0, 0};
        only_rock.add(parity_object_crc(p0, f0, land.health(r)));
        CHECK(land.parity_crc() == only_rock.value());  // dead objects are skipped
    }
    land.flush();
    CHECK(!land.exists(a) && !land.find(1) && land.all_objects().size() == 1 && land.serviced().empty());
    CHECK(land.behavior_table(beh::Selectable).members.empty() && land.behavior_table(beh::Selectable).insertion.empty());
    sp.type = trooper;
    const ObjectHandle a2 = land.create(sp);
    CHECK(a2.slot() == a.slot() && a2.generation() == a.generation() + 1 && !land.exists(a) && land.alive(a2));
    CHECK(land.id(a2) == 4);  // ids are never reused (3 went to the rock)
    ids.set_next(kNoObjectId);
    CHECK(ids.allocate() == kNoObjectId + 1);
    ids.set_next(5);

    // Cascade: attached children die with the parent; container references are cleared.
    sp.type = turret;
    sp.attach_parent = a2;
    const ObjectHandle hp1 = land.create(sp);
    sp.attach_parent = hp1;  // a hardpoint on a hardpoint: chained cascade
    const ObjectHandle hp2 = land.create(sp);
    sp.attach_parent = {};
    sp.container = a2;
    const ObjectHandle member = land.create(sp);
    sp.container = {};
    land.destroy(a2);
    land.flush();
    CHECK(!land.exists(a2) && !land.exists(hp1) && !land.exists(hp2) && land.alive(member) &&
          !land.container(member));
    CHECK(land.alive(r) && land.all_objects().size() == 2);

    // Service order: newest registered first; objects created in the loop wait a frame; delayed
    // creations last-to-first; deaths during the loop flushed at the end.
    ObjectStore st(ModeKind::Space, StoreKind::Primary, types, &ids);
    sp.type = trooper;
    std::vector<ObjectHandle> made;
    for (int k = 0; k < 4; ++k) made.push_back(st.create(sp));
    std::vector<ObjectId> order;
    ObjectHandle spawned{};
    st.update(0, [&](ObjectStore& s, ObjectHandle h) {
        order.push_back(s.id(h));
        if (h == made[2]) spawned = s.create(sp);
        if (h == made[3]) s.destroy(made[1]);
    });
    CHECK(order == (std::vector<ObjectId>{st.id(made[3]), st.id(made[2]), st.id(made[0])}));
    CHECK(!st.exists(made[1]) && st.alive(spawned));
    order.clear();
    sp.owner = 5;
    st.queue_create(1, sp);
    sp.owner = 6;
    st.queue_create(1, sp);
    sp.owner = 7;
    st.queue_create(9, sp);
    st.update(1, [&](ObjectStore& s, ObjectHandle h) { order.push_back(s.id(h)); });
    CHECK(order.size() == 4 && order.front() == st.id(spawned));
    const auto newest = st.objects_newest_first();
    CHECK(newest.size() == 6 && st.owner(newest[0]) == 5 && st.owner(newest[1]) == 6);  // last queued created first
    CHECK(st.find_all_of_type(trooper).size() == 6 && st.find_all_of_type(trooper)[0] == newest[0]);
    CHECK(st.owned_by(5).size() == 1 && st.owned_by(1).size() == 4);
    // refix t01: a death clone (kFlagDying | kFlagRemoving) exists, is alive and keeps its id / lists, but active()
    // and find_all_of_type skip it; destroy() still works on it.
    {
        const ObjectHandle clone = newest[0];
        CHECK(st.active(clone) && !st.dying(clone));
        st.set_flag(clone, kFlagDeathClone);
        CHECK((st.flags(clone) & kFlagDeathClone) == kFlagDeathClone && !(st.flags(clone) & kFlagDead));
        CHECK(st.alive(clone) && st.dying(clone) && !st.active(clone) && st.find(st.id(clone)) == clone);
        CHECK(st.find_all_of_type(trooper).size() == 5 && st.find_all_of_type(trooper)[0] == newest[1]);
        CHECK(st.owned_by(5).size() == 1);  // the lists keep it (callers filter with active())
        CHECK(!st.dying(ObjectHandle{}) && !st.active(ObjectHandle{}));
    }
    st.update(9, {});
    CHECK(st.live_count() == 7);

    // Owner change (set_owner): leaves the old owner's list, appended to the new one; hashed, not parity.
    {
        const ObjectHandle mover = st.owned_by(1)[0];
        const ObjectHandle stay = st.owned_by(1)[1];
        StateHasher h0;
        st.hash_state(h0);
        const uint32_t parity0 = st.parity_crc();
        CHECK(st.set_owner(mover, 5));
        CHECK(!st.set_owner(mover, 5) && !st.set_owner(mover, -2) && !st.set_owner(ObjectHandle{}, 1));
        CHECK(st.owner(mover) == 5 && st.owned_by(1).size() == 3 && st.owned_by(1)[0] == stay);
        CHECK(st.owned_by(5).size() == 2 && st.owned_by(5)[1] == mover);
        StateHasher h1;
        st.hash_state(h1);
        CHECK(h0.digest() != h1.digest() && st.parity_crc() == parity0);
        CHECK(st.set_owner(mover, -1) && st.owned_by(-1).size() == 1 && st.owned_by(5).size() == 1);
        CHECK(st.set_owner(mover, 9) && st.owned_by(9).size() == 1);  // grows the list table
        CHECK(st.set_owner(mover, 1) && st.owned_by(1).size() == 4 && st.owned_by(1)[3] == mover);
        st.destroy(stay);
        CHECK(!st.set_owner(stay, 5));  // dead: refused
        st.flush();
    }

    // Parity CRC is order independent; ordered digest registered with the Q1 registry.
    IdCounter i1, i2;
    ObjectStore x(ModeKind::Space, StoreKind::Primary, types, &i1), y(ModeKind::Space, StoreKind::Primary, types, &i2);
    for (int k = 0; k < 5; ++k) {
        sp.position = {static_cast<float>(k), 2.0f * static_cast<float>(k), 1.0f};
        x.create(sp);
    }
    for (int k = 4; k >= 0; --k) {
        sp.position = {static_cast<float>(k), 2.0f * static_cast<float>(k), 1.0f};
        y.create(sp);
    }
    CHECK(x.parity_crc() == y.parity_crc() && x.parity_crc() != 0);
    CrcRegistry reg;
    x.register_crc(reg, "objects.space");
    const CrcRecord rec1 = reg.capture(0);
    x.set_health(x.all_objects()[0], 1.0f);
    const CrcRecord rec2 = reg.capture(1);
    CHECK(rec1.systems.size() == 1 && rec1.systems[0] != rec2.systems[0] && rec1.parity != rec2.parity);

    // Per-behaviour tables: slot order, and per-member outputs independent of the thread count.
    const BehaviorTable& sel = x.behavior_table(beh::Selectable);
    CHECK(sel.members.size() == 5 && std::is_sorted(sel.members.begin(), sel.members.end()) &&
          sel.row(sel.members[3]) == 3 && sel.row(999) == kNone);
    auto run = [&](unsigned threads) {
        std::vector<uint32_t> out(sel.members.size());
        std::vector<std::thread> pool;
        const size_t n = sel.members.size();
        for (unsigned t = 0; t < threads; ++t)
            pool.emplace_back([&, t] {
                for (size_t i = t; i < n; i += threads) {
                    const ObjectHandle h = x.handle_of_slot(sel.members[i]);
                    const Vec3 p = x.position(h);
                    const float pos[3] = {p.x, p.y, p.z}, f[3] = {0, 0, 0};
                    out[i] = parity_object_crc(pos, f, x.health(h));
                }
            });
        for (auto& th : pool) th.join();
        return out;
    };
    CHECK(run(1) == run(2) && run(1) == run(4));

    // Parent-mode death marking: same type, then container children.
    IdCounter gid;
    ObjectStore galaxy(ModeKind::Galactic, StoreKind::Primary, types, &gid);
    ObjectStore battle(ModeKind::Space, StoreKind::Primary, types, &gid);
    sp.type = trooper;
    const ObjectHandle g_unit = galaxy.create(sp);
    sp.type = rock;
    const ObjectHandle g_fleet = galaxy.create(sp);
    sp.type = trooper;
    sp.container = g_fleet;
    const ObjectHandle g_m1 = galaxy.create(sp);
    const ObjectHandle g_m2 = galaxy.create(sp);
    sp.container = {};
    battle.on_parent_mode_death = [&](const ObjectStore& s, ObjectHandle d) { mark_parent_mode_object_for_death(galaxy, s, d); };
    sp.parent_mode_id = galaxy.id(g_unit);
    const ObjectHandle t1 = battle.create(sp);
    sp.parent_mode_id = galaxy.id(g_fleet);
    const ObjectHandle t2 = battle.create(sp);
    const ObjectHandle t3 = battle.create(sp);
    sp.parent_mode_id = kNoObjectId;
    battle.destroy(t1);
    battle.destroy(t2);
    battle.destroy(t3);
    battle.flush();
    CHECK((galaxy.flags(g_unit) & kFlagMarkedByChildMode) && (galaxy.flags(g_m1) & kFlagMarkedByChildMode) &&
          (galaxy.flags(g_m2) & kFlagMarkedByChildMode) && !(galaxy.flags(g_fleet) & kFlagMarkedByChildMode));
    CHECK(galaxy.id(g_unit) == 1 && battle.live_count() == 0 && gid.next() == 8);
    remove_all_long(root, ec);
}

// ------------------------------------------------------------------------------------------
// N1: object store save / load

static uint64_t store_digest(const ObjectStore& s) {
    StateHasher h;
    s.hash_state(h);
    return h.digest();
}

static std::vector<uint8_t> store_bytes(const ObjectStore& s) {
    ByteWriter w;
    s.write(w);
    return w.bytes();
}

struct SaveRng {  // splitmix64: the test's own decisions (not the sim RNG)
    uint64_t s;
    explicit SaveRng(uint64_t seed) : s(seed) {}
    uint64_t next() {
        uint64_t z = (s += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }
    uint32_t below(size_t n) { return n ? static_cast<uint32_t>(next() % n) : 0u; }
    float coord() { return static_cast<float>(below(20000)) * 0.125f - 1000.0f; }
};

// One frame of a deterministic mutation script. Decisions come only from `r` and the store's own state,
// so two stores in equal states stay equal: creates (containers, attached hardpoints, parent-mode ids,
// owners), delayed creations, moves, owner changes, runtime behaviours, disable counters, deaths (also
// inside the service loop, cascading to hardpoints), behaviour service with next-frame overrides.
static void store_step(ObjectStore& s, uint32_t frame, SaveRng& r, const std::vector<TypeIndex>& kinds, TypeIndex turret) {
    std::vector<ObjectHandle> objs(s.all_objects().begin(), s.all_objects().end());
    auto pick = [&]() { return objs.empty() ? ObjectHandle{} : objs[r.below(objs.size())]; };
    auto spawn = [&]() {
        SpawnParams sp;
        sp.type = kinds[r.below(kinds.size())];
        sp.owner = static_cast<int32_t>(r.below(5)) - 1;
        sp.position = {r.coord(), r.coord(), r.coord()};
        sp.facing = {r.coord(), 0.0f, 1.0f};
        if (r.below(3) == 0) sp.container = pick();
        if (sp.type == turret) sp.attach_parent = pick();
        if (r.below(5) == 0) sp.parent_mode_id = static_cast<ObjectId>(r.below(60));
        return sp;
    };
    for (uint32_t k = r.below(4); k > 0; --k) s.create(spawn());
    if (r.below(4) == 0) s.queue_create(frame + r.below(4), spawn());
    for (uint32_t k = r.below(3); k > 0; --k) {
        const ObjectHandle h = pick();
        s.set_position(h, {r.coord(), r.coord(), r.coord()});
        s.set_facing(h, {0.0f, r.coord(), 0.0f});
        s.set_health(h, static_cast<float>(r.below(500)));
    }
    if (r.below(5) == 0) s.set_owner(pick(), static_cast<int32_t>(r.below(6)) - 1);
    if (r.below(6) == 0) s.add_behavior(pick(), r.below(2) ? BehaviorTypeId{0x1B} : beh::Hunt);
    if (r.below(6) == 0) {
        const ObjectHandle h = pick();
        if (s.exists(h) && !s.behaviors(h).empty()) {
            const uint32_t k = r.below(s.behaviors(h).size());
            if (r.below(2)) s.disable_behavior(h, k); else s.enable_behavior(h, k);
        }
    }
    if (r.below(7) == 0) s.set_flag(pick(), kFlagMarkedByChildMode);
    if (r.below(4) == 0) s.destroy(pick());
    s.update(frame, [&](ObjectStore& so, ObjectHandle h) {
        so.service_behaviors(h, frame, [&](ObjectStore& s2, ObjectHandle hh, BehaviorTypeId id, uint32_t idx) {
            if (id == beh::Shielded) s2.set_health(hh, s2.health(hh) + 1.0f);
            if (r.below(40) == 0) s2.set_behavior_next_service(hh, idx, frame + 5);
        });
        if (r.below(60) == 0) so.destroy(h);
    });
}

static void test_store_save() {
    const fs::path root = fs::temp_directory_path() / "alamo_objects_save_test";
    std::error_code ec;
    remove_all_long(root, ec);
    write_file(root / "Data/XML/GameObjectFiles.xml", "<L><File>S.xml</File></L>");
    write_file(root / "Data/XML/S.xml",
               "<R><SpaceUnit Name=\"Ship\"><Tactical_Health>300</Tactical_Health>\n"
               "  <Behavior>SELECTABLE, SHIELDED, CAPTURE_POINT, HUNT, POWERED, EXPLOSION</Behavior>\n"
               "  <SpaceBehavior>BASE_SHIELD, LAND_OBSTACLE, AMBIENT_SFX, MARKER</SpaceBehavior></SpaceUnit>\n"
               "<GroundInfantry Name=\"Trooper\"><Tactical_Health>40</Tactical_Health>\n"
               "  <Behavior>SELECTABLE, WALK_LOCOMOTOR, FIGHTER_LOCOMOTOR</Behavior>\n"
               "  <LandBehavior>TEAM</LandBehavior><SpaceBehavior>SHIELDED</SpaceBehavior></GroundInfantry>\n"
               "<Prop Name=\"Fleet\"><Behavior>DUMMY_TOOLTIP</Behavior></Prop>\n"
               "<HardPoint_Object Name=\"Turret\"><Behavior>TURRET</Behavior></HardPoint_Object>\n"
               "<Projectile Name=\"Bolt\"><Behavior>PROJECTILE</Behavior></Projectile></R>");
    DirectoryProvider dp(root.string());
    ObjectTypeDatabase raw;
    CHECK(raw.load(dp));
    GameObjectTypes types;
    CHECK(types.build(raw, &dp));
    const TypeIndex turret = raw.find("Turret");
    const std::vector<TypeIndex> kinds = {raw.find("Ship"), raw.find("Trooper"), raw.find("Fleet"), turret, raw.find("Bolt")};

    // An empty store round-trips.
    {
        IdCounter i1, i2;
        ObjectStore e1(ModeKind::Space, StoreKind::Primary, types, &i1), e2(ModeKind::Space, StoreKind::Primary, types, &i2);
        const auto b = store_bytes(e1);
        ByteReader r(b);
        CHECK(e2.read(r) && r.done() && store_bytes(e2) == b && store_digest(e2) == store_digest(e1));
    }

    // Populate: many frames of the script (deaths, flushes, slot reuse), garrisons / hardpoints made explicit.
    IdCounter ids;
    ObjectStore a(ModeKind::Space, StoreKind::Primary, types, &ids);
    SimRng rng_a(0xC0FFEEu);
    uint64_t frame_a = 0;
    a.bind_sync_rng(&rng_a);
    a.bind_mode_frame(&frame_a);
    SaveRng ops(99);
    for (uint32_t f = 1; f <= 120; ++f) {
        frame_a = f;
        store_step(a, f, ops, kinds, turret);
    }
    {
        SpawnParams sp;
        sp.type = kinds[2];
        const ObjectHandle fleet = a.create(sp);
        sp.type = kinds[0];
        sp.container = fleet;
        const ObjectHandle ship = a.create(sp);
        sp.container = {};
        sp.type = turret;
        sp.attach_parent = ship;
        const ObjectHandle hp = a.create(sp);
        sp.attach_parent = hp;
        a.create(sp);  // hardpoint on a hardpoint
        CHECK(a.container(ship) == fleet && a.attach_parent(hp) == ship);
    }
    // Free slots for the continuation to reuse (LIFO order saved), then the save point with work in
    // flight: dead objects pending the flush, delayed creations due later.
    for (int k = 0; k < 3; ++k) a.destroy(a.all_objects()[static_cast<size_t>(k) * 5]);
    a.flush();
    a.destroy(a.all_objects()[a.all_objects().size() / 2]);
    a.destroy(a.all_objects()[a.all_objects().size() / 3]);
    {
        SpawnParams sp;
        sp.type = kinds[0];
        sp.owner = 2;
        a.queue_create(125, sp);
        a.queue_create(130, sp);
    }
    size_t dead = 0, slots_free = a.capacity() - a.live_count();
    for (ObjectHandle h : a.all_objects()) dead += a.alive(h) ? 0 : 1;
    CHECK(dead >= 2 && slots_free > 0 && a.live_count() > 50 && a.attach_draws() > 0);
    std::printf("  store save: %zu objects (%zu dead pending), %u slots, %zu free, ids next %d\n", a.live_count(), dead,
                a.capacity(), slots_free, ids.next());

    const std::vector<uint8_t> saved = store_bytes(a);
    {
        StateHasher h;  // same bytes from every compiler / run (compare the MSVC and clang logs)
        h.bytes(saved.data(), saved.size());
        std::printf("  store save: %zu bytes, digest %016llx\n", saved.size(), static_cast<unsigned long long>(h.digest()));
    }
    IdCounter ids_b;
    ObjectStore b(ModeKind::Space, StoreKind::Primary, types, &ids_b);
    SimRng rng_b = rng_a;  // the sync RNG is saved by its owner (GameRandom), not by the store
    uint64_t frame_b = frame_a;
    b.bind_sync_rng(&rng_b);
    b.bind_mode_frame(&frame_b);
    {
        ByteReader r(saved);
        CHECK(b.read(r) && r.done());
    }
    CHECK(store_digest(b) == store_digest(a) && b.parity_crc() == a.parity_crc() && ids_b.next() == ids.next());
    CHECK(store_bytes(b) == saved);  // save -> load -> save byte-identical
    CHECK(std::equal(a.all_objects().begin(), a.all_objects().end(), b.all_objects().begin(), b.all_objects().end()));
    CHECK(std::equal(a.serviced().begin(), a.serviced().end(), b.serviced().begin(), b.serviced().end()));
    for (ObjectHandle h : a.all_objects()) {
        CHECK(b.find(a.id(h)) == h && b.alive(h) == a.alive(h) && b.container(h) == a.container(h));
        CHECK(b.behavior_index(h, beh::Selectable) == a.behavior_index(h, beh::Selectable));
    }
    for (uint32_t id = 0; id < kBehaviorTypeCount; ++id)
        CHECK(a.behavior_table(static_cast<BehaviorTypeId>(id)).members == b.behavior_table(static_cast<BehaviorTypeId>(id)).members);
    CHECK(a.owned_by(1).size() == b.owned_by(1).size() && a.owned_by(-1).size() == b.owned_by(-1).size());

    // Continue both with the same script: identical state, parity and sync-RNG stream every frame.
    {
        SaveRng ops_a(7), ops_b(7);
        bool same = true;
        for (uint32_t f = 121; f <= 260; ++f) {
            frame_a = frame_b = f;
            store_step(a, f, ops_a, kinds, turret);
            store_step(b, f, ops_b, kinds, turret);
            same = same && store_digest(a) == store_digest(b) && a.parity_crc() == b.parity_crc() && rng_a == rng_b;
        }
        CHECK(same);
        CHECK(store_bytes(a) == store_bytes(b) && ids.next() == ids_b.next() && a.attach_draws() == b.attach_draws());
    }

    // Wrong store: other mode / kind / types are refused, nothing changed.
    {
        IdCounter i1;
        ObjectStore land(ModeKind::Land, StoreKind::Primary, types, &i1);
        ObjectStore sec(ModeKind::Space, StoreKind::Secondary, types);
        ByteReader r1(saved), r2(saved);
        CHECK(!land.read(r1) && land.live_count() == 0 && i1.next() == 1);
        CHECK(!sec.read(r2) && sec.live_count() == 0);
        ObjectTypeDatabase raw2;
        GameObjectTypes other;
        write_file(root / "Data/XML/S.xml", "<R><Prop Name=\"Fleet\"><Behavior>DUMMY_TOOLTIP</Behavior></Prop></R>");
        CHECK(raw2.load(dp) && other.build(raw2));
        ObjectStore o(ModeKind::Space, StoreKind::Primary, other, &i1);
        ByteReader r3(saved);
        CHECK(!o.read(r3) && o.live_count() == 0 && i1.next() == 1);
    }

    // Truncated / invalid data: read() false, store and counter unchanged.
    {
        IdCounter ic;
        ObjectStore c(ModeKind::Space, StoreKind::Primary, types, &ic);
        SaveRng ops_c(3);
        for (uint32_t f = 1; f <= 10; ++f) store_step(c, f, ops_c, kinds, turret);
        const std::vector<uint8_t> before = store_bytes(c);
        const uint64_t digest_before = store_digest(c);
        const ObjectId next_before = ic.next();
        auto unchanged = [&] { return store_bytes(c) == before && store_digest(c) == digest_before && ic.next() == next_before; };
        bool all_refused = true;
        for (size_t n = 0; n < saved.size(); n += (n < 256 || n + 256 > saved.size()) ? 1 : 13) {
            ByteReader r(std::span<const uint8_t>(saved).first(n));
            all_refused = all_refused && !c.read(r);
        }
        CHECK(all_refused && unchanged());
        auto refused = [&](size_t off, uint8_t v) {
            std::vector<uint8_t> bad = saved;
            bad[off] = v;
            ByteReader r(bad);
            return !c.read(r) && unchanged();
        };
        // Header: version, mode, kind, type count, types digest. Slot 0 (offset 42): generation 0, used byte 2,
        // used flipped (free list no longer matches), type out of range, first behaviour id 0 / 0x6A.
        CHECK(refused(0, 4) && refused(0, 0) && refused(4, 1) && refused(5, 1) && refused(6, 9) && refused(14, static_cast<uint8_t>(saved[14] ^ 1)));
        CHECK(refused(42, 0) && saved[43] == 0 && saved[44] == 0 && saved[45] == 0);
        CHECK(refused(46, 2) && refused(46, static_cast<uint8_t>(saved[46] ^ 1)) && refused(58, 0x7F));
        CHECK(saved[109] > 0 && refused(113, 0) && refused(113, 0x6A));
        // Single-byte damage anywhere: refused, or accepted as a consistent store (no crash on use).
        size_t accepted = 0;
        for (size_t off = 0; off < saved.size(); off += 7) {
            std::vector<uint8_t> bad = saved;
            bad[off] = static_cast<uint8_t>(bad[off] ^ 0x5A);
            IdCounter ix;
            ObjectStore x(ModeKind::Space, StoreKind::Primary, types, &ix);
            ByteReader r(bad);
            if (!x.read(r)) continue;
            ++accepted;
            SaveRng ops_x(5);
            for (uint32_t f = 300; f < 303; ++f) store_step(x, f, ops_x, kinds, turret);
            (void)store_digest(x);
            (void)x.parity_crc();
        }
        std::printf("  store save: %zu of %zu damaged saves accepted (values only), all used without fault\n", accepted,
                    (saved.size() + 6) / 7);
    }

    // A secondary store (own counter) round-trips too.
    {
        ObjectStore s1(ModeKind::Land, StoreKind::Secondary, types), s2(ModeKind::Land, StoreKind::Secondary, types);
        SaveRng o1(11);
        for (uint32_t f = 1; f <= 30; ++f) store_step(s1, f, o1, kinds, turret);
        const auto bytes = store_bytes(s1);
        ByteReader r(bytes);
        CHECK(s2.read(r) && r.done() && store_digest(s2) == store_digest(s1) && s2.id_counter().next() == s1.id_counter().next());
    }
    // a1h t08 / a1i t05: version 1 (no carried-type section) and version 2 (no landing-persist section) saves still
    // load; this store has no transports, so the v3 save ends with two empty sections (u32 0 each).
    for (const uint8_t ver : {uint8_t{1}, uint8_t{2}}) {
        std::vector<uint8_t> old = saved;
        CHECK(old.size() > 8 && old[old.size() - 8] == 0 && old[old.size() - 1] == 0);
        old[0] = ver;
        old.resize(old.size() - (ver == 1 ? 8u : 4u));
        IdCounter iv;
        ObjectStore v(ModeKind::Space, StoreKind::Primary, types, &iv);
        ByteReader r(old);
        CHECK(v.read(r) && r.done() && store_bytes(v) == saved);  // a moved on after the save point; it saves as v3
    }
    remove_all_long(root, ec);
}

// a1h t08: the transport carried type (TransportDataPack +0x48, A1_READINGS §32.2): only TRANSPORT / TRANSPORT_LANDING
// objects have one; it is saved, hashed only when set, and gone with the object.
static void test_carried_type() {
    const fs::path root = fs::temp_directory_path() / "alamo_objects_carried_test";
    std::error_code ec;
    remove_all_long(root, ec);
    write_file(root / "Data/XML/GameObjectFiles.xml", "<L><File>C.xml</File></L>");
    write_file(root / "Data/XML/C.xml",
               "<R><TransportUnit Name=\"Dropship\"><Behavior>TRANSPORT</Behavior></TransportUnit>\n"
               "<GroundCompany Name=\"Company\"><Behavior>SELECTABLE</Behavior></GroundCompany>\n"
               "<SpaceUnit Name=\"Ship\"><Behavior>SELECTABLE</Behavior></SpaceUnit></R>");
    DirectoryProvider dp(root.string());
    ObjectTypeDatabase raw;
    CHECK(raw.load(dp));
    GameObjectTypes types;
    CHECK(types.build(raw, &dp));
    const TypeIndex drop = raw.find("Dropship"), company = raw.find("Company"), ship = raw.find("Ship");
    CHECK(drop != kNone && company != kNone && ship != kNone);
    IdCounter ids;
    ObjectStore s(ModeKind::Land, StoreKind::Primary, types, &ids);
    SpawnParams sp;
    sp.type = drop;
    const ObjectHandle d = s.create(sp);
    sp.type = ship;
    const ObjectHandle o = s.create(sp);
    const ObjectHandle plain = s.create(sp);
    CHECK(s.add_behavior(o, beh::TransportLanding));  // a landing object: the pack exists through TRANSPORT_LANDING too
    const uint64_t none = store_digest(s);
    CHECK(s.carried_type(d) == kNone && s.carried_type(o) == kNone);
    CHECK(!s.set_carried_type(plain, company) && s.carried_type(plain) == kNone);  // no transport pack
    CHECK(!s.set_carried_type(d, static_cast<TypeIndex>(types.size())) && s.carried_type(d) == kNone);
    CHECK(s.set_carried_type(d, company) && s.carried_type(d) == company && store_digest(s) != none);
    CHECK(s.set_carried_type(o, company) && s.carried_type(o) == company);
    {  // round trip
        const std::vector<uint8_t> b = store_bytes(s);
        IdCounter i2;
        ObjectStore t(ModeKind::Land, StoreKind::Primary, types, &i2);
        ByteReader r(b);
        CHECK(t.read(r) && r.done() && t.carried_type(d) == company && t.carried_type(o) == company &&
              t.carried_type(plain) == kNone && store_digest(t) == store_digest(s) && store_bytes(t) == b);
        // (a1i t05: the last 4 bytes are the empty landing-persist list)
        std::vector<uint8_t> bad = b;  // the last entry's type out of range: refused, store unchanged
        bad[bad.size() - 5] = 0x7F;
        ByteReader rb(bad);
        CHECK(!t.read(rb) && store_bytes(t) == b);
        bad = b;  // the last entry names the plain object's slot (no transport pack): refused
        bad[bad.size() - 12] = static_cast<uint8_t>(plain.slot());
        ByteReader rc(bad);
        CHECK(!t.read(rc) && store_bytes(t) == b);
    }
    CHECK(s.set_carried_type(o, kNone) && s.set_carried_type(d, kNone) && store_digest(s) == none);  // cleared
    // Gone with the object: still readable while dead-pending, none after the flush and for the slot's next object.
    CHECK(s.set_carried_type(d, company) && s.destroy(d));
    CHECK(!s.set_carried_type(d, drop) && s.carried_type(d) == company);
    s.flush();
    CHECK(s.carried_type(d) == kNone);
    sp.type = drop;
    const ObjectHandle n = s.create(sp);
    CHECK(n.slot() == d.slot() && n != d && s.carried_type(n) == kNone);
    // a1i t05: the landing persist flag (TransportDataPack +0xc9): the same objects, saved (version 3), hashed when set.
    const uint64_t before_persist = store_digest(s);
    CHECK(!s.landing_persist(o) && !s.set_landing_persist(plain, true) && !s.landing_persist(plain));
    CHECK(s.set_landing_persist(o, true) && s.landing_persist(o) && store_digest(s) != before_persist);
    {
        const std::vector<uint8_t> b = store_bytes(s);
        IdCounter i3;
        ObjectStore t(ModeKind::Land, StoreKind::Primary, types, &i3);
        ByteReader r(b);
        CHECK(t.read(r) && r.done() && t.landing_persist(o) && !t.landing_persist(n) && store_digest(t) == store_digest(s));
        std::vector<uint8_t> bad = b;  // the persist entry names the plain object's slot (no transport pack): refused
        bad[bad.size() - 4] = static_cast<uint8_t>(plain.slot());
        ByteReader rb(bad);
        CHECK(!t.read(rb) && store_bytes(t) == b);
    }
    CHECK(s.set_landing_persist(o, false) && store_digest(s) == before_persist);
    std::printf("  carried type: TRANSPORT / TRANSPORT_LANDING only, saved, hashed when set, cleared with the object\n");
    remove_all_long(root, ec);
}
// ------------------------------------------------------------------------------------------
// Real data

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

static std::unique_ptr<FileProvider> open_source(const std::string& root, bool foc, std::string& kind) {
    auto dp = std::make_unique<DirectoryProvider>(root);
    if (dp->exists(ObjectTypeDatabase::kListPath)) {
        kind = "loose folder";
        return dp;
    }
#ifdef ALAMO_OBJECTS_HAVE_VFS
    vfs::VfsConfig cfg;
    cfg.foc_root = root;
    if (!foc) cfg.eaw_root = "";  // an EaW root is itself the base
    std::string err;
    auto v = vfs::Vfs::create(cfg, &err);
    if (!v || !v->exists(ObjectTypeDatabase::kListPath)) {
        std::printf("  no GameObjectFiles.xml loose or in MEGs (%s)\n", err.c_str());
        return nullptr;
    }
    kind = "VFS, " + std::to_string(v->mounts().size()) + " MEG(s)";
    return v;
#else
    (void)foc;
    std::printf("  no loose Data\\XML and this build has no vfs module\n");
    return nullptr;
#endif
}

static void integration(const std::string& root, const char* label, bool foc) {
    std::error_code ec;
    if (!fs::is_directory(path_from_utf8(root) / "Data", ec)) {
        std::printf("%s data not found at %s: integration test skipped\n", label, root.c_str());
        return;
    }
    std::string kind;
    std::unique_ptr<FileProvider> src = open_source(root, foc, kind);
    if (!src) {
        std::printf("%s data at %s has no readable object XML: integration test skipped\n", label, root.c_str());
        return;
    }
    std::vector<std::string> diag;
    ObjectTypeDatabase db;
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(db.load(*src, &diag));
    const auto t1 = std::chrono::steady_clock::now();
    const LoadStats& s = db.stats();
    const CoverageReport r = build_coverage(db);
    std::printf("== %s objects: %s (%s), %.1f ms\n", label, root.c_str(), kind.c_str(),
                std::chrono::duration<double, std::milli>(t1 - t0).count());
    std::printf("  list %u, files %zu (missing %u, XML recoveries %u); types %u (unnamed %u, duplicate names %u); "
                "variants %u (unresolved %u)\n",
                s.list_entries, db.files().size(), s.files_missing, s.xml_warnings, s.types, s.unnamed, s.duplicates,
                s.variants, s.variants_unresolved);
    std::printf("  fields %u, tags %zu: known %u sub-object %u unknown %u; occurrences known %llu sub-object %llu "
                "unknown %llu; typed %llu: ok %llu empty %llu lossy %llu failed %llu; schema rows seen %u/%u\n",
                s.fields, r.tags.size(), r.tags_known, r.tags_sub_object, r.tags_unknown,
                static_cast<unsigned long long>(r.occ_known), static_cast<unsigned long long>(r.occ_sub_object),
                static_cast<unsigned long long>(r.occ_unknown), static_cast<unsigned long long>(r.occ_typed),
                static_cast<unsigned long long>(r.occ_ok), static_cast<unsigned long long>(r.occ_empty),
                static_cast<unsigned long long>(r.occ_lossy), static_cast<unsigned long long>(r.occ_failed),
                r.schema_rows_seen, r.schema_rows);

    for (const std::string& d : diag)
        if (d.find("unable to open") != std::string::npos) std::printf("    load: %s\n", d.c_str());
    // Vanilla EaW lists 11 CIN_*.xml files it does not ship (skipped, as the original does).
    CHECK(s.files_missing == (foc ? 0u : 11u) || !foc);
    if (foc) CHECK(s.files_missing == 0 && s.types == 3345);  // = the 3,345 names alamo_data indexes
    CHECK(s.types > 1500);
    std::printf("  behavior names %zu: known %u, unknown %u; type ids used %u/%u\n", r.behaviors.size(),
                r.behaviors_known, r.behaviors_unknown, r.behavior_ids_used, kBehaviorTypeCount - 1);
    for (const BehaviorCoverage& b : r.behaviors)
        if (b.id == kNoBehavior) std::printf("    unknown behavior name: %.*s (%u uses)\n", static_cast<int>(b.name.size()), b.name.data(), b.occurrences);
    CHECK(elements_below_roots(db) == db.type_count() + db.fields().size());  // nothing dropped
    // Same named-type set as alamo_data's index (which skips unnamed elements).
    data::GameDatabase gdb;
    gdb.load(*src);
    CHECK(gdb.game_objects().size() == db.type_count() - s.unnamed);
    bool same_order = gdb.game_objects().size() == db.type_count() - s.unnamed;
    for (size_t i = 0, t = 0; same_order && i < gdb.game_objects().size(); ++i, ++t) {
        while (db.types().name[t].empty()) ++t;
        if (gdb.game_objects()[i].name != db.types().name[t]) same_order = false;
    }
    CHECK(same_order);
    if (foc) {
        const TypeIndex t = db.find("Galaxy_Core_Art_Model");
        CHECK(t != kNone && db.get_list(t, "Behavior") == std::vector<std::string_view>{"IDLE"});
    }
    // Stage 2: typed, merged view.
    GameObjectTypes types;
    std::vector<std::string> tdiag;
    const auto t2 = std::chrono::steady_clock::now();
#ifdef ALAMO_OBJECTS_HAVE_ANIM
    const AnimTypeLookup anim_lookup = &anim::find_anim_type;
#else
    const AnimTypeLookup anim_lookup = nullptr;
#endif
    CHECK(types.build(db, src.get(), &tdiag, anim_lookup));
    const auto t3 = std::chrono::steady_clock::now();
    const TypeLoadStats& ts = types.stats();
    std::printf("  typed view %.1f ms: variants %u (pass 0 %u, later %u, unresolved %u), passes %u; tags applied %u "
                "ignored %u, TBD %u, sub-object lists %u (unprocessed %u), clamped %u; companies %u squadrons %u, "
                "costs derived %u, base level normalised %u, min range raised %u; hardpoints %zu (dropped %u), "
                "factions %zu (dropped %u)\n",
                std::chrono::duration<double, std::milli>(t3 - t2).count(), ts.variants, ts.resolved_pass0,
                ts.resolved_later, ts.unresolved, ts.passes, ts.tags_applied, ts.tags_ignored, ts.tbd,
                ts.subobject_lists, ts.subobject_unprocessed, ts.clamped, ts.companies, ts.squadrons, ts.cost_derived,
                ts.base_level_normalised, ts.min_range_raised, types.hardpoint_names().size(), ts.hardpoints_dropped,
                types.faction_names().size(), ts.factions_dropped);
    if (foc) CHECK(ts.variants == 911 && ts.resolved_pass0 == 911 && ts.unresolved == 0);
    else CHECK(ts.variants == 449 && ts.resolved_later == 7 && ts.unresolved == 0);
    CHECK(ts.subobject_unprocessed == 0);
    // §9 rules on real data.
    std::printf("  section 9: enums converted %u failed %u, masks converted %u failed %u; STARSHIP_COMBATANT appended "
                "%u times, on %u types (twice or more on %u); empty Affiliation -> %.*s on %u types; population "
                "override forced %u; hardpoints redefined %u\n",
                ts.enum_converted, ts.enum_failed, ts.mask_converted, ts.mask_failed, ts.implicit_combatant_appends,
                ts.combatant_types, ts.combatant_twice, static_cast<int>(types.default_faction_name().size()),
                types.default_faction_name().data(), ts.affiliation_defaulted, ts.population_forced,
                ts.hardpoints_redefined);
    for (const char* e : {"GameObjectCategoryType", "GameObjectPropertiesType", "MovementClassType",
                          "ObjectWeatherCategoryType", "SurfaceFXTriggerType"})
        for (uint8_t k = 0; k < static_cast<uint8_t>(EnumId::Count); ++k) {
            const EnumConverter& c = types.enums().get(static_cast<EnumId>(k));
            if (c.enum_name == e) {
                std::printf("    enum %s: %s, %zu entries\n", e, c.from_file ? "file" : (c.valid ? "built-in" : "ABSENT"),
                            c.entries.size());
                CHECK(c.from_file && c.valid);
            }
        }
    {
        std::vector<std::pair<std::string, uint32_t>> fails;  // unique messages, first-seen order
        for (const std::string& d : tdiag) {
            if (d.find("Failed to convert value") == std::string::npos) continue;
            auto it = std::find_if(fails.begin(), fails.end(), [&](const auto& p) { return p.first == d; });
            if (it == fails.end()) fails.push_back({d, 1});
            else ++it->second;
        }
        for (const auto& [m, n] : fails) std::printf("    %s (x%u)\n", m.c_str(), n);
    }
    std::printf("  priority sets %zu (types converted %u, failed %u); ModelAnimType %s: converted %u, failed %u\n",
                types.targeting_priority_set_names().size(), ts.priority_set_converted, ts.priority_set_failed,
                types.anim_types_converted() ? "anim::find_anim_type" : "not converted (no anim module)",
                ts.anim_converted, ts.anim_failed);
    std::printf("  Projectile_Combat_Mod (code 0x52): converted %u, unknown %u\n", ts.combat_mod_converted, ts.combat_mod_unknown);
    CHECK(ts.combat_mod_unknown == 0 && (!foc || ts.combat_mod_converted > 0));
    if (foc) CHECK(types.have_priority_sets() && types.targeting_priority_set_names().size() == 47);
    CHECK(types.default_faction_name() == "NEUTRAL");
    CHECK(ts.combatant_types > types.size() / 2);
    GameObjectTypes types2;
    types2.build(db, src.get(), nullptr, anim_lookup);
    CHECK(same_types(types, types2));
    // Stage 3: one object of every type in a space store, then everything destroyed.
    {
        IdCounter ids;
        ObjectStore store(ModeKind::Space, StoreKind::Primary, types, &ids);
        SimRng attach_rng(1);  // OI objects: behaviour Initialise draws (interval >= 2)
        store.bind_sync_rng(&attach_rng);
        size_t behaviours = 0;
        for (TypeIndex t = 0; t < types.size(); ++t) {
            SpawnParams p;
            p.type = t;
            behaviours += store.behaviors(store.create(p)).size();
        }
        CHECK(store.live_count() == types.size() && ids.next() == static_cast<ObjectId>(types.size()) + 1);
        const uint32_t crc = store.parity_crc();
        const size_t serviced = store.serviced().size();
        // N1: the real types round-trip through a save; the copy then dies the same way.
        ByteWriter saved;
        store.write(saved);
        IdCounter ids2;
        ObjectStore copy(ModeKind::Space, StoreKind::Primary, types, &ids2);
        ByteReader rd(saved.bytes());
        CHECK(copy.read(rd) && rd.done());
        CHECK(store_digest(copy) == store_digest(store) && copy.parity_crc() == crc && ids2.next() == ids.next());
        for (ObjectHandle h : copy.objects_newest_first()) copy.destroy(h);
        copy.update(1, {});
        for (ObjectHandle h : store.objects_newest_first()) store.destroy(h);
        store.update(1, {});
        CHECK(store_digest(copy) == store_digest(store) && copy.live_count() == 0);
        std::printf("  object store save: %zu bytes for %zu objects, round trip OK\n", saved.size(), types.size());
        std::printf("  object store: %zu objects spawned (%zu behaviours, %zu serviced, %llu Initialise draws, rng %08x), "
                    "parity %08x; all destroyed\n",
                    types.size(), behaviours, serviced, static_cast<unsigned long long>(store.attach_draws()),
                    attach_rng.state(), crc);
        CHECK(store.attach_draws() > 0 && attach_rng.draws() >= store.attach_draws());
        CHECK(store.live_count() == 0);
    }
    // Deterministic: a reload formats the identical report.
    ObjectTypeDatabase again;
    again.load(*src);
    const std::string text = format_coverage(r, db, label);
    CHECK(text == format_coverage(build_coverage(again), again, label));

    std::string name = label;
    for (char& c : name) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    // Report under the build tree (ALAMO_OBJECTS_LOG_DIR = <build>/logs). Written to a temporary file and
    // renamed into place; any failure is reported, never fatal (the report is informational).
    const fs::path out = path_from_utf8(ALAMO_OBJECTS_LOG_DIR) / ("objects_coverage_" + name + ".txt");
    fs::path tmp = out;
    tmp += ".tmp";
    fs::create_directories(out.parent_path(), ec);
    bool written = false;
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (f) {
            f << text;
            f.close();
            written = !f.fail();
        }
    }
    if (written) {
        fs::rename(tmp, out, ec);
        if (ec) {
            written = false;
            fs::remove(tmp, ec);
        }
    }
    std::printf("  coverage report: %s (%s)\n", path_to_utf8(out).c_str(), written ? "written" : "NOT WRITTEN (non-fatal)");
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    log::set_min_level(log::Level::Error);  // the loaders warn by design; tests read diagnostics
    test_schema();
    test_classify();
    test_loader_synthetic();
    test_converters();
    test_types_synthetic();
    test_enum_converters();
    test_s1_rules();
    test_s1_registries();
    test_store();
    test_behavior_scheduling();
    test_store_save();
    test_carried_type();
    integration(env_or("ALAMO_FOC_DATA", "/home/claude/data/foc"), "FoC", true);
    integration(env_or("ALAMO_EAW_DATA", "/home/claude/data/eaw"), "EaW", false);
    std::printf(g_fail ? "alamo_objects_tests: %d failure(s)\n" : "alamo_objects_tests: OK\n", g_fail);
    return g_fail ? 1 : 0;
}
