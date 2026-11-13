// alamo_play_tests (data-free): Play.exe's testable half (launcher/play_launch.hpp) on a fake install in its own temp
// folder (%TEMP%\alamo_play_tests, no other test uses it):
//   * is_game_dir / normalize_game_dir: the "corruption" folder, the folder above it, quotes and trailing separators,
//     a folder without Data refused;
//   * first_line_utf8: BOM, CRLF, non-ASCII path text;
//   * find_game: game_path.txt first, then ALAMO_FOC_DATA, then the Steam libraries, then the fixed folders;
//   * game_args: --game / --cache-dir first, a mod folder (or its Data folder) becomes MODPATH=, other arguments kept;
//   * quote_arg / command_line: the CommandLineToArgvW rules (spaces, quotes, trailing backslashes).
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "alamo/test/support.hpp"
#include "play_launch.hpp"

namespace fs = std::filesystem;
using namespace alamo::play;
using ::alamo::test::g_fail;

namespace {

void touch(const fs::path& p, const std::string& text = "x") {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

void make_game(const fs::path& corruption) { touch(corruption / L"Data" / L"MegaFiles.xml", "<Mega_Files/>"); }

void test_game_dirs(const fs::path& root) {
    const fs::path swe = root / L"steam" / L"steamapps" / L"common" / L"Star Wars Empire at War";
    const fs::path game = swe / L"corruption";
    make_game(game);
    CHECK(is_game_dir(game));
    CHECK(!is_game_dir(swe));
    CHECK(normalize_game_dir(game.wstring()) == game);
    CHECK(normalize_game_dir(swe.wstring()) == game);
    CHECK(normalize_game_dir(L"  \"" + game.wstring() + L"\\\"  ") == game);
    CHECK(normalize_game_dir((root / L"nothing").wstring()).empty());
    CHECK(normalize_game_dir(L"").empty());
    // StarWarsG.exe instead of MegaFiles.xml (launcher.ps1 Test-Game accepts either)
    const fs::path gog = root / L"gog" / L"corruption";
    fs::create_directories(gog / L"Data");
    touch(gog / L"StarWarsG.exe");
    CHECK(is_game_dir(gog));
}

void test_first_line() {
    CHECK(first_line_utf8("\xEF\xBB\xBF" "C:\\Games\\corruption\r\nmore") == L"C:\\Games\\corruption");
    CHECK(first_line_utf8("D:\\x\n") == L"D:\\x");
    CHECK(first_line_utf8("") == L"");
    CHECK(first_line_utf8("C:\\Spiele\\\xC3\xBC") == L"C:\\Spiele\\\u00FC");  // UTF-8 u-umlaut
}

void test_find(const fs::path& root) {
    const fs::path steam_game = root / L"steam" / L"steamapps" / L"common" / L"Star Wars Empire at War" / L"corruption";
    const fs::path env_game = root / L"env" / L"corruption";
    make_game(env_game);
    const fs::path saved_game = root / L"saved" / L"corruption";
    make_game(saved_game);
    const fs::path gog = root / L"gog" / L"corruption";

    FindInputs in;
    in.fixed = {gog};
    CHECK(find_game(in) == gog);
    in.steam = {root / L"empty_lib", root / L"steam"};
    CHECK(find_game(in) == steam_game);
    in.env = env_game.parent_path().wstring();  // the folder above corruption is fine too
    CHECK(find_game(in) == env_game);
    in.saved_file = root / L"game_path.txt";
    touch(in.saved_file, "\xEF\xBB\xBF" + saved_game.string() + "\r\n");
    CHECK(find_game(in) == saved_game);
    touch(in.saved_file, "C:\\no\\such\\folder\r\n");  // a stale game_path.txt: the search goes on
    CHECK(find_game(in) == env_game);
    CHECK(find_game(FindInputs{}).empty());
}

void test_args(const fs::path& root) {
    const fs::path game = root / L"g";
    const fs::path cache = root / L"cache";
    const fs::path mod = root / L"My Mod";
    fs::create_directories(mod / L"Data");
    const fs::path not_mod = root / L"plain";
    fs::create_directories(not_mod);

    const std::vector<std::wstring> none = game_args(game, cache, {});
    CHECK(none == (std::vector<std::wstring>{L"--game", game.wstring(), L"--cache-dir", cache.wstring()}));
    const std::wstring modpath = L"MODPATH=" + mod.lexically_normal().wstring();  // %TEMP% may hold '/' separators
    const std::vector<std::wstring> a = game_args(game, cache, {mod.wstring(), (mod / L"Data").wstring(), mod.wstring() + L"\\",
                                                                not_mod.wstring(), L"STEAMMOD=1125571106", L"--windowed"});
    CHECK(a.size() == 10);
    if (a.size() == 10) {
        CHECK(a[4] == modpath);
        CHECK(a[5] == modpath);
        CHECK(a[6] == modpath);
        CHECK(a[7] == not_mod.wstring());
        CHECK(a[8] == L"STEAMMOD=1125571106");
        CHECK(a[9] == L"--windowed");
    }
}

void test_quote() {
    CHECK(quote_arg(L"plain") == L"plain");
    CHECK(quote_arg(L"") == L"\"\"");
    CHECK(quote_arg(L"a b") == L"\"a b\"");
    CHECK(quote_arg(L"C:\\a b\\") == L"\"C:\\a b\\\\\"");
    CHECK(quote_arg(L"say \"hi\"") == L"\"say \\\"hi\\\"\"");
    CHECK(quote_arg(L"a\\\\\"b") == L"\"a\\\\\\\\\\\"b\"");
    CHECK(quote_arg(L"C:\\x\\y") == L"C:\\x\\y");  // backslashes without a quote stay as they are
    CHECK(command_line(L"C:\\a b\\alamo_game.exe", {L"--game", L"C:\\g h"}) == L"\"C:\\a b\\alamo_game.exe\" --game \"C:\\g h\"");
}

}  // namespace

int main() {
    std::error_code ec;
    const fs::path root = fs::temp_directory_path(ec) / L"alamo_play_tests";
    fs::remove_all(root, ec);
    fs::create_directories(root);
    test_game_dirs(root);
    test_first_line();
    test_find(root);
    test_args(root);
    test_quote();
    fs::remove_all(root, ec);
    return alamo::test::report("alamo_play_tests");
}
