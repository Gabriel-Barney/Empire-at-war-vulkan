// alamo_anim tests (R3, docs/re/ANIM_PARTICLES.md Â§A):
//   * type table (119 ids, names, suffixes incl. CONTAIMINATE_LOOP, predicates), clip file names;
//   * frame rules: loops wrap at count - 1 (planet idle: one revolution = 239 s), one-shots stop at count - 2;
//   * play rules: lock, random variant draws, type -1, crossfade start / keep-previous / modes 2-3, timed
//     overlay, start frame, phase-preserving switch;
//   * crossfade bit pattern (nlerp with the retail fast inverse sqrt) against golden bit patterns;
//   * idle-fidget RNG stream (variant [1, n-1], timer 210..450) against an independent replay of the draws;
//   * poses: bind fallback for unbound clips, single-bone reads == full pose, billboard copy-parent;
//   * determinism: 4000 objects, 300 frames, state hash + poses identical for serial and 1/4/15 workers;
//   * animation events: primary and overlay slots, primary first, overlay loop wrap, cleared / timed overlays;
//   * real data (ALAMO_FOC_DATA): discovery + binding over every shipped model, planet frame 239 == frame 0.
#include <algorithm>
#include <atomic>
#include <bit>
#include <cctype>
#include <cinttypes>
#include <memory>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "alamo/anim/anim_behaviors.hpp"
#include "alamo/anim/anim_events.hpp"
#include "alamo/anim/anim_state.hpp"
#include "alamo/anim/anim_types.hpp"
#include "alamo/anim/clip.hpp"
#ifdef ALAMO_ANIM_HAVE_OBJECTS
#include "alamo/anim/type_params.hpp"
#endif
#include "alamo/core/bytes.hpp"
#include "alamo/core/dmath.hpp"
#include "alamo/core/hash.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/core/sim_rng.hpp"
#ifdef ALAMO_ANIM_HAVE_JOBS
#include "alamo/jobs/job_system.hpp"
#endif
#ifdef ALAMO_ANIM_HAVE_VFS
#include "alamo/vfs/vfs.hpp"
#endif

using namespace alamo::anim;
using alamo::SimRng;

namespace {

int g_fail = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) {                                                               \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);              \
            ++g_fail;                                                             \
        }                                                                         \
    } while (0)

// ---- synthetic data -----------------------------------------------------------------------------------

int16_t q16(double v) { return static_cast<int16_t>(std::lround(v * 32767.0)); }

// Clip of `frames` frames at `fps` driving `bones` ALA bones: bone i rotates about Z by deg_per_frame*(i+1)
// per frame and translates along X by frame * 0.5 (u16 scale 0.01).
std::shared_ptr<Clip> make_clip(const std::string& name, uint32_t frames, float fps, uint32_t bones, double deg_per_frame,
                                bool vis_toggle = false) {
    auto c = std::make_shared<Clip>();
    c->name = name;
    c->name_crc = alamo::name_crc(name);
    c->frame_count = frames;
    c->fps = fps;
    c->bone_count = bones;
    for (uint32_t i = 0; i < bones; ++i) {
        ClipTrack t;
        t.bone_name = "B" + std::to_string(i + 1);
        t.file_bone_index = i + 1;
        t.overlay_excluded = (i % 2) == 1;
        t.rot_animated = frames > 0;
        t.trans_animated = frames > 0;
        t.trans_offset = {0.0f, 1.0f, 0.0f};
        t.trans_scale = {0.01f, 0.0f, 0.0f};
        t.rot_first = static_cast<uint32_t>(c->rot_raw.size() / 4);
        t.trans_first = static_cast<uint32_t>(c->trans_raw.size() / 3);
        for (uint32_t f = 0; f < std::max(frames, 1u); ++f) {
            const double a = deg_per_frame * (i + 1) * f * 3.14159265358979323846 / 180.0;
            c->rot_raw.push_back(0);
            c->rot_raw.push_back(0);
            c->rot_raw.push_back(q16(std::sin(a / 2)));
            c->rot_raw.push_back(q16(std::cos(a / 2)));
            c->trans_raw.push_back(static_cast<uint16_t>(f * 50));
            c->trans_raw.push_back(0);
            c->trans_raw.push_back(0);
        }
        if (vis_toggle) {
            t.has_visibility = true;
            t.vis_first = static_cast<uint32_t>(c->vis_bits.size());
            for (uint32_t b = 0; b < (frames + 7) / 8; ++b) c->vis_bits.push_back(0x55);  // even frames visible
        }
        c->tracks.push_back(t);
    }
    return c;
}

// Skeleton Root -> B1 -> B2 -> ... (chain), bind locals translate by 1 along Z; bone `billboard` (if >0) copies
// its parent (mode 6).
Skeleton make_skeleton(uint32_t bones, int32_t billboard = -1) {
    Skeleton s;
    for (uint32_t b = 0; b < bones; ++b) {
        s.names.push_back(b == 0 ? "Root" : "B" + std::to_string(b));
        s.parents.push_back(static_cast<int32_t>(b) - 1);
        s.billboard_mode.push_back(static_cast<int32_t>(b) == billboard ? 6u : 0u);
        Mat34 m;
        m.m[2][3] = b == 0 ? 0.0f : 1.0f;
        s.bind_local.push_back(m);
    }
    return s;
}

// Model with `bones` skeleton bones and clips per type: {type, variants, frames, fps, ala_bones}.
struct ClipSpec {
    int32_t type;
    uint32_t variants;
    uint32_t frames;
    float fps;
    uint32_t ala_bones;  // == bones - 1 to bind
    double deg = 3.0;
};
std::unique_ptr<AnimModel> make_model(uint32_t bones, std::initializer_list<ClipSpec> specs, int32_t billboard = -1) {
    auto m = std::make_unique<AnimModel>();
    m->model = "DATA\\ART\\MODELS\\TEST_UNIT.ALO";
    m->base = "TEST_UNIT";
    m->skeleton = make_skeleton(bones, billboard);
    m->has_skeleton = true;
    for (const ClipSpec& s : specs)
        for (uint32_t v = 0; v < s.variants; ++v) {
            ClipRef r;
            r.clip = make_clip(clip_file_name("TEST_UNIT", s.type, v), s.frames, s.fps, s.ala_bones, s.deg + v);
            m->clips[static_cast<size_t>(s.type)].push_back(r);
        }
    bind_clips(*m);
    return m;
}

uint64_t world_hash(const AnimWorld& w) {
    alamo::StateHasher h;
    w.hash_state(h);
    return h.digest();
}

bool mat_bits_equal(const Mat34& a, const Mat34& b) { return std::memcmp(&a, &b, sizeof a) == 0; }

// ---- tests --------------------------------------------------------------------------------------------

void test_types() {
    CHECK(kAnimTypeCount == 119);
    CHECK(anim_type_suffix(0) == "IDLE" && anim_type_suffix(2) == "MOVE" && anim_type_name(2) == "Move");
    CHECK(anim_type_suffix(113) == "CONTAIMINATE_LOOP");
    CHECK(anim_type_name(113) == "Contaminate_Loop");
    CHECK(anim_type_suffix(90) == "HACKING" && anim_type_name(90).empty() && anim_type_name(91).empty());
    CHECK(anim_type_suffix(118) == "HEAL" && anim_type_suffix(119).empty() && anim_type_suffix(-1).empty());
    CHECK(find_anim_type("DIE") == 7 && find_anim_type("fl_die") == 41 && find_anim_type("Crushed") == 101);
    CHECK(find_anim_type("Space Idle") == 1 && find_anim_type("Move by walking") == 66 && find_anim_type("nope") == kNoAnimType);
    CHECK(find_anim_type("") == kNoAnimType);
    // Every id except 90/91 has a unique, findable name; every suffix is non-empty and unique.
    std::map<std::string, int> suffixes;
    for (int32_t i = 0; i < 119; ++i) {
        CHECK(!anim_type_suffix(i).empty());
        ++suffixes[std::string(anim_type_suffix(i))];
        if (i != 90 && i != 91) CHECK(find_anim_type(anim_type_name(i)) == i);
    }
    CHECK(suffixes.size() == 119);
    int attack = 0, idle = 0, turn = 0, crouch = 0, flinch = 0, pose = 0;
    for (int32_t i = 0; i < 119; ++i) {
        attack += is_attack_like(i);
        idle += is_idle_like(i);
        turn += is_turn(i);
        crouch += is_crouch(i);
        flinch += is_flinch(i);
        pose += is_one_shot_pose(i);
    }
    CHECK(attack == 16 && idle == 9 && turn == 8 && crouch == 4 && flinch == 8 && pose == 9);
    CHECK(clip_file_name("EI_Trooper", 0, 0) == "EI_TROOPER_IDLE_00.ALA");
    CHECK(clip_file_name("UI_IG88", 113, 1) == "UI_IG88_CONTAIMINATE_LOOP_01.ALA");
    CHECK(clip_file_name("X", 5, 123) == "X_ATTACK_123.ALA");
    CHECK(model_base_name("Data\\Art\\Models\\w_planet_kuat_high.alo") == "W_PLANET_KUAT_HIGH");
    CHECK(model_base_name("EV_Tie_Fighter") == "EV_TIE_FIGHTER");
    std::printf("types: 119 ids, groups attack %d idle %d turn %d crouch %d flinch %d one-shot-pose %d\n", attack, idle, turn, crouch, flinch, pose);
}

void test_frame_rules() {
    // Loops wrap at count - 1: a 240-frame clip never samples frame 239.
    CHECK(advance_loop(238.0f, 0.5f, 240) == 238.5f);
    CHECK(advance_loop(238.5f, 0.5f, 240) == 0.0f);
    CHECK(advance_loop(238.5f, 1.0f, 240) == 0.5f);
    CHECK(advance_loop(5.0f, 1.0f, 1) == 0.0f && advance_loop(5.0f, 1.0f, 0) == 0.0f);
    CHECK(advance_loop(0.0f, 10.5f, 4) == 1.5f);  // 10.5 - 3 - 3 - 3
    // One-shots stop at count - 2 and finish.
    bool fin = false;
    CHECK(advance_one_shot(7.0f, 0.5f, 10, fin) == 7.5f && !fin);
    CHECK(advance_one_shot(7.5f, 0.5f, 10, fin) == 8.0f && fin);
    CHECK(advance_one_shot(7.5f, 5.0f, 10, fin) == 8.0f && fin);
    CHECK(advance_one_shot(0.0f, 0.0f, 1, fin) == 0.0f && fin);
    // Sampling keys: wrapped into [0, count - 1).
    uint32_t a = 0, b = 0;
    float t = 0;
    sample_keys(238.25f, 240, a, b, t);
    CHECK(a == 238 && b == 0 && t == 0.25f);
    sample_keys(3.5f, 1, a, b, t);
    CHECK(a == 0 && b == 0 && t == 0.0f);
    sample_keys(8.0f, 10, a, b, t);
    CHECK(a == 8 && b == 0 && t == 0.0f);

    // Planet idle: 240 frames at 1 fps, looped at rate 1 -> one revolution every 239 s (7170 ticks).
    auto m = make_model(2, {{kAnimIdle, 1, 240, 1.0f, 1, 1.5}});
    AnimWorld w;
    AnimCreateParams p;
    p.loop_idle = true;
    w.create(1, m.get(), p, nullptr, 0);
    float max_frame = 0.0f;
    uint64_t wraps = 0;
    float prev = 0.0f;
    for (uint64_t f = 1; f <= 239 * 30 * 2; ++f) {
        w.advance(f);
        const float fr = w.slot(1).frame;
        max_frame = std::max(max_frame, fr);
        if (fr < prev) {
            ++wraps;
            if (wraps == 1) {
                const double secs = static_cast<double>(f) / 30.0;
                std::printf("planet idle: first wrap at tick %" PRIu64 " (%.3f s), frame %.6f\n", f, secs, static_cast<double>(fr));
                CHECK(std::fabs(secs - 239.0) < 0.05);
            }
        }
        prev = fr;
    }
    CHECK(wraps == 2);
    CHECK(max_frame < 239.0f);
    // At rate 0.25 (Coruscant_Backdrop) the period is 956 s.
    AnimWorld w2;
    p.idle_rate = 0.25f;
    w2.create(1, m.get(), p, nullptr, 0);
    uint64_t first_wrap = 0;
    prev = 0.0f;
    for (uint64_t f = 1; f <= 1000 * 30 && !first_wrap; ++f) {
        w2.advance(f);
        if (w2.slot(1).frame < prev) first_wrap = f;
        prev = w2.slot(1).frame;
    }
    std::printf("planet idle at rate 0.25: first wrap at %.3f s\n", static_cast<double>(first_wrap) / 30.0);
    CHECK(first_wrap > 0 && std::fabs(static_cast<double>(first_wrap) / 30.0 - 956.0) < 0.2);
}

void test_play_rules() {
    auto m = make_model(4, {{kAnimIdle, 3, 20, 30.0f, 3}, {kAnimAttack, 2, 12, 30.0f, 3}, {kAnimDie, 1, 31, 30.0f, 3},
                            {kAnimMove, 1, 40, 30.0f, 3}, {kAnimWalkMove, 1, 20, 30.0f, 3}, {kAnimCinematic, 1, 1, 30.0f, 3}});
    AnimWorld w;
    SimRng rng(77);
    w.add(5, m.get(), 0);
    PlayRequest req;
    req.type = kAnimAttack;
    req.index = kRandomVariant;
    const uint64_t d0 = rng.draws();
    CHECK(w.play(5, req, &rng, 0) == PlayResult::Played);
    CHECK(rng.draws() > d0);  // random variant drew
    CHECK(w.slot(5).variant < 2 && w.current_mode(5) == SlotMode::OneShot);
    // Missing variant -> NoClip (the original returns 0), state unchanged.
    req.index = 7;
    CHECK(w.play(5, req, &rng, 0) == PlayResult::NoClip);
    CHECK(w.current_type(5) == kAnimAttack);
    // Type without clips: random index draws nothing.
    req.type = kAnimDeploy;
    req.index = kRandomVariant;
    const uint64_t d1 = rng.draws();
    CHECK(w.play(5, req, &rng, 0) == PlayResult::NoClip && rng.draws() == d1);
    // Lock: a locked call blocks unlocked ones; a locked call replaces it.
    req = {};
    req.type = kAnimDie;
    req.lock = true;
    CHECK(w.play(5, req, &rng, 0) == PlayResult::Played && w.locked(5));
    req.lock = false;
    req.type = kAnimIdle;
    CHECK(w.play(5, req, &rng, 0) == PlayResult::Locked && w.current_type(5) == kAnimDie);
    req.lock = true;
    CHECK(w.play(5, req, &rng, 0) == PlayResult::Played);
    req.lock = false;
    CHECK(w.play(5, req, &rng, 0) == PlayResult::Locked);
    req.lock = true;
    req.type = kNoAnimType;
    CHECK(w.play(5, req, &rng, 0) == PlayResult::Cleared && w.current_type(5) == kNoAnimType);
    req = {};
    req.type = kAnimIdle;
    CHECK(w.play(5, req, &rng, 0) == PlayResult::Locked);  // the clear was a locked call
    req.lock = true;
    CHECK(w.play(5, req, &rng, 0) == PlayResult::Played);
    req.lock = false;
    req.type = kAnimIdle;
    CHECK(w.play(5, req, &rng, 0) == PlayResult::Locked);

    // Death clip one-shot: 31 frames at 30 fps -> finished when frame reaches 29 (29 frames = 0.9667 s = 29 ticks).
    AnimWorld d;
    d.add(9, m.get(), 100);
    req = {};
    req.type = kAnimDie;
    req.blend_seconds = 0.5f;
    CHECK(d.play(9, req, &rng, 100) == PlayResult::Played && !d.crossfading(9));  // nothing to fade from
    uint64_t done_at = 0;
    for (uint64_t f = 101; f < 200 && !done_at; ++f) {
        d.advance(f);
        if (d.current_mode(9) == SlotMode::Finished) done_at = f;
    }
    CHECK(done_at == 129 && d.slot(9).frame == 29.0f);
    CHECK(d.near_end(9));

    // Crossfade: blend 0.4 from attack to idle; weight = 1 - remaining / duration; done after 12 ticks.
    AnimWorld c;
    c.add(3, m.get(), 0);
    req = {};
    req.type = kAnimAttack;
    c.play(3, req, nullptr, 0);
    c.advance(1);
    req.type = kAnimIdle;
    req.loop = true;
    req.blend_seconds = 0.4f;
    CHECK(c.play(3, req, nullptr, 1) == PlayResult::Played);
    CHECK(c.crossfading(3) && c.crossfade_weight(3) == 0.0f && c.slot(3, kPrevious).type == kAnimAttack);
    c.advance(2);
    {
        const float rem = 0.4f - static_cast<float>(2.0 / 30.0 - 1.0 / 30.0);
        CHECK(c.crossfade_weight(3) == 1.0f - rem / 0.4f);
    }
    // A new request while the weight is <= 0.25 keeps the old previous clip (attack), replaces the primary.
    req.type = kAnimMove;
    c.play(3, req, nullptr, 2);
    CHECK(c.slot(3, kPrevious).type == kAnimAttack && c.current_type(3) == kAnimMove);
    uint64_t f = 2;
    while (c.crossfading(3) && f < 100) c.advance(++f);
    std::printf("crossfade 0.4 s started at tick 1 ended at tick %" PRIu64 "\n", f);
    CHECK(f == 13 || f == 14);  // 0.4 s = 12 ticks after tick 1 (float timer), not restarted by the keep-previous play
    CHECK(c.slot(3, kPrevious).type == kNoAnimType);
    // Weight > 0.25: the current primary becomes the previous clip.
    req.type = kAnimIdle;
    c.play(3, req, nullptr, f);
    for (int i = 0; i < 8; ++i) c.advance(++f);  // weight 8/12 > 0.25
    req.type = kAnimAttack;
    req.loop = false;
    c.play(3, req, nullptr, f);
    CHECK(c.slot(3, kPrevious).type == kAnimIdle && c.crossfade_weight(3) == 0.0f);
    // Blend modes 2 / 3 rescale a running fade.
    c.advance(++f);
    const float rem_before = 0.4f - static_cast<float>(1.0 / 30.0);
    req.type = kAnimIdle;
    req.blend_seconds = 0.25f;
    req.blend_mode = 2;
    c.play(3, req, nullptr, f);
    CHECK(c.crossfade_weight(3) == 1.0f - ((rem_before / 0.4f) * 0.25f) / 0.25f);
    req.blend_mode = 3;
    c.advance(++f);
    // Hard cut clears the fade.
    req.blend_seconds = 0.0f;
    req.blend_mode = 0;
    c.play(3, req, nullptr, f);
    CHECK(!c.crossfading(3) && c.slot(3, kPrevious).type == kNoAnimType);

    // Start frame (forced 0 for clips with < 2 frames).
    req = {};
    req.type = kAnimIdle;
    req.start_frame = 7.5f;
    c.play(3, req, nullptr, f);
    CHECK(c.slot(3).frame == 7.5f);
    req.type = kAnimCinematic;
    c.play(3, req, nullptr, f);
    CHECK(c.slot(3).frame == 0.0f);

    // Timed overlay: finishes after 3 s, speed reset to 1.
    CHECK(c.play_overlay(3, kAnimAttack, 0, 0.1f, 3.0f, nullptr, f) == PlayResult::Played);  // 12 frames at 3 fps: outlives 3 s
    const uint64_t start = f;
    while (c.slot(3, kOverlay).timed && f < start + 200) c.advance(++f);
    CHECK((f - start == 90 || f - start == 91) && c.slot(3, kOverlay).mode == SlotMode::Finished && c.slot(3, kOverlay).speed == 1.0f);

    // Move -> Walk switch keeps the phase: frame * new count / old count.
    req = {};
    req.type = kAnimMove;
    req.loop = true;
    req.start_frame = 10.0f;
    c.play(3, req, nullptr, f);
    CHECK(c.switch_preserving_phase(3, kAnimWalkMove, 0, true, 0.0f, 1.5f, f) == PlayResult::Played);
    CHECK(c.slot(3).frame == 10.0f * 20.0f / 40.0f && c.slot(3).speed == 1.5f);

    // Model created: random start frame in [0, count - 1] drawn only when the idle has a clip.
    AnimWorld cr;
    SimRng r1(5), r2(5);
    AnimCreateParams cp;
    cp.loop_idle = true;
    cp.idle_rate = 0.5f;
    cr.create(1, m.get(), cp, &r1, 0);
    const int32_t expect = r2.range_int(0, 19);
    CHECK(cr.slot(1).frame == static_cast<float>(expect) && cr.slot(1).speed == 0.5f && cr.current_mode(1) == SlotMode::Loop);
    auto empty = make_model(2, {});
    const uint64_t dr = r1.draws();
    cr.create(2, empty.get(), cp, &r1, 0);
    CHECK(r1.draws() == dr && cr.current_type(2) == kNoAnimType);
    std::printf("play rules: lock, random variant, crossfade keep/replace/modes, timed overlay, phase switch OK\n");
}

void test_crossfade_bits() {
    // Retail fast inverse sqrt (magic 0x5F375A86, two Newton steps).
    CHECK(float_bits(alamo::dmath::fast_rsqrt_retail(4.0f)) == float_bits(alamo::dmath::fast_rsqrt_retail(4.0f)));
    const int16_t qa[4] = {1200, -3000, 15000, 28000};
    const int16_t qb[4] = {-1100, 2900, -16000, -27000};  // opposite hemisphere: negated by nlerp
    const Quat a = dequantize_rotation(qa), b = dequantize_rotation(qb);
    const float ts[4] = {0.0f, 0.25f, 0.5f, 0.8333333f};
    uint64_t digest = 0;
    std::string line;
    for (float t : ts) {
        const Quat r = nlerp_fast(a, b, t);
        // Independent reference: same operation sequence written out.
        Quat bb = b;
        const float dot = ((a.x * b.x + a.y * b.y) + a.z * b.z) + a.w * b.w;
        if (dot < 0) bb = {-b.x, -b.y, -b.z, -b.w};
        const float x = a.x + (bb.x - a.x) * t, y = a.y + (bb.y - a.y) * t, z = a.z + (bb.z - a.z) * t, w = a.w + (bb.w - a.w) * t;
        const float len2 = ((x * x + y * y) + z * z) + w * w;
        const float half = 0.5f * len2;
        float g = std::bit_cast<float>(0x5f375a86u - (std::bit_cast<uint32_t>(len2) >> 1));
        g = g * (1.5f - (half * g) * g);
        g = g * (1.5f - (half * g) * g);
        CHECK(float_bits(r.x) == float_bits(x * g) && float_bits(r.y) == float_bits(y * g) && float_bits(r.z) == float_bits(z * g) &&
              float_bits(r.w) == float_bits(w * g));
        for (float v : {r.x, r.y, r.z, r.w}) {
            digest = digest * 1000003u + float_bits(v);
            char buf[16];
            std::snprintf(buf, sizeof buf, "%08x ", float_bits(v));
            line += buf;
        }
    }
    // Translations lerp.
    const Float3 tl = lerp({1.0f, 2.0f, 3.0f}, {-4.0f, 0.5f, 9.25f}, 0.3f);
    digest = digest * 1000003u + float_bits(tl.x) + float_bits(tl.y) * 3u + float_bits(tl.z) * 7u;
    std::printf("crossfade bits: %s\n  digest %016" PRIx64 "\n", line.c_str(), digest);
    // Golden (MSVC 19.51 /fp:precise; clang 20 and gcc 14 -ffp-contract=off must match).
    CHECK(digest == 0x14d597a6e9768f72ull);
}

void test_idle_fidgets() {
    // Idle: 4 variants of 10 frames, one-shot (re-triggered by the idle behaviour when finished).
    auto m = make_model(4, {{kAnimIdle, 4, 10, 30.0f, 3}});
    AnimWorld w;
    SimRng rng(1234);
    w.add(1, m.get(), 0);
    SimRng ref(1234);
    uint64_t ref_timer = 0;
    std::vector<std::pair<uint64_t, uint32_t>> fidgets;
    uint64_t services = 0, mismatches = 0, last_fidget = 0;
    bool interval_ok = true;
    for (uint64_t f = 1; f <= 6000; ++f) {
        w.advance(f);
        const bool should = w.current_type(1) == kNoAnimType || w.current_mode(1) == SlotMode::Finished;
        const bool played = w.service_idle(1, kAnimIdle, false, rng, f);
        if (played != should) ++mismatches;
        if (!played) continue;
        ++services;
        uint32_t expect_variant = 0;
        if (f >= ref_timer) {
            expect_variant = static_cast<uint32_t>(ref.range_int(1, 3));
            ref_timer = f + static_cast<uint64_t>(ref.range_int(210, 450));
        }
        if (w.slot(1).variant != expect_variant) ++mismatches;
        if (expect_variant != 0) {
            if (last_fidget && (f - last_fidget < 210 || f - last_fidget > 450 + 10)) interval_ok = false;
            last_fidget = f;
            fidgets.emplace_back(f, expect_variant);
        }
    }
    CHECK(mismatches == 0);
    CHECK(interval_ok);
    CHECK(rng.state() == ref.state() && rng.draws() == ref.draws());
    CHECK(w.idle_timer(1) == ref_timer);
    uint64_t h = 0;
    for (auto& [fr, v] : fidgets) h = h * 31u + fr * 7u + v;
    std::printf("idle fidgets: %zu fidgets over 6000 frames (%" PRIu64 " idle services), first at %" PRIu64 " v%u, stream digest %016" PRIx64 ", rng draws %" PRIu64 "\n",
                fidgets.size(), services, fidgets.empty() ? 0 : fidgets[0].first, fidgets.empty() ? 0u : fidgets[0].second, h, rng.draws());
    CHECK(fidgets.size() >= 10);
    CHECK(h == 0xee3563a44c8b6b80ull);  // golden: identical on MSVC 19.51, clang 20, gcc 14
    // One idle clip: variant 0 and no variant draw (timer still drawn).
    auto one = make_model(4, {{kAnimIdle, 1, 10, 30.0f, 3}});
    AnimWorld w1;
    SimRng r1(9), r1ref(9);
    w1.add(1, one.get(), 0);
    w1.service_idle(1, kAnimIdle, false, r1, 0);
    r1ref.range_int(210, 450);
    CHECK(w1.slot(1).variant == 0 && r1.state() == r1ref.state());
}

void test_poses() {
    // 4 skeleton bones (Root + 3), clips with 3 ALA bones bind; a 5-bone clip does not.
    auto m = make_model(4, {{kAnimIdle, 1, 20, 30.0f, 3}, {kAnimAttack, 1, 20, 30.0f, 4}, {kAnimMove, 1, 16, 30.0f, 3, 10.0}}, 2);
    CHECK(m->stats.clips == 3 && m->stats.bound == 2 && m->stats.mismatched == 1);
    CHECK(m->clip(kAnimIdle, 0)->bound && !m->clip(kAnimAttack, 0)->bound);
    AnimWorld w;
    w.add(1, m.get(), 0);
    std::vector<Mat34> world(4), bind(4);
    // No clip: bind pose.
    CHECK(w.world_pose(1, world));
    bind[0] = m->skeleton.bind_local[0];
    for (size_t b = 1; b < 4; ++b) bind[b] = (b == 2) ? bind[1] : mul(bind[b - 1], m->skeleton.bind_local[b]);
    for (size_t b = 0; b < 4; ++b) CHECK(mat_bits_equal(world[b], bind[b]));
    // Unbound clip: bind pose too.
    PlayRequest req;
    req.type = kAnimAttack;
    w.play(1, req, nullptr, 0);
    w.advance(5);
    CHECK(w.world_pose(1, world));
    for (size_t b = 0; b < 4; ++b) CHECK(mat_bits_equal(world[b], bind[b]));
    // Bound clip: bone 3 differs from bind; bone 2 copies bone 1 (billboard mode 6).
    req.type = kAnimIdle;
    req.loop = true;
    w.play(1, req, nullptr, 5);
    w.advance(9);  // frame 4
    CHECK(w.world_pose(1, world));
    CHECK(!mat_bits_equal(world[3], bind[3]));
    CHECK(mat_bits_equal(world[2], world[1]));
    // Single-bone gameplay reads are bit-identical to the full pose.
    for (int32_t b = 0; b < 4; ++b) {
        Mat34 one;
        CHECK(w.bone_transform(1, b, one) && mat_bits_equal(one, world[static_cast<size_t>(b)]));
    }
    Mat34 named;
    CHECK(w.bone_transform(1, "b3", named) && mat_bits_equal(named, world[3]));
    Mat34 obj;
    obj.m[0][3] = 100.0f;
    Float3 wp;
    CHECK(w.bone_world_position(1, 3, obj, wp) && wp.x == world[3].m[0][3] + 100.0f && wp.y == world[3].m[1][3] && wp.z == world[3].m[2][3]);
    CHECK(!w.bone_transform(1, 4, named) && !w.bone_transform(1, "nope", named) && !w.bone_transform(2, 0, named));
    // Crossfade halfway: rotation about Z of bone 1 lies between the two clips (idle 3 deg/frame, move 10).
    req.type = kAnimMove;
    req.blend_seconds = 0.2f;
    w.play(1, req, nullptr, 9);
    std::vector<Mat34> mid(4);
    w.advance(12);  // weight 0.5
    CHECK(w.crossfade_weight(1) > 0.49f && w.crossfade_weight(1) < 0.51f);
    CHECK(w.world_pose(1, mid));
    // Overlay drives only non-excluded bones (track 0 -> bone 1 not excluded, track 1 -> bone 2 excluded).
    AnimWorld o;
    o.add(1, m.get(), 0);
    CHECK(o.play_overlay(1, kAnimMove, 0, 1.0f, 0.0f, nullptr, 0) == PlayResult::Played);
    o.advance(3);
    std::vector<Mat34> local(4);
    CHECK(o.local_pose(1, local));
    CHECK(!mat_bits_equal(local[1], m->skeleton.bind_local[1]));
    CHECK(mat_bits_equal(local[2], m->skeleton.bind_local[2]));
    CHECK(!mat_bits_equal(local[3], m->skeleton.bind_local[3]));
    // Visibility bits (lower key): even frames visible.
    auto vm = make_model(2, {{kAnimIdle, 1, 16, 30.0f, 1}});
    auto vc = make_clip("V", 16, 30.0f, 1, 1.0, true);
    vm->clips[0][0].clip = vc;
    bind_clips(*vm);
    AnimWorld v;
    v.add(1, vm.get(), 0);
    req = {};
    req.type = kAnimIdle;
    req.loop = true;
    req.start_frame = 3.0f;
    v.play(1, req, nullptr, 0);
    std::vector<Mat34> vw(2);
    std::vector<uint8_t> vis(2);
    CHECK(v.world_pose(1, vw, vis) && vis[1] == 0);
    v.advance(1);  // frame 4
    CHECK(v.world_pose(1, vw, vis) && vis[1] == 1);
    std::printf("poses: bind fallback, binding rule, copy-parent bones, single-bone reads, overlay, visibility OK\n");
}

// 4000 objects with mixed clips, plays from a synced RNG on the "sim thread", advance through ParallelFor.
uint64_t determinism_run(const alamo::ParallelFor& pf, std::vector<uint64_t>& per_frame, std::vector<Mat34>& poses) {
    static auto m = make_model(6, {{kAnimIdle, 3, 37, 30.0f, 5}, {kAnimMove, 2, 24, 15.0f, 5, 7.0}, {kAnimAttack, 2, 11, 30.0f, 5, 4.0},
                                   {kAnimDie, 1, 45, 10.0f, 5}, {kAnimFlinchLeft, 1, 9, 30.0f, 5, 9.0}});
    AnimWorld w;
    SimRng rng(4242);
    AnimCreateParams cp;
    cp.loop_idle = true;
    for (ObjectId id = 1; id <= 4000; ++id) w.create(id * 3, m.get(), cp, &rng, 0);
    per_frame.clear();
    for (uint64_t f = 1; f <= 300; ++f) {
        w.advance(f, pf, 64);
        // Serial behaviour phase (service order: newest first).
        const auto ids = std::vector<ObjectId>(w.ids().begin(), w.ids().end());
        for (auto it = ids.rbegin(); it != ids.rend(); ++it) {
            const ObjectId id = *it;
            const uint32_t roll = rng.next15() % 64;
            PlayRequest req;
            if (roll == 0) {
                req.type = kAnimAttack;
                req.index = kRandomVariant;
                req.blend_seconds = 0.4f;
                w.play(id, req, &rng, f);
            } else if (roll == 1) {
                req.type = kAnimMove;
                req.index = kRandomVariant;
                req.loop = true;
                req.blend_seconds = 0.2f;
                w.play(id, req, &rng, f);
                w.set_speed(id, rng.range_float(0.5f, 1.5f));
            } else if (roll == 2) {
                req.type = kAnimFlinchLeft;
                req.blend_seconds = 0.25f;
                req.blend_mode = 2;
                w.play(id, req, &rng, f);
            } else if (roll == 3) {
                w.play_overlay(id, kAnimAttack, kRandomVariant, 1.0f, 3.0f, &rng, f);
            } else {
                w.service_idle(id, kAnimIdle, false, rng, f);
            }
        }
        per_frame.push_back(world_hash(w));
    }
    // Poses of every 37th object, evaluated in parallel with per-index outputs.
    const uint32_t n = static_cast<uint32_t>(w.size());
    const uint32_t sampled = (n + 36) / 37;
    poses.assign(static_cast<size_t>(sampled) * 6, Mat34{});
    pf(sampled, 8, [&](uint32_t b, uint32_t e) {
        for (uint32_t i = b; i < e; ++i)
            w.world_pose(w.ids()[i * 37], std::span<Mat34>(poses.data() + static_cast<size_t>(i) * 6, 6));
    });
    return per_frame.back();
}

void test_determinism() {
    std::vector<uint64_t> ref_frames, frames;
    std::vector<Mat34> ref_poses, poses;
    const uint64_t ref = determinism_run(alamo::serial_for, ref_frames, ref_poses);
    std::printf("determinism: serial final anim hash %016" PRIx64 "\n", ref);
    CHECK(ref == 0xe59508130e3b2ad0ull);  // golden: identical on MSVC 19.51, clang 20, gcc 14
#ifdef ALAMO_ANIM_HAVE_JOBS
    for (int32_t workers : {0, 1, 4, 15}) {
        alamo::jobs::JobSystemConfig cfg;
        cfg.workers = workers;
        alamo::jobs::JobSystem js(cfg);
        const alamo::ParallelFor pf = alamo::jobs::make_parallel_for(js);
        const uint64_t h = determinism_run(pf, frames, poses);
        const bool same = frames == ref_frames && poses.size() == ref_poses.size() &&
                          std::memcmp(poses.data(), ref_poses.data(), poses.size() * sizeof(Mat34)) == 0;
        std::printf("  %2d workers: final %016" PRIx64 " per-frame hashes + poses %s\n", workers, h, same ? "identical" : "DIFFER");
        CHECK(same);
    }
#else
    std::printf("  (engine/jobs not configured: thread-count runs skipped)\n");
#endif
}

// ---- R3 behaviours (anim_behaviors.hpp): flinch, idle while engaged, death clip, corpse countdown ----------

std::unique_ptr<AnimModel> behaviour_model(bool flinch_clips = true) {
    if (!flinch_clips)
        return make_model(3, {{kAnimIdle, 2, 30, 30.0f, 2}, {kAnimAttack, 1, 12, 30.0f, 2}, {kAnimDie, 2, 40, 30.0f, 2}});
    return make_model(3, {{kAnimIdle, 2, 30, 30.0f, 2},
                          {kAnimMove, 1, 24, 30.0f, 2},
                          {kAnimAttack, 1, 12, 30.0f, 2},
                          {kAnimAttackIdle, 1, 20, 30.0f, 2},
                          {kAnimDie, 3, 40, 30.0f, 2},
                          {kAnimFlinchLeft, 2, 10, 30.0f, 2},
                          {kAnimFlinchRight, 2, 10, 30.0f, 2},
                          {kAnimFlinchFront, 2, 10, 30.0f, 2},
                          {kAnimFlinchBack, 2, 10, 30.0f, 2},
                          {kAnimAttackFlinchLeft, 1, 10, 30.0f, 2},
                          {kAnimAttackFlinchRight, 1, 10, 30.0f, 2},
                          {kAnimAttackFlinchFront, 1, 10, 30.0f, 2},
                          {kAnimAttackFlinchBack, 1, 10, 30.0f, 2}});
}

// MSVC 19.51; clang 20 / gcc 14 must match. Was 4357bd5e704ab19c before the locomotor state (last yaw / crouch)
// and the corpse spin-away / damage-kind fields joined AnimBehaviors::hash; d0671711f6a07913 before the §A.11 / §A.12
// rules (flinch intensity hashed, no flinch from Attack Idle / no clip, crouch-class idle restart); 1706b32132799e3d
// before the full §A.12 flinch phases (freeze, hold from the freeze, synced replay of C, cooldown once back on C).
constexpr uint64_t kBehaviourGolden = 0x572727ad9cdf47fcull;

uint64_t behaviours_hash(const AnimWorld& w, const AnimBehaviors& b) {
    alamo::StateHasher h;
    w.hash_state(h);
    b.hash(h);
    return h.digest();
}

void test_behaviors() {
    // Frame conversion and the direction rule.
    CHECK(seconds_to_frames(0.5f) == 15 && seconds_to_frames(0.25f) == 7 && seconds_to_frames(-1.0f) == 0 && seconds_to_frames(0.0f) == 0);
    CHECK(flinch_type(1, 0, 1, 0, false) == kAnimFlinchBack);    // travelling along the facing: from behind
    CHECK(flinch_type(1, 0, -1, 0, false) == kAnimFlinchFront);  // head-on
    CHECK(flinch_type(1, 0, 0, 1, false) == kAnimFlinchRight);   // travelling to the victim's left: from the right
    CHECK(flinch_type(1, 0, 0, -1, false) == kAnimFlinchLeft);
    CHECK(flinch_type(1, 0, 1, 0.9f, false) == kAnimFlinchBack);   // 42 deg
    CHECK(flinch_type(1, 0, 1, 1.1f, false) == kAnimFlinchRight);  // 48 deg
    CHECK(flinch_type(1, 0, -1, -1.1f, false) == kAnimFlinchLeft); // 132 deg
    CHECK(flinch_type(1, 0, -1, -0.9f, false) == kAnimFlinchFront);
    CHECK(flinch_type(0, 0, 1, 0, false) == kAnimFlinchFront && flinch_type(1, 0, 0, 0, true) == kAnimAttackFlinchFront);
    CHECK(flinch_type(1, 0, 1, 0, true) == kAnimAttackFlinchBack && flinch_type(1, 0, 0, 1, true) == kAnimAttackFlinchRight);

    const auto m = behaviour_model();
    const FlinchParams fp;  // 0.25 blend, 0.5 hold (15 frames), 1.0 idle interval (30), 3.0 attack interval (90)
    IdleParams ip;
    ip.loop_idle = true;
    using Phase = AnimBehaviors::FlinchPhase;
    {
        // Full flinch from idle (I = 1, §A.12): synced variant draw, loop, crossfade 0.25; frozen once the fade has
        // ended; held 15 frames from the freeze; then C (Idle) replayed with a synced random variant, one-shot, blend
        // mode 3; the 30-frame cooldown starts once the model is back on C with the fade complete.
        AnimWorld w;
        AnimBehaviors b;
        AnimCreateParams cp;
        cp.loop_idle = true;
        w.create(1, m.get(), cp, nullptr, 0);
        SimRng rng(77), replay(77);
        CHECK(b.on_hit(w, 1, fp, ip, 1, 0, 0, -1, rng, 10));
        const uint32_t want_variant = static_cast<uint32_t>(replay.range_int(0, 1));
        CHECK(rng.state() == replay.state());
        const SlotView s = w.slot(1);
        CHECK(s.type == kAnimFlinchLeft && s.variant == want_variant && s.mode == SlotMode::Loop);
        CHECK(w.crossfading(1) && w.slot(1, kPrevious).type == kAnimIdle);
        CHECK(b.flinching(1) && b.flinch_phase(1) == Phase::BlendIn && b.flinch_return_type(1) == kAnimIdle);
        CHECK(!b.on_hit(w, 1, fp, ip, 1, 0, 0, -1, rng, 11) && rng.state() == replay.state());  // in progress: no draw
        std::vector<ObjectId> removed;
        uint64_t f = 11, frozen_at = 0;
        for (; f < 60; ++f) {
            b.service(w, f, removed, &rng);
            if (b.flinch_phase(1) == Phase::Hold) {
                frozen_at = f;
                break;
            }
            CHECK(w.crossfading(1));  // I = 1: frozen only once the fade has ended
            w.advance(f);
        }
        CHECK(frozen_at > 11 && !w.crossfading(1) && w.slot(1).speed == kFlinchFrozenSpeed && b.next_flinch_frame(1) == frozen_at + 15);
        CHECK(rng.state() == replay.state() && b.flinching(1));
        w.advance(f);
        for (++f; f < frozen_at + 15; ++f) {
            b.service(w, f, removed, &rng);
            CHECK(b.flinch_phase(1) == Phase::Hold && w.current_type(1) == kAnimFlinchLeft);
            w.advance(f);
        }
        CHECK(!b.on_hit(w, 1, fp, ip, 1, 0, 1, 0, rng, f - 1) && rng.state() == replay.state());  // holding
        b.service(w, f, removed, &rng);  // hold end: C replayed
        const uint32_t back_variant = static_cast<uint32_t>(replay.range_int(0, 1));
        CHECK(rng.state() == replay.state());
        CHECK(b.flinch_phase(1) == Phase::Return && !b.flinching(1) && w.current_type(1) == kAnimIdle && w.slot(1).variant == back_variant);
        CHECK(w.current_mode(1) == SlotMode::OneShot && w.crossfading(1) && w.slot(1).speed == 1.0f && removed.empty());
        w.advance(f);
        uint64_t back_at = 0;
        for (++f; f < frozen_at + 60; ++f) {
            b.service(w, f, removed, &rng);
            if (b.flinch_phase(1) == Phase::None) {
                back_at = f;
                break;
            }
            w.advance(f);
        }
        CHECK(back_at > frozen_at + 15 && b.next_flinch_frame(1) == back_at + 30 && b.flinch_return_type(1) == kNoAnimType);
        CHECK(!b.on_hit(w, 1, fp, ip, 1, 0, 1, 0, rng, back_at + 29));  // cooldown
        CHECK(b.on_hit(w, 1, fp, ip, 1, 0, 1, 0, rng, back_at + 30) && w.current_type(1) == kAnimFlinchBack);
        // The weapon gate reads the flinch through the type group.
        CHECK(is_flinch(w.current_type(1)));
    }
    {
        // Attack flinch while attacking replays the Attack clip (C) at the hold end, cooldown 90 once back on it; no
        // flinch while moving.
        AnimWorld w;
        AnimBehaviors b;
        w.create(2, m.get(), {}, nullptr, 0);
        w.create(3, m.get(), {}, nullptr, 0);
        SimRng rng(5);
        PlayRequest atk;
        atk.type = kAnimAttack;
        w.play(2, atk, &rng, 0);
        CHECK(b.on_hit(w, 2, fp, ip, 1, 0, -1, 0, rng, 1) && w.current_type(2) == kAnimAttackFlinchFront);
        CHECK(b.flinch_return_type(2) == kAnimAttack);
        std::vector<ObjectId> removed;
        bool replayed = false;
        uint64_t f = 2;
        for (; f < 300 && b.flinch_phase(2) != Phase::None; ++f) {
            b.service(w, f, removed, &rng);
            if (!replayed && b.flinch_phase(2) == Phase::Return) {
                replayed = true;
                CHECK(w.current_type(2) == kAnimAttack && w.current_mode(2) == SlotMode::OneShot);
            }
            w.advance(f);
        }
        CHECK(replayed && b.flinch_phase(2) == Phase::None && b.next_flinch_frame(2) == f - 1 + 90);
        PlayRequest mv;
        mv.type = kAnimMove;
        mv.loop = true;
        w.play(3, mv, &rng, 0);
        const uint32_t st = rng.state();
        CHECK(!b.on_hit(w, 3, fp, ip, 1, 0, -1, 0, rng, 1) && rng.state() == st && w.current_type(3) == kAnimMove);
        // A model without flinch clips: nothing, no draw.
        const auto plain = behaviour_model(false);
        w.create(4, plain.get(), {}, nullptr, 0);
        CHECK(!b.on_hit(w, 4, fp, ip, 1, 0, -1, 0, rng, 1) && rng.state() == st);
    }
    {
        // Idle while engaged: Attack finished -> Attack Idle + combat timer 90 + [0, 30]; replays while engaged,
        // then the plain idle behaviour.
        AnimWorld w;
        AnimBehaviors b;
        w.create(5, m.get(), {}, nullptr, 0);
        SimRng rng(9), replay(9);
        PlayRequest atk;
        atk.type = kAnimAttack;
        w.play(5, atk, &rng, 0);
        uint64_t f = 1;
        for (; w.current_mode(5) != SlotMode::Finished; ++f) w.advance(f);
        CHECK(b.service_idle(w, 5, ip, rng, f));
        const uint64_t timer = f + 90 + static_cast<uint64_t>(replay.range_int(0, 30));
        CHECK(w.current_type(5) == kAnimAttackIdle && b.combat_timer(5) == timer && rng.state() == replay.state());
        uint32_t attack_idles = 1, idles = 0;
        for (++f; f < timer + 60; ++f) {
            w.advance(f);
            if (b.service_idle(w, 5, ip, rng, f)) {
                if (w.current_type(5) == kAnimAttackIdle) {
                    ++attack_idles;
                    CHECK(f < timer);
                } else {
                    ++idles;
                    CHECK(f >= timer && w.current_type(5) == kAnimIdle);
                }
            }
        }
        CHECK(attack_idles >= 4 && idles >= 1);
    }
    {
        // Death clip (random variant, blend 0.5, locked), corpse countdown after the clip, fade, removal.
        AnimWorld w;
        AnimBehaviors b;
        w.create(6, m.get(), {}, nullptr, 0);
        SimRng rng(31), replay(31);
        DeathParams dp;
        dp.persistence_seconds = 1.0f;  // 30 frames
        dp.fade_seconds = 0.25f;        // 7 frames
        CorpseInfo info;
        info.object_type = 12;
        info.position[0] = 5.0f;
        CHECK(b.on_hit(w, 6, fp, ip, 1, 0, 1, 0, rng, 0));
        replay.range_int(0, 1);
        CHECK(b.begin_death(w, 6, dp, info, rng, 3) == DeathResult::Corpse);
        CHECK(w.slot(6).variant == static_cast<uint32_t>(replay.range_int(0, 2)) && rng.state() == replay.state());
        CHECK(w.current_type(6) == kAnimDie && w.locked(6) && w.crossfading(6) && !b.flinching(6));
        PlayRequest idle;
        CHECK(w.play(6, idle, &rng, 3) == PlayResult::Locked);
        CHECK(!b.on_hit(w, 6, fp, ip, 1, 0, 1, 0, rng, 100) && !b.service_idle(w, 6, ip, rng, 4));
        std::vector<ObjectId> removed;
        uint64_t f = 4, clip_end = 0, fade_start = 0;
        for (; f < 400 && removed.empty(); ++f) {
            b.service(w, f, removed);
            const Corpse* c = b.corpse(6);
            if (c && c->phase == CorpsePhase::Persisting && clip_end == 0) clip_end = f;
            if (c && c->phase == CorpsePhase::Fading && fade_start == 0) {
                fade_start = f;
                CHECK(c->alpha(f) == 1.0f && c->alpha(f + 7) == 0.0f && c->alpha(f + 3) > 0.5f && c->alpha(f + 3) < 0.6f);
            }
            if (removed.empty()) w.advance(f);
        }
        // 40-frame one-shot finishes at frame 38 (count - 2) after 38 ticks of advance from frame 3.
        std::printf("behaviours: death clip over at %llu, fade from %llu, removed at %llu\n", static_cast<unsigned long long>(clip_end),
                    static_cast<unsigned long long>(fade_start), static_cast<unsigned long long>(f - 1));
        CHECK(clip_end == 3 + 38 + 1 && fade_start == clip_end + 30 && f - 1 == fade_start + 7);
        CHECK(removed.size() == 1 && removed[0] == 6 && !w.contains(6) && !b.is_corpse(6) && b.corpses().empty());
    }
    {
        // No death clip: Remove_Upon_Death -> removed at once; otherwise a corpse whose countdown starts at once;
        // persistence -1 never fades; row-less objects get a corpse record (NoRow).
        AnimWorld w;
        AnimBehaviors b;
        const auto plain = make_model(3, {{kAnimIdle, 1, 30, 30.0f, 2}});
        w.create(7, plain.get(), {}, nullptr, 0);
        w.create(8, plain.get(), {}, nullptr, 0);
        w.create(9, m.get(), {}, nullptr, 0);
        SimRng rng(3);
        DeathParams rm;
        rm.remove_upon_death = true;
        const uint32_t st = rng.state();
        CHECK(b.begin_death(w, 7, rm, {}, rng, 0) == DeathResult::RemoveNow && !w.contains(7) && !b.is_corpse(7) && rng.state() == st);
        DeathParams keep;
        keep.persistence_seconds = 0.0f;
        keep.fade_seconds = 0.0f;
        CHECK(b.begin_death(w, 8, keep, {}, rng, 0) == DeathResult::Corpse && b.corpse(8)->death_type == kNoAnimType);
        DeathParams forever;  // persistence -1
        CHECK(b.begin_death(w, 9, forever, {}, rng, 0) == DeathResult::Corpse);
        CHECK(b.begin_death(w, 10, keep, {}, rng, 0) == DeathResult::NoRow && b.is_corpse(10));
        std::vector<ObjectId> removed;
        b.service(w, 1, removed);
        CHECK(removed.size() == 2 && removed[0] == 8 && removed[1] == 10 && !w.contains(8));
        for (uint64_t f = 2; f < 3000; ++f) {
            b.service(w, f, removed);
            w.advance(f);
        }
        CHECK(removed.size() == 2 && b.is_corpse(9) && w.contains(9) && b.corpse(9)->phase == CorpsePhase::Persisting);
        b.detach(9);
        CHECK(b.corpse(9)->detached);
        b.remove(9);
        CHECK(!b.is_corpse(9) && b.corpses().empty());
    }
    {
        // Determinism: 600 objects, scripted hits / deaths / idle services on one synced stream, advance through
        // the ParallelFor; state + behaviour hash identical for serial and 1 / 4 / 15 workers; golden digest.
        auto run = [&](const alamo::ParallelFor& pf) {
            AnimWorld w;
            AnimBehaviors b;
            SimRng rng(2026);
            DeathParams dp;
            dp.persistence_seconds = 0.5f;
            AnimCreateParams cp;
            for (ObjectId id = 1; id <= 600; ++id) w.create(id, m.get(), cp, &rng, 0);
            std::vector<ObjectId> removed;
            alamo::StateHasher acc;
            for (uint64_t f = 1; f <= 300; ++f) {
                for (ObjectId id = static_cast<ObjectId>(f % 7) + 1; id <= 600; id += 7) {
                    if (!w.contains(id) || b.is_corpse(id)) continue;
                    const int32_t pick = rng.range_int(0, 99);
                    if (pick < 30) {
                        const float dx = static_cast<float>(rng.range_int(-5, 5));
                        const float dy = static_cast<float>(rng.range_int(-5, 5));
                        b.on_hit(w, id, fp, ip, 1.0f, 0.0f, dx, dy, rng, f);
                    } else if (pick < 33) b.begin_death(w, id, dp, {}, rng, f);
                    else if (pick < 50) {
                        PlayRequest a;
                        a.type = kAnimAttack;
                        a.index = kRandomVariant;
                        a.blend_seconds = 0.1f;
                        w.play(id, a, &rng, f);
                    } else b.service_idle(w, id, ip, rng, f);
                }
                b.service(w, f, removed, &rng);
                w.advance(f, pf, 64);
                acc.u64(behaviours_hash(w, b));
            }
            acc.u32(static_cast<uint32_t>(removed.size()));
            return acc.digest();
        };
        const uint64_t serial = run(alamo::serial_for);
        std::printf("behaviours: 600 objects x 300 frames digest %016" PRIx64 "\n", serial);
        CHECK(serial == kBehaviourGolden);  // identical on MSVC 19.51, clang 20, gcc 14
#ifdef ALAMO_ANIM_HAVE_JOBS
        for (int32_t n : {0, 1, 4, 15}) {
            alamo::jobs::JobSystemConfig cfg;
            cfg.workers = n;
            alamo::jobs::JobSystem js(cfg);
            const uint64_t par = run(alamo::jobs::make_parallel_for(js));
            std::printf("  %2d workers: %016" PRIx64 "\n", n, par);
            CHECK(par == serial);
        }
#endif
    }
}

// ---- R3 finish: events, Berserker, visibility, locomotor, deploy, death by kind, spin-away ------------------

void test_events_and_locomotor() {
    // Event table + crossing rule.
    AnimEventMap map;
    std::vector<std::string> diag;
    const size_t rows = map.parse("; comment\nSURFACE\tei_trooper_move_00\t0\tINFANTRY_FOOT_PLANT\tB_Foot_R\n"
                                  "SURFACE ei_trooper_move_00 1032 INFANTRY_FOOT_PLANT B_Foot_L\r\n"
                                  "SOUND\tei_trooper_die_00\t500\tUnit_Death_Thud\nSURFACE bad\n",
                                  &diag);
    CHECK(rows == 3 && diag.size() == 1 && map.clips() == 2);
    const auto* ev = map.find("DATA\\ART\\MODELS\\EI_TROOPER_MOVE_00.ALA");
    CHECK(ev && ev->size() == 2 && (*ev)[0].bone == "B_Foot_R" && (*ev)[1].time_ms == 1032);
    CHECK(map.find("ei_trooper_die_00") && map.find("ei_trooper_die_00")->front().kind == AnimEventDef::Kind::Sound);
    std::vector<uint32_t> hit;
    if (ev) {
        crossed_events(*ev, 30.0f, 62, 0.0f, 1.0f, false, hit);  // [0, 33.3 ms): the 0 ms plant
        CHECK(hit.size() == 1 && hit[0] == 0);
        hit.clear();
        crossed_events(*ev, 30.0f, 62, 30.0f, 31.0f, false, hit);  // [1000, 1033.3): 1032
        CHECK(hit.size() == 1 && hit[0] == 1);
        hit.clear();
        crossed_events(*ev, 30.0f, 62, 60.5f, 0.5f, true, hit);  // wrap at 61 frames: tail empty, then 0 ms
        CHECK(hit.size() == 1 && hit[0] == 0);
        hit.clear();
        crossed_events(*ev, 30.0f, 62, 10.0f, 10.0f, false, hit);
        CHECK(hit.empty());
    }

    const auto m = make_model(3, {{kAnimIdle, 2, 30, 30.0f, 2},
                                  {kAnimMove, 1, 24, 30.0f, 2},
                                  {kAnimWalkMove, 1, 36, 30.0f, 2},
                                  {kAnimFireMove, 1, 20, 30.0f, 2},
                                  {kAnimCrouchMove, 1, 22, 30.0f, 2},
                                  {kAnimIdleCrouch, 1, 30, 30.0f, 2},
                                  {kAnimDeployedWalk, 1, 26, 30.0f, 2},
                                  {kAnimTurnLeft, 1, 12, 30.0f, 2},
                                  {kAnimTurnRight, 1, 12, 30.0f, 2},
                                  {kAnimDeploy, 2, 40, 30.0f, 2},
                                  {kAnimUndeploy, 1, 40, 30.0f, 2},
                                  {kAnimAttack, 1, 12, 30.0f, 2},
                                  {kAnimSpinMove, 1, 20, 30.0f, 2},
                                  {kAnimDie, 1, 40, 30.0f, 2},
                                  {kAnimFireDie, 1, 50, 30.0f, 2}},
                              -1);
    {
        // Berserker: Move -> SpinMove loop; Attack passes; a running spin is left alone until near its end.
        AnimWorld w;
        w.create(1, m.get(), {}, nullptr, 0);
        w.set_berserker(1, true);
        PlayRequest mv;
        mv.type = kAnimMove;
        mv.speed = 2.0f;
        CHECK(w.play(1, mv, nullptr, 0) == PlayResult::Played && w.current_type(1) == kAnimSpinMove && w.current_mode(1) == SlotMode::Loop &&
              w.slot(1).speed == 1.0f);
        CHECK(w.play(1, mv, nullptr, 1) == PlayResult::Locked);
        for (uint64_t f = 1; f <= 16; ++f) w.advance(f);  // frame 16: 16 + 2 < 19, still running
        CHECK(w.play(1, mv, nullptr, 16) == PlayResult::Locked);
        w.advance(17);  // 17 + 2 >= 19: restartable
        CHECK(w.play(1, mv, nullptr, 17) == PlayResult::Played && w.slot(1).frame == 0.0f);
        PlayRequest atk;
        atk.type = kAnimAttack;
        CHECK(w.play(1, atk, nullptr, 18) == PlayResult::Played && w.current_type(1) == kAnimAttack);
        w.set_berserker(1, false);
        CHECK(w.play(1, mv, nullptr, 19) == PlayResult::Played && w.current_type(1) == kAnimMove);
    }
    {
        // Visibility bits == the pose path's bits (toggling visibility clip, crossfade, overlay).
        const auto vm = make_model(4, {{kAnimIdle, 1, 16, 30.0f, 3}, {kAnimAttack, 1, 16, 30.0f, 3}});
        AnimWorld w;
        AnimCreateParams cp;
        cp.loop_idle = true;
        w.create(1, vm.get(), cp, nullptr, 0);
        bool same = true;
        for (uint64_t f = 1; f < 30; ++f) {
            if (f == 5) {
                PlayRequest a;
                a.type = kAnimAttack;
                a.blend_seconds = 0.3f;
                w.play(1, a, nullptr, f);
            }
            if (f == 12) w.play_overlay(1, kAnimIdle, 0, 1.0f, 0.5f, nullptr, f);
            w.advance(f);
            std::vector<Mat34> local(4);
            std::vector<uint8_t> v1(4), v2(4);
            same = same && w.local_pose(1, local, v1) && w.bone_visibility(1, v2) && v1 == v2;
        }
        CHECK(same);
    }
    {
        // Locomotor selection.
        AnimWorld w;
        AnimBehaviors b;
        SimRng rng(4);
        LocomotorParams lp;
        lp.movement_anim_speed = 0.8f;
        lp.deployed_walk_anim_speed = 2.0f;
        lp.idle.loop_idle = true;
        for (ObjectId id = 1; id <= 6; ++id) w.create(id, m.get(), {}, nullptr, 0);
        LocomotorInput in;
        in.speed = 1.0f;
        in.max_speed = 1.0f;
        CHECK(b.service_locomotor(w, 1, lp, in, rng, 1) == 1 && w.current_type(1) == kAnimMove && w.slot(1).speed == 0.8f);
        in.speed = 0.25f;  // slow: WALKMOVE, phase kept
        const float f0 = w.slot(1).frame;
        CHECK(b.service_locomotor(w, 1, lp, in, rng, 2) == 1 && w.current_type(1) == kAnimWalkMove && w.slot(1).frame == f0 * 36.0f / 24.0f);
        in.burning = true;
        CHECK(b.service_locomotor(w, 1, lp, in, rng, 3) == 1 && w.current_type(1) == kAnimFireMove);
        in.crouched = true;
        CHECK(b.service_locomotor(w, 1, lp, in, rng, 4) == 1 && w.current_type(1) == kAnimCrouchMove);
        in.deployed = true;
        CHECK(b.service_locomotor(w, 1, lp, in, rng, 5) == 1 && w.current_type(1) == kAnimDeployedWalk && w.slot(1).speed == 2.0f * 0.25f);
        in = LocomotorInput{};  // stopped: back to the idle (loop)
        CHECK(b.service_locomotor(w, 1, lp, in, rng, 6) == 1 && w.current_type(1) == kAnimIdle && w.current_mode(1) == SlotMode::Loop);
        // Crouched and stopped: the crouch change forces the idle behaviour onto Idle Crouch.
        in.crouched = true;
        CHECK(b.service_locomotor(w, 1, lp, in, rng, 7) == 1 && w.current_type(1) == kAnimIdleCrouch);
        CHECK(default_idle_type(m.get(), in, lp.idle) == kAnimIdleCrouch);
        // Presence idle (§A.11): kept even without a clip (the idle service's play fails then).
        LocomotorInput g;
        g.idle_type = kAnimIdleCrouch;
        CHECK(default_idle_type(m.get(), g, lp.idle) == kAnimIdleCrouch);
        g.idle_type = 78;  // Celebrate: no clip in this model
        CHECK(default_idle_type(m.get(), g, lp.idle) == 78);
        g.crouched = true;  // SPREAD_OUT wins over presence
        CHECK(default_idle_type(m.get(), g, lp.idle) == kAnimIdleCrouch);
        // Turning in place: + yaw -> TURNL, - yaw -> TURNR, none without a yaw change.
        LocomotorInput t;
        t.yaw_deg = 10.0f;
        b.service_locomotor(w, 2, lp, t, rng, 1);  // first call records the yaw
        t.yaw_deg = 14.0f;
        CHECK(b.service_locomotor(w, 2, lp, t, rng, 2) == 1 && w.current_type(2) == kAnimTurnLeft);
        t.yaw_deg = 2.0f;
        CHECK(b.service_locomotor(w, 2, lp, t, rng, 3) == 1 && w.current_type(2) == kAnimTurnRight);
        t.yaw_deg = 179.0f;  // wrap: 2 -> 179 is +177
        CHECK(b.service_locomotor(w, 2, lp, t, rng, 4) == 1 && w.current_type(2) == kAnimTurnLeft);
        t.yaw_deg = -179.0f;  // 179 -> -179 = +2 (wrapped), TURNL still running: no restart
        CHECK(b.service_locomotor(w, 2, lp, t, rng, 5) == 0 && w.current_type(2) == kAnimTurnLeft);
        // Deploy / undeploy: random variant (synced), rate as speed, blend 1/30 s.
        SimRng dr(8), dreplay(8);
        CHECK(b.deploy(w, 3, false, 2.0f, dr, 1) && w.current_type(3) == kAnimDeploy && w.slot(3).speed == 2.0f &&
              w.slot(3).variant == static_cast<uint32_t>(dreplay.range_int(0, 1)) && w.crossfading(3));
        CHECK(b.deploy(w, 3, true, 1.5f, dr, 2) && w.current_type(3) == kAnimUndeploy && w.current_frame_count(3) == 40);
    }
    {
        // Death type by damage kind (Damage_Fire -> FIRE_DIE when the model has it and the type sets none);
        // explicit types win; spin-away: the caller's decision (TACTICAL_GAPS §1.9, combat's one draw), no draw here,
        // no clip, removed after Spin_Away_On_Death_Time.
        AnimWorld w;
        AnimBehaviors b;
        SimRng rng(12);
        for (ObjectId id = 1; id <= 5; ++id) w.create(id, m.get(), {}, nullptr, 0);
        DeathParams dp;
        CHECK(death_type_for_kind(7) == 62 && death_type_for_kind(14) == kAnimCrushed && death_type_for_kind(1) == kNoAnimType);
        CHECK(b.begin_death(w, 1, dp, {}, rng, 0, 7) == DeathResult::Corpse && w.current_type(1) == 62 && b.corpse(1)->death_type == 62);
        CHECK(b.begin_death(w, 2, dp, {}, rng, 0, 14) == DeathResult::Corpse && w.current_type(2) == kAnimDie);  // no CRUSHED clip
        DeathParams ex;
        ex.type_explicit = true;
        CHECK(b.begin_death(w, 3, ex, {}, rng, 0, 7) == DeathResult::Corpse && w.current_type(3) == kAnimDie);
        // A Spin_Away_On_Death type whose caller decided "no spin": the death clip plays; the only draw is the random
        // variant (AnimWorld::play's), none for spin-away.
        DeathParams sp;
        sp.spin_away_seconds = 2.0f;
        sp.remove_upon_death = true;
#ifdef ALAMO_ANIM_HAVE_OBJECTS
        {
            TypeAnimParams tp;
            tp.spin_away_on_death = true;
            tp.spin_away_chance = 1.0f;
            tp.spin_away_seconds = 2.0f;
            const DeathParams fromtype = death_params(tp);
            CHECK(!fromtype.spin && fromtype.spin_away_seconds == 2.0f);  // death_params never decides a spin
        }
#endif
        SimRng replay = rng;
        replay.range_int(0, static_cast<int32_t>(m->variant_count(kAnimDie)) - 1);
        const DeathResult r4 = b.begin_death(w, 4, sp, {}, rng, 10);
        CHECK(rng.state() == replay.state());
        CHECK(r4 == DeathResult::Corpse && !b.corpse(4)->spin_away && b.corpse(4)->death_type == kAnimDie);
        sp.spin = true;  // the caller's draw said spin
        const uint32_t st = rng.state();
        CHECK(b.begin_death(w, 5, sp, {}, rng, 10) == DeathResult::Corpse && rng.state() == st && b.corpse(5)->spin_away &&
              b.corpse(5)->death_type == kNoAnimType && b.corpse(5)->phase_end == 70);
        std::vector<ObjectId> removed;
        b.service(w, 69, removed);
        CHECK(b.is_corpse(5));
        b.service(w, 70, removed);
        CHECK(!b.is_corpse(5) && !w.contains(5) && std::find(removed.begin(), removed.end(), 5) != removed.end());
    }
}

// ---- N1 save / load: AnimWorld + AnimBehaviors round trip ------------------------------------------------

// Record sizes of the documented layouts (anim_state.hpp / anim_behaviors.hpp).
constexpr size_t kAnimSaveRowBytes = 109;
constexpr size_t kAnimSaveEntryBytes = 63;  // version 4: §A.12 flinch phases, R3f t03 combat countdown
constexpr size_t kAnimSaveCorpseBytes = 77;

struct SaveScenario {
    const AnimModel* a = nullptr;
    const AnimModel* b = nullptr;
    const AnimModel* model_for(ObjectId id) const { return id % 3 == 0 ? b : a; }
};

// One tick of a battle-like driver: behaviour service, advance, a new object every 7 frames, then per-object
// behaviour calls (flinch, death, locomotor, overlay, freeze, berserker, locked timed play, idle) drawn from the
// synced RNG.
void save_step(AnimWorld& w, AnimBehaviors& b, SimRng& rng, uint64_t f, const SaveScenario& sc, std::vector<ObjectId>& removed) {
    b.service(w, f, removed, &rng);
    w.advance(f);
    FlinchParams fp;
    IdleParams ip;
    ip.loop_idle = (f / 50) % 2 == 1;
    LocomotorParams lp;
    lp.idle = ip;
    DeathParams dp;
    dp.persistence_seconds = 1.0f;
    dp.fade_seconds = 0.5f;
    if (f % 7 == 0) {
        const ObjectId id = static_cast<ObjectId>(100 + f);
        AnimCreateParams cp;
        cp.loop_idle = f % 2 == 0;
        w.create(id, sc.model_for(id), cp, &rng, f);
    }
    const std::vector<ObjectId> ids(w.ids().begin(), w.ids().end());
    for (const ObjectId id : ids) {
        if (b.is_corpse(id) || !w.model(id)) continue;
        const uint32_t roll = rng.next15() % 64;
        if (roll == 0) {
            const float dx = rng.range_float(-1.0f, 1.0f);
            const float dy = rng.range_float(-1.0f, 1.0f);
            b.on_hit(w, id, fp, ip, 1.0f, 0.0f, dx, dy, rng, f);
        } else if (roll == 1 && f > 40) {
            CorpseInfo ci;
            ci.object_type = static_cast<uint32_t>(id);
            ci.owner = id % 2;
            ci.position[0] = static_cast<float>(id);
            dp.spin = id % 4 == 0;
            dp.spin_away_seconds = 1.0f;
            b.begin_death(w, id, dp, ci, rng, f, static_cast<uint8_t>(id % 2 == 0 ? 7 : 1));
        } else if (roll < 10) {
            LocomotorInput in;
            in.speed = static_cast<float>(roll) * 0.1f;
            in.max_speed = 1.0f;
            in.yaw_deg = static_cast<float>(f % 90);
            in.crouched = roll == 9;
            b.service_locomotor(w, id, lp, in, rng, f);
        } else if (roll == 10) {
            w.play_overlay(id, kAnimAttack, kRandomVariant, 1.0f, 0.5f, &rng, f);
        } else if (roll == 11) {
            w.set_frozen(id, !w.frozen(id));
        } else if (roll == 12) {
            w.set_berserker(id, !w.berserker(id));
        } else if (roll == 13) {
            PlayRequest req;
            req.type = kAnimAttack;
            req.index = kRandomVariant;
            req.blend_seconds = 0.3f;
            req.lock = true;
            req.timed_seconds = 0.4f;
            w.play(id, req, &rng, f);
        } else {
            b.service_idle(w, id, ip, rng, f);
        }
    }
}

std::vector<uint8_t> world_bytes(const AnimWorld& w) {
    alamo::ByteWriter out;
    w.write(out);
    return out.bytes();
}
std::vector<uint8_t> behaviour_bytes(const AnimBehaviors& b) {
    alamo::ByteWriter out;
    b.write(out);
    return out.bytes();
}

void test_save() {
    const auto ma = behaviour_model();
    auto mb_owner = make_model(4, {{kAnimIdle, 3, 25, 30.0f, 3}, {kAnimMove, 1, 20, 30.0f, 3}, {kAnimAttack, 2, 14, 30.0f, 3},
                                   {kAnimDie, 1, 30, 30.0f, 3}, {kAnimFireDie, 1, 36, 30.0f, 3}});
    mb_owner->model = "DATA\\ART\\MODELS\\TEST_OTHER.ALO";
    const SaveScenario sc{ma.get(), mb_owner.get()};
    const std::function<const AnimModel*(int32_t)> model_of = [&sc](int32_t id) -> const AnimModel* { return sc.model_for(id); };

    AnimWorld w;
    AnimBehaviors b;
    SimRng rng(777);
    for (ObjectId id = 1; id <= 12; ++id) w.create(id, sc.model_for(id), {}, &rng, 0);
    w.add(13, nullptr, 0);  // a row without a model: saved, model_of not asked
    std::vector<ObjectId> removed;
    for (uint64_t f = 1; f <= 120; ++f) save_step(w, b, rng, f, sc, removed);
    CHECK(!b.corpses().empty() && b.state_count() > 0);

    // Save in the host's order: behaviours, world, rng.
    alamo::ByteWriter save;
    b.write(save);
    w.write(save);
    rng.write(save);

    AnimWorld w2;
    AnimBehaviors b2;
    SimRng rng2(1);
    w2.create(500, ma.get(), {}, nullptr, 0);  // pre-existing rows are replaced
    {
        alamo::ByteReader r(save.bytes());
        const bool ok = b2.read(r) && w2.read(r, model_of) && rng2.read(r);
        CHECK(ok && r.ok() && r.done());
    }
    CHECK(world_hash(w2) == world_hash(w) && behaviours_hash(w2, b2) == behaviours_hash(w, b) && rng2 == rng);
    CHECK(!w2.contains(500) && w2.contains(13) && w2.model(13) == nullptr && w2.model(3) == mb_owner.get());
    // write -> read -> write: identical bytes; the documented record sizes.
    CHECK(world_bytes(w2) == world_bytes(w) && behaviour_bytes(b2) == behaviour_bytes(b));
    CHECK(world_bytes(w).size() == 8 + w.size() * kAnimSaveRowBytes);
    CHECK(behaviour_bytes(b).size() == 12 + b.state_count() * kAnimSaveEntryBytes + b.corpses().size() * kAnimSaveCorpseBytes);

    // Both run on: identical hashes, removals, full saved state and poses every tick.
    bool same = true;
    size_t compared = 0;
    for (uint64_t f = 121; f <= 300; ++f) {
        std::vector<ObjectId> rem1, rem2;
        save_step(w, b, rng, f, sc, rem1);
        save_step(w2, b2, rng2, f, sc, rem2);
        same = same && rem1 == rem2 && behaviours_hash(w, b) == behaviours_hash(w2, b2) && rng == rng2 &&
               world_bytes(w) == world_bytes(w2) && behaviour_bytes(b) == behaviour_bytes(b2);
        for (const ObjectId id : w.ids()) {
            const AnimModel* m = w.model(id);
            if (!m || id % 5 != 0) continue;
            std::vector<Mat34> p1(m->skeleton.names.size()), p2(m->skeleton.names.size());
            std::vector<uint8_t> v1(p1.size()), v2(p1.size());
            const bool a1 = w.world_pose(id, p1, v1);
            const bool a2 = w2.world_pose(id, p2, v2);
            same = same && a1 == a2 && v1 == v2 && std::memcmp(p1.data(), p2.data(), p1.size() * sizeof(Mat34)) == 0;
            ++compared;
        }
    }
    CHECK(same && compared > 0);
    std::printf("save: %zu rows / %zu corpses round trip; 180 ticks after load identical (hash, removals, bytes, %zu poses)\n",
                w.size(), b.corpses().size(), compared);

    // ---- rejection: truncated / corrupted data leaves the target unchanged ----
    const std::vector<uint8_t> wb = world_bytes(w), bb = behaviour_bytes(b);
    // A target with other content (w2 one tick further).
    std::vector<ObjectId> rem;
    save_step(w2, b2, rng2, 301, sc, rem);
    const std::vector<uint8_t> tw = world_bytes(w2), tb = behaviour_bytes(b2);
    CHECK(tw != wb || tb != bb);
    const auto world_rejects = [&w2, &tw](std::span<const uint8_t> data, const std::function<const AnimModel*(int32_t)>& mo) {
        alamo::ByteReader r(data);
        const bool ok = w2.read(r, mo);
        return !ok && !r.ok() && world_bytes(w2) == tw;
    };
    const auto behaviours_reject = [&b2, &tb](std::span<const uint8_t> data) {
        alamo::ByteReader r(data);
        const bool ok = b2.read(r);
        return !ok && !r.ok() && behaviour_bytes(b2) == tb;
    };
    bool trunc_ok = true;
    for (size_t cut = 0; cut < wb.size(); ++cut) trunc_ok = trunc_ok && world_rejects(std::span<const uint8_t>(wb.data(), cut), model_of);
    for (size_t cut = 0; cut < bb.size(); ++cut) trunc_ok = trunc_ok && behaviours_reject(std::span<const uint8_t>(bb.data(), cut));
    CHECK(trunc_ok);
    {
        std::vector<uint8_t> bad = wb;
        bad[0] ^= 0x40;  // version
        CHECK(world_rejects(bad, model_of));
        bad = wb;
        bad[4] = 0xFF;  // row count past the data
        bad[5] = 0xFF;
        CHECK(world_rejects(bad, model_of));
        bad = wb;
        bad[8 + 17] = 2;  // row 0 frozen flag: not a bool
        CHECK(world_rejects(bad, model_of));
        bad = wb;
        bad[8 + 30 + 8] = 3;  // row 0 primary slot mode: no SlotMode 3
        CHECK(world_rejects(bad, model_of));
        bad = wb;
        bad[8 + 30] = 0x77;  // row 0 primary type >= 119
        bad[8 + 30 + 1] = 0x01;
        CHECK(world_rejects(bad, model_of));
        bad = wb;
        std::memcpy(bad.data() + 8 + kAnimSaveRowBytes, bad.data() + 8, 4);  // row 1 id == row 0 id: not ascending
        CHECK(world_rejects(bad, model_of));
        // Model resolution: a null model, a model with another name CRC, no resolver.
        CHECK(world_rejects(wb, [](int32_t) -> const AnimModel* { return nullptr; }));
        CHECK(world_rejects(wb, [&sc](int32_t id) -> const AnimModel* { return id % 3 == 0 ? sc.a : sc.b; }));
        CHECK(world_rejects(wb, {}));
    }
    {
        std::vector<uint8_t> bad = bb;
        bad[1] ^= 0x10;  // version
        CHECK(behaviours_reject(bad));
        uint32_t entries = 0;
        std::memcpy(&entries, bb.data() + 4, 4);
        const size_t corpse_count_at = 8 + static_cast<size_t>(entries) * kAnimSaveEntryBytes;
        uint32_t corpses = 0;
        std::memcpy(&corpses, bb.data() + corpse_count_at, 4);
        CHECK(corpses > 0 && entries > 1);
        bad = bb;
        bad[corpse_count_at + 4 + 4] = 4;  // corpse 0 phase: no CorpsePhase 4
        CHECK(corpses == 0 || behaviours_reject(bad));
        bad = bb;
        bad[8 + 61] = 5;  // entry 0 have-yaw flag: not a bool
        CHECK(entries == 0 || behaviours_reject(bad));
        bad = bb;
        bad[8 + 8] = 4;  // entry 0 flinch phase: no FlinchPhase 4
        CHECK(entries == 0 || behaviours_reject(bad));
        bad = bb;
        const float two = 2.0f;
        std::memcpy(bad.data() + 8 + 17, &two, 4);  // entry 0 flinch intensity outside [0, 1]
        CHECK(entries == 0 || behaviours_reject(bad));
        bad = bb;
        if (entries > 1) std::memcpy(bad.data() + 8 + kAnimSaveEntryBytes, bad.data() + 8, 4);  // duplicate entry id
        CHECK(entries < 2 || behaviours_reject(bad));
        bad = bb;
        bad[4] = 0xFF;  // entry count past the data
        bad[6] = 0xFF;
        CHECK(behaviours_reject(bad));
    }
    // A successful read into the dirtied target restores the saved state exactly.
    {
        alamo::ByteReader r(wb);
        CHECK(w2.read(r, model_of) && r.done() && world_bytes(w2) == wb);
        alamo::ByteReader rb(bb);
        CHECK(b2.read(rb) && rb.done() && behaviour_bytes(b2) == bb);
    }
}

#ifdef ALAMO_ANIM_HAVE_VFS
void test_real_data() {
    const char* root = std::getenv("ALAMO_FOC_DATA");
    if (!root || !*root) {
        std::printf("real data: ALAMO_FOC_DATA not set, skipped\n");
        return;
    }
    alamo::vfs::VfsConfig cfg;
    cfg.foc_root = root;
    std::string err;
    auto vfs = alamo::vfs::Vfs::create(cfg, &err);
    if (!vfs) {
        std::printf("FAIL real data: VFS: %s\n", err.c_str());
        ++g_fail;
        return;
    }
    const auto alos = vfs->list("Data\\Art\\Models\\*.alo");
    const auto alas = vfs->list("Data\\Art\\Models\\*.ala");
    ClipCache cache;
    std::vector<std::unique_ptr<AnimModel>> models(alos.size());
    std::atomic<size_t> next{0};
    std::vector<std::thread> threads;
    const unsigned nt = std::max(2u, std::thread::hardware_concurrency());
    for (unsigned i = 0; i < nt; ++i)
        threads.emplace_back([&] {
            for (size_t k = next++; k < alos.size(); k = next++) {
                auto m = std::make_unique<AnimModel>();
                load_anim_model(*vfs, alos[k], {}, *m, &cache);
                models[k] = std::move(m);
            }
        });
    for (auto& t : threads) t.join();
    // Distinct clip files reached by discovery, bound vs mismatched (a file counts once, by its own model).
    std::map<std::string, int> clip_state;  // 1 bound, 0 mismatched
    std::vector<std::string> mismatched;
    size_t models_with_clips = 0, name_mismatch = 0, no_skeleton = 0;
    std::map<std::string, size_t> by_type;
    for (const auto& m : models) {
        if (!m->has_skeleton) ++no_skeleton;
        if (m->has_any_clip()) ++models_with_clips;
        name_mismatch += m->stats.track_name_mismatch;
        for (int32_t t = 0; t < 119; ++t)
            for (const ClipRef& r : m->clips[static_cast<size_t>(t)]) {
                ++by_type[std::string(anim_type_suffix(t))];
                const auto [it, fresh] = clip_state.try_emplace(r.clip->name, r.bound ? 1 : 0);
                if (fresh && !r.bound)
                    mismatched.push_back(r.clip->name + " (model " + std::to_string(m->skeleton.size()) + " bones, ALA " + std::to_string(r.clip->bone_count) + ")");
            }
    }
    size_t bound = 0;
    for (auto& [n, s] : clip_state) bound += static_cast<size_t>(s);
    // ALA files following the naming rule <x>_<SUFFIX>_<NN>.ALA (any suffix of the table).
    size_t named = 0;
    for (const auto& a : alas) {
        std::string up = alamo::normalize_name(a);
        const size_t slash = up.find_last_of('\\');
        up = up.substr(slash + 1);
        if (up.size() < 8) continue;
        const std::string stem = up.substr(0, up.size() - 4);
        const size_t us = stem.find_last_of('_');
        if (us == std::string::npos || stem.size() - us != 3 || !std::isdigit(static_cast<unsigned char>(stem[us + 1])) ||
            !std::isdigit(static_cast<unsigned char>(stem[us + 2])))
            continue;
        for (int32_t t = 0; t < 119; ++t) {
            const std::string suf = "_" + std::string(anim_type_suffix(t));
            if (us >= suf.size() && stem.compare(us - suf.size(), suf.size(), suf) == 0) {
                ++named;
                break;
            }
        }
    }
    std::printf("real data: %zu ALO, %zu ALA (%zu follow the naming rule); %zu models with clips (%zu without a skeleton)\n", alos.size(),
                alas.size(), named, models_with_clips, no_skeleton);
    std::printf("  discovered %zu clip files: %zu bound, %zu mismatched (doc: 2006 / 27 over the EaW + FoC archives); %zu clip parse failures; "
                "%zu bound tracks named differently from their bone\n",
                clip_state.size(), bound, mismatched.size(), cache.failures(), name_mismatch);
    std::printf("  by type: IDLE %zu DIE %zu ATTACK %zu MOVE %zu ATTACKIDLE %zu CINEMATIC %zu BUILD %zu\n", by_type["IDLE"], by_type["DIE"],
                by_type["ATTACK"], by_type["MOVE"], by_type["ATTACKIDLE"], by_type["CINEMATIC"], by_type["BUILD"]);
    CHECK(bound > 1500);
    CHECK(!mismatched.empty() && mismatched.size() < 100);
    CHECK(cache.failures() == 0);
    // Report.
    if (FILE* fp = std::fopen(ALAMO_ANIM_LOG_DIR "/anim_discovery_foc.txt", "wb")) {
        std::fprintf(fp, "R3 clip discovery over the merged FoC + EaW VFS (%s)\n", root);
        std::fprintf(fp, "ALO %zu, ALA %zu (naming rule %zu), clip files discovered %zu: bound %zu, mismatched %zu\n", alos.size(), alas.size(),
                     named, clip_state.size(), bound, mismatched.size());
        for (const auto& s : mismatched) std::fprintf(fp, "mismatch %s\n", s.c_str());
        std::fclose(fp);
    }
    for (size_t i = 0; i < std::min<size_t>(mismatched.size(), 8); ++i) std::printf("  mismatch: %s\n", mismatched[i].c_str());

    // Planet idle: frame 239 duplicates frame 0 (bone Hoth), so the wrap at count - 1 is seamless.
    for (const char* planet : {"W_PLANET_ALDERAAN_HIGH", "W_PLANET_KUAT_HIGH"}) {
        const AnimModel* pm = nullptr;
        for (const auto& m : models)
            if (m->base == planet) pm = m.get();
        if (!pm || !pm->clip(kAnimIdle, 0)) {
            std::printf("  %s: not found\n", planet);
            continue;
        }
        const ClipRef& c = *pm->clip(kAnimIdle, 0);
        CHECK(c.clip->frame_count == 240 && c.clip->fps == 1.0f && c.bound);
        const int32_t hoth = pm->skeleton.find("Hoth");
        std::vector<Mat34> w0(pm->skeleton.size()), w238(pm->skeleton.size()), w2385(pm->skeleton.size());
        clip_world_pose(pm->skeleton, c, 0.0f, w0);
        clip_world_pose(pm->skeleton, c, 238.0f, w238);
        clip_world_pose(pm->skeleton, c, 238.5f, w2385);
        if (hoth >= 0) {
            const Mat34& a = w0[static_cast<size_t>(hoth)];
            const Mat34& b = w238[static_cast<size_t>(hoth)];
            const double ang0 = std::atan2(a.m[1][0], a.m[0][0]) * 180.0 / 3.14159265358979;
            const double ang238 = std::atan2(b.m[1][0], b.m[0][0]) * 180.0 / 3.14159265358979;
            const double ang2385 = std::atan2(w2385[static_cast<size_t>(hoth)].m[1][0], w2385[static_cast<size_t>(hoth)].m[0][0]) * 180.0 / 3.14159265358979;
            std::printf("  %s idle: Hoth %.3f deg at frame 0, %.3f at 238, %.3f at 238.5 (lerp toward frame 0)\n", planet, ang0, ang238, ang2385);
        }
        // The raw frame 239 key equals frame 0's key (Alderaan).
        if (std::string(planet) == "W_PLANET_ALDERAAN_HIGH" && hoth >= 0) {
            for (size_t i = 0; i < c.track_bone.size(); ++i) {
                if (c.track_bone[i] != hoth) continue;
                Quat q0, q239;
                Float3 t0, t239;
                c.clip->sample(static_cast<uint32_t>(i), 0, 0, 0.0f, q0, t0);
                c.clip->sample(static_cast<uint32_t>(i), 239, 239, 0.0f, q239, t239);
                const float dot = std::fabs(q0.x * q239.x + q0.y * q239.y + q0.z * q239.z + q0.w * q239.w);
                std::printf("  Alderaan Hoth key 239 vs key 0: |dot| = %.6f\n", static_cast<double>(dot));
                CHECK(dot > 0.9999f);
            }
        }
    }

    // Cross-compiler pose digest of a real clip (golden recorded 2026-10-27 on vanilla FoC 1.121 data).
    for (const auto& m : models) {
        if (m->base != "EI_ROYALGUARD" && m->base != "EI_ROYALGAURD") continue;
        AnimWorld w;
        AnimCreateParams cp;
        cp.loop_idle = true;
        w.create(1, m.get(), cp, nullptr, 0);
        for (uint64_t f = 1; f <= 47; ++f) w.advance(f);
        std::vector<Mat34> pose(m->skeleton.size());
        if (w.world_pose(1, pose)) {
            const uint64_t d = alamo::fnv1a64(pose.data(), pose.size() * sizeof(Mat34));
            std::printf("  %s idle pose digest at tick 47: %016" PRIx64 " (%zu bones)\n", m->base.c_str(), d, pose.size());
            CHECK(d == 0xc5ee6f248bc5ea2bull);  // golden (vanilla FoC data): identical on MSVC 19.51, clang 20, gcc 14
        }
    }
}
#endif

}  // namespace

// refixf t04: plan_death (no AnimWorld) gives the frame the corpse countdown removes the object, for clips of several
// lengths / rates, no clip, spin-away, Remove_Upon_Death and forever; begin_death draws exactly what plan_death draws, and
// the planned begin_death draws nothing.
void test_death_plan() {
    const auto m = make_model(3, {{kAnimDie, 3, 40, 30.0f, 2},
                                  {kAnimFireDie, 1, 7, 20.0f, 2},
                                  {kAnimFlDie, 1, 2, 30.0f, 2},
                                  {kAnimFwDie, 1, 95, 12.5f, 2},
                                  {kAnimCrushed, 1, 1, 30.0f, 2}});
    CHECK(one_shot_frames(40, 30.0f, 1.0f) == 38 && one_shot_frames(2, 30.0f, 1.0f) == 0 && one_shot_frames(1, 30.0f, 1.0f) == 0);
    CHECK(one_shot_frames(40, 0.0f, 1.0f) == kNeverFrames && one_shot_frames(7, 20.0f, 1.0f) == 8);  // 5 frames at 2/3 per tick
    struct Case {
        uint8_t kind;
        float persist, fade;
        bool rud;
        uint32_t index;
    };
    const Case cases[] = {{1, 1.0f, 0.25f, false, kRandomVariant}, {1, 0.0f, 0.0f, false, 2},   {7, 2.0f, 0.5f, false, kRandomVariant},
                          {4, 0.0f, 0.25f, false, 0},              {2, 0.3f, 3.0f, true, 0},      {14, 0.0f, 0.0f, false, 0},
                          {1, 0.5f, 0.25f, false, 7},              {1, 0.5f, 0.25f, true, 7}};
    uint32_t checked = 0;
    for (const Case& cs : cases) {
        DeathParams dp;
        dp.persistence_seconds = cs.persist;
        dp.fade_seconds = cs.fade;
        dp.remove_upon_death = cs.rud;
        dp.index = cs.index;
        SimRng a(77), b2(77);
        const DeathPlan plan = plan_death(m.get(), dp, cs.kind, a);
        AnimWorld w;
        AnimBehaviors b;
        constexpr uint64_t kDeath = 5;
        w.create(1, m.get(), {}, nullptr, 0);
        const DeathResult r = b.begin_death(w, 1, dp, {}, b2, kDeath, cs.kind);
        CHECK(a.state() == b2.state());  // the same draws
        if (plan.remove_now) {
            CHECK(r == DeathResult::RemoveNow && plan.type == kNoAnimType);
            ++checked;
            continue;
        }
        CHECK(r == DeathResult::Corpse && b.corpse(1)->death_type == plan.type && !plan.forever);
        if (plan.type != kNoAnimType) CHECK(w.slot(1).variant == plan.variant);
        // The service order of the sim: service, then advance, every frame from the death frame on.
        std::vector<ObjectId> removed;
        uint64_t f = kDeath;
        for (; f < 2000 && removed.empty(); ++f) {
            b.service(w, f, removed);
            if (removed.empty()) w.advance(f);
        }
        std::printf("death plan: kind %u type %d variant %u lifetime %llu, countdown removed after %llu\n", static_cast<unsigned>(cs.kind), plan.type, plan.variant,
                    static_cast<unsigned long long>(plan.lifetime), static_cast<unsigned long long>(f - 1 - kDeath));
        CHECK(removed.size() == 1 && f - 1 == kDeath + plan.lifetime);
        // The planned start: no draw, same clip and the same removal frame.
        AnimWorld w2;
        AnimBehaviors pb;
        w2.create(1, m.get(), {}, nullptr, 0);
        SimRng none(5);
        const uint32_t st = none.state();
        CHECK(pb.begin_death(w2, 1, dp, plan, {}, kDeath, cs.kind) == DeathResult::Corpse && none.state() == st);
        CHECK(plan.type == kNoAnimType || (w2.current_type(1) == plan.type && w2.slot(1).variant == plan.variant));
        ++checked;
    }
    CHECK(checked == sizeof cases / sizeof cases[0]);
    {  // spin-away: lifetime = Spin_Away_On_Death_Time; persistence < 0: forever; no model: no clip, no variant draw
        DeathParams sp;
        sp.spin = true;
        sp.spin_away_seconds = 2.0f;
        SimRng rng(9);
        const uint32_t st = rng.state();
        const DeathPlan p = plan_death(m.get(), sp, 1, rng);
        CHECK(p.spin && p.type == kNoAnimType && p.lifetime == 60 && rng.state() == st);  // the caller's spin: no draw
        DeathParams forever;
        SimRng other(9);
        CHECK(plan_death(m.get(), forever, 1, other).forever);
        DeathParams rm;
        rm.remove_upon_death = true;
        const DeathPlan none = plan_death(nullptr, rm, 1, rng);
        CHECK(none.remove_now && rng.state() == st);
        DeathParams keep;
        keep.persistence_seconds = 1.0f;
        CHECK(plan_death(nullptr, keep, 1, rng).lifetime == 30 + 7 && rng.state() == st);
    }
}

// ---- R3 t03: overlay-slot animation events ----------------------------------------------------------------

void test_overlay_events() {
    const auto m = make_model(3, {{kAnimIdle, 1, 30, 30.0f, 2}, {kAnimMove, 1, 24, 30.0f, 2}, {kAnimAttack, 2, 12, 30.0f, 2}});
    const std::string move = clip_stem(m->clip(kAnimMove, 0)->clip->name);
    const std::string atk = clip_stem(m->clip(kAnimAttack, 0)->clip->name);
    const std::string atk1 = clip_stem(m->clip(kAnimAttack, 1)->clip->name);
    AnimEventMap map;
    // Rows out of time order on purpose: the map keeps each clip's events in time order.
    const std::string text = "SOUND\t" + move + "\t20\tStep20\n" + "SURFACE\t" + move + "\t0\tStep0\tB1\n" +  //
                             "SOUND\t" + atk + "\t360\tTail\n" + "SOUND\t" + atk + "\t0\tFire_Start\n" +       //
                             "SOUND\t" + atk + "\t50\tMid\n" + "SURFACE\t" + atk + "\t110\tFX_A\tB1\n" +        //
                             "SOUND\t" + atk1 + "\t0\tOther_Start\n";
    CHECK(map.parse(text) == 7);

    std::vector<AnimWorld::EventMark> marks;
    std::vector<AnimWorld::FiredEvent> fired;
    // One tick of the protocol: mark (ascending id), advance, collect.
    const auto tick = [&](AnimWorld& w, uint64_t f) {
        marks.clear();
        fired.clear();
        for (ObjectId id : w.ids()) w.mark_events(id, marks);
        w.advance(f);
        w.fired_events(marks, map, fired);
    };
    const auto names = [&]() {
        std::string out;
        for (const AnimWorld::FiredEvent& e : fired)
            out += std::to_string(e.id) + (e.slot == kOverlay ? "o:" : "p:") + (e.def ? e.def->name : "?") + " ";
        return out;
    };

    {  // 1. An overlay clip alone crosses its events: [before, after) per advance, one-shot stops at count - 2.
        AnimWorld w;
        w.add(1, m.get(), 0);
        CHECK(w.play_overlay(1, kAnimAttack, 0, 1.0f, 0.0f, nullptr, 0) == PlayResult::Played);
        std::string all;
        for (uint64_t f = 1; f <= 20; ++f) {
            tick(w, f);
            for (const AnimWorld::FiredEvent& e : fired) {
                CHECK(e.slot == kOverlay && e.id == 1 && e.clip == m->clip(kAnimAttack, 0) && e.def);
                all += std::to_string(f) + ":" + e.def->name + " ";
            }
        }
        CHECK(all == "1:Fire_Start 2:Mid 4:FX_A ");  // Tail (360 ms) lies past the one-shot stop (frame 10 = 333 ms)
        CHECK(w.slot(1, kOverlay).mode == SlotMode::Finished);
        marks.clear();
        CHECK(w.mark_events(1, marks) == 0);  // finished overlay: no mark
    }
    {  // 2. Primary + overlay on two objects: per object the primary's events (time order), then the overlay's.
        AnimWorld w;
        for (ObjectId id : {7, 3}) {
            w.add(id, m.get(), 0);
            PlayRequest req;
            req.type = kAnimMove;
            req.loop = true;
            CHECK(w.play(id, req, nullptr, 0) == PlayResult::Played);
            CHECK(w.play_overlay(id, kAnimAttack, 0, 1.0f, 3.0f, nullptr, 0) == PlayResult::Played);
        }
        marks.clear();
        CHECK(w.mark_events(3, marks) == 2 && marks[0].slot == kPrimary && marks[1].slot == kOverlay);
        tick(w, 1);
        CHECK(names() == "3p:Step0 3p:Step20 3o:Fire_Start 7p:Step0 7p:Step20 7o:Fire_Start ");
        CHECK(fired.size() == 6 && fired[0].index == 0 && fired[1].index == 1 && fired[2].index == 0);
        CHECK(fired.size() == 6 && fired[0].def->kind == AnimEventDef::Kind::Surface && fired[0].def->bone == "B1");
        tick(w, 2);
        CHECK(names() == "3o:Mid 7o:Mid ");
        // Events are output only: firing them leaves the state hash alone (same as a world that never collected).
        AnimWorld quiet;
        for (ObjectId id : {7, 3}) {
            quiet.add(id, m.get(), 0);
            PlayRequest req;
            req.type = kAnimMove;
            req.loop = true;
            quiet.play(id, req, nullptr, 0);
            quiet.play_overlay(id, kAnimAttack, 0, 1.0f, 3.0f, nullptr, 0);
        }
        quiet.advance(1);
        quiet.advance(2);
        CHECK(world_hash(quiet) == world_hash(w));
    }
    {  // 3. Loop wrap in the overlay ([port] play_overlay loop): tail [before, end) first, then [0, after).
        AnimWorld w;
        w.add(1, m.get(), 0);
        CHECK(w.play_overlay(1, kAnimAttack, 0, 1.4f, 0.0f, nullptr, 0, true) == PlayResult::Played);
        CHECK(w.slot(1, kOverlay).mode == SlotMode::Loop);
        std::string all;
        for (uint64_t f = 1; f <= 8; ++f) {
            tick(w, f);
            for (const AnimWorld::FiredEvent& e : fired) all += std::to_string(f) + ":" + e.def->name + " ";
        }
        // Frames 1.4, 2.8, ..., 9.8, then 11.2 -> wraps at 11 to 0.2: [326.7, 366.7) holds Tail, [0, 6.7) Fire_Start.
        CHECK(all == "1:Fire_Start 2:Mid 3:FX_A 8:Tail 8:Fire_Start ");
        CHECK(w.slot(1, kOverlay).frame < 1.0f);
    }
    {  // 4. Overlay cleared / replaced between mark and query, and a timed overlay expiring inside the window.
        AnimWorld w;
        w.add(1, m.get(), 0);
        PlayRequest req;
        req.type = kAnimMove;
        req.loop = true;
        w.play(1, req, nullptr, 0);
        w.play_overlay(1, kAnimAttack, 0, 1.0f, 0.0f, nullptr, 0);
        marks.clear();
        fired.clear();
        CHECK(w.mark_events(1, marks) == 2);
        w.clear_overlay(1);  // cleared mid-window: the overlay's events of this advance are dropped
        w.advance(1);
        w.fired_events(marks, map, fired);
        CHECK(names() == "1p:Step0 1p:Step20 ");

        w.play_overlay(1, kAnimAttack, 0, 1.0f, 0.0f, nullptr, 1);
        marks.clear();
        fired.clear();
        w.mark_events(1, marks);
        w.play_overlay(1, kAnimAttack, 1, 1.0f, 0.0f, nullptr, 1);  // replaced by another variant: nothing either
        w.advance(2);
        w.fired_events(marks, map, fired);
        CHECK(names().find("o:") == std::string::npos);

        AnimWorld t;  // timed 0.05 s: expires on the second advance, which still fires [33.3, 66.7) (Mid)
        t.add(1, m.get(), 0);
        t.play_overlay(1, kAnimAttack, 0, 1.0f, 0.05f, nullptr, 0);
        std::string all;
        for (uint64_t f = 1; f <= 4; ++f) {
            tick(t, f);
            for (const AnimWorld::FiredEvent& e : fired) all += std::to_string(f) + ":" + e.def->name + " ";
        }
        CHECK(all == "1:Fire_Start 2:Mid ");
        CHECK(t.slot(1, kOverlay).mode == SlotMode::Finished);
    }
    {  // 5. The previous (fading-out) slot is never marked.
        AnimWorld w;
        w.add(1, m.get(), 0);
        PlayRequest req;
        req.type = kAnimMove;
        req.loop = true;
        w.play(1, req, nullptr, 0);
        req.type = kAnimIdle;
        req.blend_seconds = 0.5f;
        w.play(1, req, nullptr, 0);
        CHECK(w.crossfading(1));
        marks.clear();
        CHECK(w.mark_events(1, marks) == 1 && marks[0].slot == kPrimary && marks[0].type == kAnimIdle);
    }
    std::printf("overlay events: overlay alone, primary-then-overlay order, overlay loop wrap, cleared / replaced / timed OK\n");
}

// ---- R3: §A.12 flinch triggers / gates / intensity, §A.11 default idle (crouch class, presence idles) ----------

void test_flinch_and_idle_rules() {
    // Angle rule against an independent atan2 reading: D = wrap(yaw(dir) - yaw(facing)) into (-180, 180];
    // |D| < 45 back, 45..135 by sign (D > 0 Right, D < 0 Left), > 135 front. Directions avoid the exact boundaries.
    int checked = 0;
    for (int fdeg = 0; fdeg < 360; fdeg += 37)
        for (int ddeg = 1; ddeg < 360; ddeg += 7) {
            const double fr = fdeg * 3.14159265358979323846 / 180.0, dr = ddeg * 3.14159265358979323846 / 180.0;
            const float fx = static_cast<float>(std::cos(fr)), fy = static_cast<float>(std::sin(fr));
            const float dx = static_cast<float>(3.0 * std::cos(dr)), dy = static_cast<float>(3.0 * std::sin(dr));
            double d = std::atan2(static_cast<double>(dy), static_cast<double>(dx)) * 180.0 / 3.14159265358979323846 -
                       std::atan2(static_cast<double>(fy), static_cast<double>(fx)) * 180.0 / 3.14159265358979323846;
            while (d > 180.0) d -= 360.0;
            while (d <= -180.0) d += 360.0;
            const double a = std::fabs(d);
            if (std::fabs(a - 45.0) < 0.5 || std::fabs(a - 135.0) < 0.5) continue;
            const int32_t want = a < 45.0 ? kAnimFlinchBack : a > 135.0 ? kAnimFlinchFront : d > 0.0 ? kAnimFlinchRight : kAnimFlinchLeft;
            CHECK(flinch_type(fx, fy, dx, dy, false) == want);
            CHECK(flinch_type(fx, fy, dx, dy, true) == want + (kAnimAttackFlinchLeft - kAnimFlinchLeft));
            ++checked;
        }
    CHECK(checked > 400);

    // Explosion intensity: clamp(I (1 - 0.5 d2 / R2), 0.25, 1), d horizontal.
    CHECK(explosion_flinch_intensity(1.0f, 0.0f, 0.0f, 100.0f) == 1.0f);
    CHECK(explosion_flinch_intensity(1.0f, 60.0f, 80.0f, 100.0f) == 0.5f);  // d = R
    CHECK(std::fabs(explosion_flinch_intensity(1.0f, 0.0f, 100.0f * std::sqrt(1.2f), 100.0f) - 0.4f) < 1e-5f);
    CHECK(explosion_flinch_intensity(1.0f, 141.0f, 141.0f, 100.0f) == kMinExplosionFlinchIntensity);  // corner of the cube
    CHECK(explosion_flinch_intensity(0.1f, 0.0f, 0.0f, 100.0f) == 0.25f && explosion_flinch_intensity(3.0f, 0.0f, 0.0f, 100.0f) == 1.0f);
    CHECK(explosion_flinch_intensity(0.6f, 5.0f, 5.0f, 0.0f) == 0.6f);  // [port] R <= 0: the intensity alone
    CHECK(kDefaultFlinchIntensity == 1.0f && kDefaultFlinchRange == 100.0f);

    // Gates on the current type: idle-like / attack-like only; never Attack Idle (6), none, move, turn, death.
    CHECK(flinch_allowed_from(kAnimIdle) && flinch_allowed_from(kAnimIdleCrouch) && flinch_allowed_from(77) && flinch_allowed_from(78));
    CHECK(flinch_allowed_from(kAnimAttack) && flinch_allowed_from(kAnimFlinchBack));
    CHECK(!flinch_allowed_from(kAnimAttackIdle) && !flinch_allowed_from(kNoAnimType) && !flinch_allowed_from(kAnimMove));
    CHECK(!flinch_allowed_from(kAnimTurnLeft) && !flinch_allowed_from(kAnimDie) && !flinch_allowed_from(kAnimAttackFlinchBack));

    const auto m = behaviour_model();
    const FlinchParams fp;  // 0.25 blend (7.5 frames), 0.5 hold, 1.0 / 3.0 intervals
    IdleParams ip;
    ip.loop_idle = true;
    {
        AnimWorld w;
        AnimBehaviors b;
        SimRng rng(41);
        for (ObjectId id = 1; id <= 4; ++id) w.add(id, m.get(), 0);
        // No clip (type -1): no flinch, no draw.
        uint32_t st = rng.state();
        CHECK(!b.on_hit(w, 1, fp, ip, 1, 0, 1, 0, rng, 1) && rng.state() == st);
        // Attack Idle never flinches (§A.12: the old A.7 row said it did).
        PlayRequest ai;
        ai.type = kAnimAttackIdle;
        w.play(2, ai, &rng, 0);
        st = rng.state();
        CHECK(!b.on_hit(w, 2, fp, ip, 1, 0, 1, 0, rng, 1) && rng.state() == st && w.current_type(2) == kAnimAttackIdle);
        // Deploys types never flinch.
        PlayRequest idle;
        idle.type = kAnimIdle;
        idle.loop = true;
        w.play(3, idle, &rng, 0);
        FlinchParams dep = fp;
        dep.deploys = true;
        st = rng.state();
        CHECK(!b.on_hit(w, 3, dep, ip, 1, 0, 1, 0, rng, 1) && rng.state() == st && w.current_type(3) == kAnimIdle);
        // A flinch in progress blocks another even with zero hold / interval (timer expired, +0x58 / flinch set).
        FlinchParams zero = fp;
        zero.hold_seconds = 0.0f;
        zero.idle_interval_seconds = 0.0f;
        CHECK(b.on_hit(w, 3, zero, ip, 1, 0, 1, 0, rng, 2) && b.flinch_phase(3) == AnimBehaviors::FlinchPhase::BlendIn);
        st = rng.state();
        CHECK(!b.on_hit(w, 3, zero, ip, 1, 0, -1, 0, rng, 2) && rng.state() == st && w.current_type(3) == kAnimFlinchBack);
    }
    {
        // Intensity: the clip freezes (speed 1e-4) once the blend-in weight reaches I (I = 1: when the fade ends), the
        // hold is Max_Flinch_Hold_Seconds x I counted from the freeze, then C is replayed at speed 1.
        for (const float level : {1.0f, 0.5f}) {
            AnimWorld w;
            AnimBehaviors b;
            AnimCreateParams cp;
            cp.loop_idle = true;
            w.create(1, m.get(), cp, nullptr, 0);
            SimRng rng(5);
            CHECK(b.on_hit(w, 1, fp, ip, 1, 0, 0, -1, rng, 10, level));
            const uint64_t hold = seconds_to_frames(fp.hold_seconds * level);
            std::vector<ObjectId> removed;
            uint64_t f = 11, frozen_at = 0;
            float weight = 0.0f, frame_at_freeze = 0.0f;
            for (; f < 60 && frozen_at == 0; ++f) {
                b.service(w, f, removed, &rng);
                if (b.flinch_phase(1) == AnimBehaviors::FlinchPhase::Hold) {
                    frozen_at = f;
                    weight = w.crossfade_weight(1);
                    frame_at_freeze = w.slot(1).frame;
                }
                w.advance(f);
            }
            CHECK(frozen_at > 0 && w.slot(1).speed == kFlinchFrozenSpeed && b.next_flinch_frame(1) == frozen_at + hold);
            if (level < 1.0f) CHECK(weight >= 0.5f && w.crossfading(1));  // frozen mid-blend: a partial pose
            else CHECK(weight == 1.0f);
            for (; f < frozen_at + hold; ++f) {
                b.service(w, f, removed, &rng);
                w.advance(f);
            }
            CHECK(w.current_type(1) == kAnimFlinchLeft && b.flinching(1) && w.slot(1).frame - frame_at_freeze < 0.01f);
            b.service(w, f, removed, &rng);  // hold end: C replayed, speed 1
            CHECK(w.current_type(1) == kAnimIdle && b.flinch_phase(1) == AnimBehaviors::FlinchPhase::Return && w.slot(1).speed == 1.0f);
        }
    }
    {
        // §A.11 idle: crouch-class change restarts mid-clip; other changes wait for the clip end; presence idles take the
        // fidget timer and the synced draws unchanged; no clip of T: the play fails and the finished clip holds.
        const auto pm = make_model(3, {{kAnimIdle, 2, 30, 30.0f, 2}, {kAnimIdleCrouch, 1, 30, 30.0f, 2}, {77, 1, 40, 30.0f, 2}});
        const auto guard = make_model(3, {{kAnimIdle, 2, 30, 30.0f, 2}});  // Imperial Guard: no ATTENTION clip
        AnimWorld w;
        AnimBehaviors b;
        SimRng rng(77);
        w.add(1, pm.get(), 0);
        PlayRequest idle;
        idle.type = kAnimIdle;
        w.play(1, idle, nullptr, 0);
        for (uint64_t f = 1; f <= 5; ++f) w.advance(f);
        IdleParams att = ip;
        att.idle_type = 77;
        uint32_t st = rng.state();
        CHECK(!b.service_idle(w, 1, att, rng, 6) && w.current_type(1) == kAnimIdle && rng.state() == st);  // 0 -> 77 waits
        IdleParams crouch = ip;
        crouch.idle_type = kAnimIdleCrouch;
        CHECK(b.service_idle(w, 1, crouch, rng, 6) && w.current_type(1) == kAnimIdleCrouch);  // 0 -> 71: at once
        SimRng replay = rng;
        CHECK(b.service_idle(w, 1, att, rng, 7) && w.current_type(1) == 77);  // 71 -> 77: crouch class changes again
        // Timer expired at the 71 restart (timer 0 <= 6), re-armed then; here the timer has not expired: no draw.
        CHECK(rng.state() == replay.state() && w.slot(1).variant == 0);
        // 77 runs to its end (one-shot), then the idle behaviour replays T = 77 with the fidget timer rules: a single
        // ATTENTION clip -> variant 0, no variant draw; the timer draw only when it expired.
        uint64_t f = 8;
        for (; w.current_mode(1) != SlotMode::Finished; ++f) w.advance(f);
        CHECK(b.service_idle(w, 1, ip, rng, f) && w.current_type(1) == kAnimIdle);  // T = 0 after 77: at the clip end
        const uint64_t timer = w.idle_timer(1);
        CHECK(timer > 6);
        for (; w.current_mode(1) != SlotMode::Finished; ++f) w.advance(f);
        const uint64_t at = std::max(f, timer);
        replay = rng;
        CHECK(b.service_idle(w, 1, att, rng, at) && w.current_type(1) == 77 && w.slot(1).variant == 0);
        replay.range_int(210, 450);  // expired timer: only the timer draw (count 1 -> no variant draw)
        CHECK(rng.state() == replay.state() && w.idle_timer(1) > at);

        // Guard: idle finishes, T = 77 has no clip: the timer draw happens once, the play fails, the pose holds.
        AnimWorld gw;
        AnimBehaviors gb;
        gw.add(2, guard.get(), 0);
        gw.play(2, idle, nullptr, 0);
        uint64_t g = 1;
        for (; gw.current_mode(2) != SlotMode::Finished; ++g) gw.advance(g);
        const float held = gw.slot(2).frame;
        const uint32_t before = rng.state();
        CHECK(!gb.service_idle(gw, 2, att, rng, g));
        CHECK(rng.state() != before && gw.current_type(2) == kAnimIdle && gw.current_mode(2) == SlotMode::Finished);
        const uint32_t after = rng.state();
        for (uint64_t k = 1; k <= 3; ++k) {
            gw.advance(g + k);
            CHECK(!gb.service_idle(gw, 2, att, rng, g + k) && rng.state() == after && gw.slot(2).frame == held);
        }
    }
    {
        // The crouch-class restart cuts only idle / crouch clips: a crouched unit's flinch (from 71) and attack survive
        // the next locomotor services; leaving the crouch still restarts the crouch idle at once.
        CHECK(idle_class_restart(kAnimIdle, kAnimIdleCrouch) && idle_class_restart(kAnimIdleCrouch, kAnimIdle));
        CHECK(idle_class_restart(kAnimTurnRightCrouch, kAnimIdle) && idle_class_restart(77, kAnimIdleCrouch));
        CHECK(!idle_class_restart(kAnimFlinchBack, kAnimIdleCrouch) && !idle_class_restart(kAnimAttack, kAnimIdleCrouch));
        CHECK(!idle_class_restart(kAnimAttackIdle, kAnimIdleCrouch) && !idle_class_restart(kAnimDie, kAnimIdleCrouch));
        CHECK(!idle_class_restart(kAnimIdle, 77) && !idle_class_restart(kNoAnimType, kAnimIdleCrouch));
        const auto cm = make_model(3, {{kAnimIdle, 2, 30, 30.0f, 2},
                                       {kAnimIdleCrouch, 1, 30, 30.0f, 2},
                                       {kAnimAttack, 1, 40, 30.0f, 2},
                                       {kAnimFlinchBack, 1, 10, 30.0f, 2},
                                       {kAnimFlinchLeft, 1, 10, 30.0f, 2}});
        AnimWorld w;
        AnimBehaviors b;
        SimRng rng(3);
        LocomotorParams lp;
        LocomotorInput in;
        in.crouched = true;
        w.add(1, cm.get(), 0);
        w.add(2, cm.get(), 0);
        CHECK(b.service_locomotor(w, 1, lp, in, rng, 1) == 1 && w.current_type(1) == kAnimIdleCrouch);
        CHECK(b.service_locomotor(w, 2, lp, in, rng, 1) == 1 && w.current_type(2) == kAnimIdleCrouch);
        CHECK(b.on_hit(w, 1, fp, ip, 1, 0, 1, 0, rng, 2) && w.current_type(1) == kAnimFlinchBack);
        PlayRequest atk;
        atk.type = kAnimAttack;
        CHECK(w.play(2, atk, &rng, 2) == PlayResult::Played);
        std::vector<ObjectId> removed;
        for (uint64_t f = 3; f <= 8; ++f) {
            CHECK(b.service_locomotor(w, 1, lp, in, rng, f) == 0 && b.service_locomotor(w, 2, lp, in, rng, f) == 0);
            b.service(w, f, removed, &rng);
            w.advance(f);
            CHECK(w.current_type(1) == kAnimFlinchBack && b.flinch_phase(1) != AnimBehaviors::FlinchPhase::None);
            CHECK(w.current_type(2) == kAnimAttack);
        }
        AnimWorld u;  // leaving the crouch mid-clip: 71 -> 0 at once
        u.add(3, cm.get(), 0);
        b.service_locomotor(u, 3, lp, in, rng, 1);
        u.advance(2);
        in.crouched = false;
        CHECK(u.current_type(3) == kAnimIdleCrouch && u.current_mode(3) != SlotMode::Finished);
        CHECK(b.service_locomotor(u, 3, lp, in, rng, 3) == 1 && u.current_type(3) == kAnimIdle);
    }
    {
        // Stop-path fallback through the fields the sim sets (sim_anim idle_params / service_locomotor): presence idle
        // 77 in IdleParams::idle_type and LocomotorInput::idle_type, no ATTENTION clip (Imperial Guard). Stopping plays
        // IdleParams::base_idle_type (Idle), not the Move loop in place; then the idle service holds the finished idle.
        const auto gm = make_model(3, {{kAnimIdle, 2, 30, 30.0f, 2}, {kAnimMove, 1, 24, 30.0f, 2}});
        AnimWorld w;
        AnimBehaviors b;
        SimRng rng(8);
        LocomotorParams lp;
        lp.idle.idle_type = 77;
        lp.idle.base_idle_type = kAnimIdle;
        LocomotorInput in;
        in.idle_type = 77;
        in.speed = 1.0f;
        in.max_speed = 1.0f;
        w.add(1, gm.get(), 0);
        CHECK(b.service_locomotor(w, 1, lp, in, rng, 1) == 1 && w.current_type(1) == kAnimMove);
        w.advance(1);
        in.speed = 0.0f;
        CHECK(b.service_locomotor(w, 1, lp, in, rng, 2) == 1 && w.current_type(1) == kAnimIdle && w.current_mode(1) == SlotMode::OneShot);
        uint64_t f = 2;
        for (; w.current_mode(1) != SlotMode::Finished && f < 100; ++f) {
            w.advance(f);
            b.service_locomotor(w, 1, lp, in, rng, f + 1);
        }
        const float held = w.slot(1).frame;
        for (uint64_t k = 0; k < 3; ++k, ++f) {
            CHECK(b.service_locomotor(w, 1, lp, in, rng, f) == 0);  // T = 77 has no clip: the play fails
            w.advance(f);
            CHECK(w.current_type(1) == kAnimIdle && w.current_mode(1) == SlotMode::Finished && w.slot(1).frame == held);
        }
    }
    std::printf("flinch / idle rules: angle sweep %d, explosion intensity, gates, partial freeze, crouch class, presence idle OK\n", checked);
}

// ---- R3f t02/t03: the IDLE service's gates (§A.11 "Exact order of one IDLE service") through LocomotorInput -------
void test_presence_gates() {
    const auto m = behaviour_model();  // Idle 2 x 30, Attack 12, Attack Idle 20, flinches
    const FlinchParams fp;
    IdleParams ip;
    ip.loop_idle = true;
    LocomotorParams lp;
    {
        // 5a: with a target the combat countdown is redrawn EVERY frame (90 + synced int [0, 30]); an idle clip is left
        // alone; moving keeps the countdown without a draw; without a target it counts down.
        AnimWorld w;
        AnimBehaviors b;
        SimRng rng(91);
        w.add(1, m.get(), 0);
        PlayRequest idle;
        idle.type = kAnimIdle;
        w.play(1, idle, nullptr, 0);
        LocomotorInput in;
        in.idle_target = 1;
        for (uint64_t f = 1; f <= 4; ++f) {
            SimRng replay = rng;
            CHECK(b.service_locomotor(w, 1, lp, in, rng, f) == 0 && w.current_type(1) == kAnimIdle);
            const uint32_t want = kEngagedBaseFrames + static_cast<uint32_t>(replay.range_int(0, kEngagedRandomFrames));
            CHECK(b.combat_countdown(1) == want && rng.state() == replay.state() && want >= 90 && want <= 120);
            w.advance(f);
        }
        const uint32_t drawn = b.combat_countdown(1);
        in.idle_moving = true;
        uint32_t st = rng.state();
        CHECK(b.service_locomotor(w, 1, lp, in, rng, 5) == 0 && b.combat_countdown(1) == drawn && rng.state() == st);
        in.idle_moving = false;
        in.idle_hold = true;  // Deploys busy: nothing
        CHECK(b.service_locomotor(w, 1, lp, in, rng, 6) == 0 && b.combat_countdown(1) == drawn && rng.state() == st);
        in.idle_hold = false;
        in.idle_target = 0;
        CHECK(b.service_locomotor(w, 1, lp, in, rng, 7) == 0 && b.combat_countdown(1) == drawn - 1 && rng.state() == st);
        CHECK(b.combat_timer(1) == 0);  // the pre-R3f timer is not used on this path
        // The legacy path (idle_target -1) leaves the countdown alone.
        LocomotorInput legacy;
        CHECK(b.service_locomotor(w, 1, lp, legacy, rng, 8) == 0 && b.combat_countdown(1) == drawn - 1);
    }
    {
        // A finished attack: Attack Idle (blend 0) only while engaged; without a target Attack Idle again (blend draw) until
        // the countdown runs out, then the default idle T at the clip end.
        AnimWorld w;
        AnimBehaviors b;
        SimRng rng(5);
        w.add(1, m.get(), 0);
        PlayRequest atk;
        atk.type = kAnimAttack;
        atk.index = 0;
        CHECK(w.play(1, atk, &rng, 0) == PlayResult::Played);
        LocomotorInput in;
        in.idle_target = 1;
        uint64_t f = 1;
        for (; w.current_mode(1) != SlotMode::Finished && f < 100; ++f) {
            CHECK(b.service_locomotor(w, 1, lp, in, rng, f) == 0 && w.current_type(1) == kAnimAttack);  // mid-clip: no play
            w.advance(f);
        }
        CHECK(b.service_locomotor(w, 1, lp, in, rng, f) == 1 && w.current_type(1) == kAnimAttackIdle);
        ++f;
        // Engaged with an unfinished Attack Idle, or an idle clip: nothing.
        CHECK(b.service_locomotor(w, 1, lp, in, rng, f) == 0 && w.current_type(1) == kAnimAttackIdle);
        in.idle_target = 0;
        int attack_idles = 0;
        bool back_to_idle = false;
        for (++f; f < 600 && !back_to_idle; ++f) {
            w.advance(f);
            const bool fin = w.current_mode(1) == SlotMode::Finished;
            const uint32_t before = b.combat_countdown(1);
            const uint32_t played = b.service_locomotor(w, 1, lp, in, rng, f);
            CHECK(b.combat_countdown(1) == (before > 0 ? before - 1 : 0));
            CHECK(played == (fin ? 1u : 0u));
            if (played && w.current_type(1) == kAnimAttackIdle) ++attack_idles;
            if (played && w.current_type(1) == kAnimIdle) {
                back_to_idle = true;
                CHECK(b.combat_countdown(1) == 0);
            }
        }
        CHECK(back_to_idle && attack_idles >= 4);
    }
    {
        // 1. Moving: a flinch still blending in drops to Hold (intensity 0); Hold / Return untouched; no idle play while
        // a flinch is busy, even from a finished clip; the flinch then ends through its own steps.
        AnimWorld w;
        AnimBehaviors b;
        SimRng rng(17);
        AnimCreateParams cp;
        cp.loop_idle = true;
        w.create(1, m.get(), cp, nullptr, 0);
        CHECK(b.on_hit(w, 1, fp, ip, 1, 0, 1, 0, rng, 2) && b.flinch_phase(1) == AnimBehaviors::FlinchPhase::BlendIn);
        LocomotorInput in;
        in.idle_target = 0;
        in.idle_moving = true;
        CHECK(b.service_locomotor(w, 1, lp, in, rng, 3) == 0 && b.flinch_phase(1) == AnimBehaviors::FlinchPhase::Hold);
        CHECK(b.service_locomotor(w, 1, lp, in, rng, 4) == 0 && b.flinch_phase(1) == AnimBehaviors::FlinchPhase::Hold);
        in.idle_moving = false;
        std::vector<ObjectId> removed;
        uint64_t f = 5;
        for (; b.flinch_phase(1) != AnimBehaviors::FlinchPhase::None && f < 200; ++f) {
            const int32_t before = w.current_type(1);
            CHECK(b.service_locomotor(w, 1, lp, in, rng, f) == 0 && w.current_type(1) == before);
            b.service(w, f, removed, &rng);
            w.advance(f);
        }
        CHECK(f < 200 && b.flinch_phase(1) == AnimBehaviors::FlinchPhase::None);
        // The moving drop is state: hash / save round trip with the countdown.
        in.idle_target = 1;
        b.service_locomotor(w, 1, lp, in, rng, f);
        CHECK(b.combat_countdown(1) >= 90);
        alamo::StateHasher h1, h2;
        b.hash(h1);
        alamo::ByteWriter bw;
        b.write(bw);
        AnimBehaviors back;
        alamo::ByteReader br(bw.bytes());
        CHECK(back.read(br) && br.done());
        back.hash(h2);
        CHECK(h1.digest() == h2.digest() && back.combat_countdown(1) == b.combat_countdown(1));
        AnimBehaviors other;  // the countdown is hashed
        alamo::ByteReader br2(bw.bytes());
        CHECK(other.read(br2));
        in.idle_target = 0;
        other.service_locomotor(w, 1, lp, in, rng, f + 1);
        alamo::StateHasher h3;
        other.hash(h3);
        CHECK(h3.digest() != h1.digest());
    }
    std::printf("presence gates (R3f t02/t03): countdown draw per engaged frame, Attack Idle / T after attacks, moving drop OK\n");
}

int main() {
    test_types();
    test_death_plan();
    test_frame_rules();
    test_play_rules();
    test_crossfade_bits();
    test_idle_fidgets();
    test_poses();
    test_determinism();
    test_behaviors();
    test_events_and_locomotor();
    test_overlay_events();
    test_flinch_and_idle_rules();
    test_presence_gates();  // R3f t03
    test_save();
#ifdef ALAMO_ANIM_HAVE_VFS
    test_real_data();
#else
    std::printf("real data: engine/vfs not configured, skipped\n");
#endif
    if (g_fail) {
        std::printf("%d check(s) FAILED\n", g_fail);
        return 1;
    }
    std::printf("all anim tests passed\n");
    return 0;
}
