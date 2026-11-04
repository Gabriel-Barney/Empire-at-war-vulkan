// P host: story dialog scripts (Data\Scripts\Story\<Story_Dialog>.txt, one [CHAPTER n] section per Story_Chapter). Plain
// parse, no text lookup, no UI: the game host turns the result into the story dialog (game_info.cpp). Format and keyword
// table: docs/re/GALACTIC_DIALOGS.md §2.6 (parser 0x47eac0, table 0xa2a1d0).
//
// The file is one token stream (delimiters space, tab, CR, LF and ']'; line breaks carry no meaning, so an argument may
// sit on the next line). A token starting with '#' skips the rest of its line; ';' and '//' are not comments. Keywords
// are case-insensitive, arguments keep their case. An unknown keyword rejects the whole file (the original loader
// caches nothing, so the dialog has no chapters): parse_story_script returns false. Commands before the first
// [CHAPTER n] are dropped. Integer arguments are atoi (0 when not a number).
//
//   [CHAPTER n]              starts chapter atoi(n)
//   TITLE <text id>          the dialog title
//   TEXT <text id>           one text paragraph, in the current colour, shown at the current time
//   EXTRA_TEXT               where the story event's extra text (Lua Add_Dialog_Text) goes
//   NEWLINE <n>              n blank lines
//   TEXTCOLOR <r g b a>      colour of the following text; a = 255 when the file ends first
//   WAIT <ms>                the following commands come <ms> later (wall-clock ms in the original; port: seconds of
//                            presentation time, the same thing)
//   SFX <event>              an SFX event at the current time
//   DIALOG <event>           a speech event at the current time
//   MOVIE <name>             looped movie; MOVIE_ONCE <name> one-shot movie
//   IMAGE <texture>          the dialog's picture
//   PAUSE <n>                parsed (counted in `pauses`) and without effect, see story_script.cpp
//   MUSIC <event>, ANIM <n>, CLEARTEXT, LOADCINEMATIC <name>, CLEARIMAGE, HIDECINEMATIC <n>, STOPMOVIE, WAIT_SPEECH
//                            recognised and skipped: no shipped script uses them (§2.6 table)
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace alamo {
class FileProvider;
}

namespace alamo::game {

struct StoryScriptLine {
    enum class Kind : uint8_t { Text, Blank, Extra } kind = Kind::Text;
    std::string text_id;              // Kind::Text
    uint8_t rgba[4] = {0, 0, 0, 0};   // alpha 0 = no TEXTCOLOR yet (the skin's colours)
    double at = 0.0;                  // seconds after the dialog opened
};
struct StoryScriptCue {
    enum class Kind : uint8_t { Sfx, Speech } kind = Kind::Sfx;
    std::string event;
    double at = 0.0;
};
struct StoryScript {
    std::string title;                // text id
    std::vector<StoryScriptLine> lines;
    std::vector<StoryScriptCue> cues; // script order (ascending `at`)
    std::string movie;                // "" = none
    bool movie_loop = true;
    std::string image;
    uint32_t pauses = 0;
};

// Parses section [CHAPTER <chapter>] (negative = 0) of a story dialog script. False when the section is missing or the
// file holds an unknown keyword.
bool parse_story_script(std::string_view file_text, int32_t chapter, StoryScript& out);
// False when the file holds an unknown keyword ("Story script error!  Unrecognized keyword": the whole file is rejected).
bool story_script_valid(std::string_view file_text);

// GALACTIC_DIALOGS §2.8 (0x47d9c0): the loaded story dialog scripts, keyed by the name AS GIVEN (the original's cache map is
// keyed by the raw CRC of the name, so it is case-sensitive although the file system is not). Loads happen when a plot
// loads (every event's Story_Dialog), at Lua Set_Dialog and after a save load (lua::StoryModel::dialog_loads). A file that
// is missing or rejected is not cached, so its dialog has no chapters (a later load of the name tries again).
class StoryDialogCache {
public:
    // Reads Data\Scripts\Story\<name>.txt unless `name` is cached. True when it was loaded now.
    bool load(const FileProvider& files, std::string_view name);
    bool cached(std::string_view name) const { return files_.count(std::string(name)) != 0; }
    // Section [CHAPTER chapter] of a cached script; false when the name is not cached or the chapter is missing.
    bool chapter(std::string_view name, int32_t chapter, StoryScript& out) const;
    size_t size() const { return files_.size(); }
    void clear() { files_.clear(); }

private:
    std::map<std::string, std::string> files_;  // exact name -> file text (valid scripts only)
};

// ---- the holocron summary dialog's fill (GALACTIC_DIALOGS §2.8: goal list 0x107f00, executor 0x1089e0) ----
// One holocron record as the summary reads it (lua::StoryHolocronEntry without the Lua dependency): `lines` = its event's
// Add_Dialog_Text list read through the record's reference now (so later Add / Clear calls show).
struct HolocronRecord {
    uint64_t id = 0;
    std::string dialog, var, tag;
    int32_t chapter = -1;
    bool completed = false, read = false;
    std::vector<std::string> lines;
};
// A goal list row: label = the chapter's FIRST `TEXT` key (not TITLE; looked up add-if-missing by the caller), colour = the
// TEXTCOLOR before it (default 128 128 255 255), x 0.66 (rgb) once the record is read or completed.
struct HolocronRow {
    size_t record = 0;    // index into the records (list order, newest first)
    std::string text_id;  // "" = the chapter has no TEXT (or the script / chapter is missing)
    uint8_t rgba[4] = {128, 128, 255, 255};
};
// The executor's output for the selected record, in script order: Text = a text id (the caller looks it up and, when the
// record's Story_Var is not empty, prints it with Story_Var as the argument: story_var_printf); Blank = NEWLINE; Extra = one
// EXTRA_TEXT line as stored (Add_Dialog_Text already looked it up and substituted: no second lookup). `rgba` alpha 0 = no
// TEXTCOLOR yet (the skin's colours).
struct HolocronRecapLine {
    StoryScriptLine::Kind kind = StoryScriptLine::Kind::Text;
    std::string text;
    uint8_t rgba[4] = {0, 0, 0, 0};
};
struct HolocronSummaryFill {
    std::vector<HolocronRow> rows;  // the tab's records: Active = completed == false, Completed = completed == true
    bool has_completed = false;     // [port] the Completed tab is enabled while some record is completed (§2.2; the
                                    // original's campaign byte 0x140b305e8 is [L])
    int32_t selected = -1;          // row: the record with `selected_id`, else row 0 (-1 = no rows)
    std::string title_id;           // the selected chapter's TITLE ("" = none)
    std::vector<HolocronRecapLine> recap;
};
HolocronSummaryFill fill_holocron_summary(const StoryDialogCache& cache, const std::vector<HolocronRecord>& records, bool completed_tab,
                                          uint64_t selected_id);
// The TEXT command with a Story_Var (0x1089e0: swprintf(text, Story_Var)): the first %s / %S / %ls takes `var`, later ones
// print nothing, "%%" is "%"; other conversions stay as written [port: the original's wide printf with a narrow argument is
// [M]; no shipped event sets Story_Var].
std::u32string story_var_printf(std::u32string_view format, std::u32string_view var);

}  // namespace alamo::game
