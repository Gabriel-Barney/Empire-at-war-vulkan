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
#include <string>
#include <string_view>
#include <vector>

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

}  // namespace alamo::game
