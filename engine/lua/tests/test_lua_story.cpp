// engine/lua story family tests (L1 stage 2): StoryModel (plots, prereqs, triggers, rewards, firing
// order, checksum) and the story bindings (Story_Event, Get_Story_Plot, StoryPlot/EventWrapper,
// objectives, radar blips, planet highlights, Check_Story_Flag). With ALAMO_FOC_DATA: loads the
// shipped FoC campaign plot sets through the VFS, runs them and drives PGStoryMode.lua.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "alamo/core/bytes.hpp"
#include "alamo/core/file_provider.hpp"
#include "alamo/core/log.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/lua/bindings.hpp"
#include "alamo/lua/lua_vm.hpp"
#include "alamo/lua/story_model.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/object_store.hpp"
#include "alamo/objects/object_types.hpp"
#ifdef ALAMO_LUA_HAVE_VFS
#include "alamo/vfs/vfs.hpp"
#endif

#include <lua.h>

using namespace alamo;
using namespace alamo::lua;
namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)

static std::string run(LuaVm& vm, const std::string& code) {
    std::string err;
    if (!vm.run(code, "test", &err)) return err.empty() ? "error" : err;
    return {};
}
static std::string gstr(LuaVm& vm, const char* name) {
    lua_State* L = vm.state();
    lua_pushstring(L, name);
    lua_rawget(L, LUA_GLOBALSINDEX);
    const char* s = lua_tostring(L, -1);
    std::string out = s ? s : "<nil>";
    lua_pop(L, 1);
    return out;
}

static std::string fired_names(const StoryModel& m, size_t from = 0) {
    std::string s;
    for (size_t i = from; i < m.fired().size(); ++i) s += m.fired()[i].event_name + ",";
    return s;
}

static uint64_t digest(const StoryModel& m) {
    StateHasher h;
    m.hash(h);
    return h.digest();
}

static void write_file(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}

// ------------------------------------------------------------------------------------------
// Synthetic plot: every firing rule with a hand-computed order.

static const char kPlot[] = R"(<?xml version="1.0" ?>
<Story>
  <Event Name="Begin"><Event_Type>STORY_ELAPSED</Event_Type><Event_Param1>0</Event_Param1></Event>
  <Event Name="Later"><Event_Type>STORY_ELAPSED</Event_Type><Event_Param1>5</Event_Param1><Prereq>Begin</Prereq></Event>
  <Event Name="Set_Flag"><Event_Type>STORY_TRIGGER</Event_Type><Reward_Type>SET_FLAG</Reward_Type>
     <Reward_Param1>Mission_Win</Reward_Param1><Reward_Param2>0</Reward_Param2><Prereq>Begin</Prereq></Event>
  <Event Name="Obj_Add"><Event_Type>STORY_TRIGGER</Event_Type><Reward_Type>ADD_OBJECTIVE</Reward_Type>
     <Reward_Param1>TEXT_OBJ_A</Reward_Param1><Prereq>Set_Flag</Prereq><Branch>B1</Branch></Event>
  <Event Name="Notify"><Event_Type>STORY_AI_NOTIFICATION</Event_Type><Event_Param2>GO_NOW</Event_Param2>
     <Reward_Type>INCREMENT_FLAG</Reward_Type><Reward_Param1>Mission_Win</Reward_Param1><Reward_Param2>1</Reward_Param2>
     <Prereq>Obj_Add</Prereq><Branch>B1</Branch></Event>
  <Event Name="Flag_Won"><Event_Type>STORY_FLAG</Event_Type><Event_Param1>mission_win</Event_Param1>
     <Event_Param2>1</Event_Param2><Event_Param3>GREATER_THAN_EQUAL_TO</Event_Param3>
     <Reward_Type>OBJECTIVE_COMPLETE</Reward_Type><Reward_Param1>TEXT_OBJ_A</Reward_Param1></Event>
  <Event Name="And_Or"><Event_Type>STORY_TRIGGER</Event_Type><Prereq>Notify Later</Prereq><Prereq>Never</Prereq></Event>
  <Event Name="Enter"><Event_Type>STORY_ENTER</Event_Type><Event_Param1>Thyferra</Event_Param1>
     <Reward_Type>LINK_TACTICAL</Reward_Type><Reward_Param1>Thyferra</Reward_Param1><Prereq>Begin</Prereq></Event>
  <Event Name="Reset"><Event_Type>STORY_GENERIC</Event_Type><Event_Param1>retry</Event_Param1>
     <Reward_Type>RESET_BRANCH</Reward_Type><Reward_Param1>B1</Reward_Param1><Prereq>Notify</Prereq></Event>
  <Event Name="Tick"><Event_Type>STORY_TRIGGER</Event_Type><Perpetual>True</Perpetual><Prereq>Begin</Prereq></Event>
  <Event Name="Forced"><Event_Type>STORY_GENERIC</Event_Type><Event_Param1>never_pushed</Event_Param1></Event>
  <Event Name="Force_It"><Event_Type>STORY_AI_NOTIFICATION</Event_Type><Event_Param2>FORCE</Event_Param2>
     <Reward_Type>TRIGGER_EVENT</Reward_Type><Reward_Param1>Forced</Reward_Param1></Event>
  <Event Name="Off"><Event_Type>STORY_AI_NOTIFICATION</Event_Type><Event_Param2>FORCE</Event_Param2>
     <Reward_Type>DISABLE_STORY_EVENT</Reward_Type><Reward_Param1>Enter</Reward_Param1><Reward_Param2>1</Reward_Param2></Event>
</Story>
)";

static void run_synthetic(StoryModel& m, std::string* trace) {
    std::string err;
    CHECK(m.add_plot("Story_Test.xml", kPlot, true, "Story_Test", &err) == 0);
    CHECK(m.warnings().size() == 1);  // unknown prereq "Never"
    m.update(0, 0.0);  // A2i t01: the first T9 only records the prereq-less ELAPSED 0's start (0x454980)
    CHECK(m.fired().empty());
    m.update(0, 0.0);
    *trace += "u0:" + fired_names(m) + "\n";
    size_t mark = m.fired().size();
    m.update(1, 1.0);  // Tick fires again (perpetual), nothing else
    *trace += "u1:" + fired_names(m, mark) + "\n";
    mark = m.fired().size();
    CHECK(m.story_event("go_now") == 2);  // Notify (case-insensitive), then And_Or (cascade)
    *trace += "go:" + fired_names(m, mark) + "\n";
    mark = m.fired().size();
    m.update(2, 5.0);  // Later (5 s after Begin armed at 0), then And_Or (Notify Later)
    *trace += "u2:" + fired_names(m, mark) + "\n";
    mark = m.fired().size();
    const StoryParam wrong[] = {StoryParam::of_text("Kuat")};
    CHECK(m.push_trigger("STORY_ENTER", wrong) == 0);
    const StoryParam right[] = {StoryParam::of_text("THYFERRA")};
    CHECK(m.push_trigger("story_enter", right) >= 1);
    CHECK(m.push_trigger("STORY_ENTER", right) == 0);  // not perpetual: already fired
    *trace += "enter:" + fired_names(m, mark) + "\n";
    mark = m.fired().size();
    const StoryParam retry[] = {StoryParam::of_text("retry")};
    m.push_trigger("STORY_GENERIC", retry);  // RESET_BRANCH B1: Obj_Add re-fires, Notify re-arms
    *trace += "retry:" + fired_names(m, mark) + "\n";
    mark = m.fired().size();
    m.story_event("FORCE");
    *trace += "force:" + fired_names(m, mark) + "\n";
}

static void test_synthetic() {
    StoryModel m;
    std::string trace;
    run_synthetic(m, &trace);
    // docs/re/STORY.md: TRIGGER dependants fire inside their prereq's Fire (Set_Flag, Obj_Add, Tick in the
    // dependants' table order); a perpetual TRIGGER with prereqs disarms after firing and waits for its
    // prereq to fire again (u1 empty); And_Or's second group names an unknown event, so it is an EMPTY group
    // (satisfied) and And_Or fires as soon as Notify fires; FLAG events are polled at T9 (Flag_Won at u2,
    // after ELAPSED Later).
    const std::string expect =
        "u0:Begin,Set_Flag,Obj_Add,Tick,\n"
        "u1:\n"
        "go:Notify,And_Or,\n"
        "u2:Later,Flag_Won,\n"
        "enter:Enter,\n"
        "retry:Reset,Obj_Add,\n"             // RESET_BRANCH B1: reset, re-check arms + fires Obj_Add
        "force:Force_It,Forced,Off,\n";      // TRIGGER_EVENT fires inside the reward, before Off
    if (trace != expect) std::printf("  synthetic trace:\n%s", trace.c_str());
    CHECK(trace == expect);
    CHECK(m.flag("MISSION_WIN") == 1 && m.has_flag("mission_win") && !m.has_flag("other"));
    CHECK(m.objectives().size() == 1 && m.objectives()[0].text == "TEXT_OBJ_A" &&
          m.objectives()[0].state == StoryObjectiveState::Active);  // re-added by the branch reset
    const uint32_t enter = m.find_event(0, "enter");
    CHECK(enter != 0xFFFFFFFFu && !m.plot(0).state[enter].enabled);  // DISABLE_STORY_EVENT
    // Fired log keeps the reward for its owner system.
    bool link = false;
    for (const StoryFired& f : m.fired())
        link = link || (f.reward_type == "LINK_TACTICAL" && f.reward_params.size() == 1 && f.reward_params[0].text == "Thyferra");
    CHECK(link);
    for (size_t i = 0; i < m.fired().size(); ++i) CHECK(m.fired()[i].sequence == i);

    // Same inputs -> same log and checksum; a different input -> different checksum.
    StoryModel m2;
    std::string trace2;
    run_synthetic(m2, &trace2);
    CHECK(trace2 == trace && digest(m2) == digest(m));
    m2.set_flag("x", 1);
    CHECK(digest(m2) != digest(m));

    // OI lua (STORY §7 item 8): save / load. A fresh model with the same plots reads the state back: same checksum,
    // and it continues exactly like the original (both fire Begin again after a reset + T9).
    {
        ByteWriter w;
        m.write(w);
        StoryModel loaded;
        std::string err;
        CHECK(loaded.add_plot("Story_Test.xml", kPlot, true, "Story_Test", &err) == 0);
        ByteReader r(w.bytes());
        CHECK(loaded.read(r) && r.done() && digest(loaded) == digest(m) && loaded.fired().size() == m.fired().size());
        CHECK(loaded.flag("MISSION_WIN") == 1 && !loaded.plot(0).state[loaded.find_event(0, "enter")].enabled);
        StoryModel copy_a, copy_b;
        ByteReader ra(w.bytes()), rb(w.bytes());
        CHECK(copy_a.add_plot("Story_Test.xml", kPlot, true, "Story_Test", &err) == 0 && copy_a.read(ra));
        CHECK(copy_b.add_plot("Story_Test.xml", kPlot, true, "Story_Test", &err) == 0 && copy_b.read(rb));
        copy_a.reset_plot(0);
        copy_b.reset_plot(0);
        copy_a.update(3, 6.0);
        copy_b.update(3, 6.0);
        CHECK(digest(copy_a) == digest(copy_b) && copy_a.fired().size() > m.fired().size() &&
              copy_a.plot(0).state[copy_a.find_event(0, "Begin")].fire_count == 2);
        // Mismatches leave the model unchanged: a different plot set, truncated data.
        StoryModel other;
        CHECK(other.add_plot("Story_Other.xml", kPlot, true, "", &err) == 0);
        const uint64_t before = digest(other);
        ByteReader ro(w.bytes());
        CHECK(!other.read(ro) && digest(other) == before);
        std::vector<uint8_t> cut(w.bytes().begin(), w.bytes().end() - 3);
        StoryModel trunc;
        CHECK(trunc.add_plot("Story_Test.xml", kPlot, true, "Story_Test", &err) == 0);
        const uint64_t tb = digest(trunc);
        ByteReader rt(cut);
        CHECK(!trunc.read(rt) && digest(trunc) == tb);
    }
    // OI lua (STORY §7 item 3): T9 time = (float)(mode frame) x seconds-per-frame, in float.
    CHECK(StoryModel::t9_seconds(150, 1.0f / 30.0f) == static_cast<double>(150.0f * (1.0f / 30.0f)));
    CHECK(StoryModel::t9_seconds(0, 1.0f / 30.0f) == 0.0 && StoryModel::t9_seconds(30, 0.5f) == 15.0);
    // A prereq-less ELAPSED 2 driven by the host's T9 series (frame f -> t9_seconds(f, 1/30)): its LastTime starts at
    // the first T9 (frame 1), so it fires at the first frame whose float time is >= 2 s later (frame 61: 60 frames
    // at 30 Hz), never earlier; the series is a function of the frame count only.
    {
        const char* kTimer =
            "<Story><Event Name=\"Two\"><Event_Type>STORY_ELAPSED</Event_Type><Event_Param1>2</Event_Param1></Event></Story>";
        StoryModel t;
        std::string e1;
        CHECK(t.add_plot("Story_T9.xml", kTimer, true, "", &e1) == 0);
        uint32_t fired_at = 0;
        for (uint32_t f = 1; f <= 120; ++f) {
            t.update(f, StoryModel::t9_seconds(f, 1.0f / 30.0f));
            if (!fired_at && !t.fired().empty()) fired_at = f;
        }
        std::printf("  t9 timer: ELAPSED 2 fired at frame %u\n", fired_at);
        CHECK(fired_at == 61 && t.fired().size() == 1);
    }
    // A2i t01 (A2I_T01_FRAME_ORDER.md §6, 0x454980): a prereq-less ELAPSED 0 only records its start at its first T9 and
    // fires at the second (tick 2 of the host); a prereq-armed ELAPSED 0 was initialised at its arming (vf7) and fires at
    // the first T9 after it, the same frame.
    {
        const char* kZero =
            "<Story><Event Name=\"Zero\"><Event_Type>STORY_ELAPSED</Event_Type><Event_Param1>0</Event_Param1></Event>"
            "<Event Name=\"Go\"><Event_Type>STORY_AI_NOTIFICATION</Event_Type><Event_Param2>GO</Event_Param2></Event>"
            "<Event Name=\"Armed_Zero\"><Event_Type>STORY_ELAPSED</Event_Type><Event_Param1>0</Event_Param1>"
            "<Prereq>Go</Prereq></Event></Story>";
        StoryModel z;
        std::string ez;
        CHECK(z.add_plot("Story_Zero.xml", kZero, true, "", &ez) == 0);
        z.update(1, StoryModel::t9_seconds(0, 1.0f / 30.0f));  // tick 1's T9 (mode frame 0)
        CHECK(z.fired().empty() && z.plot(0).state[z.find_event(0, "Zero")].timer_init);
        z.update(2, StoryModel::t9_seconds(1, 1.0f / 30.0f));  // tick 2's T9
        CHECK(fired_names(z) == "Zero,");
        z.set_clock(2, StoryModel::t9_seconds(1, 1.0f / 30.0f));
        CHECK(z.story_event("GO") == 1);
        z.update(3, StoryModel::t9_seconds(2, 1.0f / 30.0f));
        CHECK(fired_names(z) == "Zero,Go,Armed_Zero,");
    }

    // Reset (§2.3): nothing fires at the reset; the prereq-less ELAPSED Begin keeps its accumulator and
    // fires again at the next T9; events with prereqs wait for Begin.
    const uint32_t begin = m.find_event(0, "Begin");
    m.reset_plot(0);
    CHECK(m.plot(0).state[begin].fire_count == 1 && m.plot(0).state[begin].armed &&
          !m.plot(0).state[m.find_event(0, "Later")].armed);
    m.update(3, 6.0);
    CHECK(m.plot(0).state[begin].fire_count == 2);
    m.set_plot_active(0, false);
    const size_t n = m.fired().size();
    m.update(4, 100.0);
    CHECK(m.fired().size() == n);  // suspended plots get no notifications or polls
}

// ------------------------------------------------------------------------------------------
// docs/re/STORY.md rules one by one (§1.4 prereq-less TRIGGER, §5 matching, §6 rewards, §4.6 Perpetual).

static const char kRulesA[] = R"(<Story>
  <Event Name="Jump"><Event_Type>STORY_TRIGGER</Event_Type><Reward_Type>STORY_ELEMENT</Reward_Type><Reward_Param1>Rules_B</Reward_Param1></Event>
  <Event Name="Take_Kuat"><Event_Type>STORY_CONQUER</Event_Type><Event_Param1>Kuat, Fondor</Event_Param1><Event_Param2>2 1</Event_Param2></Event>
  <Event Name="Take_Any"><Event_Type>STORY_CONQUER</Event_Type><Event_Param1>Fondor</Event_Param1><Event_Param3>FILTER_NONE</Event_Param3></Event>
  <Event Name="Enter_All"><Event_Type>STORY_ENTER</Event_Type><Event_Param1>Kuat</Event_Param1><Event_Param3>Tie, Isd</Event_Param3></Event>
  <Event Name="Ai_Obj"><Event_Type>STORY_AI_NOTIFICATION</Event_Type><Event_Param1>Vader</Event_Param1><Event_Param2>Hello</Event_Param2></Event>
  <Event Name="Ai_Plain"><Event_Type>STORY_AI_NOTIFICATION</Event_Type><Event_Param2>Hello</Event_Param2></Event>
  <Event Name="Flag_Ne"><Event_Type>STORY_FLAG</Event_Type><Event_Param1>F</Event_Param1><Event_Param2>5</Event_Param2><Event_Param3>NOT_EQUAL_TO</Event_Param3></Event>
  <Event Name="Flag_Eq"><Event_Type>STORY_FLAG</Event_Type><Event_Param1>Undefined_X F</Event_Param1><Event_Param2>3</Event_Param2></Event>
  <Event Name="Inc_Undef"><Event_Type>STORY_TRIGGER</Event_Type><Reward_Type>INCREMENT_FLAG</Reward_Type><Reward_Param1>Nope</Reward_Param1><Reward_Param2>1</Reward_Param2></Event>
  <Event Name="Set_F"><Event_Type>STORY_TRIGGER</Event_Type><Reward_Type>SET_FLAG</Reward_Type><Reward_Param1>F</Reward_Param1><Reward_Param2>3</Reward_Param2></Event>
  <Event Name="Perp"><Event_Type>STORY_AI_NOTIFICATION</Event_Type><Event_Param2>Ping</Event_Param2><Perpetual>Yes</Perpetual></Event>
  <Event Name="Disable_Bad"><Event_Type>STORY_TRIGGER</Event_Type><Reward_Type>DISABLE_STORY_EVENT</Reward_Type><Reward_Param1>Perp</Reward_Param1></Event>
  <Event Name="Reset_Ev"><Event_Type>STORY_TRIGGER</Event_Type><Reward_Type>RESET_EVENT</Reward_Type><Reward_Param1>Ai_Plain</Reward_Param1></Event>
</Story>)";
static const char kRulesB[] =
    "<Story><Event Name=\"B_Start\"><Event_Type>STORY_ELAPSED</Event_Type><Event_Param1>0</Event_Param1></Event></Story>";

static void test_story_rules() {
    StoryModel m;
    std::string err;
    CHECK(m.add_plot("Rules_A.xml", kRulesA, true, {}, &err) == 0);
    CHECK(m.add_plot("Rules_B.xml", kRulesB, false, {}, &err) == 1);
    m.set_player(0);
    // Table order: a permutation of the events / plots.
    CHECK(m.plot(0).walk.size() == m.plot(0).events.size() && m.plot_order().size() == 2);
    m.update(0, 0.0);
    CHECK(m.fired().empty());  // prereq-less STORY_TRIGGERs (Jump, Set_F, ...) never fire by themselves
    auto fired = [&m](const char* name) {
        uint32_t n = 0;
        for (const StoryFired& f : m.fired()) n += f.event_name == name ? 1u : 0u;
        return n;
    };
    // CONQUER: per-name countdowns (Param2), default FILTER_FRIENDLY_ONLY; Take_Any FILTER_NONE.
    StoryNotification kuat;
    kuat.name = "KUAT";
    kuat.has_object = true;
    kuat.owner = 0;
    CHECK(m.dispatch("STORY_CONQUER", kuat) == 0);
    StoryNotification enemy = kuat;
    enemy.owner = 1;
    CHECK(m.dispatch("STORY_CONQUER", enemy) == 0);  // filter: not friendly, counter untouched
    CHECK(m.dispatch("STORY_CONQUER", kuat) == 1 && fired("Take_Kuat") == 1);
    StoryNotification fondor = enemy;
    fondor.name = "Fondor";
    CHECK(m.dispatch("STORY_CONQUER", fondor) == 1 && fired("Take_Any") == 1);
    // ENTER: every Param3 type must be in the fleet; default FILTER_NONE.
    StoryNotification fleet;
    fleet.planet = "Kuat";
    fleet.owner = 1;
    fleet.units = {"TIE"};
    CHECK(m.dispatch("STORY_ENTER", fleet) == 0);
    fleet.units = {"TIE", "X", "ISD"};
    CHECK(m.dispatch("STORY_ENTER", fleet) == 1 && fired("Enter_All") == 1);
    // AI_NOTIFICATION: with an object its type must be in Param1; without, Param1 must be empty.
    CHECK(m.story_event("hello") == 1 && fired("Ai_Plain") == 1 && fired("Ai_Obj") == 0);
    CHECK(m.story_event("HELLO", "Vader", true) == 1 && fired("Ai_Obj") == 1);
    // FLAG: polled; undefined names skipped; EQUAL_TO default; NOT_EQUAL_TO never fires.
    CHECK(m.trigger_event_by_name("Set_F") == 1 && m.flag("F") == 3);
    m.update(1, 1.0);
    CHECK(fired("Flag_Eq") == 1 && fired("Flag_Ne") == 0);
    // INCREMENT_FLAG on an undefined flag does nothing.
    CHECK(m.trigger_event_by_name("Inc_Undef") == 1 && !m.has_flag("Nope") && m.flag("Nope") == StoryModel::kUndefinedFlag);
    // Perpetual: no once-per-frame limit.
    CHECK(m.story_event("ping") == 1 && m.story_event("ping") == 1 && fired("Perp") == 2);
    // DISABLE_STORY_EVENT without Param2 is ignored.
    m.trigger_event_by_name("Disable_Bad");
    CHECK(m.story_event("ping") == 1 && fired("Perp") == 3);
    // RESET_EVENT: Ai_Plain un-fired (prereq-less: stays armed) and fires again.
    m.trigger_event_by_name("Reset_Ev");
    CHECK(m.story_event("hello") == 1 && fired("Ai_Plain") == 2);
    // STORY_ELEMENT activates Rules_B.xml; its ELAPSED starts at the next T9.
    CHECK(!m.plot(1).active);
    m.trigger_event_by_name("Jump");
    CHECK(m.plot(1).active && fired("B_Start") == 0);
    m.update(2, 2.0);
    CHECK(fired("B_Start") == 0);  // A2i t01: the first T9 after activation records the start only (0x454980)
    m.update(3, 2.0 + 1.0 / 30.0);
    CHECK(fired("B_Start") == 1);
}

// ------------------------------------------------------------------------------------------
// Plots file loader (Lua_Script pairing) on a directory provider.

static void test_plots_file() {
    const fs::path root = fs::temp_directory_path() / "alamo_lua_story_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    write_file(root / "Data/XML/Story_Plots_T.xml",
               "<Story_Mode_Plots><Lua_Script> Script_A </Lua_Script><Active_Plot>A.xml</Active_Plot>"
               "<Suspended_Plot>B.xml</Suspended_Plot><Suspended_Plot /></Story_Mode_Plots>");
    write_file(root / "Data/XML/A.xml", "<Story><Event Name=\"A1\"><Event_Type>STORY_ELAPSED</Event_Type>"
                                        "<Event_Param1>0</Event_Param1></Event>"
                                        "<Event Name=\"A_Jump\"><Event_Type>STORY_TRIGGER</Event_Type></Event></Story>");
    write_file(root / "Data/XML/B.xml", "<Story><Event Name=\"B1\"><Event_Type>STORY_ELAPSED</Event_Type>"
                                        "<Event_Param1>0</Event_Param1></Event></Story>");
    DirectoryProvider dp(root.string());
    StoryModel m;
    std::string err;
    CHECK(m.load_plots(dp, "Story_Plots_T.xml", &err));
    CHECK(m.plot_count() == 2 && m.plot(0).lua_script == "Script_A" && m.plot(1).lua_script.empty());
    CHECK(m.plot(0).active && !m.plot(1).active && m.find_plot("b.XML") == 1);
    m.update(0, 0);
    CHECK(fired_names(m).empty());  // A2i t01: ELAPSED 0 records its start at the first T9 (0x454980)
    m.update(1, 1.0 / 30.0);
    CHECK(fired_names(m) == "A1,");  // the prereq-less STORY_TRIGGER A_Jump never fires by itself
    m.set_plot_active(1, true);
    CHECK(fired_names(m) == "A1,");  // activation only sets the flag
    m.update(2, 2.0 / 30.0);
    CHECK(fired_names(m) == "A1,");  // first T9 after activation: B1 records its start
    m.update(3, 3.0 / 30.0);
    CHECK(fired_names(m) == "A1,B1,");  // second T9 after activation
    StoryModel bad;
    CHECK(!bad.load_plots(dp, "Missing.xml", &err) && !err.empty());
    fs::remove_all(root, ec);
}

// ------------------------------------------------------------------------------------------
// Bindings.

static void test_bindings() {
    const fs::path root = fs::temp_directory_path() / "alamo_lua_story_objects";
    std::error_code ec;
    fs::remove_all(root, ec);
    write_file(root / "Data/XML/GameObjectFiles.xml", "<L><File>S.xml</File></L>");
    write_file(root / "Data/XML/S.xml", "<R><Planet Name=\"Kuat\"></Planet><Prop Name=\"Rock\"></Prop></R>");
    DirectoryProvider dp(root.string());
    objects::ObjectTypeDatabase raw;
    CHECK(raw.load(dp));
    objects::GameObjectTypes types;
    CHECK(types.build(raw));
    objects::IdCounter ids;
    objects::ObjectStore store(objects::ModeKind::Galactic, objects::StoreKind::Primary, types, &ids);
    objects::SpawnParams sp;
    sp.type = raw.find("Kuat");
    const objects::ObjectHandle planet = store.create(sp);
    sp.type = raw.find("Rock");
    store.create(sp);

    LuaHost host;
    host.store = &store;
    host.types = &types;
    LuaVm vm(host);
    // Without a story: commands are no-ops, queries nil.
    CHECK(run(vm, "Story_Event('X') p = tostring(Get_Story_Plot('Story_Test.xml'))") == "" && gstr(vm, "p") == "nil");

    StoryModel m;
    std::string err;
    CHECK(m.add_plot("Story_Test.xml", kPlot, true, "Story_Test", &err) == 0);
    attach_story(vm, &m);
    CHECK(attached_story(vm) == &m);
    m.update(0, 0.0);
    m.update(0, 0.0);  // A2i t01: the prereq-less ELAPSED 0 Begin fires at the second T9 (0x454980)
    vm.set_object_global("Object", planet);

    CHECK(run(vm, "Story_Event('GO_NOW')") == "");
    CHECK(m.flag("Mission_Win") == 1);
    CHECK(run(vm, "plot = Get_Story_Plot('story_test.XML') v = tostring(plot.Is_Valid())\n"
                  "ev = plot.Get_Event('Enter') ev2 = plot.Get_Event('enter') eq = tostring(ev == ev2)\n"
                  "none = tostring(plot.Get_Event('Nope')) ev_valid = tostring(ev.Is_Valid())\n"
                  "ev.Set_Event_Parameter(0, Object)\n"
                  "ev.Set_Reward_Parameter(1, 7)\n"
                  "ev.Set_Reward_Type('screen_text')\n"
                  "ev.Add_Dialog_Text('TEXT_A', Object) ev.Clear_Dialog_Text() ev.Add_Dialog_Text('TEXT_B', 3)\n"
                  "Add_Objective('TEXT_LUA_OBJ', true)\n"
                  "Add_Radar_Blip(Object, 'blip_1') Add_Radar_Blip(Object, 'blip_2') Remove_Radar_Blip('blip_1')\n"
                  "Add_Planet_Highlight(Object, '0') Remove_Planet_Highlight('0') Add_Planet_Highlight(Object, '1')") == "");
    CHECK(gstr(vm, "v") == "true" && gstr(vm, "eq") == "true" && gstr(vm, "none") == "nil" && gstr(vm, "ev_valid") == "true");
    const uint32_t enter = m.find_event(0, "Enter");
    const StoryEventDef& def = m.plot(0).events[enter];
    const int32_t planet_id = store.id(planet);
    // story t02: index 0 = Event_Param1; the planet object is stored as its type name (vf2 takes strings).
    CHECK(def.event_params[0].kind == StoryParam::Kind::Text && def.event_params[0].text == "Kuat");
    CHECK(def.reward_params.size() == 2 && def.reward_params[1].text == "7" && def.reward_type == "SCREEN_TEXT");
    CHECK(def.dialog.size() == 1 && def.dialog[0] == "TEXT_B|3");
    CHECK(m.blips().size() == 1 && m.blips()[0].name == "blip_2" && m.blips()[0].object_id == planet_id);
    CHECK(m.highlights().size() == 1 && m.highlights()[0].name == "1");
    bool lua_obj = false;
    for (const StoryObjective& o : m.objectives()) lua_obj = lua_obj || (o.text == "TEXT_LUA_OBJ" && o.from_lua);
    CHECK(lua_obj);

    // The Lua-set planet goes through the ENTER rule (planet name list): the old Param1 no longer matches, the planet does.
    const StoryParam by_name[] = {StoryParam::of_text("Thyferra")};
    CHECK(m.push_trigger("STORY_ENTER", by_name) == 0);
    const StoryParam by_planet[] = {StoryParam::of_text("KUAT")};
    m.push_trigger("STORY_ENTER", by_planet);
    bool entered = false;
    for (const StoryFired& f : m.fired()) entered = entered || f.event_name == "Enter";
    CHECK(entered);
    // Index contract: 0..63; a table, nil or a negative index is refused with the original's message.
    CHECK(run(vm, "Get_Story_Plot('Story_Test.xml').Get_Event('Enter').Set_Event_Parameter(-1, 1)")
              .find("must set exactly one parameter per parameter index") != std::string::npos);
    CHECK(run(vm, "Get_Story_Plot('Story_Test.xml').Get_Event('Enter').Set_Event_Parameter(64, 1)")
              .find("must set exactly one parameter per parameter index") != std::string::npos);

    // Contracts (the original's messages).
    CHECK(run(vm, "Story_Event()").find("Expected 1 or 2, got 0") != std::string::npos);
    CHECK(run(vm, "Story_Event(3)").find("Expected string") != std::string::npos);
    CHECK(run(vm, "Get_Story_Plot()").find("Get_Story_Plot -- invalid number of parameters") != std::string::npos);
    CHECK(run(vm, "Add_Objective('x')").find("Expected 2, got 1") != std::string::npos);
    CHECK(run(vm, "Add_Radar_Blip(5, 'n')").find("could not deduce a position") != std::string::npos);
    CHECK(run(vm, "Get_Story_Plot('Story_Test.xml').Get_Event('Enter').Set_Reward_Parameter('a', 1)")
              .find("Expected number") != std::string::npos);
    CHECK(run(vm, "Get_Story_Plot('Story_Test.xml').Get_Event('Enter').Set_Reward_Parameter(1, {})")
              .find("must set exactly one parameter per parameter index") != std::string::npos);
    CHECK(run(vm, "Check_Story_Flag(1, 'F', nil, true)").find("Expected player") != std::string::npos);

    // StoryPlotWrapper.Reset / Suspend / Activate.
    CHECK(run(vm, "p = Get_Story_Plot('Story_Test.xml') p.Suspend()") == "" && !m.plot(0).active);
    CHECK(run(vm, "p.Activate() p.Reset()") == "" && m.plot(0).active);
    // Wrapper identity: plots and events are cached per VM, so tables keyed by them hit.
    CHECK(run(vm, "kt = {} kt[p] = 'plot' kt[p.Get_Event('Enter')] = 'ev' q = Get_Story_Plot('STORY_TEST.xml')\n"
                  "i1 = kt[q] i2 = kt[q.Get_Event('ENTER')] i3 = rawequal(p, q) and 'same' or 'different'") == "");
    CHECK(gstr(vm, "i1") == "plot" && gstr(vm, "i2") == "ev" && gstr(vm, "i3") == "same");

    // Script delivery: fired events of a plot with a Lua_Script reach Story_Event_Trigger in order.
    CHECK(run(vm, "got = '' function Story_Event_Trigger(name) got = got .. name .. ',' end") == "");
    std::string expect;
    for (const StoryFired& f : m.fired()) expect += f.event_name + ",";
    const size_t n = m.deliver_script_triggers(0, vm, &err);
    CHECK(n == m.fired().size() && gstr(vm, "got") == expect);
    CHECK(m.deliver_script_triggers(0, vm) == 0);

    // story t02: parameters set from Lua take the event type's own rule (the shipped Intervention_Build_Units /
    // Empire Act IV patterns): a type + count on STORY_CONSTRUCT counts down, a reward amount lands in Reward_Param1, a
    // planet object in SPACE_TACTICAL Param2 is matched by its type name.
    CHECK(m.add_plot("Story_Lua.xml",
                     "<Story><Event Name=\"Build\"><Event_Type>STORY_CONSTRUCT</Event_Type></Event>"
                     "<Event Name=\"Pay\"><Event_Type>STORY_TRIGGER</Event_Type><Reward_Type>CREDITS</Reward_Type>"
                     "<Prereq>Build</Prereq></Event>"
                     "<Event Name=\"Raid\"><Event_Type>STORY_SPACE_TACTICAL</Event_Type>"
                     "<Event_Param1>Story_Plots_X.xml</Event_Param1></Event></Story>",
                     true, {}, &err) == 1);
    m.set_player(0);
    CHECK(run(vm, "lp = Get_Story_Plot('Story_Lua.xml') b = lp.Get_Event('Build')\n"
                  "b.Set_Event_Parameter(0, Find_Object_Type('Rock')) b.Set_Event_Parameter(1, 3)\n"
                  "lp.Get_Event('Pay').Set_Reward_Parameter(0, 3 * 500)\n"
                  "lp.Get_Event('Raid').Set_Event_Parameter(1, Object)") == "");
    {
        auto count = [&m](const char* name) {
            uint32_t n = 0;
            for (const StoryFired& f : m.fired()) n += f.event_name == name ? 1u : 0u;
            return n;
        };
        const StoryEventDef& build = m.plot(1).events[m.find_event(1, "Build")];
        CHECK(build.event_params.size() == 2 && build.event_params[0].text == "Rock" && build.event_params[1].text == "3");
        StoryNotification rock;
        rock.name = "ROCK";
        rock.has_object = true;
        rock.owner = 0;
        CHECK(m.dispatch("STORY_CONSTRUCT", rock) == 0 && m.dispatch("STORY_CONSTRUCT", rock) == 0);
        CHECK(m.dispatch("STORY_CONSTRUCT", rock) == 2 && count("Build") == 1 && count("Pay") == 1);  // 3rd: Build + Pay
        CHECK(m.fired().back().event_name == "Pay" && m.fired().back().reward_params.size() == 1 &&
              m.fired().back().reward_params[0].text == "1500");
        StoryNotification battle;
        battle.planet = "Thyferra";
        CHECK(m.dispatch("STORY_SPACE_TACTICAL", battle) == 0);
        battle.planet = "KUAT";
        CHECK(m.dispatch("STORY_SPACE_TACTICAL", battle) == 1 && count("Raid") == 1);
    }
    // l1s3 t01: Activate / Suspend return no values (STRATEGIC_GAPS 4 [H]).
    CHECK(run(vm, "function nvals(...) return arg.n end\n"
                  "na = nvals(lp.Suspend()) ns = tostring(lp.Is_Valid()) nb = nvals(lp.Activate())") == "");
    CHECK(gstr(vm, "na") == "0" && gstr(vm, "nb") == "0" && m.plot(1).active);

    // Detached: queries nil again.
    attach_story(vm, nullptr);
    CHECK(run(vm, "p = tostring(Get_Story_Plot('Story_Test.xml'))") == "" && gstr(vm, "p") == "nil");
    fs::remove_all(root, ec);
}

static void test_ai_flags() {
    StoryModel m;
    LuaHost host;
    LuaVm vm(host);
    attach_story(vm, &m);
    m.raise_ai_flag(2, "Attack_Here", -1);
    // PlayerWrapper userdata come from the players family; use the C++ API for the flag itself.
    CHECK(!m.check_ai_flag(1, "attack_here", -1, false));
    CHECK(m.check_ai_flag(2, "ATTACK_HERE", -1, false));
    CHECK(m.check_ai_flag(2, "attack_here", -1, true));
    CHECK(!m.check_ai_flag(2, "attack_here", -1, true));
}

// ------------------------------------------------------------------------------------------
// Story event semantics (roadmap STORY "story" t01-t03, "l1s3" t01-t03): one check per rule of docs/re/STORY.md.

static uint32_t fired_count(const StoryModel& m, const char* name) {
    uint32_t n = 0;
    for (const StoryFired& f : m.fired()) n += f.event_name == name ? 1u : 0u;
    return n;
}

// story t01: Prereq OR / AND (§3), STORY_ELAPSED arming (§4.7), Perpetual has no once-per-frame limit (§4.6).
static const char kSemantics[] = R"(<Story>
  <Event Name="A"><Event_Type>STORY_AI_NOTIFICATION</Event_Type><Event_Param2>A</Event_Param2></Event>
  <Event Name="B"><Event_Type>STORY_AI_NOTIFICATION</Event_Type><Event_Param2>B</Event_Param2></Event>
  <Event Name="C"><Event_Type>STORY_AI_NOTIFICATION</Event_Type><Event_Param2>C</Event_Param2></Event>
  <Event Name="AB_Or_C"><Event_Type>STORY_TRIGGER</Event_Type><Prereq>A, B</Prereq><Prereq>C</Prereq></Event>
  <Event Name="Wait"><Event_Type>STORY_ELAPSED</Event_Type><Event_Param1>3</Event_Param1><Prereq>A</Prereq></Event>
  <Event Name="Perp_Clock"><Event_Type>STORY_ELAPSED</Event_Type><Event_Param1>1</Event_Param1><Perpetual>True</Perpetual><Prereq>B</Prereq></Event>
  <Event Name="Perp_Note"><Event_Type>STORY_AI_NOTIFICATION</Event_Type><Event_Param2>PING</Event_Param2><Perpetual>True</Perpetual><Prereq>A</Prereq></Event>
</Story>)";

static void test_prereqs_elapsed_perpetual() {
    StoryModel m;
    std::string err;
    CHECK(m.add_plot("Semantics.xml", kSemantics, true, {}, &err) == 0 && m.warnings().empty());
    m.update(0, 0.0);
    CHECK(m.fired().empty());
    m.set_clock(1, 1.0);
    CHECK(m.story_event("A") == 1);  // AND group "A, B" (comma separated) still needs B
    const uint32_t wait = m.find_event(0, "Wait");
    CHECK(m.plot(0).state[wait].armed && m.plot(0).state[wait].activation_time == 1.0);
    m.update(2, 3.5);
    CHECK(fired_count(m, "Wait") == 0);  // armed at 1.0: 2.5 s accumulated
    m.update(3, 4.0);
    CHECK(fired_count(m, "Wait") == 1);  // 3 s after its activation, not after load
    // Perpetual: every matching notification fires, two in one frame included; a non-TRIGGER perpetual with prereqs
    // stays armed.
    CHECK(m.story_event("ping") == 1 && m.story_event("PING") == 1 && fired_count(m, "Perp_Note") == 2);
    CHECK(m.story_event("B") == 2 && fired_count(m, "AB_Or_C") == 1);  // B, then AB_Or_C inside B's Fire
    // Perpetual ELAPSED: the accumulator is never cleared, so after its first fire it fires at every T9 [M].
    m.update(4, 4.5);
    CHECK(fired_count(m, "Perp_Clock") == 0);
    m.update(5, 5.0);
    m.update(6, 5.0);
    m.update(7, 5.1);
    CHECK(fired_count(m, "Perp_Clock") == 3);
    // OR: the second group alone arms (and fires) AB_Or_C.
    StoryModel o;
    CHECK(o.add_plot("Semantics.xml", kSemantics, true, {}, &err) == 0);
    CHECK(o.story_event("c") == 2 && fired_names(o) == "C,AB_Or_C,");

    // A prereq-armed timer of a suspended plot: armed by a cascade at 1.0 (TRIGGER_EVENT reaches suspended plots),
    // nothing accumulates while suspended, then the first T9 after activation adds the whole gap [M] and fires.
    StoryModel g;
    CHECK(g.add_plot("Gap.xml",
                     "<Story><Event Name=\"Go\"><Event_Type>STORY_TRIGGER</Event_Type></Event>"
                     "<Event Name=\"Gap_Wait\"><Event_Type>STORY_ELAPSED</Event_Type><Event_Param1>5</Event_Param1>"
                     "<Prereq>Go</Prereq></Event></Story>",
                     false, {}, &err) == 0);
    g.update(0, 0.0);
    g.set_clock(1, 1.0);
    CHECK(g.trigger_event_by_name("go") == 1 && g.plot(0).state[1].armed);
    g.update(2, 4.0);
    CHECK(fired_count(g, "Gap_Wait") == 0 && g.plot(0).state[1].timer_acc == 0.0);
    g.set_plot_active(0, true);
    g.update(3, 6.5);
    CHECK(fired_count(g, "Gap_Wait") == 1);
}

// story t02: no count / threshold on the generic rule - a pushed value equal to the parameter matches, a larger one
// does not; the modelled types keep their own counters (CONQUER per-name counts, CONQUER_COUNT, thresholds).
static void test_exact_params() {
    StoryModel m;
    std::string err;
    CHECK(m.add_plot("Exact.xml",
                     "<Story><Event Name=\"Back\"><Event_Type>STORY_RETREAT</Event_Type><Event_Param1>5</Event_Param1></Event>"
                     "<Event Name=\"Sel\"><Event_Type>STORY_SELECT_PLANET</Event_Type><Event_Param1>Kuat</Event_Param1></Event>"
                     "</Story>",
                     true, {}, &err) == 0);
    const StoryParam six[] = {StoryParam::of_text("6")}, five[] = {StoryParam::of_text("5")};
    CHECK(m.push_trigger("STORY_RETREAT", six) == 0 && m.push_trigger("STORY_RETREAT", five) == 1);
    const StoryParam fondor[] = {StoryParam::of_text("Fondor")}, kuat[] = {StoryParam::of_text("KUAT")};
    CHECK(m.push_trigger("STORY_SELECT_PLANET", fondor) == 0 && m.push_trigger("STORY_SELECT_PLANET", kuat) == 1);
}

// story t03: RESET_BRANCH (exact branch compare, re-check, Param2, depth 3), DISABLE_BRANCH (Param2 required, kept by
// resets).
static const char kBranches[] = R"(<Story>
  <Event Name="Start"><Event_Type>STORY_AI_NOTIFICATION</Event_Type><Event_Param2>START</Event_Param2><Branch>Br</Branch></Event>
  <Event Name="Step"><Event_Type>STORY_TRIGGER</Event_Type><Prereq>Start</Prereq><Branch>Br</Branch></Event>
  <Event Name="Note"><Event_Type>STORY_AI_NOTIFICATION</Event_Type><Event_Param2>NOTE</Event_Param2><Prereq>Step</Prereq><Branch>Br</Branch></Event>
  <Event Name="Lower"><Event_Type>STORY_AI_NOTIFICATION</Event_Type><Event_Param2>LOWER</Event_Param2><Branch>br</Branch></Event>
  <Event Name="Reset_Br"><Event_Type>STORY_AI_NOTIFICATION</Event_Type><Event_Param2>RESET</Event_Param2><Perpetual>True</Perpetual>
     <Reward_Type>RESET_BRANCH</Reward_Type><Reward_Param1>Br</Reward_Param1><Reward_Param2>Start</Reward_Param2></Event>
  <Event Name="Off_Br"><Event_Type>STORY_AI_NOTIFICATION</Event_Type><Event_Param2>OFF</Event_Param2><Perpetual>True</Perpetual>
     <Reward_Type>DISABLE_BRANCH</Reward_Type><Reward_Param1>Br</Reward_Param1><Reward_Param2>1</Reward_Param2></Event>
  <Event Name="On_Br"><Event_Type>STORY_AI_NOTIFICATION</Event_Type><Event_Param2>ON</Event_Param2><Perpetual>True</Perpetual>
     <Reward_Type>DISABLE_BRANCH</Reward_Type><Reward_Param1>Br</Reward_Param1><Reward_Param2>0</Reward_Param2></Event>
  <Event Name="Off_Bad"><Event_Type>STORY_AI_NOTIFICATION</Event_Type><Event_Param2>BAD</Event_Param2>
     <Reward_Type>DISABLE_BRANCH</Reward_Type><Reward_Param1>Br</Reward_Param1></Event>
  <Event Name="Loop"><Event_Type>STORY_TRIGGER</Event_Type><Branch>L</Branch>
     <Reward_Type>RESET_BRANCH</Reward_Type><Reward_Param1>L</Reward_Param1><Reward_Param2>Loop</Reward_Param2></Event>
</Story>)";

static void test_branches() {
    StoryModel m;
    std::string err;
    CHECK(m.add_plot("Branches.xml", kBranches, true, {}, &err) == 0);
    const uint32_t start = m.find_event(0, "Start"), note = m.find_event(0, "Note");
    CHECK(m.story_event("LOWER") == 1);
    CHECK(m.story_event("START") == 2 && m.story_event("NOTE") == 1);  // Start + Step, then Note
    size_t mark = m.fired().size();
    // Reset Br: Start (no prereq) stays armed, Step / Note disarm; nothing re-arms (Start is un-fired); then Param2
    // fires Start, whose Fire re-arms Step (TRIGGER: fires) - Note is armed again, not fired.
    CHECK(m.story_event("RESET") == 3 && fired_names(m, mark) == "Reset_Br,Start,Step,");
    CHECK(m.plot(0).state[note].armed && !m.plot(0).state[note].fired);
    CHECK(m.story_event("LOWER") == 0);  // branch "br" is not "Br": untouched (still fired)
    // DISABLE_BRANCH 1: nothing of the branch fires, not even through RESET_BRANCH Param2; resets keep the flag.
    CHECK(m.story_event("OFF") == 1 && !m.plot(0).state[start].enabled && m.story_event("NOTE") == 0);
    mark = m.fired().size();
    CHECK(m.story_event("RESET") == 1 && fired_names(m, mark) == "Reset_Br," && !m.plot(0).state[start].enabled);
    // DISABLE_BRANCH 0 re-enables; without Param2 it is ignored.
    CHECK(m.story_event("ON") == 1 && m.plot(0).state[start].enabled);
    CHECK(m.story_event("BAD") == 1 && m.plot(0).state[start].enabled);
    CHECK(m.story_event("START") == 2);  // Start + Step again
    // RESET_BRANCH that fires itself through Param2: the depth limit 3 stops it (1 + 3 fires).
    m.trigger_event_by_name("Loop");
    CHECK(fired_count(m, "Loop") == 4 && m.plot(0).state[m.find_event(0, "Loop")].fire_count == 4);
}

// l1s3 t01 / t02: Timeout expiry fires the event [L]; the T9 paused flag stops ELAPSED accumulation.
static void test_timeouts_and_pause() {
    StoryModel m;
    std::string err;
    CHECK(m.add_plot("Timeouts.xml",
                     "<Story><Event Name=\"Go\"><Event_Type>STORY_AI_NOTIFICATION</Event_Type><Event_Param2>GO</Event_Param2></Event>"
                     "<Event Name=\"Speech\"><Event_Type>STORY_SPEECH_DONE</Event_Type><Event_Param1>Line_01</Event_Param1>"
                     "<Prereq>Go</Prereq></Event>"
                     "<Event Name=\"Timed\"><Event_Type>STORY_AI_NOTIFICATION</Event_Type><Event_Param2>NEVER</Event_Param2>"
                     "<Timeout>2</Timeout><Prereq>Go</Prereq></Event>"
                     "<Event Name=\"Early\"><Event_Type>STORY_AI_NOTIFICATION</Event_Type><Event_Param2>EARLY</Event_Param2>"
                     "<Timeout>2</Timeout><Prereq>Go</Prereq></Event>"
                     "<Event Name=\"Free\"><Event_Type>STORY_AI_NOTIFICATION</Event_Type><Event_Param2>FREE</Event_Param2>"
                     "<Timeout>1</Timeout></Event></Story>",
                     true, {}, &err) == 0);
    m.update(0, 0.0);
    m.set_clock(1, 1.0);
    CHECK(m.story_event("GO") == 1 && m.story_event("EARLY") == 1);  // Early fires: its timeout is cancelled
    m.update(2, 2.9);
    CHECK(fired_count(m, "Timed") == 0);
    m.update(3, 3.0);
    CHECK(fired_count(m, "Timed") == 1 && fired_count(m, "Early") == 1);
    m.update(4, 60.9);
    CHECK(fired_count(m, "Speech") == 0);
    m.update(5, 61.0);
    CHECK(fired_count(m, "Speech") == 1);  // SPEECH_DONE's default 60 s Timeout, from its activation at 1.0
    m.update(6, 100.0);
    CHECK(fired_count(m, "Free") == 0);  // a Timeout is registered when an event is armed by prereqs, never at load

    // Paused T9s add nothing even when the time argument moves; time goes on from the paused T9's value.
    StoryModel p;
    CHECK(p.add_plot("Pause.xml", "<Story><Event Name=\"Two\"><Event_Type>STORY_ELAPSED</Event_Type>"
                                  "<Event_Param1>2</Event_Param1></Event></Story>",
                     true, {}, &err) == 0);
    p.update(0, 0.0);
    p.update(1, 1.0, true);
    p.update(2, 2.5);
    CHECK(p.fired().empty() && p.plot(0).state[0].timer_acc == 1.5);
    p.update(3, 3.0);
    CHECK(p.fired().size() == 1);
}

// l1s3 t03: CONQUER_COUNT counts every dispatched conquest (no filter, any new owner); plot lookups are
// case-insensitive [port]; the story flag table is shared (STORY.md §0).
static void test_conquer_count_plots_flags() {
    StoryModel m;
    std::string err;
    CHECK(m.add_plot("Story_Count.xml",
                     "<Story><Event Name=\"Two_Planets\"><Event_Type>STORY_CONQUER_COUNT</Event_Type>"
                     "<Event_Param1>2</Event_Param1></Event>"
                     "<Event Name=\"Open\"><Event_Type>STORY_TRIGGER</Event_Type><Reward_Type>STORY_ELEMENT</Reward_Type>"
                     "<Reward_Param1>story_second</Reward_Param1></Event>"
                     "<Event Name=\"Win\"><Event_Type>STORY_TRIGGER</Event_Type><Reward_Type>SET_FLAG</Reward_Type>"
                     "<Reward_Param1>Mission_Win</Reward_Param1><Reward_Param2>1</Reward_Param2></Event></Story>",
                     true, {}, &err) == 0);
    CHECK(m.add_plot("Story_Second.XML", "<Story><Event Name=\"S\"><Event_Type>STORY_TRIGGER</Event_Type></Event></Story>",
                     false, {}, &err) == 1);
    m.set_player(0);
    StoryNotification enemy;
    enemy.name = "Kuat";
    enemy.has_object = true;
    enemy.owner = 3;
    CHECK(m.dispatch("STORY_CONQUER_COUNT", enemy) == 0);
    enemy.name = "Fondor";
    enemy.owner = 0;
    CHECK(m.dispatch("STORY_CONQUER_COUNT", enemy) == 1);
    CHECK(m.find_plot("STORY_SECOND.xml") == 1 && m.find_plot("Data\\XML\\story_second.xml") == 1);
    CHECK(m.trigger_event_by_name("Open") == 1 && m.plot(1).active);

    // Shared flags: the tactical plot set's SET_FLAG reaches the campaign's STORY_FLAG.
    StoryModel campaign;
    CHECK(campaign.add_plot("Campaign.xml",
                            "<Story><Event Name=\"Next\"><Event_Type>STORY_FLAG</Event_Type><Event_Param1>Mission_Win</Event_Param1>"
                            "<Event_Param2>1</Event_Param2></Event></Story>",
                            true, {}, &err) == 0);
    m.share_flags(campaign);
    CHECK(m.shares_flags(campaign) && !m.has_flag("Mission_Win"));
    CHECK(m.trigger_event_by_name("Win") == 1 && campaign.flag("MISSION_WIN") == 1);
    campaign.update(1, 1.0);
    CHECK(fired_count(campaign, "Next") == 1);
    // read() replaces the table's contents for every model sharing it.
    ByteWriter w;
    m.write(w);
    campaign.set_flag("Other", 5);
    ByteReader r(w.bytes());
    CHECK(m.read(r) && m.shares_flags(campaign) && !campaign.has_flag("Other") && campaign.flag("Mission_Win") == 1);
}

static void test_coverage() {
    const BindingCoverage c = binding_coverage();
    std::printf("  coverage: globals %u/%u, methods %u/%u, call sites %llu/%llu\n", c.globals_implemented, c.globals,
                c.methods_implemented, c.methods, static_cast<unsigned long long>(c.calls_implemented),
                static_cast<unsigned long long>(c.calls_total));
    for (const BindingInfo& b : binding_table()) {
        const std::string cls = b.cls;
        const std::string name = b.name;
        const bool mine = cls == "StoryPlotWrapper" || cls == "StoryEventWrapper" || name == "Story_Event" ||
                          name == "Get_Story_Plot" || name == "Check_Story_Flag" || name == "Add_Objective" ||
                          name == "Add_Radar_Blip" || name == "Remove_Radar_Blip" || name == "Add_Planet_Highlight" ||
                          name == "Remove_Planet_Highlight";
        if (mine && !binding_implemented(b)) {
            std::printf("  story binding not implemented: %s.%s\n", b.cls, b.name);
            ++g_fail;
        }
    }
}

// ------------------------------------------------------------------------------------------
// Shipped FoC campaigns (ALAMO_FOC_DATA).

static std::string env(const char* name) {
#ifdef _MSC_VER
    char* v = nullptr;
    size_t len = 0;
    std::string out;
    if (_dupenv_s(&v, &len, name) == 0 && v) {
        out = v;
        std::free(v);
    }
    return out;
#else
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
#endif
}

static void test_foc_campaigns() {
    const std::string root = env("ALAMO_FOC_DATA");
    if (root.empty()) {
        std::printf("  ALAMO_FOC_DATA unset: shipped campaign plots skipped\n");
        return;
    }
    std::unique_ptr<FileProvider> files;
#ifdef ALAMO_LUA_HAVE_VFS
    vfs::VfsConfig cfg;
    cfg.foc_root = root;
    cfg.eaw_root = "";
    std::string verr;
    if (auto v = vfs::Vfs::create(cfg, &verr)) files = std::move(v);
#endif
    if (!files) files = std::make_unique<DirectoryProvider>(root);

    // Underworld campaign: one plot + Lua script; the elapsed-0 start event and its trigger chain.
    StoryModel uw;
    std::string err;
    if (!uw.load_plots(*files, "Story_Plots_Campaign_Underworld.xml", &err)) {
        std::printf("  FoC: cannot load Underworld plots: %s\n", err.c_str());
        ++g_fail;
        return;
    }
    CHECK(uw.plot_count() == 1 && uw.plot(0).lua_script == "Story_Campaign_Underworld" && uw.plot(0).active);
    std::printf("  FoC Underworld: %zu events, %zu load warnings\n", uw.plot(0).events.size(), uw.warnings().size());
    CHECK(uw.warnings().empty());  // "A, B, C" prereq lists resolve
    uw.update(0, 0.0);
    CHECK(uw.fired().empty());  // A2i t01: the first T9 records the ELAPSED 0 start only (0x454980)
    uw.update(1, 1.0 / 30.0);
    CHECK(!uw.fired().empty() && uw.fired()[0].event_name == "Underworld_Campaign_Begin");
    CHECK(uw.has_flag("Underworld_Mission_01_Win") && uw.flag("Underworld_Mission_01_Win") == 0);
    const size_t start_fired = uw.fired().size();
    std::printf("  FoC Underworld: %zu events fired at start\n", start_fired);

    // Fire an armed Story_Event from Lua: the first armed STORY_AI_NOTIFICATION in index order.
    LuaHost host;
    host.files = files.get();
    LuaVm vm(host);
    attach_story(vm, &uw);
    std::string note;
    for (uint32_t e = 0; e < uw.plot(0).events.size() && note.empty(); ++e) {
        const StoryEventDef& d = uw.plot(0).events[e];
        if (d.type == "STORY_AI_NOTIFICATION" && uw.plot(0).state[e].armed && !uw.plot(0).state[e].fired &&
            d.event_params.size() >= 2 && d.event_params[0].text.empty())  // no object filter (Param1)
            note = d.event_params[1].text;
    }
    if (!note.empty()) {
        CHECK(run(vm, "Story_Event('" + note + "')") == "");
        CHECK(uw.fired().size() > start_fired);
        std::printf("  FoC Underworld: Story_Event('%s') fired %zu events\n", note.c_str(), uw.fired().size() - start_fired);
    }

    // PGStoryMode.lua: Story_Event_Trigger moves the state machine to the fired event's state.
    if (vm.require("PGStoryMode", &err)) {
        CHECK(run(vm, "StateMachine = {} StateMachineIndexes = {} StateMachineIndexLookup = {} DefineStateIndex = 1\n"
                      "StoryModeEvents = { Underworld_Campaign_Begin = function(message) end }\n"
                      "PG_Story_Mode_Init() CurrentState = NextState") == "");
        std::string derr;
        const size_t n = uw.deliver_script_triggers(0, vm, &derr);
        CHECK(n == uw.fired().size() && derr.empty());
        CHECK(gstr(vm, "NextState") == "Underworld_Campaign_Begin");
        if (!derr.empty()) std::printf("  deliver: %s\n", derr.c_str());
    } else {
        std::printf("  FoC: PGStoryMode not loadable: %s\n", err.c_str());
        ++g_fail;
    }

    // Determinism: the same load + inputs twice give the same fired log and checksum.
    StoryModel uw2;
    CHECK(uw2.load_plots(*files, "Story_Plots_Campaign_Underworld.xml", &err));
    uw2.update(0, 0.0);
    uw2.update(1, 1.0 / 30.0);
    if (!note.empty()) uw2.story_event(note);
    CHECK(fired_names(uw2) == fired_names(uw) && digest(uw2) != 0);
    StoryModel uw3;
    CHECK(uw3.load_plots(*files, "Story_Plots_Campaign_Underworld.xml", &err));
    uw3.update(0, 0.0);
    uw3.update(1, 1.0 / 30.0);
    if (!note.empty()) uw3.story_event(note);
    CHECK(digest(uw3) == digest(uw2));

    // Empire: four acts, Lua_Script pairs with the next plot only (Act II has none).
    StoryModel emp;
    CHECK(emp.load_plots(*files, "Story_Plots_Campaign_Empire.xml", &err));
    CHECK(emp.plot_count() == 5);
    if (emp.plot_count() == 5) {
        CHECK(emp.plot(0).lua_script == "Story_Campaign_Empire_Act_I" && emp.plot(0).active);
        CHECK(emp.plot(1).lua_script.empty() && !emp.plot(1).active);
        CHECK(emp.plot(2).lua_script == "Story_Campaign_Empire_Act_III" && !emp.plot(2).active);
        CHECK(emp.plot(4).lua_script.empty() && emp.plot(4).active);  // cheats
        emp.update(0, 0.0);
        std::printf("  FoC Empire: %zu plots, %zu events fired at start\n", emp.plot_count(), emp.fired().size());
    }

    // Every shipped plot set loads.
    const char* sets[] = {"Story_Plots_Campaign_Rebel.xml", "Story_Plots_Underworld_M01_LAND.xml",
                          "Story_Plots_Tutorial_M03_SPACE.xml", "Story_Plots_Universal_Triggers_E.xml"};
    for (const char* s : sets) {
        StoryModel any;
        const bool ok = any.load_plots(*files, s, &err);
        if (!ok) std::printf("  FoC: %s: %s\n", s, err.c_str());
        CHECK(ok);
        if (ok) any.update(0, 0.0);
    }
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    log::set_min_level(log::Level::Warn);
    test_synthetic();
    test_story_rules();
    test_plots_file();
    test_bindings();
    test_ai_flags();
    test_prereqs_elapsed_perpetual();
    test_exact_params();
    test_branches();
    test_timeouts_and_pause();
    test_conquer_count_plots_flags();
    test_coverage();
    test_foc_campaigns();
    if (g_fail) {
        std::printf("alamo_lua_story_tests: %d FAILED\n", g_fail);
        return 1;
    }
    std::printf("alamo_lua_story_tests: all passed\n");
    return 0;
}
