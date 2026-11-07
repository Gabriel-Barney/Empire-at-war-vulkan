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
//     reward; dependants; Perpetual: fired = false + Reset (an ELAPSED timer restarts at its next T9, so a Perpetual
//     STORY_ELAPSED T fires about every T + one story service, §4.7), and non-TRIGGER events re-armed.
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

// A story event / reward parameter: XML text, or text set from Lua (Set_Event_Parameter stores objects and types as
// their type name, numbers formatted like Lua's tostring).
// Only Kind::Text exists (cu14 t03, cu14f). The save record and the state hash still write the old layout's constant
// bytes (kind u8 0, then an i32 -1 after the text) so the story save bytes and the state hash did not change.
struct StoryParam {
    enum class Kind : uint8_t { Text };
    Kind kind = Kind::Text;
    std::string text;

    static StoryParam of_text(std::string_view s) { return {Kind::Text, std::string(s)}; }
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
    std::vector<std::string> dialog;  // the Add_Dialog_Text line list (event +0x2C0, LUA_API; Clear_Dialog_Text empties it)
    // Story dialog / chapter fields (STORY.md §1.6 field map): Story_Dialog (+0x230, a Lua Set_Dialog replaces it),
    // Story_Chapter (+0x250), Story_Var (+0x258), Story_Tag (+0x278), Story_Dialog_Popup (+0x2B8), Story_Dialog_SFX (+0x2B9).
    // Story_Incoming (+0x298) is not read by Fire step 6 and not kept. Fire step 6 uses them (StoryHolocron).
    std::string story_dialog, story_tag, story_var;
    int32_t story_chapter = -1;  // -1 = absent
    bool story_dialog_popup = false;
    bool story_dialog_sfx = false;
};

// One Fire step 6 presentation record (an event with a Story_Dialog fired), in firing order: what the UI shows for it
// (STORY.md §1.6 step 3: Popup false -> holocron flash, transmission text, SFX only when `sfx`; Popup true -> the summary
// at this chapter). Not hashed, not saved (presentation).
struct StoryDialogEntry {
    uint32_t frame = 0;
    uint32_t plot = 0;
    uint32_t event = 0;
    std::string dialog, tag;  // tag = the holocron record's tag (Story_Tag, else "<dialog>_<chapter>")
    int32_t chapter = -1;
    bool popup = false;
    bool sfx = false;          // Story_Dialog_SFX
    uint64_t holocron_id = 0;  // StoryHolocronEntry::id of the record this fire added
    std::string plot_name;
};

class StoryModel;

// GALACTIC_DIALOGS §2.8 / STORY.md §1.6: the holocron list (0x140a2a4c8). ONE list for the whole game: every plot set of
// every mode adds to it (StoryModel::share_holocron). Records are inserted at index 0 (newest first), with no
// de-duplication. A record keeps a REFERENCE to its event's Add_Dialog_Text list (not a copy), so later Add / Clear calls
// show up in it; lines() reads through that reference. REMOVE_STORY_GOAL removes and STORY_GOAL_COMPLETED completes the
// first (newest) record with a tag.
//
// [port] Not part of the per-tick CRC: no sim rule reads the list (only the summary dialog does; the two rewards only write
// it), and its contents are a pure function of the Fire order, which the CRC covers. Saved with the story session
// (StorySession::write). The `read` flag is set by the summary dialog (mark_read), as in the original (0x1089e0).
struct StoryHolocronEntry {
    uint64_t id = 0;  // [port] unique per add (1, 2, ...), lets the UI find a record after later inserts
    std::string dialog, var, tag;
    int32_t chapter = -1;
    bool completed = false;
    bool read = false;
    bool auto_tag = false;  // the tag was generated ("<dialog>_<chapter>")
    uint32_t frame = 0;     // host frame of the add ([port] the original's "last summary" frame global)
    // The event whose Add_Dialog_Text list the record shows (by reference). model == nullptr: detached (the plot set is
    // gone, e.g. a tactical story after its battle; [port: the list was copied into `detached_lines` then]) or not yet
    // re-linked after a load (`ref_set` names the saved set).
    const StoryModel* model = nullptr;
    uint32_t plot = 0, event = 0;
    int32_t ref_set = -1;  // the session's set number of `model` (save / load), -1 = none
    std::vector<std::string> detached_lines;
};

class StoryHolocron {
public:
    const std::vector<StoryHolocronEntry>& entries() const { return entries_; }
    size_t size() const { return entries_.size(); }
    // 0x47dc70: inserts at index 0. Returns the new record's id.
    uint64_t add(StoryHolocronEntry e);
    // 0x47eea0 / 0x47f5b0: the first (newest) record whose tag equals `tag` (exact compare); false when none.
    bool remove_tag(std::string_view tag);
    bool complete_tag(std::string_view tag);
    // The summary executor ran on a record (0x1089e0 sets read = 1).
    void mark_read(size_t index);
    size_t index_of(uint64_t id) const;  // SIZE_MAX when absent
    // The record's Add_Dialog_Text lines, read through its reference (empty when the event is gone).
    const std::vector<std::string>& lines(const StoryHolocronEntry& e) const;
    // A plot set is going away: its records keep a copy of their lines and drop the reference.
    void detach(const StoryModel* model);
    // Presentation records of every add, firing order (the UI drains it with a cursor; not saved).
    const std::vector<StoryDialogEntry>& log() const { return log_; }
    void push_log(StoryDialogEntry e) { log_.push_back(std::move(e)); }
    void clear();

    // Save / load. set_of(model) = the session's set number of a referencing model (-1: none, the reference is dropped and
    // the lines copied). After read(), relink(set, model) re-attaches the references to each loaded set's model.
    void write(ByteWriter& w, const std::function<int32_t(const StoryModel*)>& set_of) const;
    bool read(ByteReader& r);
    void relink(int32_t set, const StoryModel* model);

private:
    std::vector<StoryHolocronEntry> entries_;
    std::vector<StoryDialogEntry> log_;
    uint64_t next_id_ = 1;
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
    // STORY_ELAPSED / OBJECTIVE_TIMEOUT timer (§4.7): vf4 (Perpetual re-arm, Lua Reset, RESET_EVENT, RESET_BRANCH) clears
    // timer_acc and timer_init (q6 t03, 0x45e550 [H]); the next evaluation re-records timer_last.
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
    // q6 t03 (STORY.md §2.1): false when a later-loaded plot of the same file name (same model, or a later model of the
    // StoryTable) replaced it in the story manager's table: no walk, dispatch, T9 service or name lookup reaches it.
    bool in_table = true;
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
    int32_t category = -1;           // CONSTRUCT_LEVEL: 0 ground base (GROUND), 1 starbase (SPACE); value = its Base_Level
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

class StoryTable;

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
    // The same for one plot (q6 t03: StoryTable walks for the dispatches that reach every player's plots).
    uint32_t dispatch_plot(uint32_t plot, std::string_view type, const StoryNotification& n);
    // Lua Story_Event(name [, object]): STORY_AI_NOTIFICATION (`object_type` = the object's type name). This model's
    // plots only; with a StoryTable, Lua goes through StoryTable::story_event (every player's plots).
    uint32_t story_event(std::string_view name, std::string_view object_type = {}, bool has_object = false);
    // Generic notification: event parameters compared with `params` by index (types without a modelled
    // evaluator; also accepted for modelled types, through StoryNotification::params).
    uint32_t push_trigger(std::string_view type, std::span<const StoryParam> params);
    // Fire (debug / host): fires the event regardless of armed / fired (still not when disabled).
    bool trigger_event(uint32_t plot, uint32_t event);
    // TRIGGER_EVENT semantics: fires every event named `name` in all plots of this model (table order). The TRIGGER_EVENT
    // reward itself uses the StoryTable when the model is in one (every player's plots).
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
    // q6 t03: the game text table (MasterTextFile, 0x140a7bc58) for Add_Dialog_Text: true + the text of `id`, false when
    // the id has no entry (it then shows as itself). Unset [port: the headless sim has no text table]: every id shows as
    // itself. Its results land in the hashed dialog lines, so a host that sets it must give every peer the same table.
    std::function<bool(std::string_view id, std::string& out)> text_lookup;
    // q6 t03 (STORY.md §2.4): the story manager's table this model's plot set belongs to (StoryTable::assign sets it);
    // TRIGGER_EVENT, STORY_ELEMENT and DISABLE_STORY_EVENT with Param3 then reach every player's plots. nullptr = this
    // model is the whole table.
    StoryTable* table() const { return table_; }

    // ---- plots ----
    size_t plot_count() const { return plots_.size(); }
    const StoryPlot& plot(uint32_t i) const { return plots_[i]; }
    StoryPlot& plot(uint32_t i) { return plots_[i]; }
    // q6 t03 (STORY.md §2.1 [H]): by the CRC-32 of the name as passed - case-sensitive, path included, no ".xml" added -
    // among the plots in the table (in_table). UINT32_MAX if absent.
    uint32_t find_plot(std::string_view name) const;
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
    // The holocron list is one global list too (GALACTIC_DIALOGS §2.8): a model starts with its own, a host with several
    // plot sets makes them share one (like the flags). Not hashed; the host saves it (StoryHolocron::write).
    void share_holocron(const StoryModel& with) { holocron_ = with.holocron_; }
    StoryHolocron& holocron() { return *holocron_; }
    const StoryHolocron& holocron() const { return *holocron_; }
    // Story dialog script loads (GALACTIC_DIALOGS §2.8, 0x47d9c0): every event's Story_Dialog when its plot loads (event
    // order), then each Lua Set_Dialog name at the call, and the Lua-set names again after a read(). Names as given (the
    // original's cache is keyed by the CRC of the exact name); the presentation host loads them in order (it owns the
    // script parser). Not hashed, not saved.
    const std::vector<std::string>& dialog_loads() const { return dialog_loads_; }
    void request_dialog_load(std::string_view name) {
        if (!name.empty()) dialog_loads_.emplace_back(name);
    }
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
    friend class StoryTable;
    void dispatch_in(uint32_t p, const std::string& type, const StoryNotification& n);  // type already normalised
    void set_table_replaced(uint32_t plot, bool replaced);  // StoryTable: a later set's plot of the same name
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
    std::vector<uint32_t> plot_walk_;      // plots in the table (in_table), table order
    std::vector<uint8_t> table_replaced_;  // per plot: replaced by a later model's plot (StoryTable)
    StoryTable* table_ = nullptr;
    std::vector<LuaVm*> script_vms_;  // per plot
    std::vector<std::string> script_errors_;
    std::shared_ptr<std::map<std::string, int32_t>> flags_ = std::make_shared<std::map<std::string, int32_t>>();
    std::vector<StoryObjective> objectives_;
    std::vector<StoryBlip> blips_;
    std::vector<StoryHighlight> highlights_;
    std::map<std::string, uint32_t> ai_flags_;  // "player|NAME|location" -> raised count
    std::vector<StoryFired> fired_;
    std::vector<StoryDialogEntry> dialog_log_;
    std::shared_ptr<StoryHolocron> holocron_ = std::make_shared<StoryHolocron>();
    std::vector<std::string> dialog_loads_;
    std::vector<std::string> warnings_;
    int32_t player_ = -1;
    uint32_t frame_ = 0;
    double time_ = 0.0;
    bool paused_ = false;
    uint32_t fire_depth_ = 0;   // recursion guard (cascades, TRIGGER_EVENT)
    uint32_t reset_depth_ = 0;  // RESET_BRANCH depth limit 3 (§4.5)
};

// q6 t03 (STORY.md §2.1 / §2.4 [H]): one story manager's plot table. The original has no per-player plot sets: every
// player's plots of a game mode sit in ONE table (Load_Plots loads Rebel_Story_Name, Empire_Story_Name,
// Underworld_Story_Name, then the Story_Name pairs, into the mode's manager). The port keeps one StoryModel per plot set
// (player: rewards, filters, the log) and a StoryTable over them for the lookups that reach every player's plots:
//   * Get_Story_Plot / STORY_ELEMENT: find_plot - one lookup by the CRC-32 of the name as passed (case-sensitive);
//   * TRIGGER_EVENT: trigger_event_by_name - every plot, active or suspended, in table order; each plot with that event
//     fires it (no first-match stop);
//   * Lua Story_Event: story_event - every ACTIVE plot, table order (no player filter);
//   * DISABLE_STORY_EVENT with Param3: set_event_enabled - every plot with that event.
// Plot order = the original's table order (§4.2 emulation) of the plot names in load order; a later-loaded plot of the
// same name replaces the earlier one (its node, its place), which leaves its model's walks, dispatches, T9 services and
// lookups (StoryPlot::in_table). Game-system dispatches with a player stay per model (the original's player filter);
// Get_Event, the rewarding-plot rewards and prereqs stay inside one plot. Not thread-safe (sim thread). The models must
// outlive the table's use; clear() detaches them (the destructor does not touch them).
class StoryTable {
public:
    struct PlotRef {
        uint32_t set = 0xFFFFFFFFu;  // index into the models given to assign()
        uint32_t plot = 0xFFFFFFFFu;
        bool valid() const { return set != 0xFFFFFFFFu; }
    };
    // The models in load order (nullptr entries are skipped). Each model's table() becomes this table.
    void assign(std::vector<StoryModel*> models);
    void clear();
    size_t set_count() const { return models_.size(); }
    StoryModel* model(size_t set) const { return set < models_.size() ? models_[set] : nullptr; }
    uint32_t set_of(const StoryModel* m) const;  // UINT32_MAX when not in the table
    const std::vector<PlotRef>& order() const { return order_; }  // the table's plots, table order
    PlotRef find_plot(std::string_view name) const;
    // Return the number of events fired (cascades included, every model).
    uint32_t trigger_event_by_name(std::string_view name);
    uint32_t story_event(std::string_view name, std::string_view object_type = {}, bool has_object = false);
    void set_event_enabled(std::string_view name, bool enabled);

private:
    size_t fired_total() const;
    std::vector<StoryModel*> models_;
    std::vector<PlotRef> order_;
};

// Makes the story bindings of `vm` (Story_Event, Get_Story_Plot, Check_Story_Flag, Add_Objective,
// Add/Remove_Radar_Blip, Add/Remove_Planet_Highlight, StoryPlotWrapper / StoryEventWrapper) act on
// `model` (nullptr detaches; the bindings then do nothing / return nil). The model must outlive the
// attachment.
void attach_story(LuaVm& vm, StoryModel* model);
StoryModel* attached_story(LuaVm& vm);
// q6 t03: the story manager's table the VM's own model belongs to (the session's galactic plot sets): Get_Story_Plot and
// Story_Event then act on every player's plots (StoryTable). Plot / event wrappers of another set's plot remember that
// set's index in the table, so the table must keep its order for the VM's lifetime (and across a load). nullptr detaches
// (the VM's own model is then the whole table). The table and its models must outlive the attachment.
void attach_story_table(LuaVm& vm, StoryTable* table);

}  // namespace alamo::lua
