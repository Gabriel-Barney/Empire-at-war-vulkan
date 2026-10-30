// s10reader: read-only snapshot recorder for the S10 map-owner probe (FoC Steam x64 1.121).
//
// Attaches to StarWarsG.exe with PROCESS_VM_READ | PROCESS_QUERY_INFORMATION | SYNCHRONIZE and, for every tactical
// mode it sees (land by default), dumps every object of the mode's synchronised store (id, owner player id, type
// name, position, health, flags) at mode frames >= 1, 300 and 1200 (about 0 s, 10 s, 40 s). It also records the text
// of every message box (#32770 dialog) the game process opens - the hook's _MessagePopup channel - and, with
// --dismiss-popups, presses its OK button. It never writes the game's memory, never injects, never suspends.
// It ends when the game exits (or --timeout-sec).
//
//   s10reader --out <log> [--status <json>] [--wait-process-sec 900] [--timeout-sec 14400] [--modes 1]
//             [--frames 1,300,1200] [--dismiss-popups] [--allow-unknown-exe] [--pid N --test-base 0xADDR]
//
// Log format: tools/probe_mod/s10_map_owners/README.md "Reader log".
// Build: tools/probe_mod/s10_map_owners/build_reader.cmd (project-local MSVC, /W4 /WX).
#include <windows.h>
#include <tlhelp32.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "s10_layout.hpp"

namespace {

double now_s() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

template <class T>
T get(const std::vector<unsigned char>& b, size_t off) {
    T v{};
    if (off + sizeof(T) <= b.size()) std::memcpy(&v, b.data() + off, sizeof(T));
    return v;
}

std::string clean(std::string s) {
    for (char& c : s)
        if (static_cast<unsigned char>(c) <= 0x20 || static_cast<unsigned char>(c) >= 0x7F || c == '|') c = '_';
    return s;
}

struct Options {
    std::string out, status, frames = "1,300,1200", modes = "1";
    double wait_process = 900, timeout = 14400;
    bool dismiss = false, allow_unknown = false;
    DWORD pid = 0;
    uint64_t test_base = 0;
};

struct Game {
    HANDLE h = nullptr;
    DWORD pid = 0;
    uint64_t base = 0;
    uint32_t timestamp = 0, size_of_image = 0;
    bool rd(uint64_t addr, void* dst, size_t n) const {
        SIZE_T got = 0;
        if (addr == 0) return false;
        if (!ReadProcessMemory(h, reinterpret_cast<LPCVOID>(addr), dst, n, &got)) return false;
        return got == n;
    }
    template <class T>
    bool rdv(uint64_t addr, T& v) const { return rd(addr, &v, sizeof(T)); }
    bool block(uint64_t addr, size_t n, std::vector<unsigned char>& b) const {
        b.assign(n, 0);
        return rd(addr, b.data(), n);
    }
    bool alive() const { return WaitForSingleObject(h, 0) == WAIT_TIMEOUT; }
};

bool module_of(DWORD pid, uint64_t& base) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32 me{};
    me.dwSize = sizeof me;
    const bool ok = Module32First(snap, &me) != FALSE;
    if (ok) base = reinterpret_cast<uint64_t>(me.modBaseAddr);
    CloseHandle(snap);
    return ok;
}

std::vector<DWORD> find_pids(const char* exe) {
    std::vector<DWORD> r;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return r;
    PROCESSENTRY32 pe{};
    pe.dwSize = sizeof pe;
    for (BOOL ok = Process32First(snap, &pe); ok; ok = Process32Next(snap, &pe))
        if (_stricmp(pe.szExeFile, exe) == 0) r.push_back(pe.th32ProcessID);
    CloseHandle(snap);
    return r;
}

bool read_pe_identity(Game& g) {
    uint32_t lfanew = 0, sig = 0;
    if (!g.rdv(g.base + 0x3C, lfanew)) return false;
    if (!g.rdv(g.base + lfanew, sig) || sig != 0x00004550u) return false;
    if (!g.rdv(g.base + lfanew + 8, g.timestamp)) return false;
    return g.rdv(g.base + lfanew + 24 + 56, g.size_of_image);
}

std::vector<uint32_t> parse_list(const std::string& s) {
    std::vector<uint32_t> v;
    size_t i = 0;
    while (i < s.size()) {
        size_t j = s.find(',', i);
        if (j == std::string::npos) j = s.size();
        if (j > i) v.push_back(static_cast<uint32_t>(std::stoul(s.substr(i, j - i))));
        i = j + 1;
    }
    return v;
}

// ---- message boxes -------------------------------------------------------------------------------------------------
struct PopupScan {
    DWORD pid = 0;
    std::vector<HWND> found;
};

BOOL CALLBACK enum_top(HWND w, LPARAM lp) {
    auto* s = reinterpret_cast<PopupScan*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid != s->pid || !IsWindowVisible(w)) return TRUE;
    char cls[64] = {};
    GetClassNameA(w, cls, sizeof cls);
    if (std::strcmp(cls, "#32770") == 0) s->found.push_back(w);
    return TRUE;
}

BOOL CALLBACK enum_child(HWND w, LPARAM lp) {
    auto* text = reinterpret_cast<std::string*>(lp);
    char buf[4096] = {};
    GetWindowTextA(w, buf, sizeof buf);
    if (buf[0]) {
        if (!text->empty()) *text += " | ";
        *text += buf;
    }
    return TRUE;
}

class Reader {
public:
    Reader(const Options& o, Game& g) : o_(o), g_(g), frames_(parse_list(o.frames)) {
        for (uint32_t m : parse_list(o.modes)) modes_.insert(static_cast<int32_t>(m));
    }
    int run();

private:
    const Options& o_;
    Game& g_;
    std::vector<uint32_t> frames_;
    std::set<int32_t> modes_;
    FILE* f_ = nullptr;
    std::unordered_map<uint64_t, std::string> type_names_;
    std::set<HWND> popups_seen_;
    uint64_t cur_mode_ = 0;
    int battle_ = 0, dumps_ = 0, popups_ = 0, failed_reads_ = 0;
    std::vector<bool> done_;
    std::string state_ = "waiting", reason_;
    double t0_ = 0, last_status_ = 0;

    const std::string& type_name(uint64_t tp);
    bool dump(uint64_t mode, uint32_t mode_frame, uint32_t global_frame, const std::string& tag);
    void scan_popups();
    void status(bool final_);
};

const std::string& Reader::type_name(uint64_t tp) {
    auto it = type_names_.find(tp);
    if (it != type_names_.end()) return it->second;
    std::string name = "?";
    std::vector<unsigned char> b;
    if (tp && g_.block(tp + s10::kTypeName, 0x20, b)) {
        const auto size = get<uint64_t>(b, 0x10), cap = get<uint64_t>(b, 0x18);
        if (size > 0 && size < 256) {
            std::string s(static_cast<size_t>(size), '\0');
            if (cap > 15) {
                if (!g_.rd(get<uint64_t>(b, 0), s.data(), s.size())) s = "?";
            } else if (size <= 15) {
                std::memcpy(s.data(), b.data(), s.size());
            }
            name = clean(s);
        }
    }
    return type_names_[tp] = name;
}

bool Reader::dump(uint64_t mode, uint32_t mode_frame, uint32_t global_frame, const std::string& tag) {
    uint64_t gom = 0, head = 0, size = 0, node = 0;
    if (!g_.rdv(mode + s10::kModePrimary, gom) || !g_.rdv(gom + s10::kGomIdMapHead, head) ||
        !g_.rdv(gom + s10::kGomIdMapSize, size) || size > 1000000 || !g_.rdv(head + s10::kNodeNext, node))
        return false;
    std::vector<std::string> lines;
    std::vector<unsigned char> nb, ob;
    uint64_t matched = 0;
    for (uint64_t i = 0; i < size && node != head && node != 0; ++i) {
        if (!g_.block(node, s10::kNodeBlock, nb)) return false;
        const int32_t key = get<int32_t>(nb, s10::kNodeKey);
        const uint64_t obj = get<uint64_t>(nb, s10::kNodeValue);
        node = get<uint64_t>(nb, s10::kNodeNext);
        if (!g_.block(obj + s10::kObjReadFrom, s10::kObjReadTo - s10::kObjReadFrom, ob)) continue;
        auto at = [](uint32_t off) { return static_cast<size_t>(off - s10::kObjReadFrom); };
        const int32_t id = get<int32_t>(ob, at(s10::kObjId));
        if (id == key) ++matched;
        char buf[512];
        std::snprintf(buf, sizeof buf, "O id=%d owner=%d type=%s pos=%.2f,%.2f,%.2f health=%.2f flags=%u parent_mode=%d",
                      id, get<int32_t>(ob, at(s10::kObjOwner)), type_name(get<uint64_t>(ob, at(s10::kObjType))).c_str(),
                      static_cast<double>(get<float>(ob, at(s10::kObjPos))),
                      static_cast<double>(get<float>(ob, at(s10::kObjPos) + 4)),
                      static_cast<double>(get<float>(ob, at(s10::kObjPos) + 8)),
                      static_cast<double>(get<float>(ob, at(s10::kObjHealth))),
                      static_cast<unsigned>(get<uint8_t>(ob, at(s10::kObjFlags))),
                      get<int32_t>(ob, at(s10::kObjParentModeId)));
        lines.emplace_back(buf);
    }
    if (lines.empty() || matched * 10 < lines.size() * 9) return false;  // mid-update or wrong layout: retry later
    std::fprintf(f_, "DUMP battle=%d tag=%s mode_frame=%u global_frame=%u count=%zu t=%.1f\n", battle_, tag.c_str(),
                 mode_frame, global_frame, lines.size(), now_s() - t0_);
    for (const auto& l : lines) std::fprintf(f_, "%s\n", l.c_str());
    std::fprintf(f_, "END battle=%d tag=%s count=%zu\n", battle_, tag.c_str(), lines.size());
    std::fflush(f_);
    ++dumps_;
    return true;
}

void Reader::scan_popups() {
    PopupScan s;
    s.pid = g_.pid;
    EnumWindows(enum_top, reinterpret_cast<LPARAM>(&s));
    for (HWND w : s.found) {
        if (popups_seen_.count(w)) continue;
        popups_seen_.insert(w);
        char title[512] = {};
        GetWindowTextA(w, title, sizeof title);
        std::string text;
        EnumChildWindows(w, enum_child, reinterpret_cast<LPARAM>(&text));
        std::fprintf(f_, "POPUP battle=%d t=%.1f title=%s text=%s\n", battle_, now_s() - t0_, title, text.c_str());
        std::fflush(f_);
        ++popups_;
        if (o_.dismiss) PostMessageA(w, WM_COMMAND, IDOK, 0);
    }
}

void Reader::status(bool final_) {
    if (o_.status.empty()) return;
    const std::string tmp = o_.status + ".tmp";
    FILE* s = std::fopen(tmp.c_str(), "wb");
    if (!s) return;
    std::fprintf(s, "{\n  \"tool\": \"s10reader\",\n  \"state\": \"%s\",\n  \"final\": %s,\n  \"reason\": \"%s\",\n"
                    "  \"pid\": %lu,\n  \"battles\": %d,\n  \"dumps\": %d,\n  \"popups\": %d,\n  \"failed_reads\": %d,\n"
                    "  \"wall_seconds\": %.1f\n}\n",
                 state_.c_str(), final_ ? "true" : "false", reason_.c_str(), static_cast<unsigned long>(g_.pid), battle_,
                 dumps_, popups_, failed_reads_, now_s() - t0_);
    std::fclose(s);
    MoveFileExA(tmp.c_str(), o_.status.c_str(), MOVEFILE_REPLACE_EXISTING);
}

int Reader::run() {
    t0_ = now_s();
    f_ = std::fopen(o_.out.c_str(), "ab");
    if (!f_) return 1;
    std::fprintf(f_, "S10READER 1\nGAME pid=%lu base=0x%llx timestamp=0x%08x size_of_image=0x%08x\n",
                 static_cast<unsigned long>(g_.pid), static_cast<unsigned long long>(g_.base), g_.timestamp,
                 g_.size_of_image);
    std::fflush(f_);
    const uint64_t gmm = g_.base + s10::kRvaGmm, fs = g_.base + s10::kRvaFrameSync;
    std::vector<unsigned char> gb, fb;
    double last_popup = 0;
    state_ = "watching";
    for (;;) {
        if (!g_.alive()) {
            reason_ = "game_exited";
            break;
        }
        if (now_s() - t0_ > o_.timeout) {
            reason_ = "timeout";
            break;
        }
        if (now_s() - last_popup > 0.25) {
            scan_popups();
            last_popup = now_s();
        }
        if (now_s() - last_status_ > 2.0) {
            status(false);
            last_status_ = now_s();
        }
        if (!g_.block(gmm, s10::kGmmBlock, gb) || !g_.block(fs, s10::kFsBlock, fb)) {
            Sleep(50);
            continue;
        }
        const int32_t stack = get<int32_t>(gb, s10::kGmmStackCount), mtype = get<int32_t>(gb, s10::kGmmModeType);
        const uint64_t mode = get<uint64_t>(gb, s10::kGmmCurMode);
        const bool stopped = get<uint8_t>(fb, s10::kFsSimStopped) != 0;
        if (mode != cur_mode_) {
            if (cur_mode_ != 0 && !done_.empty()) std::fprintf(f_, "MODE_END battle=%d\n", battle_);
            cur_mode_ = mode;
            done_.clear();
            if (mode != 0 && stack >= 1 && modes_.count(mtype)) {
                ++battle_;
                done_.assign(frames_.size(), false);
                std::fprintf(f_, "MODE battle=%d mode_ptr=0x%llx type=%d stack=%d game_type=%d global_frame=%u t=%.1f\n",
                             battle_, static_cast<unsigned long long>(mode), mtype, stack,
                             get<int32_t>(gb, s10::kGmmGameType), get<uint32_t>(fb, s10::kFsFrame), now_s() - t0_);
            } else {
                std::fprintf(f_, "SEEN mode_ptr=0x%llx type=%d stack=%d t=%.1f\n", static_cast<unsigned long long>(mode),
                             mtype, stack, now_s() - t0_);
            }
            std::fflush(f_);
        }
        if (!done_.empty() && !stopped) {
            uint32_t mframe = 0;
            g_.rdv(mode + s10::kModeFrame, mframe);
            for (size_t i = 0; i < frames_.size(); ++i) {
                if (done_[i] || mframe < frames_[i]) continue;
                uint8_t in_tick = 1;
                for (int spin = 0; spin < 200 && in_tick; ++spin) {
                    g_.rdv(fs + s10::kFsInTick, in_tick);
                    if (in_tick) Sleep(0);
                }
                if (dump(mode, mframe, get<uint32_t>(fb, s10::kFsFrame), "f" + std::to_string(frames_[i]))) {
                    done_[i] = true;
                    for (size_t j = i + 1; j < frames_.size(); ++j)
                        if (frames_[j] <= mframe) done_[j] = true;  // late start: one dump covers the passed marks
                } else {
                    ++failed_reads_;
                }
                break;
            }
        }
        Sleep(20);
    }
    std::fprintf(f_, "EXIT reason=%s battles=%d dumps=%d popups=%d failed_reads=%d\n", reason_.c_str(), battle_, dumps_,
                 popups_, failed_reads_);
    std::fclose(f_);
    state_ = "done";
    status(true);
    return dumps_ > 0 ? 0 : 10;
}

bool parse_args(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](std::string& v) {
            if (i + 1 >= argc) return false;
            v = argv[++i];
            return true;
        };
        std::string v;
        if (a == "--out") { if (!next(o.out)) return false; }
        else if (a == "--status") { if (!next(o.status)) return false; }
        else if (a == "--frames") { if (!next(o.frames)) return false; }
        else if (a == "--modes") { if (!next(o.modes)) return false; }
        else if (a == "--wait-process-sec") { if (!next(v)) return false; o.wait_process = std::stod(v); }
        else if (a == "--timeout-sec") { if (!next(v)) return false; o.timeout = std::stod(v); }
        else if (a == "--dismiss-popups") o.dismiss = true;
        else if (a == "--allow-unknown-exe") o.allow_unknown = true;
        else if (a == "--pid") { if (!next(v)) return false; o.pid = static_cast<DWORD>(std::stoul(v)); }
        else if (a == "--test-base") { if (!next(v)) return false; o.test_base = std::stoull(v, nullptr, 16); }
        else return false;
    }
    return !o.out.empty();
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    if (!parse_args(argc, argv, o)) {
        std::fprintf(stderr, "usage: s10reader --out <log> [--status <json>] [--wait-process-sec N] [--timeout-sec N]\n"
                             "       [--modes 1] [--frames 1,300,1200] [--dismiss-popups] [--allow-unknown-exe]\n"
                             "       [--pid N --test-base 0xADDR]\n");
        return 1;
    }
    Game g;
    const double t0 = now_s();
    std::set<DWORD> ignored;
    std::fprintf(stdout, "waiting for StarWarsG.exe (FoC 1.121) up to %.0f s\n", o.wait_process);
    std::fflush(stdout);
    for (;;) {
        std::vector<DWORD> pids;
        if (o.pid) pids.push_back(o.pid);
        else pids = find_pids("StarWarsG.exe");
        for (DWORD pid : pids) {
            HANDLE h = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION | SYNCHRONIZE, FALSE, pid);
            if (!h) continue;
            Game c;
            c.h = h;
            c.pid = pid;
            if (o.test_base) {
                c.base = o.test_base;
            } else if (!module_of(pid, c.base) || !read_pe_identity(c)) {
                CloseHandle(h);
                continue;
            }
            if (!o.test_base && !o.allow_unknown &&
                (c.timestamp != s10::kFocTimeDateStamp || c.size_of_image != s10::kFocSizeOfImage)) {
                if (ignored.insert(pid).second) {
                    std::fprintf(stdout, "ignored pid %lu: timestamp 0x%08x size_of_image 0x%08x (not FoC 1.121)\n",
                                 static_cast<unsigned long>(pid), c.timestamp, c.size_of_image);
                    std::fflush(stdout);
                }
                CloseHandle(h);  // EaW's StarWarsG.exe or another build
                continue;
            }
            g = c;
            std::fprintf(stdout, "attached pid %lu base 0x%llx\n", static_cast<unsigned long>(pid),
                         static_cast<unsigned long long>(c.base));
            std::fflush(stdout);
            break;
        }
        if (g.h) break;
        if (now_s() - t0 > o.wait_process) {
            std::fprintf(stderr, "no FoC StarWarsG.exe 1.121 process found\n");
            return 2;
        }
        Sleep(500);
    }
    Reader r(o, g);
    const int code = r.run();
    CloseHandle(g.h);
    return code;
}
