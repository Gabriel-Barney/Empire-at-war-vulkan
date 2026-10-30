// fake_game: test target for q2reader. Lays out the documented StarWarsG.exe structures (q2_layout.hpp)
// in its own memory and runs a tiny deterministic "simulation" so the reader and q2trace.py can be
// tested end to end without the real game. Not used in real runs.
//
//   fake_game.exe <galactic|space> <ticks> <ticks_per_second> [seed_hex]
// Prints "BASE 0x... PID n" on stdout, then runs. RNG draws on frame f: f % 5 (checked by q2trace.py
// selftest-fake). Galactic: a fiscal cycle every 30 mode frames.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

#include "q2_layout.hpp"

namespace {

uint32_t g_tab[256];
uint32_t crc_raw(const unsigned char* p, size_t n) {
    uint32_t c = 0;
    for (size_t i = 0; i < n; ++i) c = g_tab[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    return c;
}
uint32_t fbits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}
uint32_t obj_crc(const float* pos, const float* fac, float hp) {
    const uint32_t x = fbits(pos[0]) ^ fbits(pos[1]) ^ fbits(pos[2]) ^ fbits(fac[0]) ^ fbits(fac[1]) ^ fbits(fac[2]);
    unsigned char b[8];
    size_t n = 4;
    for (int i = 0; i < 4; ++i) b[i] = static_cast<unsigned char>(x >> (8 * i));
    if (hp != 0.0f) {
        const uint32_t h = fbits(hp);
        for (int i = 0; i < 4; ++i) b[4 + i] = static_cast<unsigned char>(h >> (8 * i));
        n = 8;
    }
    return crc_raw(b, n);
}

template <class T>
T& at(unsigned char* base, uint64_t off) {
    return *reinterpret_cast<T*>(base + off);
}

struct World {
    unsigned char* img = nullptr;
    unsigned char* mode = nullptr;
    unsigned char* gom = nullptr;
    std::unordered_map<int, void*>* idmap = nullptr;
    std::vector<unsigned char*> types;
    std::vector<unsigned char*> objects;
    std::vector<unsigned char*> players;
    unsigned char* player_vec = nullptr;
    unsigned char* production = nullptr;  // first planet's production behaviour (queue 0, one entry)
    int next_id = 1;
};

unsigned char* make_type(World& w, uint32_t index, const char* name) {
    auto* t = static_cast<unsigned char*>(std::calloc(1, 0x200));
    at<uint32_t>(t, q2::kTypeIndex) = index;
    new (t + q2::kTypeName) std::string(name);
    at<uint32_t>(t, q2::kTypeNameCrc) = 0xA0000000u + index;
    w.types.push_back(t);
    return t;
}

unsigned char* spawn(World& w, unsigned char* type, int owner, float x, bool planet) {
    auto* o = static_cast<unsigned char*>(std::calloc(1, 0x8A0));
    const int id = w.next_id++;
    at<int32_t>(o, q2::kObjId) = id;
    at<int32_t>(o, q2::kObjParentModeId) = q2::kNoObject;
    at<int32_t>(o, q2::kObjOwner) = owner;
    at<float>(o, q2::kObjHealth) = 100.0f;
    at<float>(o, q2::kObjPos) = x;
    at<float>(o, q2::kObjFacing + 4) = 1.0f;
    at<unsigned char*>(o, q2::kObjType) = type;
    if (planet) {
        auto* pk = static_cast<unsigned char*>(std::calloc(1, q2::kPlanBlock));
        at<float>(pk, q2::kPlanControl) = 1.0f;
        at<int32_t>(pk, q2::kPlanBase) = 100;
        at<float>(pk, q2::kPlanBlackMkt) = 1.0f;
        at<int32_t>(pk, q2::kPlanCorruption) = -1;
        at<int32_t>(pk, q2::kPlanSiphoner) = -1;
        at<unsigned char*>(o, q2::kObjPlanetPack) = pk;
        // Production behaviour (GALACTIC_ECONOMY.md 2.1): the first planet gets one ground-queue entry.
        auto* pb = static_cast<unsigned char*>(std::calloc(1, q2::kProdBlock));
        at<unsigned char*>(o, q2::kObjProduction) = pb;
        if (!w.production) {
            auto* uc = static_cast<unsigned char*>(std::calloc(1, q2::kUcSize));
            at<unsigned char*>(uc, q2::kUcType) = type;
            at<float>(uc, q2::kUcBuildTime) = 22.8f;
            at<uint32_t>(uc, q2::kUcFrames) = 684;
            at<int32_t>(uc, q2::kUcPlayer) = owner;
            at<float>(uc, q2::kUcCost) = 338.0f;
            at<int32_t>(uc, q2::kUcPlanet) = id;
            auto** data = static_cast<unsigned char**>(std::calloc(4, sizeof(unsigned char*)));
            data[0] = uc;
            at<unsigned char**>(pb, q2::kProdQueueData[0]) = data;
            w.production = pb;
        }
    }
    (*w.idmap)[id] = o;
    w.objects.push_back(o);
    return o;
}

uint32_t& rng(World& w) { return at<uint32_t>(w.img, q2::kRvaRngSync); }
uint32_t draw(World& w) {
    rng(w) = rng(w) * q2::kLcgMul + q2::kLcgAdd;
    return (rng(w) >> 10) & 0x7FFFu;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "fake_game <galactic|space> <ticks> <ticks_per_second> [seed_hex]\n");
        return 1;
    }
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        g_tab[i] = c;
    }
    const bool galactic = std::strcmp(argv[1], "galactic") == 0;
    const int ticks = std::atoi(argv[2]);
    const int tps = std::atoi(argv[3]);
    const uint32_t seed = argc > 4 ? static_cast<uint32_t>(std::strtoul(argv[4], nullptr, 16)) : 0x1234ABCDu;

    World w;
    w.img = static_cast<unsigned char*>(VirtualAlloc(nullptr, q2::kImageSpan, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!w.img) return 2;
    std::printf("BASE 0x%llx PID %lu\n", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(w.img)),
                static_cast<unsigned long>(GetCurrentProcessId()));
    std::fflush(stdout);
    unsigned char* fs = w.img + q2::kRvaFrameSync;
    unsigned char* gmm = w.img + q2::kRvaGmm;
    at<uint8_t>(fs, q2::kFsSimStopped) = 1;
    at<uint32_t>(w.img, q2::kRvaRngFree) = 0x55555555u;
    Sleep(600);  // "main menu"

    // Players: a DynamicVector-like (data, count) at PlayerList +0x10.
    for (int i = 0; i < 3; ++i) {
        auto* p = static_cast<unsigned char*>(std::calloc(1, q2::kPlayerBlock));
        at<float>(p, q2::kPlayerCredits) = 1000.0f * static_cast<float>(i + 1);
        at<float>(p, q2::kPlayerCap) = -1.0f;
        at<uint8_t>(p, q2::kPlayerIsHuman) = i == 0 ? 1 : 0;
        at<int32_t>(p, q2::kPlayerIndex) = i;
        at<uint64_t>(p, q2::kPlayerAi) = i == 0 ? 0 : 1;
        w.players.push_back(p);
    }
    // space: DynamicVector (data, count) at PlayerList +0x10; galactic: a std::vector one pointer away
    // (PlayerList +0x20 -> holder, vector at holder +0x08), the two shapes q2reader searches for.
    std::vector<unsigned char*>* holder_vec = nullptr;
    if (!galactic) {
        at<unsigned char**>(w.img, q2::kRvaPlayerList + 0x10) = w.players.data();
        at<int32_t>(w.img, q2::kRvaPlayerList + 0x18) = 3;
    } else {
        auto* holder = static_cast<unsigned char*>(std::calloc(1, 0x80));
        holder_vec = new (holder + 0x08) std::vector<unsigned char*>(w.players);
        at<unsigned char*>(w.img, q2::kRvaPlayerList + 0x20) = holder;
    }

    // Load: Start_Game seeds the RNG, then building the world draws a few numbers.
    w.mode = static_cast<unsigned char*>(std::calloc(1, 0x400));
    w.gom = static_cast<unsigned char*>(std::calloc(1, 0x640));
    w.idmap = new (w.gom + 0x80) std::unordered_map<int, void*>();
    at<unsigned char*>(w.mode, q2::kModePrimary) = w.gom;
    at<uint32_t>(gmm, q2::kGmmSeed) = seed;
    at<int32_t>(gmm, q2::kGmmModeType) = galactic ? 0 : 2;
    at<unsigned char*>(gmm, q2::kGmmCurMode) = w.mode;
    at<int32_t>(gmm, q2::kGmmStackCount) = 1;
    rng(w) = seed;
    unsigned char* t_planet = make_type(w, 7, "PLANET_TEST");
    unsigned char* t_ship = make_type(w, 3, "STAR_DESTROYER");
    unsigned char* t_fighter = make_type(w, 12, "A_VERY_LONG_TYPE_NAME_FOR_SSO_CHECK");
    auto build = [&]() {
        for (int i = 0; i < 4; ++i) spawn(w, t_planet, i % 3, 1000.0f * static_cast<float>(i), true);
        for (int i = 0; i < 20; ++i) spawn(w, i % 2 ? t_ship : t_fighter, i % 3, static_cast<float>(draw(w)), false);
    };
    // galactic: like the real GC start, the sim flag clears before the galaxy exists (built during tick 1).
    if (!galactic) build();
    Sleep(300);
    at<uint8_t>(fs, q2::kFsSimStopped) = 0;
    Sleep(200);

    const DWORD period = static_cast<DWORD>(1000 / (tps > 0 ? tps : 30));
    for (int t = 0; t < ticks; ++t) {
        Sleep(period);
        at<uint8_t>(fs, q2::kFsInTick) = 1;
        uint32_t& frame = at<uint32_t>(fs, q2::kFsFrame);
        frame += 1;
        const uint32_t f = frame;
        if (galactic && f == 1) build();
        for (uint32_t k = 0; k < f % 5; ++k) draw(w);
        for (auto* o : w.objects) {
            if (at<uint8_t>(o, q2::kObjFlags) & q2::kFlagDead) continue;
            if (at<unsigned char*>(o, q2::kObjType) != t_planet) at<float>(o, q2::kObjPos + 4) += 0.5f;
        }
        if (f % 40 == 0) spawn(w, t_ship, 1, 5.0f, false);
        if (w.production && f % 50 == 0) {  // the queue entry comes and goes
            int32_t& n = at<int32_t>(w.production, q2::kProdQueueCount[0]);
            n = n ? 0 : 1;
        }
        if (f % 55 == 0 && w.objects.size() > 10) {
            unsigned char* victim = w.objects[5];
            at<uint8_t>(victim, q2::kObjFlags) |= q2::kFlagDead;  // dead this frame, removed next
        }
        if (f % 55 == 1) {
            for (size_t i = 0; i < w.objects.size(); ++i)
                if (at<uint8_t>(w.objects[i], q2::kObjFlags) & q2::kFlagDead) {
                    w.idmap->erase(at<int32_t>(w.objects[i], q2::kObjId));
                    w.objects.erase(w.objects.begin() + static_cast<std::ptrdiff_t>(i));
                    break;
                }
        }
        uint32_t& mf = at<uint32_t>(w.mode, q2::kModeFrame);
        mf += 1;
        if (galactic && mf >= at<uint32_t>(w.mode, q2::kGalNextCycle)) {
            at<uint32_t>(w.mode, q2::kGalLastCycle) = at<uint32_t>(w.mode, q2::kGalNextCycle);
            at<uint32_t>(w.mode, q2::kGalNextCycle) += 30;
            at<uint32_t>(w.mode, q2::kGalCycleCount) += 1;
            at<float>(w.players[1], q2::kPlayerCredits) += 250.0f;
            at<float>(at<unsigned char*>(w.objects[0], q2::kObjPlanetPack), q2::kPlanControl) -= 0.125f;
        }
        for (auto* o : w.objects)
            at<uint32_t>(o, q2::kObjCrc) = obj_crc(&at<float>(o, q2::kObjPos), &at<float>(o, q2::kObjFacing),
                                                   at<float>(o, q2::kObjHealth));
        at<uint8_t>(fs, q2::kFsInTick) = 0;
    }
    Sleep(2000);
    (void)holder_vec;
    return 0;
}
