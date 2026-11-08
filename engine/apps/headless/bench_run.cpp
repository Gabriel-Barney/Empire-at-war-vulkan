// alamo_headless BM1 benchmark: see bench_run.hpp.
//
// Determinism: the timings, the profiler and the machine probe are read here, in the driver, after or around ticks; the
// sim never sees any of them (CLAUDE.md rule 3). The CRCs are the sim's own per-tick log.
#include "bench_run.hpp"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "alamo/commands/recording.hpp"
#include "alamo/core/profile.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/sim/bench_json.hpp"
#include "alamo/sim/bench_scenario.hpp"
#include "alamo/sim/bench_stats.hpp"
#include "crc_out.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi.h>
#endif

#ifndef ALAMO_BUILD_GIT
#define ALAMO_BUILD_GIT "unknown"
#endif
#ifndef ALAMO_BUILD_CONFIG
#define ALAMO_BUILD_CONFIG "unknown"
#endif

namespace {

using namespace alamo;
namespace bench = alamo::sim::bench;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

constexpr int kExitNotComparable = 3;
constexpr uint32_t kTopZones = 24;

double ms_between(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

std::string read_text(const std::string& path) {
    std::ifstream f(fs::path(path), std::ios::binary);
    return f ? std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()) : std::string();
}

// Records with tick <= last.
CrcLog until(const CrcLog& log, uint64_t last) {
    CrcLog out = log;
    out.records.clear();
    for (const CrcRecord& r : log.records)
        if (r.tick <= last) out.records.push_back(r);
    return out;
}

// ------------------------------------------------------------------------------------------------- machine / build
#ifdef _WIN32
std::string reg_string(const char* key, const char* value) {
    char buf[512] = {};
    DWORD size = sizeof buf;
    if (RegGetValueA(HKEY_LOCAL_MACHINE, key, value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return {};
    std::string s(buf);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
    const size_t first = s.find_first_not_of(' ');
    return first == std::string::npos ? std::string() : s.substr(first);
}
uint32_t reg_dword(const char* key, const char* value) {
    DWORD v = 0;
    DWORD size = sizeof v;
    if (RegGetValueA(HKEY_LOCAL_MACHINE, key, value, RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS) return 0;
    return v;
}
std::string narrow(const wchar_t* w) {
    std::string s;
    for (; *w; ++w) s += *w < 0x80 ? static_cast<char>(*w) : '?';
    return s;
}
#endif

// CPU time of the whole machine and of this process, to tell how much of the machine other processes took during a run
// (a benchmark on a busy machine is noise: other builds and tests on the same CPU slowed a run 35x in BM1's first try).
struct CpuSample {
    uint64_t idle = 0, total = 0, process = 0;  // 100 ns units, summed over all logical CPUs
    bool ok = false;
};
CpuSample cpu_sample() {
    CpuSample s;
#ifdef _WIN32
    FILETIME idle, kernel, user, created, exited, pkernel, puser;
    auto u64 = [](const FILETIME& f) { return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    if (GetSystemTimes(&idle, &kernel, &user) && GetProcessTimes(GetCurrentProcess(), &created, &exited, &pkernel, &puser)) {
        s.idle = u64(idle);
        s.total = u64(kernel) + u64(user);  // kernel time includes idle time
        s.process = u64(pkernel) + u64(puser);
        s.ok = true;
    }
#endif
    return s;
}
// Share of the machine's CPU time (all logical CPUs) other processes used between a and b, in percent; < 0 when unknown.
double other_cpu_pct(const CpuSample& a, const CpuSample& b) {
    if (!a.ok || !b.ok || b.total <= a.total) return -1;
    const double total = static_cast<double>(b.total - a.total);
    const double busy = total - static_cast<double>(b.idle - a.idle);
    const double own = static_cast<double>(b.process - a.process);
    return std::max(0.0, (busy - own) / total * 100.0);
}
constexpr double kNoisyOtherCpuPct = 10.0;  // above this, the result is flagged noisy

bench::Json machine_json() {
    bench::Json m = bench::Json::object();
    std::string cpu, os, gpu, driver;
    double memory_mb = 0, gpu_memory_mb = 0;
#ifdef _WIN32
    cpu = reg_string("HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString");
    const char* nt = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    os = reg_string(nt, "ProductName");
    const std::string build = reg_string(nt, "CurrentBuild");
    const std::string display = reg_string(nt, "DisplayVersion");
    if (!build.empty()) os += " build " + build + "." + std::to_string(reg_dword(nt, "UBR"));
    if (!display.empty()) os += " (" + display + ")";
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof ms;
    if (GlobalMemoryStatusEx(&ms)) memory_mb = static_cast<double>(ms.ullTotalPhys / (1024ull * 1024ull));
    // The first DXGI adapter (the primary GPU) and its user-mode driver version. Sim-only runs do not use it; it is recorded
    // so results from different machines can be told apart (the game-side benchmark records the Vulkan device it renders on).
    IDXGIFactory1* factory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(IID_IDXGIFactory1, reinterpret_cast<void**>(&factory))) && factory) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(0, &adapter) == S_OK && adapter) {
            DXGI_ADAPTER_DESC1 d{};
            if (SUCCEEDED(adapter->GetDesc1(&d))) {
                gpu = narrow(d.Description);
                gpu_memory_mb = static_cast<double>(d.DedicatedVideoMemory / (1024ull * 1024ull));
            }
            LARGE_INTEGER v{};
            if (SUCCEEDED(adapter->CheckInterfaceSupport(IID_IDXGIDevice, &v))) {
                const auto q = static_cast<uint64_t>(v.QuadPart);
                driver = std::to_string((q >> 48) & 0xFFFF) + "." + std::to_string((q >> 32) & 0xFFFF) + "." +
                         std::to_string((q >> 16) & 0xFFFF) + "." + std::to_string(q & 0xFFFF);
            }
            adapter->Release();
        }
        factory->Release();
    }
#endif
    m.set("cpu", bench::Json::string(cpu.empty() ? "unknown" : cpu));
    m.set("logical_cores", bench::Json::number(std::thread::hardware_concurrency()));
    m.set("memory_mb", bench::Json::number(memory_mb));
    m.set("os", bench::Json::string(os.empty() ? "unknown" : os));
    m.set("gpu", bench::Json::string(gpu.empty() ? "unknown" : gpu));
    m.set("gpu_memory_mb", bench::Json::number(gpu_memory_mb));
    m.set("gpu_driver", bench::Json::string(driver.empty() ? "unknown" : driver));
    m.set("gpu_used", bench::Json::boolean(false));  // sim-only: nothing is rendered
    return m;
}

std::string compiler_text() {
#if defined(__clang__)
    return std::string("clang ") + __clang_version__;
#elif defined(_MSC_VER)
    return "MSVC " + std::to_string(_MSC_FULL_VER);
#else
    return "unknown";
#endif
}

bench::Json build_json() {
    bench::Json b = bench::Json::object();
    b.set("compiler", bench::Json::string(compiler_text()));
    b.set("config", bench::Json::string(ALAMO_BUILD_CONFIG));
    b.set("git", bench::Json::string(ALAMO_BUILD_GIT));
#ifdef NDEBUG
    b.set("asserts", bench::Json::boolean(false));
#else
    b.set("asserts", bench::Json::boolean(true));
#endif
    return b;
}

std::string utc_now() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

// ticks_per_run: the pooled series of several runs (worst_index counts across them) -> the tick within its run.
bench::Json stats_json(const bench::TickStats& s, uint64_t first_tick, uint64_t ticks_per_run) {
    bench::Json j = bench::Json::object();
    j.set("ticks", bench::Json::number(static_cast<double>(s.count)));
    j.set("median_ms", bench::Json::number(s.median_ms));
    j.set("p99_ms", bench::Json::number(s.p99_ms));
    j.set("worst_ms", bench::Json::number(s.worst_ms));
    j.set("worst_tick", bench::Json::number(static_cast<double>(first_tick + s.worst_index % ticks_per_run + 1)));
    if (s.count > ticks_per_run) j.set("worst_run", bench::Json::number(static_cast<double>(s.worst_index / ticks_per_run + 1)));
    j.set("mean_ms", bench::Json::number(s.mean_ms));
    j.set("total_ms", bench::Json::number(s.total_ms));
    j.set("ticks_per_second", bench::Json::number(s.total_ms > 0 ? static_cast<double>(s.count) * 1000.0 / s.total_ms : 0));
    return j;
}

bench::TickStats stats_from_json(const bench::Json* j) {
    bench::TickStats s;
    if (!j) return s;
    auto num = [&](const char* k) {
        const bench::Json* v = j->find(k);
        return v ? v->as_number() : 0.0;
    };
    s.count = static_cast<uint64_t>(num("ticks"));
    s.median_ms = num("median_ms");
    s.p99_ms = num("p99_ms");
    s.worst_ms = num("worst_ms");
    s.mean_ms = num("mean_ms");
    s.total_ms = num("total_ms");
    return s;
}

bench::ResultKey key_from_json(const bench::Json& r) {
    bench::ResultKey k;
    auto str = [&](const char* p) {
        const bench::Json* v = r.at(p);
        return v ? v->as_string() : std::string();
    };
    k.scenario = str("scenario.name");
    const bench::Json* v = r.at("scenario.version");
    k.version = v ? static_cast<uint32_t>(v->as_number()) : 0;
    k.fingerprint_digest = str("scenario.fingerprint_digest");
    const bench::Json* t = r.at("settings.ticks");
    k.ticks = t ? static_cast<uint64_t>(t->as_number()) : 0;
    const bench::Json* ok = r.at("fingerprint.ok");
    k.fingerprint_ok = ok && ok->as_bool();
    return k;
}

void print_stats(const char* label, const bench::TickStats& s, uint64_t first_tick, uint64_t ticks_per_run) {
    std::printf("%s %" PRIu64 " ticks: median %.3f ms, p99 %.3f ms, worst %.3f ms (tick %" PRIu64 "), mean %.3f ms, %.1f ticks/s\n", label,
                s.count, s.median_ms, s.p99_ms, s.worst_ms, first_tick + s.worst_index % ticks_per_run + 1, s.mean_ms,
                s.total_ms > 0 ? static_cast<double>(s.count) * 1000.0 / s.total_ms : 0.0);
}

int not_comparable(const BenchOptions& opt, const std::string& why) {
    std::printf("bench: scenario changed, not comparable: %s\n", why.c_str());
    std::printf("bench: regenerate it with tools\\benchmark\\regen_scenario.cmd (alamo_headless --bench-regen %s)\n", opt.scenario.c_str());
    return opt.allow_changed ? 0 : kExitNotComparable;
}

}  // namespace

// ================================================================================================= --bench
int bench_run(const BenchOptions& opt) {
    std::string err;
    bench::Scenario sc;
    if (!bench::Scenario::load(opt.scenario, sc, &err)) {
        std::fprintf(stderr, "bench: %s\n", err.c_str());
        return 1;
    }
    std::printf("bench: scenario %s version %u: %s\n", sc.name.c_str(), sc.version, sc.description.c_str());
    const bool game_host = sc.host == bench::Host::Game;  // BM2: an alamo_game save (game.campaign / game.skirmish)
#ifndef ALAMO_HEADLESS_HAVE_GAME_CORE
    if (game_host) {  // bm2c t02: timed sim-only through alamo_game_core (bench_game.cpp), not built into this exe
        std::fprintf(stderr, "bench: %s was made by alamo_game (host game) and this alamo_headless has no alamo_game_core: run it with "
                     "alamo_game --bench-scenario %s\n", sc.name.c_str(), opt.scenario.c_str());
        return 2;
    }
#endif
    if (!bench::verify_files(sc, &err)) return not_comparable(opt, err);
    CrcLog fingerprint;
    if (!CrcLog::load(sc.resolve(sc.fingerprint), fingerprint, &err) || fingerprint.records.empty()) {
        std::fprintf(stderr, "bench: fingerprint: %s\n", err.empty() ? "no records" : err.c_str());
        return 1;
    }
    commands::CommandRecording orders;
    const bool have_orders = sc.orders.present();
    if (have_orders && !commands::CommandRecording::load(sc.resolve(sc.orders), orders, &err)) {
        std::fprintf(stderr, "bench: orders: %s\n", err.c_str());
        return 1;
    }
    std::unique_ptr<SaveSessionSource> src;
#ifdef ALAMO_HEADLESS_HAVE_GAME_CORE
    const int opened = game_host ? open_game_save_source(sc.resolve(sc.save), opt.game_dir, opt.game_args, src, err)
                                 : open_save_source(sc.resolve(sc.save), opt.game_dir, opt.game_args, src, err);
#else
    const int opened = open_save_source(sc.resolve(sc.save), opt.game_dir, opt.game_args, src, err);
#endif
    if (opened == 0) {
        std::printf("bench: %s: %s: skipped\n", sc.name.c_str(), err.c_str());
        return 0;
    }
    if (opened < 0) return not_comparable(opt, "the save does not load (" + err + ")");
    if (src->save_tick() != sc.start_tick) return not_comparable(opt, "the save is at tick " + std::to_string(src->save_tick()));

    const uint64_t ticks = opt.ticks_set ? std::min(opt.ticks, sc.ticks) : sc.ticks;
    if (ticks == 0) {
        std::fprintf(stderr, "bench: --ticks must be at least 1\n");
        return 2;
    }
    const int32_t workers = opt.workers_set ? opt.workers : sc.workers;
    const uint64_t end_tick = sc.start_tick + ticks;
    const CrcLog expected = until(fingerprint, end_tick);
    if (expected.records.empty() || expected.records.back().tick < sc.start_tick) {
        std::fprintf(stderr, "bench: the fingerprint has no record in ticks %" PRIu64 "..%" PRIu64 "\n", sc.start_tick, end_tick);
        return 1;
    }
    CrcLog golden;
    if (!opt.golden.empty() && !CrcLog::load(opt.golden, golden, &err)) {
        std::fprintf(stderr, "bench: golden: %s\n", err.c_str());
        return 1;
    }
    std::printf("bench: %s, save tick %" PRIu64 ", %" PRIu64 " ticks, %u warm-up + %u timed run(s), workers %s\n", src->describe().c_str(),
                sc.start_tick, ticks, opt.warmup, opt.runs, workers < 0 ? "auto" : std::to_string(workers).c_str());

    bool fp_ok = true, failed = false;
    std::string fp_message;
    CrcLog first_log;
    uint32_t actual_workers = 0;
    double max_other_cpu = -1;  // < 0: unknown
    std::vector<double> pooled;
    std::vector<profile::TickRecord> zone_ticks;
    bench::Json runs_json = bench::Json::array();
    const uint32_t total_runs = opt.warmup + std::max<uint32_t>(opt.runs, 1);
    for (uint32_t run = 0; run < total_runs; ++run) {
        const bool timed = run >= opt.warmup;
        const Clock::time_point l0 = Clock::now();
        std::unique_ptr<LoadedSession> s = src->load(workers, have_orders ? &orders : nullptr, nullptr, err);
        if (!s) {
            if (run == 0) return not_comparable(opt, "the save does not load (" + err + ")");
            std::fprintf(stderr, "bench: run %u: %s\n", run, err.c_str());
            return 1;
        }
        const double load_ms = ms_between(l0, Clock::now());
        actual_workers = s->worker_count();
        std::vector<double> ms;
        ms.reserve(static_cast<size_t>(ticks));
        if (timed) profile::start(static_cast<uint32_t>(std::min<uint64_t>(ticks + 64, 1u << 20)));
        const CpuSample c0 = cpu_sample();
        const Clock::time_point r0 = Clock::now();
        for (uint64_t t = 0; t < ticks; ++t) {
            const Clock::time_point t0 = Clock::now();
            if (!s->tick(err)) {
                if (timed) profile::stop();
                std::fprintf(stderr, "bench: run %u: %s\n", run, err.c_str());
                return 1;
            }
            ms.push_back(ms_between(t0, Clock::now()));
        }
        const double wall_ms = ms_between(r0, Clock::now());
        const double other_cpu = other_cpu_pct(c0, cpu_sample());
        if (timed) {
            profile::stop();
            std::vector<profile::TickRecord> recs = profile::snapshot();
            zone_ticks.insert(zone_ticks.end(), recs.begin(), recs.end());
        }
        const CrcLog& log = s->crc_log();
        const CrcLog actual = until(log, expected.records.back().tick);
        const CrcDivergence d = first_divergence(expected, actual);
        const bool replay_ok = s->replay_failures() == 0;
        const bool run_fp_ok = !d.diverged && replay_ok;
        const std::string run_message =
            run_fp_ok ? std::string() : !replay_ok ? std::to_string(s->replay_failures()) + " recorded orders failed to insert" : d.message;
        if (!run_fp_ok && fp_ok) fp_message = run_message;
        fp_ok = fp_ok && run_fp_ok;
        if (run == 0) {
            first_log = log;
            if (!opt.crc_out.empty()) {
                CrcLog out = log;
                out.comments = {"bench " + sc.name + " version " + std::to_string(sc.version) + " workers " + std::to_string(actual_workers)};
                if (!alamo_headless::save_crc_log(out, opt.crc_out, &err)) {
                    std::fprintf(stderr, "bench: crc-out: %s\n", err.c_str());
                    return 1;
                }
            }
        } else {
            const CrcDivergence dr = first_divergence(first_log, log);
            if (dr.diverged) {
                std::printf("bench: run %u differs from run 0 (non-deterministic): %s\n", run, dr.message.c_str());
                failed = true;
            }
        }
        if (!opt.golden.empty()) {
            const CrcDivergence dg = first_divergence(golden, log);
            if (dg.diverged) {
                std::printf("bench: run %u vs golden %s: %s\n", run, fs::path(opt.golden).filename().string().c_str(), dg.message.c_str());
                failed = true;
            }
        }
        const bench::TickStats st = bench::summarize(ms);
        char label[64];
        std::snprintf(label, sizeof label, "  %s %u (load %.0f ms, %.0f ms):", timed ? "run" : "warm-up", timed ? run - opt.warmup + 1 : run + 1,
                      load_ms, wall_ms);
        if (!opt.quiet || timed) print_stats(label, st, sc.start_tick, ticks);
        if (!run_fp_ok) std::printf("    fingerprint: %s\n", run_message.c_str());
        if (other_cpu > kNoisyOtherCpuPct)
            std::printf("    noisy: other processes used %.0f%% of the machine's CPU during this run\n", other_cpu);
        if (timed) {
            max_other_cpu = std::max(max_other_cpu, other_cpu);
            pooled.insert(pooled.end(), ms.begin(), ms.end());
            bench::Json rj = stats_json(st, sc.start_tick, ticks);
            rj.set("run", bench::Json::number(run - opt.warmup + 1));
            rj.set("load_ms", bench::Json::number(load_ms));
            rj.set("wall_ms", bench::Json::number(wall_ms));
            rj.set("fingerprint_ok", bench::Json::boolean(run_fp_ok));
            rj.set("other_cpu_pct", other_cpu < 0 ? bench::Json() : bench::Json::number(other_cpu));
            rj.set("final_crc", bench::Json::string(bench::hex64(log.records.empty() ? 0 : log.records.back().combined)));
            runs_json.push(std::move(rj));
        }
    }

    const bench::TickStats summary = bench::summarize(pooled);
    if (fp_ok) print_stats("bench: all timed runs,", summary, sc.start_tick, ticks);
    else std::printf("bench: timings above are NOT comparable (the run did not match the scenario fingerprint)\n");
    const bool noisy = max_other_cpu > kNoisyOtherCpuPct;
    if (noisy)
        std::printf("bench: warning: noisy result, other processes used up to %.0f%% of the CPU during a timed run (close builds / tests)\n",
                    max_other_cpu);

    // OPT1 per-system zones over the timed runs (inclusive thread time; a zone in a parallel loop can exceed the tick).
    const profile::Report rep = profile::build_report(zone_ticks);
    bench::Json zones = bench::Json::array();
    for (size_t i = 0; i < rep.zones.size() && i < kTopZones; ++i) {
        const profile::ZoneStat& z = rep.zones[i];
        bench::Json zj = bench::Json::object();
        zj.set("name", bench::Json::string(z.name));
        zj.set("share", bench::Json::number(z.share));
        zj.set("mean_ms", bench::Json::number(z.mean_ms));
        zj.set("p50_ms", bench::Json::number(z.p50_ms));
        zj.set("p99_ms", bench::Json::number(z.p99_ms));
        zj.set("max_ms", bench::Json::number(z.max_ms));
        zones.push(std::move(zj));
        if (!opt.quiet && i < 8) std::printf("  zone %-24s %5.1f%%  p50 %.3f ms  p99 %.3f ms  max %.3f ms\n", z.name.c_str(), z.share * 100.0, z.p50_ms,
                                             z.p99_ms, z.max_ms);
    }

    bench::Json r = bench::Json::object();
    r.set("format", bench::Json::string("alamo-bench-result"));
    r.set("format_version", bench::Json::number(1));
    r.set("tool", bench::Json::string("alamo_headless"));
    r.set("mode", bench::Json::string("sim-only"));
    r.set("date_utc", bench::Json::string(utc_now()));
    bench::Json sj = bench::Json::object();
    sj.set("name", bench::Json::string(sc.name));
    sj.set("version", bench::Json::number(sc.version));
    sj.set("format", bench::Json::number(sc.format));
    sj.set("file", bench::Json::string(fs::path(opt.scenario).filename().string()));
    sj.set("session", bench::Json::string(src->describe()));
    sj.set("start_tick", bench::Json::number(static_cast<double>(sc.start_tick)));
    sj.set("ticks", bench::Json::number(static_cast<double>(sc.ticks)));
    sj.set("save_digest", bench::Json::string(bench::hex64(sc.save.digest)));
    sj.set("fingerprint_digest", bench::Json::string(bench::hex64(sc.fingerprint.digest)));
    r.set("scenario", std::move(sj));
    bench::Json fj = bench::Json::object();
    fj.set("ok", bench::Json::boolean(fp_ok));
    fj.set("message", bench::Json::string(fp_ok ? "every run matched the fingerprint" : fp_message));
    r.set("fingerprint", std::move(fj));
    r.set("comparable", bench::Json::boolean(fp_ok));
    bench::Json set = bench::Json::object();
    set.set("ticks", bench::Json::number(static_cast<double>(ticks)));
    set.set("workers_requested", workers < 0 ? bench::Json::string("auto") : bench::Json::number(workers));
    set.set("workers", bench::Json::number(actual_workers));
    set.set("warmup_runs", bench::Json::number(opt.warmup));
    set.set("timed_runs", bench::Json::number(std::max<uint32_t>(opt.runs, 1)));
    set.set("rendering", bench::Json::boolean(false));
    set.set("pacing", bench::Json::string("as fast as possible"));
    set.set("profiler_zones", bench::Json::boolean(true));
    r.set("settings", std::move(set));
    r.set("machine", machine_json());
    r.set("build", build_json());
    bench::Json sum = stats_json(summary, sc.start_tick, ticks);
    sum.set("max_other_cpu_pct", max_other_cpu < 0 ? bench::Json() : bench::Json::number(max_other_cpu));
    sum.set("noisy", bench::Json::boolean(noisy));
    r.set("summary", std::move(sum));
    r.set("runs", std::move(runs_json));
    r.set("zones", std::move(zones));

    if (!opt.baseline.empty()) {
        bench::Json base;
        const std::string text = read_text(opt.baseline);
        if (text.empty() || !bench::Json::parse(text, base, &err)) {
            std::fprintf(stderr, "bench: baseline %s: %s\n", opt.baseline.c_str(), text.empty() ? "cannot read" : err.c_str());
            return 1;
        }
        const bench::Comparison c = bench::compare(key_from_json(base), stats_from_json(base.find("summary")), key_from_json(r), summary);
        std::printf("bench: vs baseline %s: %s\n", fs::path(opt.baseline).filename().string().c_str(), c.text.c_str());
        auto same = [&](const char* p) {
            const bench::Json* a = base.at(p);
            const bench::Json* b = r.at(p);
            return a && b && a->as_string() == b->as_string() && a->as_number() == b->as_number();
        };
        std::vector<std::string> notes;
        if (!same("machine.cpu")) notes.push_back("other CPU");
        if (!same("build.compiler")) notes.push_back("other compiler");
        if (!same("settings.workers")) notes.push_back("other worker count");
        const bench::Json* base_noisy = base.at("summary.noisy");
        if (noisy || (base_noisy && base_noisy->as_bool())) notes.push_back("a noisy (busy machine) result on one side");
        if (c.comparable && !notes.empty()) {
            std::string n;
            for (const std::string& s : notes) n += (n.empty() ? "" : ", ") + s;
            std::printf("bench: note, compared with the baseline: %s\n", n.c_str());
        }
        bench::Json bj = bench::Json::object();
        bj.set("file", bench::Json::string(fs::path(opt.baseline).filename().string()));
        bj.set("comparable", bench::Json::boolean(c.comparable));
        if (c.comparable) {
            bj.set("median_ratio", bench::Json::number(c.median_ratio));
            bj.set("p99_ratio", bench::Json::number(c.p99_ratio));
            bj.set("worst_ratio", bench::Json::number(c.worst_ratio));
        } else {
            bj.set("reason", bench::Json::string("scenario changed, not comparable: " + c.reason));
        }
        bench::Json nj = bench::Json::array();
        for (const std::string& s : notes) nj.push(bench::Json::string(s));
        bj.set("notes", std::move(nj));
        r.set("baseline", std::move(bj));
    }

    if (!opt.json_out.empty()) {
        const std::string text = r.to_text();
        if (opt.json_out == "-") {
            std::fputs(text.c_str(), stdout);
        } else {
            std::ofstream f(fs::path(opt.json_out), std::ios::binary | std::ios::trunc);
            if (!f || !f.write(text.data(), static_cast<std::streamsize>(text.size()))) {
                std::fprintf(stderr, "bench: cannot write %s\n", opt.json_out.c_str());
                return 1;
            }
            std::printf("bench: wrote %s\n", opt.json_out.c_str());
        }
    }
    if (failed) {
        std::printf("bench: FAILED\n");
        return 1;
    }
    if (!fp_ok) return not_comparable(opt, fp_message);
    std::printf("bench: OK\n");
    return 0;
}

// ================================================================================================= --bench-make
int bench_make(const BenchOptions& opt) {
    std::string err;
    if (opt.make.session.empty() || !opt.make.save_at_set || !opt.ticks_set || opt.ticks == 0) {
        std::fprintf(stderr, "bench-make: needs --save-session KIND, --bench-save-at T and --ticks K\n");
        return 2;
    }
    const fs::path manifest(opt.scenario);
    const std::string name = manifest.stem().string();
    if (name.empty() || name.find_first_of(" \t") != std::string::npos) {
        std::fprintf(stderr, "bench-make: the scenario file name must be one word (%s)\n", opt.scenario.c_str());
        return 2;
    }
    const fs::path dir = manifest.parent_path();
    std::error_code ec;
    if (!dir.empty()) fs::create_directories(dir, ec);
    auto in_dir = [&](const std::string& f) { return (dir.empty() ? fs::path(f) : dir / f).string(); };
    const std::string save_name = name + ".alsv", orders_name = name + ".acmd", fp_name = name + ".crc";
    const std::string tmp_save = in_dir(save_name + ".tmp"), tmp_orders = in_dir(orders_name + ".tmp"), tmp_fp = in_dir(fp_name + ".tmp");
    auto cleanup = [&] {
        std::error_code e2;
        for (const std::string& p : {tmp_save, tmp_orders, tmp_fp}) fs::remove(fs::path(p), e2);
    };

    // 1. The save.
    SaveRunOptions so = opt.make;
    so.save_file = tmp_save;
    so.workers = opt.workers;
    so.game_dir = opt.game_dir;
    so.game_args = opt.game_args;
    uint64_t saved_tick = 0;
    const int made = make_save(so, saved_tick, err);
    if (made == 0) {
        std::printf("bench-make: %s: skipped\n", err.c_str());
        return 0;
    }
    if (made < 0) {
        std::fprintf(stderr, "bench-make: save: %s\n", err.c_str());
        cleanup();
        return 1;
    }
    std::unique_ptr<SaveSessionSource> src;
    if (open_save_source(tmp_save, opt.game_dir, opt.game_args, src, err) != 1) {
        std::fprintf(stderr, "bench-make: reopen the save: %s\n", err.c_str());
        cleanup();
        return 1;
    }
    std::printf("bench-make: %s saved at tick %" PRIu64 "\n", src->describe().c_str(), saved_tick);

    // 2. Load + record the orders given after the load.
    auto run = [&](const commands::CommandRecording* replay, commands::CommandRecording* record, CrcLog& out) {
        std::unique_ptr<LoadedSession> s = src->load(opt.workers, replay, record, err);
        if (!s) return false;
        for (uint64_t t = 0; t < opt.ticks; ++t)
            if (!s->tick(err)) return false;
        if (s->replay_failures() != 0) {
            err = std::to_string(s->replay_failures()) + " recorded orders failed to insert";
            return false;
        }
        out = s->crc_log();
        return true;
    };
    commands::CommandRecording rec;
    CrcLog recorded;
    if (!run(nullptr, &rec, recorded) || !rec.save(tmp_orders, &err)) {
        std::fprintf(stderr, "bench-make: recording run: %s\n", err.c_str());
        cleanup();
        return 1;
    }
    // 3. Load + replay the orders as written: the bench's own path; it must give the recording run's CRCs.
    commands::CommandRecording replay;
    CrcLog replayed;
    if (!commands::CommandRecording::load(tmp_orders, replay, &err) || !run(&replay, nullptr, replayed)) {
        std::fprintf(stderr, "bench-make: replay run: %s\n", err.c_str());
        cleanup();
        return 1;
    }
    const CrcDivergence d = first_divergence(recorded, replayed);
    if (d.diverged) {
        std::fprintf(stderr, "bench-make: the scenario does not rerun identically from its save (%s)\n", d.message.c_str());
        cleanup();
        return 1;
    }
    CrcLog fp = replayed;
    fp.comments = {"bench scenario " + name + ": " + src->describe() + ", from tick " + std::to_string(saved_tick) + ", " +
                   std::to_string(opt.ticks) + " ticks"};
    if (opt.fingerprint_every > 1) fp = fp.checkpointed(opt.fingerprint_every);
    if (!fp.save(tmp_fp, &err)) {
        std::fprintf(stderr, "bench-make: %s\n", err.c_str());
        cleanup();
        return 1;
    }

    // 4. Move the files in place, pin them, write the manifest (version kept when nothing changed).
    bench::Scenario sc;
    sc.name = name;
    sc.make_args = opt.make_args;
    sc.save.path = save_name;
    sc.orders.path = orders_name;
    sc.fingerprint.path = fp_name;
    sc.fingerprint_every = std::max<uint32_t>(opt.fingerprint_every, 1);
    sc.start_tick = saved_tick;
    sc.ticks = opt.ticks;
    sc.workers = opt.workers_set ? opt.workers : -1;
    sc.final_crc = replayed.records.empty() ? 0 : replayed.records.back().combined;
    sc.dir = dir.string();
    bench::Scenario old;
    const bool had_old = bench::Scenario::load(opt.scenario, old, nullptr);
    sc.description = !opt.description.empty() ? opt.description : had_old ? old.description : std::string();
    if (had_old) {  // BM2 format-2 keys (the rendered run's settings and camera) stay with the scenario
        sc.copy_run_settings(old);
        sc.host = bench::Host::Headless;
    }
    // Everything is written next to its final name first (pins taken from the written bytes), then moved in place with the
    // manifest last, so a failure before the moves leaves the old scenario untouched.
    for (auto [tmp, file] : {std::pair{tmp_save, &sc.save}, std::pair{tmp_orders, &sc.orders}, std::pair{tmp_fp, &sc.fingerprint}}) {
        if (!bench::pin_file(tmp, *file, &err)) {
            std::fprintf(stderr, "bench-make: %s\n", err.c_str());
            cleanup();
            return 1;
        }
    }
    sc.version = had_old ? (old.same_content(sc) ? old.version : old.version + 1) : 1;
    const std::string tmp_manifest = opt.scenario + ".tmp";
    if (!sc.save_manifest(tmp_manifest, &err)) {
        std::fprintf(stderr, "bench-make: %s\n", err.c_str());
        cleanup();
        return 1;
    }
    for (auto [tmp, final_path] : {std::pair{tmp_save, in_dir(save_name)}, std::pair{tmp_orders, in_dir(orders_name)},
                                   std::pair{tmp_fp, in_dir(fp_name)}, std::pair{tmp_manifest, opt.scenario}}) {
        fs::rename(fs::path(tmp), fs::path(final_path), ec);
        if (ec) {
            std::fprintf(stderr, "bench-make: %s: %s (rerun --bench-make)\n", final_path.c_str(), ec.message().c_str());
            return 1;
        }
    }
    std::printf("bench-make: wrote %s version %u%s: save %" PRIu64 " bytes, %zu orders, fingerprint %zu records, final CRC %s\n",
                opt.scenario.c_str(), sc.version, had_old ? (sc.version == old.version ? " (unchanged)" : " (changed: version bumped)") : "",
                sc.save.bytes, rec.command_count(), fp.records.size(), bench::hex64(sc.final_crc).c_str());
    return 0;
}
