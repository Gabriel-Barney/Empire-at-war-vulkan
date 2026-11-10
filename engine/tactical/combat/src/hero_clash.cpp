// s78n t06 - hero clashes: HEROCLASH.XML and the land mode's HeroClashManager (docs/re/HERO_CLASH.md §1-§6).
#include "alamo/tactical/combat/hero_clash.hpp"

#include <algorithm>
#include <cmath>

#include "alamo/core/bytes.hpp"
#include "alamo/core/dmath.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/data/xml.hpp"
#include "alamo/objects/type_fields.hpp"

namespace alamo::tactical::combat {

namespace {

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool iequal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (lower(a[i]) != lower(b[i])) return false;
    return true;
}

float number(std::string_view v, float fallback) { return v.empty() ? fallback : static_cast<float>(objects::c_atof(v)); }

bool yes(std::string_view v) { return iequal(v, "yes") || iequal(v, "true") || v == "1"; }

std::vector<std::string> split_list(std::string_view v) {
    std::vector<std::string> out;
    std::string cur;
    for (const char c : v) {
        if (c == ',' || c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (!cur.empty()) out.push_back(std::move(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(std::move(cur));
    return out;
}

// 0x20acd0's yaw: atan2 in degrees, [0, 360).
float yaw360(float dx, float dy) {
    float y = dmath::atan2(dy, dx) * (180.0f / dmath::kPiF);
    if (y < 0.0f) y += 360.0f;
    if (y >= 360.0f) y -= 360.0f;
    return y;
}

float norm360(float y) {
    while (y < 0.0f) y += 360.0f;
    while (y >= 360.0f) y -= 360.0f;
    return y;
}

float wrap180(float d) {
    while (d > 180.0f) d -= 360.0f;
    while (d <= -180.0f) d += 360.0f;
    return d;
}

float dist2(Vec3 a, Vec3 b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

Vec3 lerp(Vec3 a, Vec3 b, float s) { return {a.x + (b.x - a.x) * s, a.y + (b.y - a.y) * s, a.z + (b.z - a.z) * s}; }

void put_vec(ByteWriter& w, Vec3 v) { w.f32(v.x).f32(v.y).f32(v.z); }
Vec3 get_vec(ByteReader& r) {
    Vec3 v;
    v.x = r.f32();
    v.y = r.f32();
    v.z = r.f32();
    return v;
}

constexpr uint32_t kSaveVersion = 1;

}  // namespace

// ---- HEROCLASH.XML (§1) ---------------------------------------------------------------------------------------------

bool parse_hero_clash_xml(std::span<const uint8_t> bytes, HeroClashData& out, std::vector<std::string>* diag) {
    const data::XmlDocument doc = data::XmlDocument::parse(bytes, "HeroClash.xml");
    const data::XmlNode root = doc.root();
    if (!root || !iequal(root.name(), "Hero_Clashes")) return false;
    for (data::XmlNode n : root.children("Hero_Clash")) {
        HeroClashEntry e;
        e.name = std::string(n.attribute("Name"));
        const std::string_view ct = n.child_text("Clash_Type");
        e.clash_type = iequal(ct, "Melee_Duel") ? kClashMelee : iequal(ct, "Action_Driven") ? kClashActionDriven : -1;
        e.clash_range = number(n.child_text("Clash_Range"), 0.0f);
        e.play_conversation = yes(n.child_text("Play_Conversation_Events"));
        e.combat_distance = number(n.child_text("Combat_Distance"), 0.0f);
        e.damage_amount = number(n.child_text("Damage_Amount"), 0.0f);
        e.damage_percentage = number(n.child_text("Damage_Percentage"), 0.0f);
        e.first_win = number(n.child_text("First_Hero_Win_Exchange_Chance"), 0.0f);
        e.second_win = number(n.child_text("Second_Hero_Win_Exchange_Chance"), 0.0f);
        e.first_multiplier = number(n.child_text("First_Hero_Damage_Multiplier"), 0.0f);
        e.second_multiplier = number(n.child_text("Second_Hero_Damage_Multiplier"), 0.0f);
        e.first_type = std::string(n.child_text("First_Hero_Type"));
        e.second_type = std::string(n.child_text("Second_Hero_Type"));
        e.involved = split_list(n.child_text("Involved_Hero_Types"));
        static constexpr const char* kFirst[4] = {"First_Hero_Win_Anim_Type", "First_Hero_Lose_Anim_Type",
                                                  "First_Hero_Draw_Anim_Type", "First_Hero_Conversation_Anim_Type"};
        static constexpr const char* kSecond[4] = {"Second_Hero_Win_Anim_Type", "Second_Hero_Lose_Anim_Type",
                                                   "Second_Hero_Draw_Anim_Type", "Second_Hero_Conversation_Anim_Type"};
        for (size_t k = 0; k < 4; ++k) {
            e.first_anims[k] = std::string(n.child_text(kFirst[k]));
            e.second_anims[k] = std::string(n.child_text(kSecond[k]));
        }
        e.first_speech = std::string(n.child_text("First_Hero_Win_Speech"));
        e.second_speech = std::string(n.child_text("Second_Hero_Win_Speech"));
        if (const data::XmlNode acts = n.child("Clash_Actions")) e.action_count = static_cast<uint32_t>(acts.child_count());
        // §1.1: first + second > 1 is the loader's error; both chances become 0.3.
        if (e.first_win + e.second_win > 1.0f) {
            if (diag) diag->push_back("hero clash " + e.name + ": win exchange chances add up to more than 1; both set to 0.3");
            e.first_win = 0.3f;
            e.second_win = 0.3f;
        }
        out.entries.push_back(std::move(e));  // file order, duplicate names kept (0x4814d0)
    }
    return true;
}

bool load_hero_clash_data(const FileProvider& files, HeroClashData& out, std::vector<std::string>* diag) {
    out = HeroClashData{};
    std::vector<uint8_t> bytes;
    if (!files.read("Data\\XML\\HEROCLASH.XML", bytes)) {
        if (diag) diag->push_back("hero clash: HEROCLASH.XML not found (no hero clashes)");
        return false;
    }
    return parse_hero_clash_xml(bytes, out, diag);
}

// ---- exchange rules (§3.6) ------------------------------------------------------------------------------------------

uint8_t clash_pick_outcome(uint8_t previous, float first_win, float second_win, SimRng& rng) {
    const float w[4] = {0.0f, first_win, second_win, 1.0f - first_win - second_win};
    float total = 0.0f;
    for (uint8_t k = 1; k <= 3; ++k)
        if (k != previous) total += w[k];
    float r = rng.range_float(0.0f, total);  // the one synced draw of the exchange (0x532480)
    for (uint8_t k = 1; k <= 3; ++k) {
        if (k == previous) continue;
        r -= w[k];
        if (r <= 0.0f) return k;
    }
    return kExchangeDraw;
}

int32_t clash_exchange_damage(const HeroClashEntry& e, float loser_start_health, bool winner_is_first_type, bool winner_is_second_type) {
    const float mult = winner_is_first_type ? e.first_multiplier : winner_is_second_type ? e.second_multiplier : 0.0f;
    const float raw = (loser_start_health * e.damage_percentage + e.damage_amount) * mult;
    return static_cast<int32_t>(std::round(raw));  // round half away from zero
}

// ---- manager (§2) -----------------------------------------------------------------------------------------------------

void HeroClashManager::set_data(const HeroClashData* data, std::vector<std::pair<int32_t, int32_t>> types) {
    data_ = data;
    types_ = std::move(types);
    if (data_) types_.resize(data_->entries.size(), {-1, -1});
}

std::vector<std::pair<ObjectId, ObjectId>> HeroClashManager::pairs() const {
    std::vector<std::pair<ObjectId, ObjectId>> out;
    out.reserve(clashes_.size());
    for (const Clash& c : clashes_) out.emplace_back(c.hero1, c.hero2);
    return out;
}

void HeroClashManager::clear() {
    if (!clashes_.empty()) changed_ = true;
    clashes_.clear();
    ended_.clear();
}

void HeroClashManager::update(uint32_t frame, HeroClashHost& host, SimRng& rng) {
    if (!data_) return;
    detect(frame, host);
    // Every running clash in list order; a clash may end itself (removed here, the next one still runs).
    for (size_t i = 0; i < clashes_.size();) {
        if (update_clash(clashes_[i], frame, host, rng)) {
            ++i;
        } else {
            clashes_.erase(clashes_.begin() + static_cast<std::ptrdiff_t>(i));
            changed_ = true;
        }
    }
}

bool HeroClashManager::candidate_ok(HeroClashHost& host, ObjectId id, int32_t other_type) const {
    // 0x56c540 (an object that is not a team container): not on an active jet pack, not dying / a death clone /
    // removed, not in a clash, and no attack target or one of the other hero type.
    if (!host.alive(id) || host.in_clash(id) || host.jetpacking(id)) return false;
    const ObjectId t = host.attack_target(id);
    return t == kNoObject || host.type_of(t) == other_type;
}

void HeroClashManager::detect(uint32_t frame, HeroClashHost& host) {
    if (frame % 15u != 0u) return;  // 0x56c790: every 15th frame; no draws
    std::vector<ObjectId> a_list, b_list;
    for (uint32_t i = 0; i < data_->entries.size(); ++i) {
        const HeroClashEntry& e = data_->entries[i];
        // s78n t06 [port] (§4): Action_Driven entries are parsed and never started (shipped Clash_Range 0.0: retail needs
        // two coincident positions); an entry without a known type never starts in retail either.
        if (e.clash_type != kClashMelee) continue;
        const auto [t1, t2] = types_[i];
        if (t1 < 0 || t2 < 0) continue;
        a_list.clear();
        b_list.clear();
        host.candidates(t1, a_list);
        // s78n t06 [port]: ascending object id (retail: the object manager's list order, creation order in practice).
        std::sort(a_list.begin(), a_list.end());
        std::erase_if(a_list, [&](ObjectId id) { return !candidate_ok(host, id, t2); });
        if (a_list.empty()) continue;
        host.candidates(t2, b_list);
        std::sort(b_list.begin(), b_list.end());
        std::erase_if(b_list, [&](ObjectId id) { return !candidate_ok(host, id, t1); });
        const float r2 = e.clash_range * e.clash_range;
        for (const ObjectId a : a_list) {
            for (size_t k = 0; k < b_list.size(); ++k) {
                const ObjectId b = b_list[k];
                if (dist2(host.position(a), host.position(b)) > r2) continue;
                if (host.in_clash(a) || host.in_clash(b)) continue;  // the involved list: any +0x3b4 == 1 -> skip
                if (start(i, a, b, host, frame)) {
                    b_list.erase(b_list.begin() + static_cast<std::ptrdiff_t>(k));  // each B joins at most one clash
                    break;                                                       // next a
                }
            }
        }
    }
}

bool HeroClashManager::start(uint32_t entry, ObjectId a, ObjectId b, HeroClashHost& host, uint32_t frame) {
    (void)frame;
    // 0x6ac780: hero1 / hero2 = the involved objects of First / Second_Hero_Type (the first match each).
    const auto [t1, t2] = types_[entry];
    const ObjectId involved[2] = {a, b};
    ObjectId h1 = kNoObject, h2 = kNoObject;
    for (const ObjectId id : involved)
        if (h1 == kNoObject && host.type_of(id) == t1) h1 = id;
    for (const ObjectId id : involved)
        if (h2 == kNoObject && host.type_of(id) == t2) h2 = id;
    if (h1 == kNoObject || h2 == kNoObject) return false;
    if (!host.hero_type(t1) || !host.hero_type(t2)) return false;
    const int32_t o1 = host.owner(h1), o2 = host.owner(h2);
    if (o1 == o2 || !host.enemies(o1, o2)) return false;
    // TARGETING suspended and +0x3b4 = 1 on the hero objects only (not their team containers); the host locks the
    // movement orders with it (Begin 0x6abde0's 0x3a8e50(h, 1): the port's coordinator creation never fails).
    host.set_in_clash(h1, true);
    host.set_in_clash(h2, true);
    Clash c;
    c.entry = entry;
    c.hero1 = h1;
    c.hero2 = h2;
    c.type1 = t1;
    c.type2 = t2;
    c.health1 = host.health(h1);
    c.health2 = host.health(h2);
    // Begin 0x6abde0: spots on the world X axis through the midpoint, Combat_Distance apart.
    c.state = kApproach;
    const Vec3 p1 = host.position(h1), p2 = host.position(h2);
    const Vec3 mid{(p1.x + p2.x) / 2.0f, (p1.y + p2.y) / 2.0f, (p1.z + p2.z) / 2.0f};
    const float half = data_->entries[entry].combat_distance / 2.0f;
    if (mid.x <= p1.x) {
        c.spot1 = {mid.x + half, mid.y, mid.z};
        c.spot2 = {mid.x - half, mid.y, mid.z};
    } else {
        c.spot1 = {mid.x - half, mid.y, mid.z};
        c.spot2 = {mid.x + half, mid.y, mid.z};
    }
    host.move_to(h1, c.spot1);
    host.move_to(h2, c.spot2);
    clashes_.push_back(c);
    ++started_;
    changed_ = true;
    return true;
}

bool HeroClashManager::update_clash(Clash& c, uint32_t frame, HeroClashHost& host, SimRng& rng) {
    // §2.3 / §3.8 object removed (0x6ac940). s78n t06 [port]: polled at the clash's update - a hero that is no longer alive
    // (dying, removed, retreated) counts as removed at once (retail: when it leaves the object manager).
    const bool gone1 = !host.alive(c.hero1), gone2 = !host.alive(c.hero2);
    if (gone1 || gone2) {
        end(c, gone1 ? c.hero2 : c.hero1, frame, host, true);
        return false;
    }
    const HeroClashEntry& e = data_->entries[c.entry];
    switch (c.state) {
        case kApproach: {  // 0x6acc30 state 1
            if (host.walking(c.hero1) || host.walking(c.hero2)) return true;
            const float d = e.combat_distance;
            if (std::fabs(dist2(host.position(c.hero1), host.position(c.hero2)) - d * d) > 0.5f) {
                begin_snap(c, frame, host);
            } else {
                fight(c, host);
            }
            return true;
        }
        case kSnap: {  // 0x6acfb0: a direct lerp of position and angles
            float s = 1.0f;
            if (frame < c.snap_end) {
                s = static_cast<float>(frame - c.snap_start) / static_cast<float>(c.snap_end - c.snap_start);
                s = std::clamp(s, 0.0f, 1.0f);
            }
            host.place(c.hero1, lerp(c.snap_pos1, c.spot1, s), norm360(c.snap_yaw1 + wrap180(c.want_yaw1 - c.snap_yaw1) * s));
            host.place(c.hero2, lerp(c.snap_pos2, c.spot2, s), norm360(c.snap_yaw2 + wrap180(c.want_yaw2 - c.snap_yaw2) * s));
            if (frame >= c.snap_end) fight(c, host);
            return true;
        }
        default: break;
    }
    // State 3, the exchanges (0x6ad230).
    if (c.outcome == kExchangeConversation) {
        if (host.speech_idle()) new_exchange(c, frame, host, rng);
        return true;
    }
    if (!c.playing) {  // hero 1 plays no clash animation (the first fight frame, or nothing was played)
        new_exchange(c, frame, host, rng);
        return true;
    }
    // Hero 1's clip position: clip fps x elapsed sim seconds (ANIM_PARTICLES A.5); a one-shot finishes at count - 2.
    const float pos = static_cast<float>(frame - c.ex_start) * c.ex_fps / 30.0f;
    const float last = c.ex_frames >= 2u ? static_cast<float>(c.ex_frames - 2u) : 0.0f;
    if (pos >= last) {  // both clips finished (equal clips: they finish together)
        if (!c.damage_applied && !apply_damage(c, frame, host)) return false;
        new_exchange(c, frame, host, rng);
        return true;
    }
    const float progress = c.ex_frames > 0u ? std::clamp(pos / static_cast<float>(c.ex_frames), 0.0f, 1.0f) : 1.0f;
    if (progress < 0.8f || c.damage_applied) return true;
    return apply_damage(c, frame, host);
}

void HeroClashManager::begin_snap(Clash& c, uint32_t frame, HeroClashHost& host) {
    // 0x6aba30.
    c.state = kSnap;
    c.snap_pos1 = host.position(c.hero1);
    c.snap_pos2 = host.position(c.hero2);
    c.snap_yaw1 = norm360(host.yaw_deg(c.hero1));  // [port] the stored yaw read in [0, 360) like 0x20acd0's
    c.snap_yaw2 = norm360(host.yaw_deg(c.hero2));
    const float err_pos = std::clamp(std::max(dmath::sqrt(dist2(c.snap_pos1, c.spot1)), dmath::sqrt(dist2(c.snap_pos2, c.spot2))), 0.0f, 3.0f);
    c.want_yaw1 = yaw360(c.spot2.x - c.spot1.x, c.spot2.y - c.spot1.y);
    c.want_yaw2 = yaw360(c.spot1.x - c.spot2.x, c.spot1.y - c.spot2.y);
    float err_ang = std::max(std::fabs(c.snap_yaw1 - c.want_yaw1), std::fabs(c.snap_yaw2 - c.want_yaw2));
    if (err_ang > 180.0f) err_ang -= 180.0f;  // retail subtracts 180 (not 360 - x) [H quirk, kept]
    err_ang = std::clamp(err_ang, 0.0f, 180.0f);
    const float t = std::max(0.1f + 0.4f * err_pos / 3.0f, 0.1f + 0.9f * err_ang / 180.0f);
    c.snap_start = frame;
    c.snap_end = frame + static_cast<uint32_t>(static_cast<int32_t>(30.0f * t));
}

void HeroClashManager::fight(Clash& c, HeroClashHost& host) {
    // 0x6abcd0: state 3; each hero snaps to face the other; IDLE and the locomotor suspended.
    c.state = kFight;
    const Vec3 p1 = host.position(c.hero1), p2 = host.position(c.hero2);
    host.face(c.hero1, yaw360(p2.x - p1.x, p2.y - p1.y));
    host.face(c.hero2, yaw360(p1.x - p2.x, p1.y - p2.y));
    host.hold(c.hero1, true);
    host.hold(c.hero2, true);
    c.playing = false;
}

void HeroClashManager::new_exchange(Clash& c, uint32_t frame, HeroClashHost& host, SimRng& rng) {
    // 0x6ac1b0.
    const HeroClashEntry& e = data_->entries[c.entry];
    c.damage_applied = false;
    c.ex_winner = kNoObject;
    c.ex_loser = kNoObject;
    uint8_t o = kExchangeDraw;
    // [port] "the previous outcome was not 4" reads the current outcome (+0xb8) as the one just finished: a conversation is
    // never followed by another; the weights below exclude the last fought outcome (+0xbc).
    if (c.outcome != kExchangeConversation && e.play_conversation && host.speech_idle()) {
        o = kExchangeConversation;  // no draw
    } else {
        o = clash_pick_outcome(c.previous, e.first_win, e.second_win, rng);
        c.previous = o;
    }
    size_t k1 = 2, k2 = 2;  // Draw
    if (o == kExchangeFirstWins) {
        k1 = 0;
        k2 = 1;
        c.ex_winner = c.hero1;
        c.ex_loser = c.hero2;
    } else if (o == kExchangeSecondWins) {
        k1 = 1;
        k2 = 0;
        c.ex_winner = c.hero2;
        c.ex_loser = c.hero1;
    } else if (o == kExchangeConversation) {
        k1 = k2 = 3;
    }
    const std::string& a1 = e.first_anims[k1];
    const std::string& a2 = e.second_anims[k2];
    uint32_t n1 = 0, n2 = 0, f1 = 0, f2 = 0;
    float fps1 = 0.0f, fps2 = 0.0f;
    const bool m1 = host.clip(c.hero1, a1, 0, n1, f1, fps1);
    const bool m2 = host.clip(c.hero2, a2, 0, n2, f2, fps2);
    const bool fallback = !m1 || !m2;  // [port] no model data: the shipped clips (one variant, 101 frames @ 30 fps)
    if (fallback) {
        n1 = n2 = 1;
        f1 = f2 = kClashFallbackClipFrames;
        fps1 = fps2 = kClashFallbackClipFps;
    }
    bool play = false;
    if (n1 == n2 && n1 > 0) {
        uint32_t v = 0;
        if (n1 >= 2) v = static_cast<uint32_t>(rng.range_int(0, static_cast<int32_t>(n1) - 1));  // 0x1ffb40
        if (v != 0 && !fallback) {
            uint32_t vn = 0;
            const bool ok1 = host.clip(c.hero1, a1, v, vn, f1, fps1);
            const bool ok2 = host.clip(c.hero2, a2, v, vn, f2, fps2);
            if (!ok1 || !ok2) f1 = f2 = 0;
        }
        // Both clips exist with equal frame counts and rates: both heroes play them (one-shot, speed 1).
        play = f1 > 0 && f1 == f2 && fps1 == fps2 && fps1 > 0.0f;
    }
    // Otherwise nothing is played: the next frame starts a new exchange (and draws again) [H; M in practice].
    c.playing = play;
    c.ex_start = frame;
    c.ex_frames = play ? f1 : 0u;
    c.ex_fps = play ? fps1 : 0.0f;
    c.outcome = o;
    ++c.exchanges;
}

bool HeroClashManager::apply_damage(Clash& c, uint32_t frame, HeroClashHost& host) {
    c.damage_applied = true;
    if (c.ex_loser == kNoObject || c.ex_winner == kNoObject) return true;
    const HeroClashEntry& e = data_->entries[c.entry];
    const float start_health = c.ex_loser == c.hero1 ? c.health1 : c.health2;  // health at the clash start, not max
    const int32_t wt = c.ex_winner == c.hero1 ? c.type1 : c.type2;
    const int32_t dmg = clash_exchange_damage(e, start_health, wt == c.type1, wt == c.type2);
    host.damage(c.ex_loser, static_cast<float>(dmg), c.ex_winner);
    if (!host.alive(c.ex_loser)) {
        end(c, c.ex_winner, frame, host, false);
        return false;
    }
    return true;
}

void HeroClashManager::release(const Clash& c, ObjectId hero, HeroClashHost& host) const {
    // 0x6acb80: state 3: IDLE and the locomotor resumed first; states 1 / 3: unlock, TARGETING resumed, +0x3b4 = 0.
    // State 2 (snap): nothing - retail quirk kept (s78n t06 [port: parity]): a hero whose opponent is removed during the
    // snap stays locked with TARGETING suspended and +0x3b4 = 1 for the rest of the battle.
    if (c.state == kSnap) return;
    if (c.state == kFight) host.hold(hero, false);
    host.set_in_clash(hero, false);
}

void HeroClashManager::end(Clash& c, ObjectId winner, uint32_t frame, HeroClashHost& host, bool by_removal) {
    // 0x6ac660: release both heroes (per state); the winner's speech is audio only.
    for (const ObjectId h : {c.hero1, c.hero2}) {
        if (by_removal && !host.alive(h)) {
            // [port] the removed hero is gone in retail (the handler clears its slot, no release); the port keeps the dead
            // unit's record, so only its +0x3b4 flag is cleared.
            host.set_in_clash(h, false);
            continue;
        }
        release(c, h, host);
    }
    EndEvent ev;
    ev.entry = c.entry;
    ev.winner = winner;
    ev.loser = winner == c.hero1 ? c.hero2 : c.hero1;
    ev.frame = frame;
    ev.by_removal = by_removal;
    ended_.push_back(ev);
}

// ---- state ------------------------------------------------------------------------------------------------------------

void HeroClashManager::hash(StateHasher& h) const {
    h.u32(static_cast<uint32_t>(clashes_.size()));
    for (const Clash& c : clashes_) {
        h.u32(c.entry);
        h.i32(c.hero1);
        h.i32(c.hero2);
        h.u8(c.state);
        h.f32(c.health1);
        h.f32(c.health2);
        h.u32(c.snap_start);
        h.u32(c.snap_end);
        h.u8(c.outcome);
        h.u8(c.previous);
        h.i32(c.ex_winner);
        h.i32(c.ex_loser);
        h.boolean(c.damage_applied);
        h.boolean(c.playing);
        h.u32(c.ex_start);
        h.u32(c.ex_frames);
        h.u32(c.exchanges);
    }
}

void HeroClashManager::write(ByteWriter& w) const {
    w.u32(kSaveVersion).u32(started_).u32(static_cast<uint32_t>(clashes_.size()));
    for (const Clash& c : clashes_) {
        w.u32(c.entry).i32(c.hero1).i32(c.hero2).i32(c.type1).i32(c.type2).f32(c.health1).f32(c.health2).u8(c.state);
        put_vec(w, c.spot1);
        put_vec(w, c.spot2);
        put_vec(w, c.snap_pos1);
        put_vec(w, c.snap_pos2);
        w.f32(c.snap_yaw1).f32(c.snap_yaw2).f32(c.want_yaw1).f32(c.want_yaw2).u32(c.snap_start).u32(c.snap_end);
        w.u8(c.outcome).u8(c.previous).i32(c.ex_winner).i32(c.ex_loser).boolean(c.damage_applied).boolean(c.playing);
        w.u32(c.ex_start).u32(c.ex_frames).f32(c.ex_fps).u32(c.exchanges);
    }
}

bool HeroClashManager::read(ByteReader& r) {
    if (r.u32() != kSaveVersion || !r.ok()) return false;
    const uint32_t started = r.u32();
    const uint32_t n = r.u32();
    if (!r.ok() || n > r.remaining()) return false;
    std::vector<Clash> list(n);
    for (Clash& c : list) {
        c.entry = r.u32();
        c.hero1 = r.i32();
        c.hero2 = r.i32();
        c.type1 = r.i32();
        c.type2 = r.i32();
        c.health1 = r.f32();
        c.health2 = r.f32();
        c.state = r.u8();
        c.spot1 = get_vec(r);
        c.spot2 = get_vec(r);
        c.snap_pos1 = get_vec(r);
        c.snap_pos2 = get_vec(r);
        c.snap_yaw1 = r.f32();
        c.snap_yaw2 = r.f32();
        c.want_yaw1 = r.f32();
        c.want_yaw2 = r.f32();
        c.snap_start = r.u32();
        c.snap_end = r.u32();
        c.outcome = r.u8();
        c.previous = r.u8();
        c.ex_winner = r.i32();
        c.ex_loser = r.i32();
        c.damage_applied = r.boolean();
        c.playing = r.boolean();
        c.ex_start = r.u32();
        c.ex_frames = r.u32();
        c.ex_fps = r.f32();
        c.exchanges = r.u32();
        if (!r.ok()) return false;
        if (c.state < kApproach || c.state > kFight || c.outcome > kExchangeConversation || c.previous > kExchangeConversation) return false;
        if (!data_ || c.entry >= data_->entries.size()) return false;  // set_data before read (the host's data)
    }
    clashes_ = std::move(list);
    started_ = started;
    ended_.clear();
    changed_ = true;
    return true;
}

}  // namespace alamo::tactical::combat
