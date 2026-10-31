// StorySession for builds without engine/lua (ALAMO_ENGINE_MODULES without "lua"): never starts.
#include <stdexcept>

#include "alamo/core/bytes.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/objects/object_store.hpp"  // ObjectHandle (complete type for add_unit_names)
#include "alamo/sim/story_session.hpp"

namespace alamo::sim {

struct StorySession::Script {};
struct StorySession::Set {};
struct StorySession::Families {};
struct StorySession::Battle {};
struct StorySession::Tactical {};

std::string_view tactical_story_event_name(TacticalStoryEvent) { return "?"; }
std::string_view tactical_story_event_type(TacticalStoryEvent) { return {}; }
bool StorySession::begin_battle(const StoryBattleStart&, uint64_t, std::vector<std::string>* diagnostics) {
    if (diagnostics) diagnostics->push_back("story: built without engine/lua");
    return false;
}
void StorySession::end_battle(int32_t, uint64_t) {}
void StorySession::set_corruption_mission(int32_t, int32_t) {}  // L1/S4 wave 2b
bool StorySession::battle_attached() const { return false; }
int32_t StorySession::tactical_player() const { return -1; }  // S4
std::string StorySession::tactical_plots_file() const { return {}; }  // L1/S4 wave 1
void StorySession::load_tactical_map(PlanetIndex, bool, uint64_t) {}  // S4
std::string StorySession::take_tactical_map(bool) { return {}; }   // S4
void StorySession::notify_tactical(const TacticalStoryNotification&) {}
void StorySession::planet_destroyed_in_battle(PlanetIndex, int32_t, uint64_t) {}  // L1 systems
const lua::StoryModel* StorySession::tactical_model() const { return nullptr; }
lua::StoryModel* StorySession::tactical_model() { return nullptr; }

std::string StoryTrigger::text() const { return "T " + std::to_string(frame) + " " + kind; }
std::string StoryLogEntry::text() const { return "F " + std::to_string(frame) + " " + event; }
std::vector<StoryPlotSet> campaign_plot_sets(const data::CampaignDef&, const galactic::GalacticSim&) { return {}; }

StorySession::StorySession(galactic::GalacticSim& sim, objects::ObjectStore& store, const objects::GameObjectTypes& types,
                           GameRandom& rng)
    : sim_(&sim), store_(&store), types_(&types), rng_(&rng) {}
StorySession::~StorySession() = default;
bool StorySession::start(const StoryStart&, std::vector<std::string>* diagnostics) {
    if (diagnostics) diagnostics->push_back("story: built without engine/lua");
    return false;
}
void StorySession::service(uint64_t, double) {}
double StorySession::t9_seconds(uint64_t mode_frame) {
    return static_cast<double>(static_cast<float>(mode_frame) * (1.0f / 30.0f));
}
void StorySession::host_event(StoryHostEvent, uint64_t) {}
void StorySession::notify_ui(const StoryUiNotification&) {}
void StorySession::poll_planets(std::vector<StoryTrigger>&) {}
void StorySession::add_unit_names(objects::ObjectHandle, std::vector<std::string>&) const {}
std::string_view story_ui_event_name(StoryUiEvent) { return "?"; }
std::string_view story_ui_event_type(StoryUiEvent) { return {}; }
void StorySession::set_command_pipeline(commands::CommandPipeline*) {}
lua::PresentationLog* StorySession::presentation() { return nullptr; }
lua::ObjectBridge* StorySession::object_bridge() { return nullptr; }
lua::LuaPlayerWorld* StorySession::player_world() { return nullptr; }
const lua::StoryModel& StorySession::model(size_t) const { throw std::out_of_range("no story models"); }
lua::StoryModel& StorySession::model(size_t) { throw std::out_of_range("no story models"); }
int32_t StorySession::plot_set_player(size_t) const { return -1; }
size_t StorySession::script_count() const { return 0; }
std::string StorySession::log_text(std::string_view) const { return {}; }
void StorySession::hash(StateHasher& h) const { h.u32(0); }
// N1: a session never starts here, so SimHost never saves one.
std::string StorySession::save_blocker() const { return {}; }
void StorySession::write(ByteWriter&) const {}
bool StorySession::read(ByteReader&) { return false; }
void StorySession::write_scripts(ByteWriter&) const {}
bool StorySession::read_scripts(ByteReader&) { return false; }
void StorySession::write_bridge(ByteWriter&) const {}
bool StorySession::read_bridge(ByteReader&) { return false; }
void StorySession::write_players(ByteWriter&) const {}
bool StorySession::read_players(ByteReader&) { return false; }

}  // namespace alamo::sim
