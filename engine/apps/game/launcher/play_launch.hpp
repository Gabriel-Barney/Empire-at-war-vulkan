// Play.exe (alamo_play): the test build's double-click launcher, the window-free twin of Play.cmd. This half is the
// part the tests drive (alamo_play_tests); play_main.cpp adds the Windows side (folder dialog, process, message boxes).
//   find_game      the Forces of Corruption "corruption" folder, in Play.cmd's order (launcher.ps1 Find-Game): the
//                  saved game_path.txt, ALAMO_FOC_DATA, the Steam libraries, a few fixed install folders
//   game_args      alamo_game's arguments: --game, --cache-dir, then the user's own (a dropped mod folder becomes
//                  MODPATH=<folder>, as Play_Mod.cmd does for a dragged folder)
//   command_line   one CreateProcessW command line (CommandLineToArgvW quoting)
// Launcher only: nothing here is read by the engine or the sim.
#pragma once
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace alamo::play {

// A Forces of Corruption folder: has Data, and Data\MegaFiles.xml or StarWarsG.exe (launcher.ps1 Test-Game).
bool is_game_dir(const std::filesystem::path& dir);

// `text` as a game folder: the "corruption" folder itself, or the "Star Wars Empire at War" folder above it. Quotes,
// spaces and trailing separators are dropped. Empty when it is neither.
std::filesystem::path normalize_game_dir(std::wstring text);

// The first line of a UTF-8 text file's bytes (a BOM dropped), as a path text. game_path.txt is written by
// launcher.ps1 (Set-Content -Encoding UTF8, with a BOM) or by Play.exe (the same form).
std::wstring first_line_utf8(std::string_view bytes);

struct FindInputs {
    std::filesystem::path saved_file;            // game_path.txt next to the exe (may not exist)
    std::wstring env;                            // ALAMO_FOC_DATA (may be empty)
    std::vector<std::filesystem::path> steam;    // Steam library folders, in search order
    std::vector<std::filesystem::path> fixed;    // last resort install folders (GOG)
};

// The game folder, or empty when none of the inputs names one.
std::filesystem::path find_game(const FindInputs& in);

// --game <game> --cache-dir <cache>, then `user` in order; an argument that is a folder holding a Data folder (or a
// Data folder itself) becomes MODPATH=<that folder, absolute>.
std::vector<std::wstring> game_args(const std::filesystem::path& game, const std::filesystem::path& cache,
                                    const std::vector<std::wstring>& user);

// One argument quoted the way CommandLineToArgvW / the C runtime split it back.
std::wstring quote_arg(const std::wstring& arg);

// "<exe>" arg arg ...
std::wstring command_line(const std::filesystem::path& exe, const std::vector<std::wstring>& args);

}  // namespace alamo::play
