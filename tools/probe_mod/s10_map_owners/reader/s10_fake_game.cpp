// s10_fake_game: test target for s10reader (not used in real runs). Lays out the documented StarWarsG.exe structures
// (s10_layout.hpp) in its own memory: 1 s of galactic mode, then a land mode whose object store holds the objects of
// <objects.txt> ("TYPE owner x y z" per line); the mode frame advances 30 per second for <seconds>. At land start it
// opens a message box (the _MessagePopup channel) that the reader should record and dismiss.
//
//   s10_fake_game.exe <objects.txt> <seconds> [popup text]      prints "BASE 0x... PID n" and runs
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

#include "s10_layout.hpp"

namespace {

template <class T>
T& at(unsigned char* base, uint64_t off) {
    return *reinterpret_cast<T*>(base + off);
}

std::string g_popup = "S10 t0 _MP_LAND_POLUS REBEL=0:1 EMPIRE=1:1";

DWORD WINAPI popup_thread(LPVOID) {
    MessageBoxA(nullptr, g_popup.c_str(), "S10 probe", MB_OK);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3 && argc != 4) {
        std::fprintf(stderr, "usage: s10_fake_game <objects.txt> <seconds> [popup text]\n");
        return 1;
    }
    if (argc == 4) g_popup = argv[3];
    const int seconds = std::atoi(argv[2]);
    auto* img = static_cast<unsigned char*>(VirtualAlloc(nullptr, s10::kImageSpan, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    auto* galactic = static_cast<unsigned char*>(std::calloc(1, 0x400));
    auto* land = static_cast<unsigned char*>(std::calloc(1, 0x400));
    auto* gom = static_cast<unsigned char*>(std::calloc(1, 0x200));
    if (!img || !galactic || !land || !gom) return 2;
    auto* idmap = new (gom + 0x80) std::unordered_map<int, void*>();
    at<unsigned char*>(land, s10::kModePrimary) = gom;
    at<unsigned char*>(galactic, s10::kModePrimary) = gom;
    std::unordered_map<std::string, unsigned char*> types;
    std::vector<unsigned char*> objects;
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) return 3;
    char name[256];
    int owner = 0;
    float x = 0, y = 0, z = 0;
    int next_id = 100;
    while (std::fscanf(f, "%255s %d %f %f %f", name, &owner, &x, &y, &z) == 5) {
        unsigned char*& t = types[name];
        if (!t) {
            t = static_cast<unsigned char*>(std::calloc(1, 0x200));
            new (t + s10::kTypeName) std::string(name);
            at<uint32_t>(t, s10::kTypeIndex) = static_cast<uint32_t>(types.size());
        }
        auto* o = static_cast<unsigned char*>(std::calloc(1, 0x400));
        at<int32_t>(o, s10::kObjId) = next_id;
        at<int32_t>(o, s10::kObjOwner) = owner;
        at<float>(o, s10::kObjHealth) = 100.0f;
        at<float>(o, s10::kObjPos) = x;
        at<float>(o, s10::kObjPos + 4) = y;
        at<float>(o, s10::kObjPos + 8) = z;
        at<unsigned char*>(o, s10::kObjType) = t;
        objects.push_back(o);
        ++next_id;
    }
    std::fclose(f);
    unsigned char* gmm = img + s10::kRvaGmm;
    unsigned char* fs = img + s10::kRvaFrameSync;
    at<int32_t>(gmm, s10::kGmmStackCount) = 1;
    at<int32_t>(gmm, s10::kGmmModeType) = 0;
    at<unsigned char*>(gmm, s10::kGmmCurMode) = galactic;
    std::printf("BASE 0x%llx PID %lu\n", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(img)),
                static_cast<unsigned long>(GetCurrentProcessId()));
    std::fflush(stdout);
    Sleep(1000);
    // Land battle inside the campaign: stack 2, objects appear, then the mode becomes current.
    for (size_t i = 0; i < objects.size(); ++i) (*idmap)[at<int32_t>(objects[i], s10::kObjId)] = objects[i];
    at<int32_t>(gmm, s10::kGmmStackCount) = 2;
    at<int32_t>(gmm, s10::kGmmModeType) = 1;
    at<unsigned char*>(gmm, s10::kGmmCurMode) = land;
    if (!g_popup.empty()) CreateThread(nullptr, 0, popup_thread, nullptr, 0, nullptr);
    for (int i = 0; i < seconds * 30; ++i) {
        at<uint8_t>(fs, s10::kFsInTick) = 1;
        at<uint32_t>(fs, s10::kFsFrame) += 1;
        at<uint32_t>(land, s10::kModeFrame) += 1;
        at<uint8_t>(fs, s10::kFsInTick) = 0;
        Sleep(33);
    }
    return 0;
}
