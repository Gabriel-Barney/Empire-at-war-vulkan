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
#include "alamo/lua/ai_bridge.hpp"  // reset_script_threads
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
    CHECK(m.plot(0).active && !m.plot(1).active && m.find_plot("B.xml") == 1 && m.find_plot("b.XML") == 0xFFFFFFFFu);
    m.update(0, 0);
    CHECK(fired_names(m) == "A1,");  // the prereq-less STORY_TRIGGER A_Jump never fires by itself
    m.set_plot_active(1, true);
    CHECK(fired_names(m) == "A1,");  // activation only sets the flag
    m.update(1, 1.0 / 30.0);
    CHECK(fired_names(m) == "A1,B1,");  // first T9 after activation
    StoryModel bad;
    CHECK(!bad.load_plots(dp, "Missing.xml", &err) && !err.empty());
    // Q6: names with a folder below Data\XML (Thrawn's Revenge) are read as written; a folder that does not exist falls
    // back to the base name (the FoC rule).
    write_file(root / "Data/XML/Conquests/Sub/Story_Plots_S.xml",
               "<Story_Mode_Plots><Active_Plot>Conquests\\Sub\\S1.xml</Active_Plot>"
               "<Active_Plot>Nowhere/B.xml</Active_Plot></Story_Mode_Plots>");
    write_file(root / "Data/XML/Conquests/Sub/S1.xml", "<Story><Event Name=\"S1\"><Event_Type>STORY_ELAPSED</Event_Type>"
                                                       "<Event_Param1>0</Event_Param1></Event></Story>");
    DirectoryProvider dp2(root.string());  // DirectoryProvider indexes its files when it is made
    StoryModel sub;
    CHECK(sub.load_plots(dp2, "Conquests\\Sub\\Story_Plots_S.xml", &err));
    CHECK(sub.plot_count() == 2 && sub.find_plot("Conquests\\Sub\\S1.xml") == 0 &&
          sub.find_plot("Nowhere/B.xml") == 1 && sub.find_plot("S1.xml") == 0xFFFFFFFFu);
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
    // Without a story: commands are no-ops, queries return nothing.
    CHECK(run(vm, "Story_Event('X') p = tostring(Get_Story_Plot('Story_Test.xml'))") != "");  // tostring() of no value
    CHECK(run(vm, "p = Get_Story_Plot('Story_Test.xml') p = tostring(p)") == "" && gstr(vm, "p") == "nil");

    StoryModel m;
    std::string err;
    CHECK(m.add_plot("Story_Test.xml", kPlot, true, "Story_Test", &err) == 0);
    attach_story(vm, &m);
    CHECK(attached_story(vm) == &m);
    m.update(0, 0.0);
    vm.set_object_global("Object", planet);

    CHECK(run(vm, "Story_Event('GO_NOW')") == "");
    CHECK(m.flag("Mission_Win") == 1);
    // q6 t03: the plot name matches case-sensitively (STORY.md §2.1: CRC of the name as passed); not found -> a warning
    // and no value.
    const size_t msgs0 = vm.messages().size();
    CHECK(run(vm, "wrong = Get_Story_Plot('story_test.XML') wrong = tostring(wrong)") == "" && gstr(vm, "wrong") == "nil");
    CHECK(vm.messages().size() == msgs0 + 1 &&
          vm.messages().back().find("Get_Story_Plot -- plot story_test.XML not found.") != std::string::npos);
    CHECK(run(vm, "plot = Get_Story_Plot('Story_Test.xml') v = tostring(plot.Is_Valid())\n"
                  "ev = plot.Get_Event('Enter') ev2 = plot.Get_Event('enter') eq = tostring(ev == ev2)\n"
                  "ev_valid = tostring(ev.Is_Valid())\n"
                  "ev.Set_Event_Parameter(0, Object)\n"
                  "ev.Set_Reward_Parameter(1, 7)\n"
                  "ev.Set_Reward_Type('screen_text')\n"
                  "ev.Add_Dialog_Text('TEXT_A', Object) ev.Clear_Dialog_Text() ev.Add_Dialog_Text('TEXT_B', 3)\n"
                  "Add_Objective('TEXT_LUA_OBJ', true)\n"
                  "Add_Radar_Blip(Object, 'blip_1') Add_Radar_Blip(Object, 'blip_2') Remove_Radar_Blip('blip_1')\n"
                  "Add_Planet_Highlight(Object, '0') Remove_Planet_Highlight('0') Add_Planet_Highlight(Object, '1')") == "");
    CHECK(gstr(vm, "v") == "true" && gstr(vm, "eq") == "true" && gstr(vm, "ev_valid") == "true");
    CHECK(!vm.exit_requested());
    // Get_Event: a missing event is a script error (no unwind, no value; the script is flagged to end).
    CHECK(run(vm, "none = plot.Get_Event('Nope') none = tostring(none)") == "" && gstr(vm, "none") == "nil");
    CHECK(vm.exit_requested() && vm.messages().back().find("SCRIPT ERROR: StoryPlotWrapper::Get_Event -- cannot find event "
                                                           "Nope in plot Story_Test.xml.") != std::string::npos);
    const uint32_t enter = m.find_event(0, "Enter");
    const StoryEventDef& def = m.plot(0).events[enter];
    const int32_t planet_id = store.id(planet);
    // story t02: index 0 = Event_Param1; the planet object is stored as its type NAME, upper-cased (q6 t03: the converter
    // 0x73e610 gives the representative type's +0xf8 name); numbers as rounded "%d".
    CHECK(def.event_params[0].kind == StoryParam::Kind::Text && def.event_params[0].text == "KUAT");
    CHECK(def.reward_params.size() == 2 && def.reward_params[1].text == "7" && def.reward_type == "SCREEN_TEXT");
    // q6 t03: Add_Dialog_Text appends the formatted line (no text table: ids show as themselves).
    CHECK(def.dialog.size() == 1 && def.dialog[0] == "TEXT_B");
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
    // Event parameter index (q6 t03, LUA_API.md: the binding does not range-check, vf2 ignores what it has no parameter
    // for): [port] outside 0..63 nothing happens, no error.
    const size_t params_before = def.event_params.size();
    CHECK(run(vm, "Get_Story_Plot('Story_Test.xml').Get_Event('Enter').Set_Event_Parameter(-1, 1)") == "");
    CHECK(run(vm, "Get_Story_Plot('Story_Test.xml').Get_Event('Enter').Set_Event_Parameter(64, 1)") == "");
    CHECK(def.event_params.size() == params_before);

    // Contracts (the original's messages). The StoryEventWrapper setters use the script error model (logged, no unwind).
    auto last_msg_has = [&vm](const char* text) {
        return !vm.messages().empty() && vm.messages().back().find(text) != std::string::npos;
    };
    CHECK(run(vm, "Story_Event()") == "" && vm.exit_requested() && last_msg_has("Expected 1 or 2, got 0"));
    CHECK(run(vm, "Story_Event(3)") == "" && vm.exit_requested() && last_msg_has("Expected string"));
    CHECK(run(vm, "Get_Story_Plot()") == "" && vm.exit_requested() && last_msg_has("Get_Story_Plot -- invalid number of parameters"));
    CHECK(run(vm, "Add_Objective('x')") == "" && vm.exit_requested() && last_msg_has("Expected 2, got 1"));
    CHECK(run(vm, "Add_Radar_Blip(5, 'n')") == "" && vm.exit_requested() && last_msg_has("could not deduce a position"));
    CHECK(run(vm, "Get_Story_Plot('Story_Test.xml').Get_Event('Enter').Set_Reward_Parameter('a', 1)") == "" &&
          last_msg_has("Set_Reward_Parameter -- invalid type for parameter 1. Expected number."));
    CHECK(run(vm, "Get_Story_Plot('Story_Test.xml').Get_Event('Enter').Set_Reward_Parameter(1, {})") == "" &&
          last_msg_has("Set_Reward_Parameter -- must set exactly one parameter per parameter index."));
    CHECK(run(vm, "Get_Story_Plot('Story_Test.xml').Get_Event('Enter').Set_Reward_Parameter(1, true)") == "" &&
          last_msg_has("must set exactly one parameter per parameter index."));
    CHECK(run(vm, "Get_Story_Plot('Story_Test.xml').Get_Event('Enter').Set_Reward_Parameter(1)") == "" &&
          last_msg_has("Set_Reward_Parameter -- invalid number of parameters. Expected 2, got 1."));
    CHECK(run(vm, "Get_Story_Plot('Story_Test.xml').Get_Event('Enter').Set_Event_Parameter(1)") == "" &&
          last_msg_has("Set_Event_Parameter -- invalid number of parameters. Expected 2, got 1."));
    CHECK(run(vm, "Get_Story_Plot('Story_Test.xml').Get_Event('Enter').Set_Reward_Type(5)") == "" &&
          last_msg_has("Set_Reward_Type -- invalid type for parameter 1. Expected string."));
    CHECK(run(vm, "Get_Story_Plot('Story_Test.xml').Get_Event('Enter').Set_Dialog()") == "" &&
          last_msg_has("Set_Dialog -- invalid number of parameters. Expected 1, got 0."));
    CHECK(run(vm, "Check_Story_Flag(1, 'F', nil, true)") == "" && vm.exit_requested() && last_msg_has("Expected player"));
    reset_script_threads(vm);  // q6 t11: the binding errors above stopped the script; a live script for what follows

    // StoryPlotWrapper.Reset / Suspend / Activate.
    CHECK(run(vm, "p = Get_Story_Plot('Story_Test.xml') p.Suspend()") == "" && !m.plot(0).active);
    CHECK(run(vm, "p.Activate() p.Reset()") == "" && m.plot(0).active);
    // Wrapper identity: plots and events are cached per VM, so tables keyed by them hit.
    CHECK(run(vm, "kt = {} kt[p] = 'plot' kt[p.Get_Event('Enter')] = 'ev' q = Get_Story_Plot('Story_Test.xml')\n"
                  "i1 = kt[q] i2 = kt[q.Get_Event('ENTER')] i3 = rawequal(p, q) and 'same' or 'different'") == "");
    CHECK(gstr(vm, "i1") == "plot" && gstr(vm, "i2") == "ev" && gstr(vm, "i3") == "same");

    // Script delivery: fired events of a plot with a Lua_Script reach Story_Event_Trigger in order.
    CHECK(run(vm, "got = '' function Story_Event_Trigger(name) got = got .. name .. ',' end") == "");
    std::string expect;
    for (const StoryFired& f : m.fired()) expect += f.event_name + ",";
    const size_t n = m.deliver_script_triggers(0, vm, &err);
    CHECK(n == m.fired().size() && gstr(vm, "got") == expect);
    CHECK(m.deliver_script_triggers(0, vm) == 0);
    // q6 t11 (LUA_API.md "Binding error model across all bindings"): a binding error stops the plot script; its events
    // still fire, but Story_Event_Trigger is not called any more (the queued names are dropped).
    {
        const auto fire_one = [&m] {  // an enabled event of plot 0
            for (uint32_t e = 0; e < m.plot(0).events.size(); ++e)
                if (m.trigger_event(0, e)) return true;
            return false;
        };
        const size_t fired0 = m.fired().size();
        CHECK(run(vm, "got = '' Story_Event() after = 'yes'") == "" && vm.exit_requested() && gstr(vm, "after") == "yes");
        CHECK(fire_one() && m.fired().size() > fired0);  // (with its cascade)
        CHECK(m.deliver_script_triggers(0, vm) == 0 && gstr(vm, "got") == "");
        reset_script_threads(vm);
        const size_t fired1 = m.fired().size();
        CHECK(fire_one() && m.deliver_script_triggers(0, vm) == m.fired().size() - fired1 &&
              gstr(vm, "got") == fired_names(m, fired1));
    }

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
        CHECK(build.event_params.size() == 2 && build.event_params[0].text == "ROCK" && build.event_params[1].text == "3");
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
    CHECK(run(vm, "p = Get_Story_Plot('Story_Test.xml') p = tostring(p)") == "" && gstr(vm, "p") == "nil");
    fs::remove_all(root, ec);
}

// q6 t03 (STORY.md §2.1 / §2.4 [H]): one plot table per game mode, every player's plot set in it (StoryTable).
// Get_Story_Plot finds a plot of another player's set by its exact name (Thrawn's Revenge GCPlayerAgnostic, in the
// Independent_Forces set, asks for "Conquests\\Factions\\Story_Generic_EMPIRE.xml", which the Empire's set loads) and the
// wrapper then acts on that set; Lua Story_Event reaches every ACTIVE plot of every set; TRIGGER_EVENT fires the event
// in every plot that has it, suspended ones too; STORY_ELEMENT opens any set's plot; a later plot of the same name
// replaces the earlier one.
static uint32_t fired_count(const StoryModel& m, const char* name);

static const char kTablePlot[] =
    "<Story><Event Name=\"Recon_01\"><Event_Type>STORY_TRIGGER</Event_Type></Event>"
    "<Event Name=\"Note\"><Event_Type>STORY_AI_NOTIFICATION</Event_Type><Event_Param2>PING</Event_Param2>"
    "<Perpetual>True</Perpetual></Event>"
    "<Event Name=\"Shared\"><Event_Type>STORY_TRIGGER</Event_Type></Event></Story>";

static void test_story_table() {
    StoryModel own, empire, rebel;
    std::string err;
    CHECK(own.add_plot("Conquests\\Player_Agnostic_Plot.xml", kTablePlot, true, "GCPlayerAgnostic", &err) == 0);
    CHECK(own.add_plot("Conquests\\Rewards.xml",
                       "<Story><Event Name=\"Fire_Shared\"><Event_Type>STORY_TRIGGER</Event_Type>"
                       "<Reward_Type>TRIGGER_EVENT</Reward_Type><Reward_Param1>Shared</Reward_Param1></Event>"
                       "<Event Name=\"Open_Rebel\"><Event_Type>STORY_TRIGGER</Event_Type><Reward_Type>STORY_ELEMENT</Reward_Type>"
                       "<Reward_Param1>Conquests\\Factions\\Story_Generic_REBEL</Reward_Param1></Event></Story>",
                       true, {}, &err) == 1);
    CHECK(empire.add_plot("Conquests\\Historical\\Story_Campaign_Empire.xml", kTablePlot, true, {}, &err) == 0);
    CHECK(empire.add_plot("Conquests\\Factions\\Story_Generic_EMPIRE.xml", kTablePlot, true, {}, &err) == 1);
    CHECK(rebel.add_plot("Conquests\\Factions\\Story_Generic_REBEL.xml", kTablePlot, false, {}, &err) == 0);
    LuaHost host;
    LuaVm vm(host);
    attach_story(vm, &own);
    // Without a table: the own set only.
    CHECK(run(vm, "p = Get_Story_Plot('Conquests\\\\Factions\\\\Story_Generic_EMPIRE.xml') p = tostring(p)") == "" &&
          gstr(vm, "p") == "nil");
    StoryTable table;
    table.assign({&empire, &own, &rebel});
    CHECK(own.table() == &table && empire.table() == &table && table.set_count() == 3 && table.order().size() == 5);
    CHECK(table.set_of(&rebel) == 2 && table.set_of(nullptr) == 0xFFFFFFFFu);
    attach_story_table(vm, &table);
    CHECK(run(vm, "pe = Get_Story_Plot('Conquests\\\\Factions\\\\Story_Generic_EMPIRE.xml')\n"
                  "po = Get_Story_Plot('Conquests\\\\Player_Agnostic_Plot.xml')\n"
                  "v = tostring(pe.Is_Valid()) ev = pe.Get_Event('Recon_01') evv = tostring(ev.Is_Valid())\n"
                  "ev.Set_Dialog('Recon') ev.Add_Dialog_Text('TEXT_RECON')\n"
                  "same = rawequal(pe, Get_Story_Plot('Conquests\\\\Factions\\\\Story_Generic_EMPIRE.xml')) and 'same' or 'x'\n"
                  "other = rawequal(pe, po) and 'same' or 'different'") == "");
    CHECK(gstr(vm, "v") == "true" && gstr(vm, "evv") == "true");
    CHECK(gstr(vm, "same") == "same" && gstr(vm, "other") == "different");
    // The name matches byte for byte: no base-name match, case kept, no ".xml" added; each miss warns.
    const size_t msgs = vm.messages().size();
    CHECK(run(vm, "a = Get_Story_Plot('Story_Generic_REBEL.xml')\n"
                  "b = Get_Story_Plot('conquests\\\\factions\\\\story_generic_rebel.xml')\n"
                  "c = Get_Story_Plot('Conquests\\\\Factions\\\\Story_Generic_REBEL')\n"
                  "a = tostring(a) .. tostring(b) .. tostring(c)") == "" && gstr(vm, "a") == "nilnilnil");
    CHECK(vm.messages().size() == msgs + 3);
    // The edits landed in the Empire's set, not the caller's.
    const StoryEventDef& recon = empire.plot(1).events[empire.find_event(1, "Recon_01")];
    CHECK(recon.story_dialog == "Recon" && recon.dialog.size() == 1 && recon.dialog[0] == "TEXT_RECON");
    CHECK(own.plot(0).events[0].dialog.empty() && empire.plot(0).events[0].dialog.empty());
    // Story_Event: every ACTIVE plot of every set (the Rebel plot is suspended).
    CHECK(run(vm, "Story_Event('PING')") == "");
    CHECK(fired_count(own, "Note") == 1 && fired_count(empire, "Note") == 2 && fired_count(rebel, "Note") == 0);
    // TRIGGER_EVENT: every plot that has the event, suspended ones too, every set.
    CHECK(own.trigger_event(1, own.find_event(1, "Fire_Shared")));
    CHECK(fired_count(own, "Shared") == 1 && fired_count(empire, "Shared") == 2 && fired_count(rebel, "Shared") == 1);
    CHECK(table.trigger_event_by_name("shared") == 4);
    // STORY_ELEMENT: any set's plot (Reward_Param1 + ".xml").
    CHECK(own.trigger_event(1, own.find_event(1, "Open_Rebel")) && rebel.plot(0).active);
    CHECK(table.story_event("ping") == 4 && fired_count(rebel, "Note") == 1);
    // A later set listing the same file replaces the earlier plot: the lookups, dispatches and walks reach only the new one.
    StoryModel later;
    CHECK(later.add_plot("Conquests\\Player_Agnostic_Plot.xml", kTablePlot, true, {}, &err) == 0);
    table.assign({&empire, &own, &rebel, &later});
    CHECK(!own.plot(0).in_table && later.plot(0).in_table && own.plot_order().size() == 1 && table.order().size() == 5);
    CHECK(own.find_plot("Conquests\\Player_Agnostic_Plot.xml") == 0xFFFFFFFFu);
    const StoryTable::PlotRef r = table.find_plot("Conquests\\Player_Agnostic_Plot.xml");
    CHECK(r.valid() && r.set == 3 && r.plot == 0);
    const uint32_t own_notes = fired_count(own, "Note");
    CHECK(table.story_event("PING") == 4 && fired_count(own, "Note") == own_notes && fired_count(later, "Note") == 1);
    // Inside one model the later plot of a name wins the same way.
    StoryModel twice;
    CHECK(twice.add_plot("Same.xml", kTablePlot, true, {}, &err) == 0 && twice.add_plot("Same.xml", kTablePlot, true, {}, &err) == 1);
    CHECK(twice.find_plot("Same.xml") == 1 && !twice.plot(0).in_table && twice.plot_order().size() == 1);
    CHECK(twice.story_event("PING") == 1 && twice.fired()[0].plot == 1);
    // clear() hands every plot back to its model.
    table.clear();
    CHECK(own.table() == nullptr && own.plot(0).in_table && own.plot_order().size() == 2);
    attach_story_table(vm, nullptr);
    CHECK(run(vm, "q = Get_Story_Plot('Conquests\\\\Factions\\\\Story_Generic_EMPIRE.xml') q = tostring(q)") == "" &&
          gstr(vm, "q") == "nil");
}

// q6 t03 (LUA_API.md "StoryEventWrapper dialog and parameter setters" [H]): Add_Dialog_Text slots and formatting, the
// error / silent-abort cases, Clear_Dialog_Text, the shared converter of Set_Event_Parameter / Set_Reward_Parameter, and
// the silent return of every method when the wrapper's event is gone.
static void test_dialog_setters() {
    const fs::path root = fs::temp_directory_path() / "alamo_lua_story_dialog";
    std::error_code ec;
    fs::remove_all(root, ec);
    write_file(root / "Data/XML/GameObjectFiles.xml", "<L><File>S.xml</File></L>");
    write_file(root / "Data/XML/S.xml",
               "<R><Planet Name=\"Kuat\"><Text_ID>TEXT_PLANET_KUAT</Text_ID></Planet><Prop Name=\"Rock\"></Prop></R>");
    DirectoryProvider dp(root.string());
    objects::ObjectTypeDatabase raw;
    CHECK(raw.load(dp));
    objects::GameObjectTypes types;
    CHECK(types.build(raw));
    objects::IdCounter ids;
    objects::ObjectStore store(objects::ModeKind::Galactic, objects::StoreKind::Primary, types, &ids);
    objects::SpawnParams sp;
    sp.type = raw.find("Kuat");
    const objects::ObjectHandle kuat = store.create(sp);
    sp.type = raw.find("Rock");
    const objects::ObjectHandle rock = store.create(sp);
    const objects::ObjectHandle gone = store.create(sp);

    StoryModel m;
    std::string err;
    CHECK(m.add_plot("Dialog.xml", "<Story><Event Name=\"Ev\"><Event_Type>STORY_TRIGGER</Event_Type></Event></Story>", true,
                     {}, &err) == 0);
    m.text_lookup = [](std::string_view id, std::string& out) {
        if (id == "TEXT_FMT") out = "A=%s B=%s C=%s 100%%";
        else if (id == "TEXT_PLANET_KUAT") out = "Kuat Drive Yards";
        else if (id == "TEXT_CREDITS") out = "credits";
        else return false;
        return true;
    };
    LuaHost host;
    host.store = &store;
    host.types = &types;
    LuaVm vm(host);
    attach_story(vm, &m);
    vm.set_object_global("Kuat", kuat);
    vm.set_object_global("Rock", rock);
    vm.set_object_global("Gone", gone);
    CHECK(store.destroy(gone));
    CHECK(run(vm, "ev = Get_Story_Plot('Dialog.xml').Get_Event('Ev')\n"
                  "ev.Add_Dialog_Text('TEXT_FMT')\n"                                // empty slots
                  "ev.Add_Dialog_Text('TEXT_FMT', Kuat, 'TEXT_CREDITS')\n"          // display name, text lookup
                  "ev.Add_Dialog_Text('TEXT_FMT', 2.5, -1.7)\n"                     // (int)(n + 0.5)
                  "ev.Add_Dialog_Text('TEXT_FMT', Find_Object_Type('Kuat'), 'NO_SUCH_TEXT')\n"  // type; missing id verbatim
                  "ev.Add_Dialog_Text('TEXT_FMT', Gone, 1)\n"                       // dead object: nothing added
                  "ev.Add_Dialog_Text('TEXT_FMT', Rock)\n"                          // no Text_ID: empty name
                  "ev.Add_Dialog_Text('UNKNOWN_LINE')") == "");                     // missing format id: itself
    const StoryEventDef& def = m.plot(0).events[0];
    const std::vector<std::string> want{"A= B= C= 100%",    "A=Kuat Drive Yards B=credits C= 100%",
                                        "A=3 B=-1 C= 100%", "A=Kuat Drive Yards B=NO_SUCH_TEXT C= 100%",
                                        "A= B= C= 100%",    "UNKNOWN_LINE"};
    CHECK(def.dialog == want);
    CHECK(!vm.exit_requested() && vm.messages().empty());
    // Errors: a slot of another type is an error, but the line is still added; 4+ arguments or a non-string id add nothing.
    CHECK(run(vm, "ev.Add_Dialog_Text('TEXT_FMT', true, 7)") == "" && def.dialog.size() == 7 &&
          def.dialog.back() == "A= B=7 C= 100%");
    CHECK(vm.exit_requested() && vm.messages().size() == 1 &&
          vm.messages()[0] == "SCRIPT ERROR: StoryEventWrapper::Add_Dialog_Text -- invalid type for parameter 1.");
    CHECK(run(vm, "ev.Add_Dialog_Text('a', 'b', 'c', 'd') ev.Add_Dialog_Text(5)") == "" && def.dialog.size() == 7);
    CHECK(vm.messages().size() == 3 && vm.messages()[1].find("invalid number of parameters. Expected 2.") != std::string::npos &&
          vm.messages()[2].find("invalid type for parameter 1. Expected string.") != std::string::npos);
    CHECK(run(vm, "ev.Clear_Dialog_Text(1, 2)") == "" && def.dialog.empty());
    // The shared converter: object / type -> upper-cased type name, string as is, number rounded; others dropped. One
    // Set_Event_Parameter call sets a whole name list.
    CHECK(run(vm, "ev.Set_Event_Parameter(0, 'Abc', Kuat, 2.4, nil, true, {}, Gone)\n"
                  "ev.Set_Event_Parameter(1.6, Find_Object_Type('Rock'))\n"
                  "ev.Set_Reward_Parameter(0, Kuat) ev.Set_Reward_Parameter(0.4999, 'Rock')") == "");
    CHECK(def.event_params.size() == 3 && def.event_params[0].text == "Abc KUAT 2" && def.event_params[2].text == "ROCK");
    CHECK(def.reward_params.size() == 1 && def.reward_params[0].text == "Rock");
    CHECK(run(vm, "ev.Set_Reward_Parameter(1, Gone)") == "" && def.reward_params.size() == 1 &&
          vm.messages().back().find("must set exactly one parameter per parameter index.") != std::string::npos);
    // Set_Dialog: the event's Story_Dialog only.
    CHECK(run(vm, "ev.Add_Dialog_Text('X') ev.Set_Dialog('Intervention_Dialog')") == "" &&
          def.story_dialog == "Intervention_Dialog" && def.dialog.size() == 1);
    // ... and loads that dialog script at the call (q6 t14, GALACTIC_DIALOGS §2.8).
    CHECK(!m.dialog_loads().empty() && m.dialog_loads().back() == "Intervention_Dialog");
    // An event wrapper whose event is gone: every method returns silently, before any argument check.
    StoryModel empty;
    attach_story(vm, &empty);
    const size_t msgs = vm.messages().size();
    CHECK(run(vm, "ev.Add_Dialog_Text(1, 2, 3, 4) ev.Set_Event_Parameter() ev.Set_Reward_Parameter('x') ev.Set_Dialog()\n"
                  "ev.Set_Reward_Type() ev.Clear_Dialog_Text()") == "");
    CHECK(vm.messages().size() == msgs);
    fs::remove_all(root, ec);
}

// q6 t14 (STORY.md §1.6, §6.2; GALACTIC_DIALOGS §2.8): Fire step 6 and the holocron list.
static void test_holocron() {
    StoryModel m;
    std::string err;
    const char* kHoloPlot =
        "<Story>"
        "<Event Name=\"A\"><Event_Type>STORY_TRIGGER</Event_Type><Story_Dialog>Dlg_A</Story_Dialog><Story_Chapter>2</Story_Chapter>"
        "<Perpetual>True</Perpetual></Event>"
        "<Event Name=\"B\"><Event_Type>STORY_TRIGGER</Event_Type><Story_Dialog>Dlg_B</Story_Dialog><Story_Chapter>0</Story_Chapter>"
        "<Story_Tag>Goal_B</Story_Tag><Story_Var>Kuat</Story_Var><Story_Dialog_SFX>True</Story_Dialog_SFX>"
        "<Story_Dialog_Popup>True</Story_Dialog_Popup></Event>"
        "<Event Name=\"TagOnly\"><Event_Type>STORY_TRIGGER</Event_Type><Story_Tag>T</Story_Tag></Event>"
        "<Event Name=\"Done\"><Event_Type>STORY_TRIGGER</Event_Type><Reward_Type>STORY_GOAL_COMPLETED</Reward_Type>"
        "<Reward_Param1>Dlg_A_2</Reward_Param1></Event>"
        "<Event Name=\"Gone\"><Event_Type>STORY_TRIGGER</Event_Type><Reward_Type>REMOVE_STORY_GOAL</Reward_Type>"
        "<Reward_Param1>Goal_B</Reward_Param1></Event>"
        "</Story>";
    CHECK(m.add_plot("Holo.xml", kHoloPlot, true, {}, &err) == 0);
    // The dialog scripts are asked for when the plot loads (0x4562a0), names as written.
    CHECK(m.dialog_loads() == (std::vector<std::string>{"Dlg_A", "Dlg_B"}));
    const StoryEventDef& b = m.plot(0).events[1];
    CHECK(b.story_var == "Kuat" && b.story_dialog_sfx && b.story_dialog_popup && !m.plot(0).events[0].story_dialog_sfx);
    std::vector<std::string> reward_hook;
    m.on_reward = [&](uint32_t, uint32_t, const std::string& type, const std::vector<StoryParam>&) { reward_hook.push_back(type); };
    const StoryHolocron& h = m.holocron();
    const uint64_t crc0 = digest(m);
    CHECK(m.trigger_event(0, 0));  // A: no Story_Tag -> "<dialog>_<chapter>", auto-tag
    CHECK(h.size() == 1 && h.entries()[0].tag == "Dlg_A_2" && h.entries()[0].auto_tag && h.entries()[0].chapter == 2 &&
          h.entries()[0].model == &m && h.entries()[0].plot == 0 && h.entries()[0].event == 0 && !h.entries()[0].completed);
    // The record references the event's Add_Dialog_Text list: a later edit shows in it.
    CHECK(h.lines(h.entries()[0]).empty());
    m.plot(0).events[0].dialog.push_back("Line 1");
    CHECK(h.lines(h.entries()[0]) == (std::vector<std::string>{"Line 1"}));
    CHECK(m.trigger_event(0, 0));  // fired again (Perpetual): a second record, newest first, no de-duplication
    CHECK(h.size() == 2 && h.entries()[0].id > h.entries()[1].id && h.entries()[0].tag == "Dlg_A_2");
    CHECK(m.trigger_event(0, 1));  // B: its own tag, Story_Var, popup + SFX in the presentation record
    CHECK(h.size() == 3 && h.entries()[0].tag == "Goal_B" && !h.entries()[0].auto_tag && h.entries()[0].var == "Kuat");
    CHECK(h.log().size() == 3 && h.log().back().popup && h.log().back().sfx && h.log().back().tag == "Goal_B" &&
          h.log().back().holocron_id == h.entries()[0].id && h.log().back().plot_name == "Holo.xml" && !h.log()[0].popup);
    CHECK(m.dialog_log().size() == 3);
    CHECK(m.trigger_event(0, 2) && h.size() == 3 && h.log().size() == 3);  // a Story_Tag without a Story_Dialog: nothing
    CHECK(m.trigger_event(0, 3));  // STORY_GOAL_COMPLETED Dlg_A_2: the NEWEST record with the tag
    CHECK(h.entries()[1].completed && !h.entries()[2].completed && !h.entries()[0].completed);
    CHECK(m.trigger_event(0, 4));  // REMOVE_STORY_GOAL Goal_B
    CHECK(h.size() == 2 && h.entries()[0].tag == "Dlg_A_2" && h.entries()[0].completed);
    CHECK(reward_hook.empty());  // both are the model's rewards, not the host's
    // [port] the list is not in the per-tick CRC (no sim rule reads it): the digest moves only with the fires.
    StoryModel twin;
    CHECK(twin.add_plot("Holo.xml", kHoloPlot, true, {}, &err) == 0);
    for (const uint32_t e : {0u, 0u, 1u, 2u, 3u, 4u}) twin.trigger_event(0, e);
    twin.plot(0).events[0].dialog.push_back("Line 1");
    CHECK(digest(twin) == digest(m) && digest(m) != crc0);
    m.holocron().mark_read(1);
    CHECK(h.entries()[1].read && !h.entries()[0].read && digest(twin) == digest(m));
    CHECK(!m.holocron().remove_tag("No_Such_Tag") && !m.holocron().complete_tag("dlg_a_2"));  // exact compare
    // One list for every plot set (share_holocron), records keep their own model.
    StoryModel other;
    other.share_holocron(m);
    CHECK(other.add_plot("Other.xml",
                         "<Story><Event Name=\"O\"><Event_Type>STORY_TRIGGER</Event_Type><Story_Dialog>Dlg_O</Story_Dialog>"
                         "<Story_Chapter>1</Story_Chapter><Story_Tag>Tag_O</Story_Tag></Event></Story>",
                         true, {}, &err) == 0);
    CHECK(other.trigger_event(0, 0) && h.size() == 3 && h.entries()[0].model == &other && h.entries()[0].tag == "Tag_O");
    other.plot(0).events[0].dialog = {"O1", "O2"};
    // Save / load: references by set number, re-linked after the read.
    ByteWriter w;
    h.write(w, [&](const StoryModel* p) { return p == &m ? 0 : p == &other ? 1 : -1; });
    StoryHolocron back;
    ByteReader r(w.bytes());
    CHECK(back.read(r) && r.ok() && r.done() && back.size() == 3);
    CHECK(back.lines(back.entries()[0]).empty());  // not linked yet
    back.relink(0, &m);
    back.relink(1, &other);
    for (size_t i = 0; i < 3; ++i) {
        const StoryHolocronEntry &x = h.entries()[i], &y = back.entries()[i];
        CHECK(x.id == y.id && x.tag == y.tag && x.dialog == y.dialog && x.chapter == y.chapter && x.completed == y.completed &&
              x.read == y.read && x.auto_tag == y.auto_tag && y.model == x.model && back.lines(y) == h.lines(x));
    }
    CHECK(back.add(StoryHolocronEntry{}) > h.entries()[0].id);  // ids continue after the load
    // A plot set going away: its records keep a copy of their lines (and save it).
    m.holocron().detach(&other);
    CHECK(h.entries()[0].model == nullptr && h.lines(h.entries()[0]) == (std::vector<std::string>{"O1", "O2"}));
    ByteWriter w2;
    h.write(w2, [&](const StoryModel* p) { return p == &m ? 0 : -1; });
    StoryHolocron back2;
    ByteReader r2(w2.bytes());
    CHECK(back2.read(r2) && back2.lines(back2.entries()[0]) == (std::vector<std::string>{"O1", "O2"}));
    ByteReader cut(std::span<const uint8_t>(w2.bytes().data(), w2.bytes().size() - 1));
    StoryHolocron back3;
    CHECK(!back3.read(cut) && back3.size() == 0);
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
    // Perpetual ELAPSED (q6 t03, STORY.md §4.7 [H]): the re-arm runs vf4, which clears the accumulator and the
    // initialised flag; the next T9 only re-records LastTime, so it fires again T seconds after that T9.
    m.update(4, 4.5);
    CHECK(fired_count(m, "Perp_Clock") == 0);
    m.update(5, 5.0);  // armed at 4.0 by B: 1 s -> fire
    const uint32_t perp = m.find_event(0, "Perp_Clock");
    CHECK(fired_count(m, "Perp_Clock") == 1 && m.plot(0).state[perp].armed && !m.plot(0).state[perp].timer_init &&
          m.plot(0).state[perp].timer_acc == 0.0);
    m.update(6, 5.0);  // re-initialised here (LastTime = 5.0), nothing added
    CHECK(m.plot(0).state[perp].timer_init && m.plot(0).state[perp].timer_last == 5.0);
    m.update(7, 5.9);
    CHECK(fired_count(m, "Perp_Clock") == 1);
    m.update(8, 6.0);  // 1 s after the re-initialising T9
    CHECK(fired_count(m, "Perp_Clock") == 2);
    m.update(9, 6.5);
    m.update(10, 7.5);  // re-initialised at 6.5: 1 s later
    CHECK(fired_count(m, "Perp_Clock") == 3);
    // Lua Reset / RESET_EVENT share vf4: a prereq-less ELAPSED restarts from its next evaluation.
    StoryModel r;
    CHECK(r.add_plot("Reset.xml",
                     "<Story><Event Name=\"Clock\"><Event_Type>STORY_ELAPSED</Event_Type><Event_Param1>2</Event_Param1></Event>"
                     "</Story>",
                     true, {}, &err) == 0);
    r.update(0, 0.0);
    r.update(1, 1.5);
    CHECK(r.plot(0).state[0].timer_acc == 1.5);
    r.reset_plot(0);
    CHECK(r.plot(0).state[0].timer_acc == 0.0 && !r.plot(0).state[0].timer_init && r.plot(0).state[0].armed);
    r.update(2, 2.0);  // re-initialised
    r.update(3, 3.5);
    CHECK(r.fired().empty());
    r.update(4, 4.0);
    CHECK(fired_count(r, "Clock") == 1);
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

// l1s3 t03: CONQUER_COUNT counts every dispatched conquest (no filter, any new owner); plot lookups match the name
// byte for byte (q6 t03, STORY.md §2.1 [H]); the story flag table is shared (STORY.md §0).
static void test_conquer_count_plots_flags() {
    StoryModel m;
    std::string err;
    CHECK(m.add_plot("Story_Count.xml",
                     "<Story><Event Name=\"Two_Planets\"><Event_Type>STORY_CONQUER_COUNT</Event_Type>"
                     "<Event_Param1>2</Event_Param1></Event>"
                     "<Event Name=\"Open\"><Event_Type>STORY_TRIGGER</Event_Type><Reward_Type>STORY_ELEMENT</Reward_Type>"
                     "<Reward_Param1>Story_Second</Reward_Param1></Event>"
                     "<Event Name=\"Win\"><Event_Type>STORY_TRIGGER</Event_Type><Reward_Type>SET_FLAG</Reward_Type>"
                     "<Reward_Param1>Mission_Win</Reward_Param1><Reward_Param2>1</Reward_Param2></Event></Story>",
                     true, {}, &err) == 0);
    CHECK(m.add_plot("Story_Second.xml", "<Story><Event Name=\"S\"><Event_Type>STORY_TRIGGER</Event_Type></Event></Story>",
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
    CHECK(m.find_plot("Story_Second.xml") == 1 && m.find_plot("STORY_SECOND.xml") == 0xFFFFFFFFu &&
          m.find_plot("Data\\XML\\Story_Second.xml") == 0xFFFFFFFFu);
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
    if (!note.empty()) uw2.story_event(note);
    CHECK(fired_names(uw2) == fired_names(uw) && digest(uw2) != 0);
    StoryModel uw3;
    CHECK(uw3.load_plots(*files, "Story_Plots_Campaign_Underworld.xml", &err));
    uw3.update(0, 0.0);
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

// q6 t03 (STORY.md §1.1, §3): Thrawn's Revenge's Thrawn_Empire plot names a prereq `Unlock_Wayland` that no event
// defines (the event is `Trigger_Unlock_Wayland`; the other plot of the set and the plot's Lua do not define it either).
// The name is dropped from its group with a warning, so the event is nobody's dependant and never arms, however many
// other events fire; a definition in another plot does not resolve it. Only a RESET_BRANCH re-check arms it (its now
// empty group counts as satisfied). Names that do exist resolve case-insensitively.
static const char kUnknownPrereq[] = R"(<Story>
  <Event Name="Obroa_Skai_Raid"><Event_Type>STORY_GENERIC</Event_Type><Event_Param1>raid</Event_Param1></Event>
  <Event Name="Trigger_Unlock_Wayland"><Event_Type>STORY_TRIGGER</Event_Type><Prereq>Obroa_Skai_Raid</Prereq></Event>
  <Event Name="Jump_Camera_Wayland"><Event_Type>STORY_TRIGGER</Event_Type><Branch>Wayland</Branch>
     <Prereq>Unlock_Wayland</Prereq></Event>
  <Event Name="Flash_Wayland"><Event_Type>STORY_TRIGGER</Event_Type><Prereq>Unlock_Wayland</Prereq></Event>
  <Event Name="Mixed_Case"><Event_Type>STORY_TRIGGER</Event_Type><Prereq>trigger_unlock_WAYLAND</Prereq></Event>
  <Event Name="Reset_W"><Event_Type>STORY_GENERIC</Event_Type><Event_Param1>reset</Event_Param1>
     <Reward_Type>RESET_BRANCH</Reward_Type><Reward_Param1>Wayland</Reward_Param1></Event>
</Story>)";

static void test_unknown_prereq() {
    StoryModel m;
    std::string err;
    CHECK(m.add_plot("Story_ThrawnCampaign_Empire.xml", kUnknownPrereq, true, {}, &err) == 0);
    CHECK(m.warnings().size() == 2);
    for (const std::string& w : m.warnings()) CHECK(w.find("has unknown prereq UNLOCK_WAYLAND") != std::string::npos);
    CHECK(m.add_plot("Other.xml",
                     "<Story><Event Name=\"Unlock_Wayland\"><Event_Type>STORY_GENERIC</Event_Type>"
                     "<Event_Param1>other</Event_Param1></Event></Story>",
                     true, {}, &err) == 1);
    CHECK(m.warnings().size() == 2);
    const uint32_t jump = m.find_event(0, "Jump_Camera_Wayland"), flash = m.find_event(0, "Flash_Wayland");
    CHECK(m.plot(0).events[jump].unresolved_prereq && m.plot(0).events[jump].prereqs.size() == 1 &&
          m.plot(0).events[jump].prereqs[0].empty());
    m.update(0, 0.0);
    const StoryParam other[] = {StoryParam::of_text("other")};
    CHECK(m.push_trigger("STORY_GENERIC", other) == 1 && fired_count(m, "Unlock_Wayland") == 1);  // plot 1 only
    const StoryParam raid[] = {StoryParam::of_text("raid")};
    CHECK(m.push_trigger("STORY_GENERIC", raid) == 3);  // Obroa_Skai_Raid, then Trigger_Unlock_Wayland and Mixed_Case
    CHECK(fired_count(m, "Trigger_Unlock_Wayland") == 1 && fired_count(m, "Mixed_Case") == 1);
    for (uint32_t f = 1; f <= 3; ++f) m.update(f, static_cast<double>(f));
    CHECK(fired_count(m, "Jump_Camera_Wayland") == 0 && fired_count(m, "Flash_Wayland") == 0);
    CHECK(!m.plot(0).state[jump].armed && !m.plot(0).state[flash].armed);
    // RESET_BRANCH Wayland: the re-check finds the empty group satisfied, arms Jump_Camera_Wayland and (TRIGGER) fires it.
    const StoryParam reset[] = {StoryParam::of_text("reset")};
    CHECK(m.push_trigger("STORY_GENERIC", reset) == 2);
    CHECK(fired_count(m, "Jump_Camera_Wayland") == 1 && fired_count(m, "Flash_Wayland") == 0);
}

// q6 t03: Thrawn's Revenge (workshop 1125571106, next to a Steam FoC install) plot sets load; their load warnings are
// printed. Thrawn_Empire's Empire set has the two `Unlock_Wayland` warnings (a data error in the mod, see above).
static void test_tr_campaign_plots() {
#ifdef ALAMO_LUA_HAVE_VFS
    const std::string foc = env("ALAMO_FOC_DATA");
    const size_t at = foc.find("\\steamapps\\common\\");
    const std::string tr = at == std::string::npos ? std::string() : foc.substr(0, at) + "\\steamapps\\workshop\\content\\32470\\1125571106";
    if (tr.empty() || !fs::exists(path_from_utf8(tr + "\\Data\\XML\\GameObjectFiles.xml"))) {
        std::printf("  Thrawn's Revenge not found next to ALAMO_FOC_DATA: TR plots skipped\n");
        return;
    }
    vfs::VfsConfig cfg;
    cfg.foc_root = foc;
    cfg.mod_paths = {tr};
    std::string err;
    std::unique_ptr<vfs::Vfs> files = vfs::Vfs::create(cfg, &err);
    CHECK(files != nullptr);
    if (!files) return;
    int32_t wayland = 0;
    for (const char* set : {"Conquests\\Historical\\9_ABY_ThrawnCampaign\\Story_Plots_ThrawnCampaign_Empire.xml",
                            "Conquests\\Historical\\9_ABY_ThrawnCampaign\\Story_Plots_ThrawnCampaign_Container.xml",
                            "Conquests\\Historical\\9_ABY_ThrawnCampaign\\Story_Plots_ThrawnCampaign_Rebel.xml",
                            "Conquests\\Historical\\10_ABY_OperationShadowHand\\Story_Plots_OperationShadowHand_Empire.xml",
                            "Conquests\\Historical\\10_ABY_OperationShadowHand\\Story_Plots_OperationShadowHand_Container.xml",
                            "Conquests\\Historical\\10_ABY_OperationShadowHand\\Story_Plots_OperationShadowHand_Rebel.xml"}) {
        StoryModel m;
        CHECK(m.load_plots(*files, set, &err));
        std::printf("  TR %s: %zu plots, %zu load warnings\n", set, m.plot_count(), m.warnings().size());
        for (const std::string& w : m.warnings()) {
            std::printf("    %s\n", w.c_str());
            wayland += w.find("unknown prereq UNLOCK_WAYLAND") != std::string::npos ? 1 : 0;
        }
    }
    CHECK(wayland == 2);
#endif
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    log::set_min_level(log::Level::Warn);
    test_synthetic();
    test_story_rules();
    test_plots_file();
    test_bindings();
    test_ai_flags();
    test_story_table();      // q6 t03
    test_dialog_setters();   // q6 t03
    test_holocron();         // q6 t14
    test_prereqs_elapsed_perpetual();
    test_exact_params();
    test_branches();
    test_unknown_prereq();  // q6 t03
    test_timeouts_and_pause();
    test_conquer_count_plots_flags();
    test_coverage();
    test_foc_campaigns();
    test_tr_campaign_plots();  // q6 t03
    if (g_fail) {
        std::printf("alamo_lua_story_tests: %d FAILED\n", g_fail);
        return 1;
    }
    std::printf("alamo_lua_story_tests: all passed\n");
    return 0;
}
