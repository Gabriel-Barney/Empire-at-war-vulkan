// BM1 benchmark core (no game data): the scenario manifest (bench_scenario.hpp), tick statistics and the comparison rule
// (bench_stats.hpp), and the result JSON value (bench_json.hpp). The end-to-end run is alamo_headless_bench_synthetic.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "alamo/sim/bench_json.hpp"
#include "alamo/sim/bench_scenario.hpp"
#include "alamo/sim/bench_stats.hpp"
#include "alamo/test/support.hpp"

namespace bench = alamo::sim::bench;

using ::alamo::test::g_fail;

static bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

static bench::Scenario sample() {
    bench::Scenario s;
    s.name = "space_battle";
    s.version = 3;
    s.description = "one large space battle (FoC _MP_SPACE_KUAT)";
    s.make_args = {"--save-session", "skirmish", "--campaign", "foc-fleet", "--bench-desc", "a \"quoted\" text\\with spaces", ""};
    s.save = {"space_battle.alsv", 123456, 0x0123456789abcdefull};
    s.orders = {"space_battle.acmd", 77, 0xfedcba9876543210ull};
    s.fingerprint = {"space_battle.crc", 99999, 0x1ull};
    s.fingerprint_every = 1;
    s.start_tick = 312;
    s.ticks = 1800;
    s.workers = -1;
    s.final_crc = 0xdeadbeefcafef00dull;
    return s;
}

static void test_manifest() {
    const bench::Scenario s = sample();
    const std::string text = s.to_text();
    CHECK(text.rfind("ALAMO_BENCH 1\nname space_battle\nversion 3\n", 0) == 0);
    CHECK(text.find("workers auto\n") != std::string::npos);
    CHECK(text.find("final_crc deadbeefcafef00d\n") != std::string::npos);
    bench::Scenario r;
    std::string err;
    CHECK(bench::Scenario::parse(text, r, &err));
    CHECK(r.name == s.name && r.version == s.version && r.description == s.description);
    CHECK(r.make_args == s.make_args);
    CHECK(r.save == s.save && r.orders == s.orders && r.fingerprint == s.fingerprint);
    CHECK(r.start_tick == 312 && r.ticks == 1800 && r.workers == -1 && r.final_crc == s.final_crc);
    CHECK(r.to_text() == text);  // byte-stable
    CHECK(r.same_content(s));
    bench::Scenario changed = s;
    changed.fingerprint.digest = 2;
    CHECK(!changed.same_content(s));

    // No orders, explicit workers, CRLF and comments.
    bench::Scenario s2 = s;
    s2.orders = {};
    s2.workers = 4;
    std::string t2 = "# a comment\r\n" + s2.to_text();
    CHECK(t2.find("orders -\n") != std::string::npos);
    CHECK(bench::Scenario::parse(t2, r, &err) && !r.orders.present() && r.workers == 4);

    // Errors.
    CHECK(!bench::Scenario::parse("ALAMO_BENCH 3\nname x\n", r, &err) && err.find("format") != std::string::npos);
    CHECK(!bench::Scenario::parse("name x\n", r, &err));
    CHECK(!bench::Scenario::parse(text + "bogus 1\n", r, &err) && err.find("unknown key") != std::string::npos);
    CHECK(!bench::Scenario::parse("ALAMO_BENCH 1\nname x\nticks 5\n", r, &err) && err.find("missing") != std::string::npos);
    CHECK(!bench::Scenario::parse("ALAMO_BENCH 1\nname two words\n", r, &err));
    CHECK(!bench::Scenario::parse("ALAMO_BENCH 1\nname x\nsave a.alsv 1 zz\n", r, &err));
}

// BM2 format 2: alamo_game's run settings and the scripted camera; format 1 stays byte for byte.
static void test_format2() {
    const bench::Scenario v1 = sample();
    CHECK(!v1.has_v2());
    CHECK(v1.to_text().rfind("ALAMO_BENCH 1\n", 0) == 0);  // no format-2 key: still written as format 1
    bench::Scenario s = v1;
    s.host = bench::Host::Game;
    s.mode = "fixed";
    s.preset = "bm-smoke";
    s.frames_per_tick = 3;
    s.camera_anchor = bench::CameraAnchor::Start;
    bench::CameraKey k0;
    k0.tick = 0;
    k0.target[0] = 1.5f;
    k0.target[1] = -2.0f;
    k0.target[2] = 0.1f;  // not exact in binary: the text must still round-trip the bits
    k0.yaw_deg = 0;
    k0.pitch_deg = 30;
    k0.distance = 1200;
    bench::CameraKey k1 = k0;
    k1.tick = 60;
    k1.yaw_deg = 720;
    k1.pitch_deg = 45;
    k1.distance = 600;
    k1.fov_deg = 50;
    s.camera = {k0, k1};
    CHECK(s.has_v2());
    const std::string text = s.to_text();
    CHECK(text.rfind("ALAMO_BENCH 2\n", 0) == 0);
    CHECK(text.find("final_crc deadbeefcafef00d\nhost game\nmode fixed\npreset bm-smoke\nframes_per_tick 3\ncamera_anchor start\n"
                    "camera 0 1.5 -2 0.1 0 30 1200\ncamera 60 1.5 -2 0.1 720 45 600 50\n") != std::string::npos);
    bench::Scenario r;
    std::string err;
    CHECK(bench::Scenario::parse(text, r, &err));
    CHECK(r.host == bench::Host::Game && r.mode == "fixed" && r.preset == "bm-smoke" && r.frames_per_tick == 3);
    CHECK(r.camera_anchor == bench::CameraAnchor::Start && r.camera == s.camera);
    CHECK(r.to_text() == text && r.same_content(s));
    bench::Scenario moved = s;
    moved.camera[1].yaw_deg = 360;  // another camera path is another scenario (the version bumps on a remake)
    CHECK(!moved.same_content(s));
    bench::Scenario copy = v1;
    copy.copy_run_settings(s);
    CHECK(copy.same_content(s));

    // Format-2 keys need format 2; bad values; key order.
    CHECK(!bench::Scenario::parse(v1.to_text() + "host game\n", r, &err) && err.find("format-2 key") != std::string::npos);
    const std::string head = "ALAMO_BENCH 2\nname x\nsave a 1 1\nfingerprint b 1 1\nticks 5\n";
    CHECK(bench::Scenario::parse(head, r, &err) && r.host == bench::Host::Headless && !r.has_v2());
    CHECK(!bench::Scenario::parse(head + "host both\n", r, &err));
    CHECK(!bench::Scenario::parse(head + "mode slow\n", r, &err));
    CHECK(!bench::Scenario::parse(head + "frames_per_tick 0\n", r, &err));
    CHECK(!bench::Scenario::parse(head + "camera_anchor here\n", r, &err));
    CHECK(!bench::Scenario::parse(head + "camera 0 0 0 0 0 30\n", r, &err));         // too few values
    CHECK(!bench::Scenario::parse(head + "camera 0 0 0 0 0 90 100\n", r, &err));     // pitch out of range
    CHECK(!bench::Scenario::parse(head + "camera 0 0 0 0 0 30 0\n", r, &err));       // distance 0
    CHECK(!bench::Scenario::parse(head + "camera 0 0 0 0 0 30 1e\n", r, &err));      // not a number
    CHECK(!bench::Scenario::parse(head + "camera 5 0 0 0 0 30 100\ncamera 5 0 0 0 0 30 100\n", r, &err) &&
          err.find("increase") != std::string::npos);
    CHECK(bench::Scenario::parse(head + "camera 0 0 0 0 0 30 100\ncamera 9 0 0 0 0 30 100 60\n", r, &err) && r.camera.size() == 2);

    // A recipe: settings and camera, no pins.
    CHECK(!bench::Scenario::parse("ALAMO_BENCH 2\nname x\nhost game\n", r, &err));
    CHECK(bench::Scenario::parse_recipe("ALAMO_BENCH 2\nname x\nhost game\ncamera 0 0 0 0 0 30 100\n", r, &err) &&
          r.host == bench::Host::Game && r.camera.size() == 1 && !r.save.present());
    CHECK(!bench::Scenario::parse_recipe("ALAMO_BENCH 2\nhost game\n", r, &err));  // a name is still needed
}

// The camera path: a function of the tick count and the render alpha only.
static void test_camera() {
    CHECK(bench::camera_time(0, 0.5f) == 0.0);  // before the first tick: the first pose
    CHECK(bench::camera_time(1, 0.5f) == 0.5);
    CHECK(bench::camera_time(10, 1.0f) == 10.0);
    CHECK(bench::camera_time(10, 2.0f) == 10.0);  // alpha clamped to 0..1
    bench::CameraKey a;
    a.tick = 10;
    a.target[0] = 100;
    a.yaw_deg = 0;
    a.pitch_deg = 20;
    a.distance = 1000;
    bench::CameraKey b = a;
    b.tick = 20;
    b.target[0] = 200;
    b.yaw_deg = 360;
    b.pitch_deg = 40;
    b.distance = 500;
    b.fov_deg = 60;
    bench::CameraKey c = b;
    c.tick = 30;
    c.fov_deg = 40;
    const std::vector<bench::CameraKey> keys = {a, b, c};
    CHECK(bench::camera_pose(keys, 0.0) == a);  // held before the first key
    CHECK(bench::camera_pose(keys, 99.0) == c); // and after the last
    const bench::CameraKey m = bench::camera_pose(keys, 15.0);
    CHECK(m.target[0] == 150.0f && m.yaw_deg == 180.0f && m.pitch_deg == 30.0f && m.distance == 750.0f);
    CHECK(m.fov_deg == 0.0f);  // a has no fov: the earlier key's (the view's own)
    const bench::CameraKey q = bench::camera_pose(keys, 25.0);
    CHECK(q.fov_deg == 50.0f && q.yaw_deg == 360.0f);
    CHECK(bench::camera_pose(keys, 20.0) == bench::camera_pose(keys, 20.0));
    const bench::CameraKey at = bench::camera_pose(keys, 20.0);
    CHECK(at.target[0] == 200.0f && at.yaw_deg == 360.0f);  // exactly the key at its tick
    CHECK(bench::camera_pose({a}, 15.0) == a);
}

static void test_files() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "alamo_sim_bench_tests";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    auto write = [&](const char* name, const std::string& bytes) {
        std::ofstream f(dir / name, std::ios::binary | std::ios::trunc);
        f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    };
    write("t.alsv", "save bytes");
    write("t.crc", "fingerprint");
    bench::Scenario s;
    s.name = "t";
    s.ticks = 10;
    s.save.path = "t.alsv";
    s.fingerprint.path = "t.crc";
    s.dir = dir.string();
    std::string err;
    CHECK(bench::pin_file(s.resolve(s.save), s.save, &err) && s.save.bytes == 10);
    CHECK(bench::pin_file(s.resolve(s.fingerprint), s.fingerprint, &err));
    CHECK(bench::verify_files(s, &err));
    CHECK(s.save_manifest((dir / "t.abench").string(), &err));
    bench::Scenario r;
    CHECK(bench::Scenario::load((dir / "t.abench").string(), r, &err) && r.dir == dir.string() && bench::verify_files(r, &err));
    write("t.crc", "fingerprinT");  // same size, other bytes
    CHECK(!bench::verify_files(r, &err) && err.find("t.crc differs") != std::string::npos);
    fs::remove(dir / "t.alsv", ec);
    CHECK(!bench::verify_files(r, &err) && err.find("cannot read") != std::string::npos);
    fs::remove_all(dir, ec);
}

static void test_stats() {
    const std::vector<double> odd = {5, 1, 3};
    bench::TickStats s = bench::summarize(odd);
    CHECK(s.count == 3 && near(s.median_ms, 3) && near(s.worst_ms, 5) && s.worst_index == 0 && near(s.mean_ms, 3) && near(s.total_ms, 9));
    const std::vector<double> even = {4, 1, 3, 2};
    s = bench::summarize(even);
    CHECK(near(s.median_ms, 2.5) && near(s.worst_ms, 4) && s.worst_index == 0);
    std::vector<double> hundred;
    for (int i = 1; i <= 200; ++i) hundred.push_back(i);  // nearest rank: ceil(0.99 * 200) = 198
    s = bench::summarize(hundred);
    CHECK(near(s.p99_ms, 198) && near(s.worst_ms, 200) && s.worst_index == 199 && near(s.median_ms, 100.5));
    CHECK(near(bench::percentile(hundred, 50), 100));
    CHECK(near(bench::percentile(hundred, 100), 200));
    for (size_t n : {100u, 300u, 700u, 1800u, 5400u}) {  // whole-number ranks: ceil(0.99 * n) without floating-point round-up
        std::vector<double> v;
        for (size_t i = 1; i <= n; ++i) v.push_back(static_cast<double>(i));
        CHECK(near(bench::percentile(v, 99), static_cast<double>((99 * n + 99) / 100)));
    }
    s = bench::summarize(std::vector<double>{});
    CHECK(s.count == 0 && s.median_ms == 0);
    s = bench::summarize(std::vector<double>{7});
    CHECK(near(s.median_ms, 7) && near(s.p99_ms, 7));
}

static void test_compare() {
    bench::ResultKey a{"space_battle", 2, "00000000000000aa", 1800, true};
    bench::TickStats sa;
    sa.count = 1800;
    sa.median_ms = 20;
    sa.p99_ms = 400;
    sa.worst_ms = 800;
    bench::TickStats sb = sa;
    sb.median_ms = 10;
    sb.p99_ms = 500;
    bench::Comparison c = bench::compare(a, sa, a, sb);
    CHECK(c.comparable && near(c.median_ratio, 2.0) && near(c.p99_ratio, 0.8));
    CHECK(c.text.find("2.00x faster") != std::string::npos);
    c = bench::compare(a, sb, a, sa);
    CHECK(c.comparable && c.text.find("2.00x slower") != std::string::npos);
    auto refused = [&](bench::ResultKey b, const char* why) {
        const bench::Comparison x = bench::compare(a, sa, b, sb);
        CHECK(!x.comparable);
        CHECK(x.text.find("scenario changed, not comparable") != std::string::npos);
        CHECK(x.reason.find(why) != std::string::npos);
        CHECK(x.median_ratio == 0);
    };
    bench::ResultKey b = a;
    b.version = 3;
    refused(b, "version");
    b = a;
    b.fingerprint_digest = "00000000000000bb";
    refused(b, "fingerprint");
    b = a;
    b.fingerprint_ok = false;
    refused(b, "did not match");
    b = a;
    b.scenario = "other";
    refused(b, "other scenario");
    b = a;
    b.ticks = 900;
    refused(b, "ticks");
}

static void test_json() {
    bench::Json r = bench::Json::object();
    r.set("name", bench::Json::string("a \"b\"\n\tc\\"));
    r.set("n", bench::Json::number(1.25));
    r.set("big", bench::Json::number(12345678901.0));
    r.set("neg", bench::Json::number(-3));
    r.set("ok", bench::Json::boolean(true));
    r.set("none", bench::Json());
    bench::Json arr = bench::Json::array();
    arr.push(bench::Json::number(1));
    arr.push(bench::Json::number(2));
    r.set("list", std::move(arr));
    bench::Json inner = bench::Json::object();
    inner.set("version", bench::Json::number(4));
    bench::Json objs = bench::Json::array();
    objs.push(inner);
    r.set("scenario", inner);
    r.set("objs", std::move(objs));
    r.set("n", bench::Json::number(2.5));  // replace keeps the position
    const std::string text = r.to_text();
    CHECK(text.find("\"n\": 2.5") != std::string::npos && text.find("\"n\"") < text.find("\"big\""));
    CHECK(text.find("\"big\": 12345678901") != std::string::npos);
    CHECK(text.find("\"list\": [1, 2]") != std::string::npos);
    bench::Json p;
    std::string err;
    CHECK(bench::Json::parse(text, p, &err));
    CHECK(p.to_text() == text);
    CHECK(p.find("name") && p.find("name")->as_string() == "a \"b\"\n\tc\\");
    CHECK(p.at("scenario.version") && near(p.at("scenario.version")->as_number(), 4));
    CHECK(p.at("scenario.missing") == nullptr && p.at("name.x") == nullptr);
    CHECK(p.find("ok")->as_bool() && p.find("none")->is_null() && p.find("objs")->items().size() == 1);
    CHECK(bench::Json::parse("{\"u\": \"\\u0041\\u00e9\", \"e\": 1e3}", p, &err) && p.find("u")->as_string() == "A?" &&
          near(p.find("e")->as_number(), 1000));
    CHECK(!bench::Json::parse("{\"a\": }", p, &err));
    CHECK(!bench::Json::parse("{\"a\": 1} x", p, &err) && err.find("trailing") != std::string::npos);
    CHECK(!bench::Json::parse("[1, 2", p, &err));
    CHECK(!bench::Json::parse("\"abc", p, &err));
}

int main() {
    test_manifest();
    test_format2();
    test_camera();
    test_files();
    test_stats();
    test_compare();
    test_json();
    if (g_fail) {
        std::printf("alamo_sim_bench_tests: %d check(s) FAILED\n", g_fail);
        return 1;
    }
    std::printf("alamo_sim_bench_tests: OK\n");
    return 0;
}
