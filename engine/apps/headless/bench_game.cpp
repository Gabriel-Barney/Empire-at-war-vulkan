// alamo_headless bm2c t02: `host game` benchmark scenarios (alamo_game --bench-make / --bench-regen) timed sim-only by --bench.
//
// The scenario's save is an alamo_game session (game.skirmish / game.campaign), so the session is built the way alamo_game
// loads a save (GameSession::spec_from_save -> GameSession -> start_from_save, the render-free alamo_game_core; the same
// path as alamo_game's presentation on/off check), with the scenario's recorded orders as its replay. Each tick is one
// GameSession::tick (SimHost::tick_once + the session's after-tick step); nothing is drawn, nothing paces the ticks. The
// per-tick CRC log is the one the rendered run made the fingerprint from, so bench_run.cpp's fingerprint check, timing,
// zones and result JSON apply unchanged. Built only with alamo_game_core (ALAMO_HEADLESS_HAVE_GAME_CORE).
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "alamo/commands/recording.hpp"
#include "alamo/core/save_file.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/game/game_data.hpp"
#include "alamo/game/game_session.hpp"
#include "alamo/jobs/job_system.hpp"
#include "alamo/sim/save_session.hpp"
#include "alamo/sim/sim_host.hpp"
#include "bench_run.hpp"

namespace {

using namespace alamo;

std::string env_text(const char* name) {
#ifdef _WIN32
    char* v = nullptr;
    size_t n = 0;
    if (_dupenv_s(&v, &n, name) != 0 || !v) return {};
    std::string s(v);
    std::free(v);
    return s;
#else
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
#endif
}

// The job system outlives the session (declared first, destroyed last).
class GameLoadedSession final : public LoadedSession {
public:
    GameLoadedSession(std::unique_ptr<jobs::JobSystem> js, std::unique_ptr<game::GameSession> s) : js_(std::move(js)), s_(std::move(s)) {}
    ~GameLoadedSession() override {
        s_.reset();
        js_.reset();
    }
    bool tick(std::string& err) override {
        if (s_->tick()) return true;
        err = "the game session stopped at tick " + std::to_string(s_->host().frame());
        return false;
    }
    const CrcLog& crc_log() const override { return s_->host().crc_log(); }
    uint64_t frame() const override { return s_->host().frame(); }
    size_t replay_failures() const override { return s_->host().pipeline().replay_failures(); }
    uint32_t worker_count() const override { return js_->worker_count(); }

private:
    std::unique_ptr<jobs::JobSystem> js_;
    std::unique_ptr<game::GameSession> s_;
};

// The save and the game data are read once; every load() builds a fresh session over them (sessions are destroyed
// before the source).
class GameSaveSource final : public SaveSessionSource {
public:
    SaveFile save;
    sim::SessionDescriptor d;
    game::GameData data;

    uint64_t save_tick() const override { return save.tick; }
    std::string describe() const override {
        const std::string map = d.option("map");
        return d.kind + " '" + d.campaign + "' seed " + std::to_string(d.seed) + (map.empty() ? std::string() : " map " + map);
    }
    std::unique_ptr<LoadedSession> load(int32_t workers, const commands::CommandRecording* replay, commands::CommandRecording* record,
                                        std::string& err) override {
        game::SessionSpec spec;
        if (!game::GameSession::spec_from_save(save, spec, &err)) return nullptr;
        auto js = std::make_unique<jobs::JobSystem>(jobs::JobSystemConfig{workers});
        auto s = std::make_unique<game::GameSession>(data, *js, std::move(spec));
        if (!s->start_from_save(save, &err, replay, record)) {
            err = "load refused: " + err;
            return nullptr;
        }
        return std::make_unique<GameLoadedSession>(std::move(js), std::move(s));
    }
};

}  // namespace

int open_game_save_source(const std::string& save_path, const std::string& game_dir, const std::vector<std::string>& game_args,
                          std::unique_ptr<SaveSessionSource>& out, std::string& err) {
    auto src = std::make_unique<GameSaveSource>();
    if (!SaveFile::read_file(save_path, src->save, &err) || !sim::read_session(src->save, src->d, &err)) return -1;
    if (src->d.kind != game::GameSession::kSkirmishKind && src->d.kind != game::GameSession::kCampaignKind) {
        err = "the save holds a '" + src->d.kind + "' session, not an alamo_game session";
        return -1;
    }
    // The game data: --game, else ALAMO_FOC_DATA (a committed scenario's descriptor names no folder); the VFS arguments the
    // save was made with unless given on the command line.
    const std::string root = game_dir.empty() ? env_text("ALAMO_FOC_DATA") : game_dir;
    if (root.empty()) {
        err = "no --game <corruption dir> and ALAMO_FOC_DATA unset";
        return 0;
    }
    const std::vector<std::string>& args = game_args.empty() ? src->d.data_args : game_args;
    if (!game::load_game_data(root, args, src->data, &err)) {
        err = "game data " + root + ": " + err;
        return -1;
    }
    out = std::move(src);
    return 1;
}
