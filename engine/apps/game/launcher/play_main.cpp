// Play.exe (alamo_play): double-click launcher for the test build, so the game starts without a console window.
// Does what Play.cmd does: finds Forces of Corruption (play_launch.hpp find_game; when nothing is found it asks for the
// folder once with a folder dialog and keeps it in game_path.txt, the file Play.cmd / Play_Mod.cmd also read), starts
// alamo_game.exe next to it with --game and --cache-dir <here>\cache, sends its output to alamo_game_log.txt and waits.
// Only an error shows a window (a message box offering the log). Its own arguments are passed on, so a mod folder
// dropped onto Play.exe plays that mod (MODPATH=); mods are also picked in the game (main menu, Mods).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "alamo/vfs/steam_workshop.hpp"
#include "play_launch.hpp"

namespace {
namespace fs = std::filesystem;
using namespace alamo;

constexpr const wchar_t* kTitle = L"Alamo engine test build";

fs::path exe_dir() {
    std::wstring buf(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    buf.resize(n);
    return fs::path(buf).parent_path();
}

std::wstring env(const wchar_t* name) {
    std::wstring buf(32768, L'\0');
    const DWORD n = GetEnvironmentVariableW(name, buf.data(), static_cast<DWORD>(buf.size()));
    buf.resize(n < buf.size() ? n : 0);
    return buf;
}

fs::path from_utf8(const std::string& s) {
    std::u8string u8;
    for (const char c : s) u8.push_back(static_cast<char8_t>(c));
    return fs::path(u8);
}

std::vector<std::wstring> own_args() {
    std::vector<std::wstring> out;
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int i = 1; i < argc; ++i) out.emplace_back(argv[i]);
        LocalFree(argv);
    }
    return out;
}

// A folder from the Windows folder dialog; empty when cancelled.
fs::path pick_folder() {
    fs::path out;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return out;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(L"Forces of Corruption folder (\"corruption\" or \"Star Wars Empire at War\")");
    if (SUCCEEDED(dlg->Show(nullptr))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                out = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dlg->Release();
    return out;
}

// game_path.txt in launcher.ps1's form: UTF-8 with a BOM, one line.
void save_game_path(const fs::path& file, const fs::path& game) {
    const std::u8string u8 = game.u8string();
    FILE* f = nullptr;
    if (_wfopen_s(&f, file.c_str(), L"wb") != 0 || !f) return;
    std::fwrite("\xEF\xBB\xBF", 1, 3, f);
    std::fwrite(u8.data(), 1, u8.size(), f);
    std::fwrite("\r\n", 1, 2, f);
    std::fclose(f);
}

fs::path ask_for_game(const fs::path& saved) {
    MessageBoxW(nullptr,
                L"Forces of Corruption was not found.\n\nPick its \"corruption\" folder (or the \"Star Wars Empire at War\" "
                L"folder) in the next window. It is remembered in game_path.txt.",
                kTitle, MB_OK | MB_ICONINFORMATION);
    for (;;) {
        const fs::path picked = pick_folder();
        if (picked.empty()) return {};
        if (fs::path g = play::normalize_game_dir(picked.wstring()); !g.empty()) {
            save_game_path(saved, g);
            return g;
        }
        if (MessageBoxW(nullptr, (L"\"" + picked.wstring() + L"\" has no Forces of Corruption Data folder. Pick another folder?").c_str(),
                        kTitle, MB_RETRYCANCEL | MB_ICONWARNING) != IDRETRY)
            return {};
    }
}

int run() {
    const fs::path here = exe_dir();
    const fs::path game_exe = here / L"alamo_game.exe";
    std::error_code ec;
    if (!fs::exists(game_exe, ec)) {
        MessageBoxW(nullptr, (L"alamo_game.exe is missing next to Play.exe:\n" + here.wstring()).c_str(), kTitle, MB_OK | MB_ICONERROR);
        return 1;
    }

    play::FindInputs in;
    in.saved_file = here / L"game_path.txt";
    in.env = env(L"ALAMO_FOC_DATA");
    for (const std::string& lib : vfs::find_steam_libraries("", vfs::default_steam_roots())) in.steam.push_back(from_utf8(lib));
    in.fixed = {L"F:\\SteamLibrary\\steamapps\\common\\Star Wars Empire at War\\corruption",
                L"C:\\GOG Games\\Star Wars - Empire at War\\corruption",
                L"C:\\Program Files (x86)\\GOG Galaxy\\Games\\Star Wars - Empire at War\\corruption"};
    fs::path game = play::find_game(in);
    if (game.empty()) game = ask_for_game(in.saved_file);
    if (game.empty()) return 1;

    const std::vector<std::wstring> args = play::game_args(game, here / L"cache", own_args());
    std::wstring line = play::command_line(game_exe, args);
    const fs::path log = here / L"alamo_game_log.txt";
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    HANDLE log_h = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    if (log_h != INVALID_HANDLE_VALUE) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = log_h;
        si.hStdError = log_h;
        si.hStdInput = nullptr;
    }
    PROCESS_INFORMATION pi{};
    // CREATE_NO_WINDOW: alamo_game is a console program; its output goes to the log instead of a console window.
    const BOOL ok = CreateProcessW(game_exe.c_str(), line.data(), nullptr, nullptr, log_h != INVALID_HANDLE_VALUE, CREATE_NO_WINDOW,
                                   nullptr, here.c_str(), &si, &pi);
    const DWORD start_err = GetLastError();
    if (log_h != INVALID_HANDLE_VALUE) CloseHandle(log_h);
    if (!ok) {
        MessageBoxW(nullptr, (L"alamo_game.exe did not start (error " + std::to_wstring(start_err) + L").").c_str(), kTitle,
                    MB_OK | MB_ICONERROR);
        return 1;
    }
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    if (code != 0) {
        const std::wstring text = L"alamo_game exited with an error (code " + std::to_wstring(static_cast<int>(code)) +
                                  L").\n\nOpen alamo_game_log.txt? Please send it with your report.";
        if (MessageBoxW(nullptr, text.c_str(), kTitle, MB_YESNO | MB_ICONERROR) == IDYES)
            ShellExecuteW(nullptr, L"open", log.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return 1;
    }
    return 0;
}

}  // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const int rc = run();
    if (SUCCEEDED(co)) CoUninitialize();
    return rc;
}
