// AI framework: the AI XML definitions (docs/re/AI_GALACTIC.md Â§1.2, Â§1.4, Â§1.7):
//   Data\XML\Enum\AIGoalCategoryType.xml   goal categories (DynamicEnum; list order = XML order)
//   Data\XML\AI\Goals\*.xml                goal types (element name = type)
//   Data\XML\AI\GoalFunctions\*.xml        goal function sets (set name = file base name) [M]
//   Data\XML\AI\Players\*.xml              AIPlayerType
//   Data\XML\AI\Templates\*.xml            templates
// Files are read in the order given (the host passes Vfs::list(), byte-sorted) [port]. Scalars repeated
// inside one element: the last write wins [L]; repeated list tags append [port: not checked, Â§1.4].
// Duplicate element names across files: the first definition wins (later ones are reported) [port].
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/ai/ai_types.hpp"
#include "alamo/core/file_provider.hpp"

namespace alamo::ai {

struct GoalTypeDef {
    std::string name;
    uint32_t application = 0;          // AIGoalApplicationFlags (+0x20)
    AiMode mode = AiMode::Galactic;    // GameMode (+0x24)
    bool mode_set = false;
    int32_t category = -1;             // Category (+0x28): index into AiDefinitions::categories
    Reachability reachability = Reachability::Any;  // (+0x2c)
    float time_limit = -1.0f;          // Time_Limit s (+0x30), -1 = none
    float build_time_delay_tolerance = 1.0f;  // (+0x34)
    std::vector<uint32_t> is_like;     // Is_Like (+0x38), goal type indices
    std::vector<uint32_t> exclusions;  // Global_Exclusions (+0x90)
    std::vector<std::string> is_like_names, exclusion_names;  // as written (resolved by link())
    float tracking_duration = 0.0f;    // (+0xa8)
    float per_failure_desire_adjust = 0.0f;             // w1 (+0xac)
    float activation_tracking_duration = 0.0f;          // (+0xb0)
    float per_activation_failure_desire_adjust = 0.0f;  // w2 (+0xb4)
    bool global() const { return (application & app::Global) != 0; }
};

struct GoalFunctionEntry {
    uint32_t goal = kAiNone;  // goal type index
    std::string function;     // perception equation name
};
struct GoalFunctionSet {
    std::string name;  // file base name ("BasicOffensiveGalacticSet")
    std::vector<GoalFunctionEntry> entries;
};

struct TemplateSwitch {
    std::vector<int32_t> goal_categories;   // Goals/Category
    std::vector<uint32_t> goal_types;       // Goals/Goal_Type
    std::vector<int32_t> plan_categories;   // Plans/Goal_Category
    std::vector<std::string> plan_names;    // Plans/Name (resolved by the plan library)
};
struct TemplateDef {
    std::string name;
    int32_t priority = 1;
    std::string trigger, untrigger;  // perception equation names ("" = none)
    std::vector<std::pair<int32_t, std::string>> budget;     // category -> budget-weight equation
    std::vector<std::pair<int32_t, float>> tactical_allowance;
    TemplateSwitch turn_on, turn_off;
};

struct AiPlayerTypeDef {
    std::string name;
    std::array<std::string, kAiModeCount> free_store_script;  // by AiMode
    std::vector<std::string> function_sets;                   // GoalProposalFunctionSets (list order)
    std::array<std::vector<std::string>, kAiModeCount> templates;  // element 0 = base template
    std::array<std::string, 3> difficulty;                    // Easy / Normal / Hard adjustment names
    // GoalSetExtensionSize (+0xE8, ctor 0x60f400 default 2, parser 0x60f840: atoi, unsigned min 5): the extra goals a
    // goal-system category keeps beyond its active ones (Commit / Resolve_Category truncation 0x721b60) [H].
    int32_t goal_set_extension_size = 2;
};

class AiDefinitions {
public:
    AiDefinitions();  // categories = the shipped AIGoalCategoryType list (replaced by load_category_enum)

    // In-memory loaders (bytes of one file each). `file` names it in diagnostics.
    void load_category_enum(std::string_view xml, const std::string& file);
    void add_goals(std::string_view xml, const std::string& file);
    void add_function_set(std::string_view set_name, std::string_view xml, const std::string& file);
    void add_players(std::string_view xml, const std::string& file);
    void add_templates(std::string_view xml, const std::string& file);

    int32_t find_category(std::string_view name) const;  // case-insensitive, -1
    uint32_t find_goal(std::string_view name) const;     // kAiNone
    const GoalFunctionSet* find_set(std::string_view name) const;
    const AiPlayerTypeDef* find_player(std::string_view name) const;
    const TemplateDef* find_template(std::string_view name) const;
    uint32_t template_index(std::string_view name) const;  // kAiNone

    std::vector<std::string> categories;  // index = category id (DynamicEnum value order)
    std::vector<int32_t> category_value;  // the XML value per category
    std::vector<GoalTypeDef> goals;
    std::vector<GoalFunctionSet> sets;
    std::vector<AiPlayerTypeDef> players;
    std::vector<TemplateDef> templates;
    std::vector<std::string> diagnostics;

    // Resolves Is_Like / Global_Exclusions names (goals may reference later files). Call after the last add.
    void link();

private:
    void index_goal(uint32_t g);
    std::map<std::string, uint32_t> goal_index_;  // upper-cased name -> index (lookup only)
};

// The game-relative globs the host lists (Vfs::list) for each kind.
struct AiDataPaths {
    std::string category_enum = "Data\\XML\\Enum\\AIGoalCategoryType.xml";
    std::vector<std::string> goals, goal_functions, players, templates;
};
using AiFileLister = std::function<std::vector<std::string>(std::string_view glob)>;
AiDataPaths list_ai_data(const AiFileLister& list);
// Reads every file in `paths` (order kept) into `out`. False when nothing could be read.
bool load_ai_definitions(const FileProvider& files, const AiDataPaths& paths, AiDefinitions& out);
// "Data\\XML\\AI\\GoalFunctions\\BasicOffensiveGalacticSet.xml" -> "BasicOffensiveGalacticSet".
std::string file_base_name(std::string_view path);

// Who gets which AIPlayerType (AI_GALACTIC §1.2, AI_TACTICAL §1): campaigns assign through the campaign's
// AI_Player_Control "Faction, Type" pairs [M]; skirmish / MP slots through Faction.Basic_AI [H].
// ai_player_control_type: the Type of the first pair naming `faction` (case-insensitive, trimmed), empty when no pair
// names it. "Human" means no AI (Set_AI_Type deletes the AIPlayer); "None" is a real AIPlayerType (PLAYER_NONE.XML).
std::string ai_player_control_type(const std::vector<std::string>& pairs, std::string_view faction);
// Faction.Basic_AI of every <Faction> in the files Data\XML\FactionFiles.xml lists (first definition wins; upper-cased
// faction name -> AIPlayerType name, factions without Basic_AI omitted).
std::map<std::string, std::string> load_faction_basic_ai(const FileProvider& files);
// The AIPlayerType for a campaign player of `faction`: the AI_Player_Control pair, else (with `fill_unassigned`, for
// AI-vs-AI hosts [port]) the faction's Basic_AI; empty = no AI ("Human" or nothing found).
std::string campaign_ai_type(const std::vector<std::string>& pairs, const std::map<std::string, std::string>& basic_ai,
                             std::string_view faction, bool fill_unassigned);

}  // namespace alamo::ai
