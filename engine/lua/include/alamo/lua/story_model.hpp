// Story plots (roadmap L1 "story" family; S4 "Story & campaign events").
//
// One StoryModel = one story manager's plot set (the original keeps one story manager per game mode;
// TheStoryMode::Load_Plots loads one plot set per faction, docs/re/MAIN_LOOP.md 5.2). It holds the
// plots (Data\XML\Story_*.xml: <Story><Event Name=..> with Event_Type, Event_ParamN, Reward_Type,
// Reward_ParamN, Reward_Param_List, Prereq, Branch, Perpetual, Timeout, Inactive_Delay, Multiplayer),
// the story flags, objectives, radar blips and planet highlights, and a log of fired events.
//
// Behaviour follows docs/re/STORY.md (section numbers below); port choices are marked [port],
// guesses the doc marks [M]/[L] keep that mark.
//
//   * Arming (§1): an event is ARMED ("active") at load iff it has no <Prereq> tag. Afterwards it is
//     armed only (a) when one of its prereqs fires and some prereq group is all fired (edge-triggered,
//     dependants loop of Fire), or (b) by RESET_BRANCH's re-check. Groups: one per <Prereq> tag (OR),
//     names inside a tag AND (whitespace / comma separated), same plot only; an unknown name is dropped
//     from its group (an empty group counts as satisfied). Activation, loading and Reset never arm.
//   * STORY_TRIGGER never self-evaluates: it fires when armed by its prereqs (synchronously, inside the
//     prereq's Fire) or explicitly (TRIGGER_EVENT, RESET_BRANCH Param2, trigger_event()).
//   * Fire (§1.5): disabled -> nothing; Inactive_Delay > 0 and not forced -> restart the inactivity
//     timer, no fire; script Story_Event_Trigger(name) (synchronous when a VM is attached with
//     set_script_vm, else queued for deliver_script_triggers) and timeout cancelled; fired = true;
//     reward; dependants; Perpetual: fired = false + Reset, and non-TRIGGER events re-armed.
//     Fire checks neither armed nor fired.
//   * Notifications (dispatch, §1.3/§4.3/§5): only to ARMED, unfired, enabled events of ACTIVE plots,
//     per-type match rules (story_model.cpp). STORY_ELAPSED, STORY_FLAG, OBJECTIVE_TIMEOUT, Timeout and
//     Inactive_Delay are serviced by the host's T9 calls below.
//   * Order (§4.2): plots and events are visited in the original's hash-table order (an emulation of
//     its CRC-keyed unordered_map: plot key = CRC-32 of the file name as written, event key = CRC-32 of
//     the upper-cased event name). plot_order() / StoryPlot::walk give it. Duplicate event names are
//     not in the table (the first one wins) and are never visited.
//   * Rewards applied by the model (§6): SET_FLAG, INCREMENT_FLAG, objectives, TRIGGER_EVENT,
//     RESET_EVENT, RESET_BRANCH, DISABLE_BRANCH, DISABLE_STORY_EVENT, STORY_ELEMENT. Every other reward
//     goes to the host's reward hook (synchronously, at Fire step 5) and is kept in the fired log.
//
// Determinism: no pointer hashing; vectors in load order, std::map by upper-cased name, the
// emulated table order is a pure function of the names; time comes from the host (set_clock).
// Not thread-safe: owned by the sim thread.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace alamo {
class FileProvider;
class StateHasher;
class ByteWriter;
class ByteReader;
}  // namespace alamo

namespace alamo::lua {

class LuaVm;

// A story event / reward parameter: XML text, or a value set from Lua (object, player).
struct StoryParam {
    enum class Kind : uint8_t { Text, Object, Player };
    Kind kind = Kind::Text;
    std::string text;  // Kind::Text (numbers from Lua are formatted like Lua's tostring)
    int32_t id = -1;   // Kind::Object: object id; Kind::Player: player id

    static StoryParam of_text(std::string_view s) { return {Kind::Text, std::string(s), -1}; }
    static StoryParam of_object(int32_t object_id) { return {Kind::Object, {}, object_id}; }
    static StoryParam of_player(int32_t player_id) { return {Kind::Player, {}, player_id}; }
    bool empty() const { return kind == Kind::Text && text.empty(); }
};

enum class StoryObjectiveState : uint8_t { Active, Complete, Failed };

struct StoryObjective {
    std::string text;  // text id (TEXT_OBJECTIVE_...)
    StoryObjectiveState state = StoryObjectiveState::Active;
    bool from_lua = false;  // added by Lua Add_Objective
};

struct StoryBlip {
    std::string name;
    int32_t object_id = -1;
};

struct StoryHighlight {
    std::string name;
    int32_t object_id = -1;  // planet object id
};

// One entry of the fired log.
struct StoryFired {
    uint32_t sequence = 0;  // 0, 1, 2, ... in firing order
    uint32_t frame = 0;     // host frame at firing (set_clock)
    uint32_t plot = 0;
    uint32_t event = 0;
    std::string plot_name;
    std::string event_name;
    std::string event_type;
    std::string reward_type;
    std::vector<StoryParam> reward_params;  // index 0 = Reward_Param1
};

struct StoryEventDef {
    std::string name;
    std::string type;                      // upper-case, e.g. STORY_TRIGGER
    std::vector<StoryParam> event_params;  // index 0 = Event_Param1 (sized to the highest index set)
    std::string reward_type;               // upper-case, may be empty
    std::vector<StoryParam> reward_params;
    std::vector<std::string> reward_param_list;  // <Reward_Param_List> names (RESET_EVENT)
    bool has_prereq_tags = false;                // at least one <Prereq>: not armed at load
    std::vector<std::vector<uint32_t>> prereqs;  // OR of AND groups (event indices of the plot)
    bool unresolved_prereq = false;              // a prereq name was unknown (dropped from its group)
    std::vector<uint32_t> dependants;            // events naming this one as a prereq, table order
    std::string branch;                          // as written (RESET_BRANCH compares case-sensitively)
    bool perpetual = false;
    bool multiplayer = false;      // <Multiplayer> (gate only matters in multiplayer games)
    double timeout = 0.0;          // <Timeout> seconds (SPEECH_DONE defaults to 60), 0 = none
    double inactive_delay = 0.0;   // <Inactive_Delay> seconds, 0 = none
    bool in_table = true;          // false: a duplicate name (never visited)
    std::vector<std::string> dialog;  // Add_Dialog_Text / Set_Dialog lines (Lua)
    // Story dialog / chapter fields (STORY.md 1.5 step 6; the only ones shipped FoC/EaW events use:
    // Story_Dialog, Story_Chapter, Story_Tag, Story_Dialog_Popup). Recorded in dialog_log() at Fire.
    std::string story_dialog, story_tag;
    int32_t story_chapter = -1;  // -1 = absent
    bool story_dialog_popup = false;
};

// One Fire step 6 record (an event with Story_Dialog / Story_Tag fired): what the campaign "holocron" /
// dialog UI shows. [port] the original's handling at step 6 is not traced (STORY.md 7.7): recorded only.
struct StoryDialogEntry {
    uint32_t frame = 0;
    uint32_t plot = 0;
    uint32_t event = 0;
    std::string dialog, tag;
    int32_t chapter = -1;
    bool popup = false;
};

struct StoryEventState {
    bool enabled = true;          // !disabled (DISABLE_*)
    bool fired = false;           // "triggered"
    bool armed = false;           // "active"
    uint32_t fire_count = 0;
    uint32_t last_fire_frame = 0;
    double activation_time = 0.0;  // story time when armed
    double timeout_at = -1.0;      // < 0: no timeout registered
    double inactive_start = -1.0;  // < 0: inactivity timer not started
    // STORY_ELAPSED / OBJECTIVE_TIMEOUT timer (§4.7): never cleared by Reset / Perpetual.
    bool timer_init = false;
    double timer_last = 0.0;
    double timer_acc = 0.0;
    // Counters (vf4 Reset restores them): per-name countdowns (SingleObjectName family), a count
    // (CONQUER_COUNT, WIN/LOSE_BATTLES), the TECH_LEVEL "-1" observation.
    std::vector<int32_t> countdowns;  // empty = not initialised from Event_Param2 yet
    int32_t count = 0;
    int32_t seen_level = -1;
    uint32_t lua_edits = 0;  // Set_Event_Parameter / Set_Reward_* / dialog edits (hashed with the params)
};

struct StoryPlot {
    std::string name;        // file name as listed, e.g. "Story_Campaign_Underworld.xml"
    std::string lua_script;  // <Lua_Script> paired with this plot ("" = none)
    bool active = false;     // false = suspended
    std::vector<StoryEventDef> events;
    std::vector<StoryEventState> state;
    std::map<std::string, uint32_t> by_name;  // upper-case event name -> index (first definition)
    std::vector<uint32_t> walk;               // event indices in the original's table order
    std::vector<std::string> pending_script_triggers;  // fired names not delivered (no VM attached)
};

// A game-side notification (dispatch). Fields a type does not use are ignored.
struct StoryNotification {
    std::string name;                // dispatched name: object / planet / built type name, notification ...
    bool has_object = false;         // an object came with it (filters / AI_NOTIFICATION Param1)
    int32_t owner = -1;              // owner of that object (FILTER_*)
    bool owner_neutral = false;      // that owner is the neutral player
    std::string planet;              // planet (Enter family, CONSTRUCT_LEVEL, battles)
    std::vector<std::string> units;  // fleet contents, unit type names (Enter family, MOVE)
    double value = 0.0;              // level / credits / count
    int32_t category = -1;           // CONSTRUCT_LEVEL: 0 starbase, 1 ground base
    std::vector<StoryParam> params;  // generic rule for types without a modelled evaluator
    // L1/S4 wave 2a - battles (WIN/LOSE_BATTLES Param2 / Param3, STORY.md §5):
    int32_t location = 0;            // 1 land, 2 space (the S10 battle event's value); 0 unknown
    bool base = false;               // a starbase / ground base of the defender took part (BASE_SPACE / BASE_LAND)
    std::vector<uint64_t> friendly;  // CategoryMask of every unit of the plot player's side in the battle
    std::vector<uint64_t> enemy;     // ... of the other side(s)
    // Tactical notifications (COMMAND_UNIT / GUARD_UNIT): the target / guarded object's type name, distance.
    std::string target;
    double distance = -1.0;          // < 0 unknown
    // L1/S4 wave 2b (SCRIPT_GAPS.md §4): COMMAND_UNIT data2 = the order's destination; DESTROY data2 = the planet the
    // dead object was in (filter on the planet's owner); CORRUPTION_* use `value` = the corruption type (-1 none).
    bool has_position = false;
    double x = 0.0, y = 0.0, z = 0.0;
    bool has_planet = false;         // DESTROY: the object was inside a container chain reaching `planet`
    int32_t planet_owner = -1;
    bool planet_owner_neutral = false;
    // S4 (docs/re/L1_S4_REMAINING.md §4.2): CAPTURE_STRUCTURE data2 = the new owner's faction name.
    std::string faction;
};

class StoryModel {
public:
    static constexpr int32_t kUndefinedFlag = -99999999;  // value of a never-set flag (0x327d00)

    // ---- loading ----
    // Reads Data\XML\<plots_file> (<Story_Mode_Plots>: <Lua_Script> pairs with the next plot; any other
    // tag with text loads a plot, active iff the tag is exactly Active_Plot) and every plot it lists.
    bool load_plots(const FileProvider& files, std::string_view plots_file, std::string* err = nullptr);
    // Adds one plot from XML text (tests, tools). Returns its index, or UINT32_MAX + *err.
    uint32_t add_plot(std::string_view plot_name, std::string_view xml, bool active, std::string_view lua_script = {},
                      std::string* err = nullptr);

    // ---- host: owner, clock, T9 service ----
    void set_player(int32_t player) { player_ = player; }  // the plot set's player (FILTER_*)
    int32_t player() const { return player_; }
    // Story clock (mode seconds) for everything that follows; paused = galactic pause (ELAPSED).
    void set_clock(uint32_t frame, double seconds, bool paused = false);
    // T9 step 2: STORY_ELAPSED for every active plot.
    void service_elapsed();
    // T9 step 3 (after the plot's Lua service): for an ACTIVE plot, inactivity timers, the STORY_FLAG
    // poll and Timeout expiry [L: an expired Timeout fires the event, STORY.md §7.1].
    void service_plot(uint32_t plot);
    // T9 step 5: OBJECTIVE_TIMEOUT (UNIT_PROXIMITY / CHECK_DESTROYED: the host's poll() below, wave 2a; FOG
    // reveals have no source yet).
    void service_objective_timeouts();
    // Convenience (tests, hosts without scripts): set_clock + the steps above in T9 order.
    void update(uint32_t frame, double seconds, bool paused = false);
    // Plots in the original's table order (the order of every walk across plots).
    const std::vector<uint32_t>& plot_order() const { return plot_walk_; }

    // ---- notifications / explicit fires ----
    // Dispatches a game notification of `type` (e.g. "STORY_CONQUER") to every armed, unfired, enabled
    // event of that type in the active plots. Returns the number of events fired (cascades included).
    uint32_t dispatch(std::string_view type, const StoryNotification& n);
    // Lua Story_Event(name [, object]): STORY_AI_NOTIFICATION (`object_type` = the object's type name).
    uint32_t story_event(std::string_view name, std::string_view object_type = {}, bool has_object = false);
    // Generic notification: event parameters compared with `params` by index (types without a modelled
    // evaluator; also accepted for modelled types, through StoryNotification::params).
    uint32_t push_trigger(std::string_view type, std::span<const StoryParam> params);
    // Fire (debug / host): fires the event regardless of armed / fired (still not when disabled).
    bool trigger_event(uint32_t plot, uint32_t event);
    // TRIGGER_EVENT semantics: fires every event named `name` in all plots (table order).
    uint32_t trigger_event_by_name(std::string_view name);
    // Host poll of a self-evaluated type (STORY.md 1.3 / 4.1 steps 4-5: UNIT_PROXIMITY, CHECK_DESTROYED; L1/S4
    // wave 2a): walks the active plots and events in table order like dispatch and fires every armed, unfired,
    // enabled event of `type` whose definition `match` accepts. Returns the number fired (cascades included).
    uint32_t poll(std::string_view type, const std::function<bool(const StoryEventDef&)>& match);

    // ---- Lua script delivery ----
    // Story_Event_Trigger(name) is called synchronously at Fire in `vm` (nullptr: queued instead).
    void set_script_vm(uint32_t plot, LuaVm* vm);
    // Errors raised by synchronous Story_Event_Trigger calls (host drains it).
    std::vector<std::string>& script_errors() { return script_errors_; }
    // Calls Story_Event_Trigger(name) in `vm` for each queued fired event of `plot` (firing order),
    // then clears the queue. Lua errors are returned in *err (delivery continues). Returns the count.
    size_t deliver_script_triggers(uint32_t plot, LuaVm& vm, std::string* err = nullptr);

    // ---- host reward hook ----
    // Called at Fire step 5 for rewards the model does not apply itself (CREDITS, SET_TECH_LEVEL,
    // LINK_TACTICAL, ...), before the dependants of the firing event are checked.
    std::function<void(uint32_t plot, uint32_t event, const std::string& type, const std::vector<StoryParam>& params)>
        on_reward;
    // GameObjectCategoryType name -> CategoryMask bit(s) (WIN/LOSE_BATTLES CLASS_* composition tests); unset or
    // 0 = unknown class (never contained).
    std::function<uint64_t(std::string_view category)> category_mask;
    // L1/S4 wave 2b: player -> faction index (FILTER_* compare factions, SCRIPT_GAPS.md §4.5); unset or < 0 = compare
    // players. objects_within(type name, x, y, z, range): some live object of that type in the mode is strictly
    // within `range` (3D) of the point (COMMAND_UNIT §4.3); unset = the notification's target / distance stand in.
    std::function<int32_t(int32_t player)> faction_of;
    std::function<bool(std::string_view type, double x, double y, double z, double range)> objects_within;
    // S4 (L1_S4_REMAINING.md §4.4, LOAD_TACTICAL_MAP Param2 entry i): the i-th object of that type in the current store
    // has the battle planet as its first planet ancestor. Unset: no Param2 entry matches.
    std::function<bool(std::string_view type, size_t index)> load_map_hero;

    // ---- plots ----
    size_t plot_count() const { return plots_.size(); }
    const StoryPlot& plot(uint32_t i) const { return plots_[i]; }
    StoryPlot& plot(uint32_t i) { return plots_[i]; }
    uint32_t find_plot(std::string_view name) const;  // file name, case-insensitive [port], path ignored
    uint32_t find_event(uint32_t plot, std::string_view name) const;  // UINT32_MAX if absent
    // Activate / Suspend / STORY_ELEMENT: flag only (STORY §7 item 2 [H], confirmed from the bytes: plot +0x660 := 1 / 0,
    // nothing re-evaluated, no Lua return values).
    void set_plot_active(uint32_t plot, bool active);
    void reset_plot(uint32_t plot);                    // StoryPlotWrapper.Reset (§2.3)

    // ---- state ----
    int32_t flag(std::string_view name) const;  // kUndefinedFlag when never set
    bool has_flag(std::string_view name) const;
    void set_flag(std::string_view name, int32_t value);  // no evaluation (FLAG events are polled)
    const std::map<std::string, int32_t>& flags() const { return *flags_; }  // upper-case names
    // Story flags are ONE global table shared by every plot and player (STORY.md §0 [H]; the tactical missions' SET_FLAG
    // ..._WIN is read by the galactic campaign's STORY_FLAG). A model starts with a table of its own; a host with several
    // plot sets (one per faction, the tactical story) makes them use one table. hash() / write() cover the table in each
    // model; read() replaces its contents (so every sharing model sees the loaded flags).
    void share_flags(const StoryModel& with) { flags_ = with.flags_; }
    bool shares_flags(const StoryModel& with) const { return flags_ == with.flags_; }
    const std::vector<StoryObjective>& objectives() const { return objectives_; }
    void add_objective(std::string_view text, bool from_lua);
    const std::vector<StoryBlip>& blips() const { return blips_; }
    void add_blip(std::string_view name, int32_t object_id);  // same name replaces
    bool remove_blip(std::string_view name);
    const std::vector<StoryHighlight>& highlights() const { return highlights_; }
    void add_highlight(std::string_view name, int32_t object_id);
    bool remove_highlight(std::string_view name);
    // AI story flags (Check_Story_Flag(player, name, location, reset)) [inferred: raised by the host].
    void raise_ai_flag(int32_t player, std::string_view name, int32_t location_id);
    bool check_ai_flag(int32_t player, std::string_view name, int32_t location_id, bool reset);

    // ---- log / checksum ----
    const std::vector<StoryFired>& fired() const { return fired_; }
    // Dialog / chapter records in firing order (UI side; not hashed, like the presentation log).
    const std::vector<StoryDialogEntry>& dialog_log() const { return dialog_log_; }
    uint32_t frame() const { return frame_; }
    double time() const { return time_; }
    // Canonical state for the per-tick CRC (CrcRegistry system "story").
    void hash(StateHasher& h) const;

    // ---- save / load (OI lua: STORY.md §7 item 8; STRATEGIC_GAPS §4 [H]) ----
    // The original saves per plot the active flag, the owning player, every event's own state (triggered / active /
    // disabled, counters, ELAPSED LastTime / accumulated / initialised, parameters) and the plot script's state. Here
    // [port: the new-engine save layout, not the original's chunks] everything hash() covers is written, plus the fired
    // log and the pending script triggers; definitions are not (load the same plots first). The plot scripts' Lua state
    // lives in their VMs (not here). read(): false and the model unchanged on a version / plot / event mismatch or
    // truncated data; after a successful read hash() equals the saved model's.
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);
    // OI lua: STORY.md §7 item 3 [H] - the T9 time argument = (float)(current mode frame) x seconds-per-frame (a float,
    // 0x140b0a344), computed in float. Hosts pass it to set_clock / update.
    static double t9_seconds(uint64_t mode_frame, float seconds_per_frame) {
        return static_cast<double>(static_cast<float>(mode_frame) * seconds_per_frame);
    }
    // Warnings collected while loading (unknown prereqs, duplicate names, malformed events).
    const std::vector<std::string>& warnings() const { return warnings_; }

private:
    bool gated(uint32_t p, uint32_t e) const;  // armed, unfired, enabled, plot active
    bool prereqs_hold(uint32_t p, uint32_t e) const;
    void arm(uint32_t p, uint32_t e);
    void reset_event(uint32_t p, uint32_t e);  // vf4
    bool fire(uint32_t p, uint32_t e, bool forced = false);
    bool evaluate(uint32_t p, uint32_t e, std::string_view type, const StoryNotification& n);
    bool same_side(int32_t owner) const;  // FILTER "friendly": same faction (faction_of) / player
    bool tick_timer(StoryEventState& st, double threshold);
    void apply_reward(uint32_t p, uint32_t e);
    void call_script(uint32_t p, const std::string& name);
    void build_plot_order();
    StoryObjective* find_objective(std::string_view text);

    std::vector<StoryPlot> plots_;
    std::vector<uint32_t> plot_walk_;
    std::vector<LuaVm*> script_vms_;  // per plot
    std::vector<std::string> script_errors_;
    std::shared_ptr<std::map<std::string, int32_t>> flags_ = std::make_shared<std::map<std::string, int32_t>>();
    std::vector<StoryObjective> objectives_;
    std::vector<StoryBlip> blips_;
    std::vector<StoryHighlight> highlights_;
    std::map<std::string, uint32_t> ai_flags_;  // "player|NAME|location" -> raised count
    std::vector<StoryFired> fired_;
    std::vector<StoryDialogEntry> dialog_log_;
    std::vector<std::string> warnings_;
    int32_t player_ = -1;
    uint32_t frame_ = 0;
    double time_ = 0.0;
    bool paused_ = false;
    uint32_t fire_depth_ = 0;   // recursion guard (cascades, TRIGGER_EVENT)
    uint32_t reset_depth_ = 0;  // RESET_BRANCH depth limit 3 (§4.5)
};

// Makes the story bindings of `vm` (Story_Event, Get_Story_Plot, Check_Story_Flag, Add_Objective,
// Add/Remove_Radar_Blip, Add/Remove_Planet_Highlight, StoryPlotWrapper / StoryEventWrapper) act on
// `model` (nullptr detaches; the bindings then do nothing / return nil). The model must outlive the
// attachment.
void attach_story(LuaVm& vm, StoryModel* model);
StoryModel* attached_story(LuaVm& vm);
// q6 t03: the other plot sets of the same story manager (the session's galactic plot sets, load order), which
// Get_Story_Plot also searches after the VM's own model (STORY.md §0: one plot table per game mode). Plot / event wrappers
// of a group model remember its index, so the group must keep its order for the VM's lifetime (and across a load).
// nullptr detaches. The vector and its models must outlive the attachment.
void attach_story_group(LuaVm& vm, const std::vector<StoryModel*>* group);

}  // namespace alamo::lua
