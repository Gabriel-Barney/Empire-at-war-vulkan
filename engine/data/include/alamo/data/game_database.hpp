// Game database: the XML-defined tables needed by the galactic simulation, loaded through
// alamo::FileProvider (VFS rules apply; tests use DirectoryProvider).
//
// Loading follows the original's list files:
//   Data\XML\GameConstants.xml                         -> GameConstants
//   Data\XML\GameObjectFiles.xml  -> listed files      -> object index (+ Planet types)
//   Data\XML\TradeRouteFiles.xml  -> listed files      -> TradeRoute definitions
//   Data\XML\CampaignFiles.xml    -> listed files      -> Campaign definitions
// List semantics (docs/re/FALLBACK_CAMPAIGNS.md, docs/re/PHASE2_ANSWERS.md Q13): a list file is
// opened once (first match through the provider, no merging across tiers); each <File> is
// "Data\XML\<name>"; a listed file that cannot be opened is skipped with a warning (the original
// only writes a debug-log line). Only a missing or unparsable *list* file makes load() return false.
//   * GameObjectFiles.xml: parsed in LIST ORDER; a file listed twice is parsed twice.
//   * TradeRouteFiles.xml, CampaignFiles.xml: de-duplicated and parsed in ASCENDING CRC32 ORDER of
//     the listed file name (list_file_crc). Q13 says the key is the CRC32 of the name string "as
//     written (no upper-casing; likely)". ASSUMPTION: the hashed string is the <File> text exactly
//     as written in the list (case kept, separators kept, no "Data\XML\" prefix), with surrounding
//     whitespace trimmed. Two names with the same CRC collapse to the first one listed.
//
// Duplicate object names (Q13, confirmed):
//   * Game object types: every definition creates a record (game_objects(), planets() rows), in
//     load order; name lookup returns the FIRST one. Later duplicates are flagged `duplicate`.
//     (The original's Variant_Of_Existing_Type re-passes also land on the first object; variants
//     are not resolved by this loader.)
//   * Trade routes, campaigns: LAST WINS with whole-object replacement (defaults + the later
//     element's own XML, no field merge). The replacement takes the slot of the replaced object,
//     so trade_routes()/campaigns() are in order of first appearance in the parse order above.
//     "Last" = document order within a file, then file CRC order.
//   Vanilla FoC only has identical duplicate routes (Anaxes_Carida, Mandalore_Utapau).
//
// Every parsed document stays alive inside the database, and every definition keeps an XmlNode
// to its element, so later loaders (units, factions, ...) can read more fields without re-parsing.
#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "alamo/core/file_provider.hpp"
#include "alamo/core/host_fs.hpp"
#include "alamo/core/galaxy.hpp"
#include "alamo/data/xml.hpp"
#include "alamo/data/xml_list.hpp"

namespace alamo::data {

struct LoadOptions {
    // Opt-in (default off, vanilla = off): also read these campaign list files and union their
    // <File> entries with the primary list's (keyed by list_file_crc; on a collision the primary
    // list's entry is kept); the union is parsed in ascending CRC order. Each list is read
    // through `provider` (nullptr = the main provider); its listed files resolve through the main
    // provider first, then the list's own provider.
    bool merge_campaign_lists = false;
    struct ExtraList {
        std::string path;                       // e.g. "Data\\XML\\CampaignFiles.xml"
        const FileProvider* provider = nullptr;  // e.g. a DirectoryProvider on the EaW tree
    };
    std::vector<ExtraList> extra_campaign_lists;
};

// One parsed file, owned by the database.
struct LoadedXmlFile {
    std::string path;    // as requested, e.g. "Data\\XML\\Planets.xml"
    std::string origin;  // provider's origin string
    std::unique_ptr<XmlDocument> doc;
};

// Generic tag -> text table for <GameConstants>. Lookups are case-insensitive. For a tag that
// appears more than once the LAST occurrence is returned (the parser assigns each occurrence).
struct GameConstants {
    XmlNode element;  // the <GameConstants> root (null if the file was missing)
    std::vector<std::pair<std::string_view, std::string_view>> fields;  // document order, trimmed text
    float max_fleet_move_distance = 100.0f;  // <MaximumFleetMovementDistance>

    std::optional<std::string_view> get(std::string_view tag) const;
    float get_float(std::string_view tag, float fallback) const;
    int32_t get_int(std::string_view tag, int32_t fallback) const;
    bool get_bool(std::string_view tag, bool fallback) const;

private:
    friend class GameDatabase;
    std::unordered_map<std::string, uint32_t> index_;  // normalize_name(tag) -> last field index
};

// Every top-level element (with a Name attribute) of every file listed in GameObjectFiles.xml.
struct GameObjectRecord {
    std::string_view type;  // element name: "Planet", "SpaceUnit", ...
    std::string_view name;  // Name attribute as written
    uint32_t name_crc = 0;
    XmlNode element;
    bool duplicate = false;  // a later definition of an earlier name: kept, but not found by name
};

// Planet type definitions (SoA), in GameObjectFiles order. Same column meaning as
// alamo::PlanetTable, plus the source element.
struct PlanetTypes {
    std::vector<std::string> name;
    std::vector<uint32_t> name_crc;
    std::vector<Vec3> position;          // <Galactic_Position>
    std::vector<uint32_t> flags;         // PlanetFlags: <In_Background>, <Always_Instantiate_Galactic>,
                                         // kPlanetHasPlanetBehavior (<Behavior> lists PLANET)
    std::vector<int32_t> credit_value;   // <Planet_Credit_Value>
    std::vector<int32_t> max_space_base;   // <Max_Space_Base>
    std::vector<int32_t> max_ground_base;  // <Max_Ground_Base> (absent in vanilla -> 0)
    // <Behavior> tokens as written (comma/whitespace list; repeated tags append), e.g.
    // {"SELECTABLE", "PLANET", "PRODUCTION"}; Galaxy_Core_Art_Model: {"IDLE"}.
    std::vector<std::vector<std::string>> behaviors;
    std::vector<uint8_t> duplicate;      // 1 = later definition of an earlier name (not found by name)
    std::vector<XmlNode> element;

    size_t size() const { return name.size(); }
    uint32_t find(std::string_view planet_name) const;  // first definition; kInvalidPlanet if absent
private:
    friend class GameDatabase;
    std::unordered_map<uint32_t, std::vector<uint32_t>> by_crc_;
};

struct TradeRouteDef {
    std::string name;
    uint32_t name_crc = 0;
    std::string point_a, point_b;          // planet names
    float hs_speed_factor = 1.0f;          // <HS_Speed_Factor>
    float political_control_gain = 0.0f;   // <Political_Control_Gain> (int in the original)
    float credit_gain_factor = 0.0f;       // <Credit_Gain_Factor>
    std::string visible_line_name;         // <Visible_Line_Name>
    std::string file;                      // list entry ("Data\\XML\\<name>") of the winning definition
    XmlNode element;                       // the winning (last) definition
    uint32_t definitions = 1;              // how many definitions of this name were parsed
};

struct CampaignDef {
    std::string name;
    uint32_t name_crc = 0;
    std::string file;  // list entry the winning (last) definition came from
    uint32_t definitions = 1;  // how many definitions of this name were parsed
    std::string campaign_set, text_id, description_text, starting_active_player;
    int32_t sort_order = 0;
    std::vector<std::string> locations;     // <Locations> (repeated tags append)
    std::vector<std::string> trade_routes;  // <Trade_Routes> (repeated tags append)
    std::vector<std::pair<std::string, std::string>> home_locations;  // faction -> planet
    // Raw strings of repeated per-player tags, one entry per occurrence.
    std::vector<std::string> ai_player_control, starting_forces, starting_credits, starting_tech_level, max_tech_level;
    // Every child element in document order: (tag, trimmed text). Views into the database.
    std::vector<std::pair<std::string_view, std::string_view>> raw;
    XmlNode element;

    std::string_view raw_value(std::string_view tag) const;                  // first occurrence or ""
    std::vector<std::string_view> raw_values(std::string_view tag) const;    // every occurrence
};

class GameDatabase {
public:
    GameDatabase() = default;
    GameDatabase(const GameDatabase&) = delete;
    GameDatabase& operator=(const GameDatabase&) = delete;
    GameDatabase(GameDatabase&&) = default;
    GameDatabase& operator=(GameDatabase&&) = default;

    // Loads everything. Warnings (missing files, XML recoveries, duplicates, bad values) are
    // appended to `diagnostics` (if given) and logged on channel "data". Returns false only when
    // a list file or GameConstants.xml is missing/unparsable; whatever could be loaded is kept.
    // A provider with no loose XML (e.g. a DirectoryProvider on a game install, whose XML is in
    // MEGs) yields an empty database: every accessor stays valid, lookups return null/invalid.
    bool load(const FileProvider& provider, const LoadOptions& options = {},
              std::vector<std::string>* diagnostics = nullptr);

    const GameConstants& constants() const { return constants_; }
    const PlanetTypes& planets() const { return planets_; }
    const std::vector<TradeRouteDef>& trade_routes() const { return routes_; }
    const std::vector<CampaignDef>& campaigns() const { return campaigns_; }  // parse order
    // Campaigns ordered for UI: by Sort_Order ascending, ties in campaigns() order.
    std::vector<const CampaignDef*> campaigns_by_sort_order() const;
    const std::vector<GameObjectRecord>& game_objects() const { return objects_; }
    const std::vector<LoadedXmlFile>& files() const { return files_; }

    const TradeRouteDef* find_trade_route(std::string_view name) const;
    const CampaignDef* find_campaign(std::string_view name) const;
    const GameObjectRecord* find_game_object(std::string_view name) const;  // first definition
    // Every definition of `name` (case-insensitive), in load order; [0] is what find returns.
    std::vector<const GameObjectRecord*> find_game_objects(std::string_view name) const;

private:
    const XmlDocument* load_file(const FileProvider& provider, const FileProvider* fallback,
                                 std::string_view path, bool required);
    // <File> entries of a list file (xml_list.hpp: list order, duplicates kept, empty entries skipped).
    std::vector<ListedXmlEntry> read_list(const FileProvider& provider, std::string_view list_path, bool& ok);
    void load_constants(const FileProvider& provider, bool& ok);
    void load_game_objects(const FileProvider& provider, bool& ok);
    void load_trade_routes(const FileProvider& provider, bool& ok);
    void load_campaigns(const FileProvider& provider, const LoadOptions& options, bool& ok);
    void warn(std::string message);

    GameConstants constants_;
    PlanetTypes planets_;
    std::vector<TradeRouteDef> routes_;
    std::vector<CampaignDef> campaigns_;
    std::vector<GameObjectRecord> objects_;
    std::vector<LoadedXmlFile> files_;
    std::unordered_map<std::string, uint32_t> file_index_;  // normalize_path -> files_ index
    std::unordered_map<uint32_t, std::vector<uint32_t>> route_by_crc_, campaign_by_crc_, object_by_crc_;
    std::vector<std::string>* diag_ = nullptr;
};

// Key of a TradeRouteFiles/CampaignFiles entry: CRC32 of the <File> name as written (trimmed),
// no upper-casing, no path prefix (see the ASSUMPTION above). Files are parsed in ascending order.
uint32_t list_file_crc(std::string_view listed_name);

// Builds the galactic map of one campaign: planet rows in <Locations> order, route rows in
// <Trade_Routes> order, endpoints resolved to row indices, planet flags copied from PlanetTypes
// (incl. kPlanetHasPlanetBehavior; rows are NOT filtered by it), max_fleet_move_distance from
// GameConstants. Unknown planets/routes, duplicate locations, routes whose endpoints are not in
// the campaign, self-routes and HS_Speed_Factor <= 0 are skipped and reported in `diagnostics`.
// The result always passes GalaxyMap::validate(). Unknown campaign -> empty map + diagnostic.
GalaxyMap build_galaxy_map(const GameDatabase& db, std::string_view campaign,
                           std::vector<std::string>* diagnostics = nullptr);

}  // namespace alamo::data
