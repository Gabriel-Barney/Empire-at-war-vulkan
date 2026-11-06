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

namespace bench = alamo::sim::bench;

static int g_fail = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)

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
    CHECK(!bench::Scenario::parse("ALAMO_BENCH 2\nname x\n", r, &err) && err.find("format") != std::string::npos);
    CHECK(!bench::Scenario::parse("name x\n", r, &err));
    CHECK(!bench::Scenario::parse(text + "bogus 1\n", r, &err) && err.find("unknown key") != std::string::npos);
    CHECK(!bench::Scenario::parse("ALAMO_BENCH 1\nname x\nticks 5\n", r, &err) && err.find("missing") != std::string::npos);
    CHECK(!bench::Scenario::parse("ALAMO_BENCH 1\nname two words\n", r, &err));
    CHECK(!bench::Scenario::parse("ALAMO_BENCH 1\nname x\nsave a.alsv 1 zz\n", r, &err));
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
