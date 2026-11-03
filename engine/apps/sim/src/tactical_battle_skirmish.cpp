// --- skai (q2f t04): skirmish map battles. A skirmish started straight on its _MP_ map (CampaignBattles::start_map_battle with
// BattleConfig::skirmish, TacticalBattle::skirmish_map): the Income_Stream_Ability credit trickle (docs/re/SKIRMISH_SETUP.md).
// The skirmish victory types 0x13 / 0x16 are the victory monitor's (tactical_battle_victory.cpp, story t06). Serial code, no
// synced draws; credits go through the host's credit hooks.
#include <algorithm>
#include <cstdlib>
#include <string>
#include <string_view>

#include "alamo/galactic/campaign.hpp"
#include "alamo/objects/behaviors.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/sim/tactical_battle.hpp"

namespace alamo::sim {

namespace {

constexpr float kFrameSeconds = 1.0f / 30.0f;  // tactical frames are 1/30 s

bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.remove_suffix(1);
    return s;
}

float to_f(std::string_view v) { return static_cast<float>(std::atof(std::string(trim(v)).c_str())); }

}  // namespace

const TacticalBattle::SkIncome& TacticalBattle::sk_income_of(uint32_t type) {
    if (sk_income_.size() <= type) sk_income_.resize(static_cast<size_t>(type) + 1);
    SkIncome& in = sk_income_[type];
    if (in.parsed) return in;
    in.parsed = true;
    const objects::GameObjectTypes& types = catalog_->types();
    if (type >= types.size()) return in;
    const objects::ObjectTypeDatabase& raw = types.raw();
    const objects::FieldColumns& fc = raw.fields();
    const std::string_view self = raw.types().name[type];
    for (const objects::FieldIndex f : types.get_sub_objects(static_cast<objects::TypeIndex>(type), "Abilities")) {
        const std::string_view tag = raw.tag_name(f);
        const bool stream = ieq(tag, "Income_Stream_Ability"), mod = ieq(tag, "Income_Stream_Mod_Ability");
        if (!stream && !mod) continue;
        float value = 0.0f, interval = 0.0f, additive = 0.0f, multiplier = 1.0f;
        bool enabled = true, own = false;
        for (objects::FieldIndex c = f + 1; c < fc.size() && fc.depth[c] > fc.depth[f]; ++c) {
            if (fc.parent[c] != f) continue;
            const std::string_view k = raw.tag_name(c);
            if (ieq(k, "Base_Income_Value")) value = to_f(fc.text[c]);
            else if (ieq(k, "Base_Interval_In_Secs")) interval = to_f(fc.text[c]);
            else if (ieq(k, "Income_Additive_Value")) additive = to_f(fc.text[c]);
            else if (ieq(k, "Income_Multiplier")) multiplier = to_f(fc.text[c]);
            else if (ieq(k, "Initially_Enabled")) enabled = !ieq(trim(fc.text[c]), "No") && !ieq(trim(fc.text[c]), "False");
            else if (ieq(k, "Target_Stream_Source")) own = ieq(trim(fc.text[c]), self);
        }
        if (stream) {
            in.value = value;
            in.interval = interval;
        } else if (own) {
            // [M] a mod on the object's own stream (the skirmish stations' Supply Dock bonus, Initially_Enabled No, switched on
            // by the station's supply-dock hard point): counted while the object lives. The retail trace pays the Empire
            // station 30 + 20 per 10 s from frame 0 (q2f Geonosis trace), so the hard point is up at the start; its
            // destruction is not modelled. Mods on other sources (Mineral Processor x2 on the Mining Facility, the power
            // generators' reverse mods, upgrade objects) are [port] not applied.
            (void)enabled;
            in.multiplier *= multiplier;
            in.additive += additive;
        }
    }
    return in;
}

void TacticalBattle::sk_income(const TacticalMode& mode) {
    // Income_Stream_Ability [trace]: every frame the owner gets (Base_Income_Value x mods + additive) / Base_Interval_In_Secs
    // x 1/30 s, a float add into its credits. The retail Geonosis trace (q2f, 2026-10-30) shows the human's credits at
    // 6000 + 299.7 / 599.4 / 899.1 after 1800 / 3600 / 5400 frames: 50 per 10 s paid per frame (a per-interval payout would
    // give exactly 300 / 600 / 900; the 0.3 shortfall per 1800 frames is the float rounding of a 1/6 credit add near 6000).
    if (!skirmish_map() || !credits_add_) return;
    const objects::ObjectStore& st = mode.primary();
    const int32_t slots[2] = {skirmish_slot_player(0), skirmish_slot_player(1)};
    for (const Unit& u : units_) {
        if (!u.alive || u.retreated || !st.alive(u.h)) continue;
        const int32_t owner = st.owner(u.h);
        if (owner < 0 || (owner != slots[0] && owner != slots[1])) continue;
        const SkIncome& in = sk_income_of(u.type);
        if (in.value <= 0.0f || in.interval <= 0.0f) continue;
        const float per_interval = in.value * in.multiplier + in.additive;
        credits_add_(owner, per_interval / in.interval * kFrameSeconds);
    }
}

}  // namespace alamo::sim
