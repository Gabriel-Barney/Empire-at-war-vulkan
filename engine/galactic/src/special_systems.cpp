// P4 sim (owner: galactic agent): FoC galactic special systems (special_systems.hpp; docs/re/P4_SIM_SYSTEMS.md = P4).
// Confidence markers follow the spec: [H] read, [M] inferred from strong evidence, [L] guess; [inferred] / [port] mark
// rules the port chose where the spec is [M] / [L] or the port's object model differs (galactic teams are single
// objects, no orbital special-structure list, no local-player notion: a human is treated as the local player, SP).
//
// Determinism: serial code only; every list in stored order (item table, ability list, planet rows, structure /
// surface / orbit lists, player index order); every random choice on the synced SimRng in the spec's draw order
// (P4 6.3); float32 arithmetic in the documented order, no transcendental CRT calls (powf replaced by repeated
// multiplication for an integer exponent).
#include <algorithm>
#include <climits>

#include "alamo/commands/event_types.hpp"
#include "alamo/core/hash.hpp"
#include "alamo/galactic/campaign.hpp"

namespace alamo::galactic {

namespace {

void put32(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
uint32_t get32(std::span<const uint8_t> b, size_t at) {
    uint32_t v = 0;
    for (size_t i = 0; i < 4; ++i) v |= static_cast<uint32_t>(b[at + i]) << (8 * i);
    return v;
}
commands::EventData special_event(uint32_t kind, const std::vector<uint8_t>& bytes) {
    commands::EventData e(commands::EventType::SpecialAbility);
    e.set("kind", kind).set_bytes("data", std::span<const uint8_t>(bytes.data(), bytes.size()));
    return e;
}

// round half away from zero of a float to int (cost > 0 here), saturating.
int32_t round_cost(float v) {
    if (v >= 2147483520.0f) return INT32_MAX;
    if (v <= -2147483520.0f) return INT32_MIN;
    return v >= 0.0f ? static_cast<int32_t>(v + 0.5f) : -static_cast<int32_t>(-v + 0.5f);
}

constexpr int8_t kCorruptionBlackMarket = 6;  // Corruption_Black_Market (P4 1.2 accept)

}  // namespace

// ---- payloads ---------------------------------------------------------------------------------------------------------

uint32_t economy_mining_tag() { return special_name_crc("TEXT_ECONOMY_MINING_FACILITIES"); }

commands::EventData special_activate_event(uint32_t planet_id, uint32_t unit_id) {
    std::vector<uint8_t> b;
    put32(b, planet_id);
    put32(b, unit_id);
    return special_event(kSpecialActivate, b);
}

commands::EventData black_market_purchase_event(uint32_t planet_id, uint32_t hero_id, uint32_t item_crc, float price,
                                                float respawn_seconds, bool only_one) {
    std::vector<uint8_t> b;
    put32(b, planet_id);
    put32(b, hero_id);
    put32(b, item_crc);
    put32(b, commands::quantize(kQuantAmount, price));
    put32(b, commands::quantize(kQuantDuration, respawn_seconds));
    b.push_back(static_cast<uint8_t>(only_one ? 1 : 0));
    return special_event(kSpecialBlackMarket, b);
}

commands::EventData sabotage_structure_event(uint32_t planet_id, uint32_t saboteur_id, uint32_t structure_id, float cost,
                                             float respawn_seconds) {
    std::vector<uint8_t> b;
    put32(b, planet_id);
    put32(b, saboteur_id);
    put32(b, structure_id);
    put32(b, commands::quantize(kQuantAmount, cost));
    put32(b, commands::quantize(kQuantDuration, respawn_seconds));
    return special_event(kSpecialSabotage, b);
}

commands::EventData slice_event(uint32_t planet_id, uint32_t slicer_id, uint32_t type, float price, float chance,
                                float respawn_seconds, bool only_one) {
    std::vector<uint8_t> b;
    put32(b, planet_id);
    put32(b, slicer_id);
    put32(b, type);
    put32(b, commands::quantize(kQuantAmount, price));
    put32(b, commands::quantize(kQuantDuration, chance * 1000.0f));
    put32(b, commands::quantize(kQuantDuration, respawn_seconds));
    b.push_back(static_cast<uint8_t>(only_one ? 1 : 0));
    return special_event(kSpecialSlice, b);
}

commands::EventData hack_super_weapon_event(uint32_t planet_id, uint32_t hacker_id, float cost) {
    std::vector<uint8_t> b;
    put32(b, planet_id);
    put32(b, hacker_id);
    put32(b, commands::quantize(kQuantAmount, cost));
    return special_event(kSpecialHack, b);
}

bool read_special_payload(const commands::EventData& ev, SpecialPayload& out) {
    if (ev.type() != commands::EventType::SpecialAbility) return false;
    out = SpecialPayload{};
    out.kind = ev.u("kind");
    const std::span<const uint8_t> b = ev.bytes_view("data");
    switch (out.kind) {
    case kSpecialActivate:
        if (b.size() != 8) return false;
        break;
    case kSpecialSlice:
        if (b.size() != 25) return false;
        out.type = get32(b, 8);
        out.amount = commands::dequantize(kQuantAmount, get32(b, 12));
        out.chance = commands::dequantize(kQuantDuration, get32(b, 16)) / 1000.0f;  // unpack 0x4b4470 divides by 1000
        out.seconds = commands::dequantize(kQuantDuration, get32(b, 20));
        out.only_one = b[24] != 0;
        break;
    case kSpecialBlackMarket:
        if (b.size() != 21) return false;
        out.item_crc = get32(b, 8);
        out.amount = commands::dequantize(kQuantAmount, get32(b, 12));
        out.seconds = commands::dequantize(kQuantDuration, get32(b, 16));
        out.only_one = b[20] != 0;
        break;
    case kSpecialSabotage:
        if (b.size() != 20) return false;
        out.structure = get32(b, 8);
        out.amount = commands::dequantize(kQuantAmount, get32(b, 12));
        out.seconds = commands::dequantize(kQuantDuration, get32(b, 16));
        break;
    case kSpecialHack:
        if (b.size() != 12) return false;
        out.amount = commands::dequantize(kQuantAmount, get32(b, 8));
        break;
    default: return false;
    }
    out.planet = get32(b, 0);
    out.unit = get32(b, 4);
    return true;
}

// ---- helpers ----------------------------------------------------------------------------------------------------------

const TypeSpecial& GalacticSim::special_type(ObjectHandle h) const {
    return special().type(store_->exists(h) ? static_cast<uint32_t>(store_->type(h)) : kNoIndex);
}

bool GalacticSim::is_hero_type(uint32_t type) const {
    const TypeSpecial& t = special().type(type);
    return t.named_hero || t.generic_hero;
}

// 0x48d900 [H]: by faction name; P Rebel -> owner Empire / Underworld, Empire -> Rebel / Underworld, Underworld ->
// Rebel / Empire; and the owner is another player.
bool GalacticSim::faction_pairing(int32_t player, int32_t owner) const {
    if (!valid_player(player) || !valid_player(owner) || player == owner) return false;
    const std::string& a = faction_of(player).name;
    const std::string& b = faction_of(owner).name;
    auto is = [](const std::string& n, const char* f) { return iequals(n, f); };
    // [name-ok-begin: retail by-name pairing in 0x48d900, docs/re/P4_SIM_SYSTEMS.md §1.2 step 1]
    if (is(a, "Rebel")) return is(b, "Empire") || is(b, "Underworld");
    if (is(a, "Empire")) return is(b, "Rebel") || is(b, "Underworld");
    if (is(a, "Underworld")) return is(b, "Rebel") || is(b, "Empire");
    // [name-ok-end]
    return false;
}

void GalacticSim::emit_special(GalacticEventKind kind, int32_t player, PlanetIndex planet, ObjectHandle object, uint32_t type,
                               int32_t value, int32_t other) {
    if (!emit_events_) return;
    GalacticEvent ev;
    ev.kind = kind;
    ev.player = player;
    ev.planet = planet;
    ev.object = store_->exists(object) ? static_cast<int32_t>(store_->id(object)) : 0;
    ev.type = type;
    ev.value = value;
    ev.other_player = other;
    emit(ev);
}

void GalacticSim::despawn(ObjectHandle h) {
    // 0x48cf20: the hero and its team leave the map; not a death (no ObjectDestroyed / story DESTROY) [inferred].
    const bool e = emit_events_;
    emit_events_ = false;
    destroy_object(h);
    emit_events_ = e;
}

int32_t GalacticSim::player_tech(int32_t player) const {
    // 0x2813c0 [H]: P +0x84 plus the faction's Displayed_Tech_Level_Adjustment (not in skirmish: the port's galaxy is
    // always a campaign).
    if (!valid_player(player)) return 0;
    return players_[static_cast<size_t>(player)].tech_level + faction_of(player).displayed_tech_level_adjustment;
}

// ---- hero trigger and respawns (P4 6) ---------------------------------------------------------------------------------

bool GalacticSim::special_accepts(const SpecialAbilityDef& a, ObjectHandle unit, int32_t owner, PlanetIndex planet) const {
    if (!valid_planet(planet)) return false;
    const PlanetState& p = planets_[planet];
    switch (a.cls) {
    case SpecialAbilityClass::BlackMarket:
        // vf5 0x61aad0 [H]: a planet, the hero not allied with the owner [port: another player], a playable owner
        // faction, Corruption_Black_Market at the planet.
        return valid_player(p.owner) && p.owner != owner && faction_of(p.owner).is_playable &&
               p.corruption == kCorruptionBlackMarket;
    case SpecialAbilityClass::Sabotage:  // vf5 0x5b20c0 [H]
        return p.owner != owner && !sabotage_options_of(a, planet, owner).empty();
    case SpecialAbilityClass::Hack:  // vf5 0x61b9b0 [H]
        return store_->alive(hack_find_super_weapon(planet));
    case SpecialAbilityClass::Corruption:
        return corruption_accepts(planet, unit);
    case SpecialAbilityClass::Slicer:
        return slice_accepts(owner, planet);
    case SpecialAbilityClass::SystemSpy:  // GRC 2.3: no accept rule recovered [L]: any planet, a non-empty mask
        return a.spy_mask != 0;
    default: return false;  // income abilities are Galactic_Automatic (service_income_abilities)
    }
}

bool GalacticSim::special_apply(uint32_t ability, ObjectHandle unit, int32_t owner, PlanetIndex planet) {
    const SpecialAbilityDef& a = special().abilities[ability];
    bool applied = false;
    switch (a.cls) {
    case SpecialAbilityClass::BlackMarket: applied = apply_black_market(a, unit, owner, planet); break;
    case SpecialAbilityClass::Sabotage: applied = apply_sabotage(a, unit, owner, planet); break;
    case SpecialAbilityClass::Hack: applied = apply_hack(unit, owner, planet); break;
    case SpecialAbilityClass::Corruption: applied = apply_corruption(unit, owner, planet); break;
    case SpecialAbilityClass::Slicer: applied = apply_slicer(a, unit, owner, planet); break;
    case SpecialAbilityClass::SystemSpy: applied = apply_system_spy(a, unit, owner, planet); break;  // GRC 2.3
    default: break;
    }
    // Story LAND_ON (2) + DEPLOY (0xC) for every successful apply (0x3284c0) [H]; type = the unit's type.
    if (applied)
        emit_special(GalacticEventKind::SpecialApplied, owner, planet, unit, static_cast<uint32_t>(store_->exists(unit) ? store_->type(unit) : kNoIndex),
                     static_cast<int32_t>(ability), -1);
    return applied;
}

bool GalacticSim::ground_activated_trigger(ObjectHandle unit, PlanetIndex planet, bool* accepted) {
    if (accepted) *accepted = false;
    if (!store_->alive(unit) || !valid_planet(planet)) return false;
    const int32_t owner = store_->owner(unit);
    if (!valid_player(owner)) return false;
    const uint32_t type = static_cast<uint32_t>(store_->type(unit));
    const std::vector<uint32_t> list = special().type(type).abilities;  // copy: the unit may leave during the walk
    bool any = false;
    for (const uint32_t ai : list) {
        if (!store_->alive(unit)) break;  // [inferred] a departed hero tries nothing more
        const SpecialAbilityDef& a = special().abilities[ai];
        if (a.style != ActivationStyle::GroundActivated || !a.initially_enabled) continue;  // style + "ready" (0x37be10)
        if (!special_accepts(a, unit, owner, planet)) continue;
        if (!bm_allows_special_ability(owner, a.name_crc, a.must_be_bought)) continue;  // 0x39b0b0
        if (accepted) *accepted = true;
        if (!special_apply(ai, unit, owner, planet)) continue;
        any = true;
        if (a.causes_despawn) {
            // vf12 = Get_Float(Min_Respawn_Time, Max_Respawn_Time): one synced draw [H]. [L] P4 6.1 double departure: when
            // the apply already departed the hero (black-market AI, corruption, vigilance), the draw is still made and
            // nothing is scheduled again (in-game probe item 2 of P4 8).
            const float r = rng_->inline_range_float(a.min_respawn, a.max_respawn);
            if (store_->alive(unit)) hero_depart(unit, planet, r);
        }
    }
    return any;
}

bool GalacticSim::schedule_hero_respawn(ObjectHandle hero, PlanetIndex planet, float seconds) {
    if (!store_->exists(hero)) return false;
    if (!valid_planet(planet)) planet = location_of(hero);
    if (!valid_planet(planet)) return false;
    const uint32_t type = static_cast<uint32_t>(store_->type(hero));
    const int32_t owner = store_->owner(hero);
    if (type >= rules_->types.size() || !valid_player(owner)) return false;  // 0x396380: the type must resolve
    float t = seconds;
    if (t <= 0.0f) t = special().default_hero_respawn_time;
    // [H quirk, kept on purpose] 0x3fe0a0 tests 0 <= time: a negative time queues nothing, the caller still reports
    // success. Thrawn's Revenge sets Default_Hero_Respawn_Time = -360, so every default-time respawn is lost there.
    if (t < 0.0f) return true;
    if (special().type(type).named_hero)  // 0x3fa300: a named hero is not queued twice [inferred: same type]
        for (const PlanetState& p : planets_)
            for (const RespawnEntry& e : p.respawns)
                if (e.type == type) return true;
    RespawnEntry e;
    e.due = frame_ + static_cast<int32_t>(30.0f * t + 0.5f);
    e.type = type;
    e.owner = owner;
    planets_[planet].respawns.push_back(e);
    emit_special(GalacticEventKind::HeroRespawnScheduled, owner, planet, hero, type, e.due, -1);
    return true;
}

bool GalacticSim::hero_depart(ObjectHandle hero, PlanetIndex planet, float seconds) {
    if (!store_->exists(hero)) return false;
    const bool ok = schedule_hero_respawn(hero, planet, seconds);
    if (store_->alive(hero)) despawn(hero);
    return ok;
}

std::span<const RespawnEntry> GalacticSim::respawns_pending(PlanetIndex planet) const {
    if (!valid_planet(planet)) return {};
    return planets_[planet].respawns;
}

void GalacticSim::service_respawns() {
    for (PlanetIndex i = 0; i < planets_.size(); ++i) {
        if (planets_[i].respawns.empty()) continue;
        std::vector<RespawnEntry> due;
        auto& list = planets_[i].respawns;
        for (size_t k = 0; k < list.size();) {
            if (list[k].due <= frame_) {
                due.push_back(list[k]);
                list.erase(list.begin() + static_cast<std::ptrdiff_t>(k));
            } else {
                ++k;
            }
        }
        const Vec3 at = planets_[i].position;
        for (const RespawnEntry& e : due) {
            // The owner's planets ordered by 0x3fefc0 relative to the scheduling planet [M: nearest first]; the port
            // orders by squared distance, ties by row [inferred]. First that accepts (0x3f6650 [L]: any owned planet
            // where the type has a galactic role). No owned planet: dropped.
            std::vector<std::pair<float, PlanetIndex>> order;
            for (PlanetIndex r = 0; r < planets_.size(); ++r) {
                if (!planets_[r].is_planet || planets_[r].owner != e.owner) continue;
                const float dx = planets_[r].position.x - at.x, dy = planets_[r].position.y - at.y, dz = planets_[r].position.z - at.z;
                const float d2 = dx * dx + dy * dy + dz * dz;
                order.push_back({d2, r});
            }
            std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            for (const auto& [d2, r] : order) {
                (void)d2;
                last_spawn_ = {};
                if (!create_unit(e.owner, r, e.type)) continue;
                emit_special(GalacticEventKind::HeroRespawned, e.owner, r, last_spawn_, e.type, 0, -1);
                break;
            }
        }
    }
}

// ---- black market (P4 1) ----------------------------------------------------------------------------------------------

std::vector<BlackMarketOffer> GalacticSim::offers_with(const SpecialAbilityDef* a, int32_t player, PlanetIndex planet) const {
    std::vector<BlackMarketOffer> out;
    if (!valid_player(player) || !valid_planet(planet)) return out;
    const PlayerState& ps = players_[static_cast<size_t>(player)];
    const PlanetState& p = planets_[planet];
    const uint32_t xf = valid_player(p.owner) ? players_[static_cast<size_t>(p.owner)].faction : kNoIndex;
    const bool tut = ps.bm_tutorial;
    // m(P) = 0x281960: 1.0 + the fold of the timed-modifier list, floored at 0.0001 [M] (the S3 m_techupg value).
    const float m = std::max(0.0001f, ps.tech_cost_mult);
    const float pm = a ? a->price_modifier : 1.0f;
    const float planet_mod = special().type(p.type).black_market_modifier;
    for (uint32_t i = 0; i < special().items.size(); ++i) {
        const BlackMarketItemDef& it = special().items[i];
        if (it.faction != kNoIndex && xf != kNoIndex && xf != it.faction) continue;
        const int32_t base = tut ? it.tutorial_base_cost : it.base_cost;
        const int32_t tech_i = tut ? it.tutorial_tech_level : it.tech_level;
        float price = tut ? static_cast<float>(base) : (static_cast<float>(base) + planet_mod) * pm * m;
        float u = 1.0f;
        if (it.faction != kNoIndex) {
            // Single player [H] (the multiplayer human branch, max tech over the other players, is not modelled): the
            // first player of the item's faction with tech < the item's tech sets the uncommon factor.
            for (size_t q = 0; q < players_.size(); ++q)
                if (players_[q].faction == it.faction && player_tech(static_cast<int32_t>(q)) < tech_i) {
                    u = it.uncommon_price_multiplier;
                    break;
                }
        }
        price = std::max(0.0f, price * u);
        BlackMarketOffer o;
        o.item = i;
        o.price = price;
        o.purchased = bm_has_item(player, i);
        out.push_back(o);
    }
    return out;
}

std::vector<BlackMarketOffer> GalacticSim::bm_offers(int32_t player, PlanetIndex planet, ObjectHandle hero) const {
    const SpecialAbilityDef* a = nullptr;
    if (store_->exists(hero))
        for (uint32_t ai : special_type(hero).abilities)
            if (special().abilities[ai].cls == SpecialAbilityClass::BlackMarket) {
                a = &special().abilities[ai];
                break;
            }
    return offers_with(a, player, planet);
}

std::span<const uint32_t> GalacticSim::bm_purchased(int32_t player) const {
    if (!valid_player(player)) return {};
    return players_[static_cast<size_t>(player)].bm_purchased;
}

bool GalacticSim::bm_has_item(int32_t player, uint32_t item) const {
    if (!valid_player(player)) return false;
    const std::vector<uint32_t>& v = players_[static_cast<size_t>(player)].bm_purchased;
    return std::find(v.begin(), v.end(), item) != v.end();
}

bool GalacticSim::bm_allows_unit_ability(int32_t player, uint32_t ability_name_crc, bool must_be_bought) const {
    if (!must_be_bought) return true;
    bool listed = false;
    for (uint32_t i = 0; i < special().items.size(); ++i) {
        const std::vector<uint32_t>& c = special().items[i].ability_crcs;
        if (std::find(c.begin(), c.end(), ability_name_crc) == c.end()) continue;
        listed = true;
        if (bm_has_item(player, i)) return true;
    }
    return !listed;
}

bool GalacticSim::bm_allows_special_ability(int32_t player, uint32_t ability_name_crc, bool must_be_bought) const {
    return bm_allows_unit_ability(player, ability_name_crc, must_be_bought);  // same rule (0x39b0b0) [H]
}

bool GalacticSim::bm_purchase(int32_t player, PlanetIndex planet, ObjectHandle hero, uint32_t item, float price, float seconds) {
    // 0x61ab60 [H]: price > credits -> nothing; else charge, add (append if absent), SFX (UI), hero departure.
    if (price > players_[static_cast<size_t>(player)].account.credits) return false;
    add_credits(player, -price, true);
    std::vector<uint32_t>& v = players_[static_cast<size_t>(player)].bm_purchased;
    if (std::find(v.begin(), v.end(), item) == v.end()) v.push_back(item);
    emit_special(GalacticEventKind::BlackMarketPurchase, player, planet, hero, kNoIndex, static_cast<int32_t>(item),
                 players_[static_cast<size_t>(player)].human ? 1 : 0);
    hero_depart(hero, planet, seconds);
    return true;
}

bool GalacticSim::apply_black_market(const SpecialAbilityDef& a, ObjectHandle hero, int32_t player, PlanetIndex planet) {
    // vf30 0x61a4a0 [H].
    const PlanetState& p = planets_[planet];
    if (!faction_pairing(player, p.owner)) {  // 1.
        emit_special(GalacticEventKind::SpecialError, player, planet, hero, kNoIndex, 0, -1);
        return false;
    }
    const std::vector<BlackMarketOffer> offers = offers_with(&a, player, planet);  // 2.
    float r = 0.0f;  // 3. respawn time (0 -> default respawn)
    const int32_t t = player_tech(player);
    if (t >= 1 && t <= 5 && !a.bm_min_respawn.empty() && static_cast<size_t>(t) <= a.bm_min_respawn.size())
        r = rng_->inline_range_float(a.bm_min_respawn[static_cast<size_t>(t - 1)], a.bm_max_respawn[static_cast<size_t>(t - 1)]);
    const PlayerState& ps = players_[static_cast<size_t>(player)];
    if (ps.human) {  // 4. the dialog (offers, R, buyer, planet); the purchase is command kind 4
        emit_special(GalacticEventKind::SpecialDialog, player, planet, hero, kNoIndex,
                     static_cast<int32_t>(commands::quantize(kQuantDuration, r)), static_cast<int32_t>(kSpecialBlackMarket));
        return false;
    }
    if (!ps.ai) return false;  // 6. remote human
    // 5. AI: offers with 0 < price <= credits, item set, offer order (purchased items NOT excluded [H]); weight =
    // price; one Get_Float(0, total) [M args]; first cumulative >= r.
    std::vector<size_t> cand;
    std::vector<float> cum;
    float total = 0.0f;
    for (size_t i = 0; i < offers.size(); ++i) {
        if (offers[i].item == kNoIndex || offers[i].price <= 0.0f || offers[i].price > ps.account.credits) continue;
        total += offers[i].price;
        cand.push_back(i);
        cum.push_back(total);
    }
    if (cand.empty()) return false;  // [inferred]: nothing affordable, nothing applied (no draw)
    const float x = rng_->inline_range_float(0.0f, total);
    size_t pick = cand.size() - 1;
    for (size_t k = 0; k < cum.size(); ++k)
        if (cum[k] >= x) {
            pick = k;
            break;
        }
    const BlackMarketOffer& o = offers[cand[pick]];
    bm_purchase(player, planet, hero, o.item, o.price, r);
    return true;
}

RejectReason GalacticSim::bm_execute_purchase(int32_t player, PlanetIndex planet, ObjectHandle hero, uint32_t item, float price,
                                             float respawn_seconds) {
    if (!valid_player(player)) return RejectReason::InvalidPlayer;
    if (!valid_planet(planet)) return RejectReason::InvalidTarget;
    // "Cannot complete black market action because the buyer no longer exists!" [H]; owner test [port].
    if (!store_->alive(hero) || store_->owner(hero) != player) return RejectReason::InvalidUnit;
    if (item >= special().items.size()) return RejectReason::Malformed;
    const float q_price = commands::requantize(kQuantAmount, price);  // COMMANDS 3.2 local round-trip [H]
    const float q_seconds = commands::requantize(kQuantDuration, respawn_seconds);
    return bm_purchase(player, planet, hero, item, q_price, q_seconds) ? RejectReason::None : RejectReason::CannotAfford;
}

bool GalacticSim::bm_give_item(int32_t player, std::string_view item_name) {
    const uint32_t i = special().find_item_by_name(item_name);
    if (!valid_player(player) || i == kNoIndex) return false;
    std::vector<uint32_t>& v = players_[static_cast<size_t>(player)].bm_purchased;
    if (std::find(v.begin(), v.end(), i) == v.end()) v.push_back(i);
    return true;
}

void GalacticSim::set_bm_tutorial(int32_t player, bool on) {
    if (valid_player(player)) players_[static_cast<size_t>(player)].bm_tutorial = on;
}
void GalacticSim::set_sabotage_tutorial(int32_t player, bool on) {
    if (valid_player(player)) players_[static_cast<size_t>(player)].sabotage_tutorial = on;
}

// ---- sabotage (P4 2) --------------------------------------------------------------------------------------------------

std::vector<uint8_t> GalacticSim::sabotage_options_of(const SpecialAbilityDef& a, PlanetIndex planet, int32_t player) const {
    std::vector<uint8_t> out;
    if (!valid_planet(planet)) return out;
    const PlanetState& p = planets_[planet];
    bool has_structure = false;
    for (ObjectHandle s : p.structures) has_structure = has_structure || store_->alive(s);
    if (a.sabotage_can[0] && ground_base_level(planet) > 0) out.push_back(kSabotageGroundBase);
    if (a.sabotage_can[1] && p.starbase_level > 0) out.push_back(kSabotageStarbase);
    if (a.sabotage_can[2] && (!p.queue[0].empty() || !p.queue[1].empty())) out.push_back(kSabotageProduction);
    if (a.sabotage_can[3] && p.corruption != kCorruptionNone && has_structure) out.push_back(kSabotageSpecialStructure);
    if (a.sabotage_can[4] && faction_pairing(player, p.owner)) out.push_back(kSabotageHalt);
    return out;
}

std::vector<uint8_t> GalacticSim::sabotage_options(PlanetIndex planet, ObjectHandle saboteur) const {
    if (!store_->exists(saboteur)) return {};
    for (uint32_t ai : special_type(saboteur).abilities)
        if (special().abilities[ai].cls == SpecialAbilityClass::Sabotage)
            return sabotage_options_of(special().abilities[ai], planet, store_->owner(saboteur));
    return {};
}

int32_t GalacticSim::sabotage_cost_with(const SpecialAbilityDef* a, int32_t player, PlanetIndex planet, ObjectHandle structure) const {
    if (!valid_planet(planet) || !store_->exists(structure)) return -1;
    const TypeSpecial& ts = special_type(structure);
    if (valid_player(player) && players_[static_cast<size_t>(player)].sabotage_tutorial)
        return ts.tutorial_sabotage_cost;  // tutorial GUI cost, unscaled [H]
    if (ts.sabotage_cost <= 0) return -1;
    const float factor = a ? a->cost_increase_factor : 2.0f;
    float f = 1.0f;
    for (int32_t i = 0; i < planets_[planet].sabotage_count; ++i) f *= factor;  // powf(factor, n) [port]
    const int32_t cost = round_cost(static_cast<float>(ts.sabotage_cost) * f);
    return cost <= 0 ? -1 : cost;
}

int32_t GalacticSim::sabotage_cost(int32_t player, PlanetIndex planet, ObjectHandle structure, ObjectHandle saboteur) const {
    // The dialog uses the saboteur's first ability named in Activated_Sabotage_Ability_Names [H]; else its first sabotage
    // ability; else the default factor 2.0 [inferred].
    const SpecialAbilityDef* a = nullptr;
    if (store_->exists(saboteur)) {
        for (uint32_t ai : special_type(saboteur).abilities) {
            const SpecialAbilityDef& d = special().abilities[ai];
            if (d.cls != SpecialAbilityClass::Sabotage) continue;
            const auto& names = special().activated_sabotage;
            if (std::find(names.begin(), names.end(), d.name) != names.end()) {
                a = &d;
                break;
            }
            if (!a) a = &d;
        }
    }
    return sabotage_cost_with(a, player, planet, structure);
}

std::vector<SabotageTarget> GalacticSim::sabotage_targets(int32_t player, PlanetIndex planet, ObjectHandle saboteur) const {
    std::vector<SabotageTarget> out;
    if (!valid_planet(planet)) return out;
    // Rows (0x3e6ff0): ground special structures then orbital ones with cost >= 1 [H]; the port has one structure list
    // (no orbital special-structure list) [port].
    for (ObjectHandle s : planets_[planet].structures) {
        if (!store_->alive(s)) continue;
        const int32_t c = sabotage_cost(player, planet, s, saboteur);
        if (c < 1) continue;
        SabotageTarget t;
        t.structure = s;
        t.type = static_cast<uint32_t>(store_->type(s));
        t.cost = c;
        out.push_back(t);
    }
    return out;
}

void GalacticSim::destroy_structure_sabotage(PlanetIndex planet, ObjectHandle structure, int32_t player, bool story) {
    const uint32_t type = static_cast<uint32_t>(store_->type(structure));
    destroy_object(structure);  // 0x4bb420, then ground production re-checked against the new level
    reevaluate_queue(planet, kGroundQueue, ground_base_level(planet));
    if (story) emit_special(GalacticEventKind::GalacticSabotage, player, planet, {}, type, kSabotageSpecialStructure, -1);
}

bool GalacticSim::apply_sabotage(const SpecialAbilityDef& a, ObjectHandle saboteur, int32_t player, PlanetIndex planet) {
    // vf30 0x5b1920 [H].
    const PlayerState& ps = players_[static_cast<size_t>(player)];
    if (!ps.human && !ps.ai) return false;  // 1. (a human is the local player in the port's SP model)
    const std::vector<uint8_t> opts = sabotage_options_of(a, planet, player);
    if (opts.empty()) {  // 2.
        emit_special(GalacticEventKind::SpecialError, player, planet, saboteur, kNoIndex, 0, -1);
        return false;
    }
    const uint8_t k = opts[static_cast<size_t>(rng_->range_int(0, static_cast<int32_t>(opts.size()) - 1))];  // 3. no draw when n = 1
    PlanetState& p = planets_[planet];
    if (special().type(p.type).planet_vigilance && special_type(saboteur).generic_hero) {  // 4. vigilance
        emit_special(GalacticEventKind::SabotageVigilance, player, planet, saboteur, kNoIndex, 0, p.owner);
        const float r = rng_->inline_range_float(a.min_respawn, a.max_respawn);  // vf12: one synced draw
        if (r > 0.0f) schedule_hero_respawn(saboteur, planet, r);
        destroy_object(saboteur);  // killed (0x391310)
        return true;
    }
    bool ok = false;
    switch (k) {
    case kSabotageGroundBase: {
        // 0x5b2910: queued base upgrades cancelled (0x5b1d40), the base replaced by its Prev_Level_Base or destroyed.
        // [port] the ground base level is the structure count: the queued ground structures are dropped (no refund
        // [inferred]) and the newest structure goes.
        auto& g = p.queue[kGroundQueue];
        for (size_t i = g.size(); i-- > 0;)
            if (rules_->types[g[i].type].kind == UnitKind::GroundStructure) {
                emit_production(GalacticEventKind::ProductionCanceled, planet, kGroundQueue, g[i], 0);
                g.erase(g.begin() + static_cast<std::ptrdiff_t>(i));
                if (i == 0 && !g.empty()) restart_head(g[0]);
            }
        for (size_t i = p.structures.size(); i-- > 0;)
            if (store_->alive(p.structures[i])) {
                destroy_object(p.structures[i]);
                ok = true;
                break;
            }
        reevaluate_queue(planet, kGroundQueue, ground_base_level(planet));
        break;
    }
    case kSabotageStarbase: {  // 0x5b3b60: the starbase becomes its Prev_Level_Base, or is destroyed (level 0)
        if (!store_->alive(p.starbase)) break;
        const uint32_t prev = special_type(p.starbase).prev_level_base;
        ok = replace_starbase(planet, prev < rules_->types.size() ? prev : kNoIndex);
        break;
    }
    case kSabotageProduction: {  // 0x5b2f50: head of queue 1, else queue 0 (0x403900; no refund [inferred])
        for (uint32_t q : {1u, 0u})
            if (!p.queue[q].empty()) {
                ok = cancel(p.queue[q][0].player, planet, q, 0, false) == RejectReason::None;
                break;
            }
        break;
    }
    case kSabotageSpecialStructure: {  // 0x5b32c0
        if (ps.human) {  // the sabotage dialog; the choice is command kind 5
            emit_special(GalacticEventKind::SpecialDialog, player, planet, saboteur, kNoIndex, 0, static_cast<int32_t>(kSpecialSabotage));
            return false;
        }
        // AI: live structures in list order with cost <= credits (cost -1 counts as affordable) [H], one Get_Int(0, n-1).
        std::vector<ObjectHandle> cand;
        for (ObjectHandle s : p.structures) {
            if (!store_->alive(s)) continue;
            const int32_t c = sabotage_cost_with(&a, player, planet, s);
            if (static_cast<float>(c) <= ps.account.credits) cand.push_back(s);
        }
        if (cand.empty()) break;  // [inferred]
        const ObjectHandle s = cand[static_cast<size_t>(rng_->range_int(0, static_cast<int32_t>(cand.size()) - 1))];
        destroy_structure_sabotage(planet, s, player, false);  // no story event, no departure here (P4 6.1 does it)
        ++p.sabotage_count;
        // [H quirk kept] the cost is subtracted from P +0x7C, the per-tick stat accumulator, not from the credits: the AI is
        // not charged.
        ok = true;
        break;
    }
    case kSabotageHalt: {  // 0x5b2450: halt-list entry, expiry now + int(FPS * duration + 0.5), -1 = permanent [H]
        const int32_t until = a.halt_duration == -1.0f ? INT32_MAX : frame_ + static_cast<int32_t>(30.0f * a.halt_duration + 0.5f);
        set_credit_halt(planet, until);  // [port] the keyed halt list folds into PlanetState::halt_until
        ok = true;
        break;
    }
    default: break;
    }
    return ok;
}

RejectReason GalacticSim::sabotage_execute_structure(int32_t player, PlanetIndex planet, ObjectHandle saboteur, ObjectHandle structure,
                                                    float cost, float respawn_seconds) {
    // 0x5b21a0(structure, cost, R, saboteur, planet, story = 0) [H].
    if (!valid_player(player)) return RejectReason::InvalidPlayer;
    if (!valid_planet(planet)) return RejectReason::InvalidTarget;
    if (!store_->alive(saboteur) || store_->owner(saboteur) != player) return RejectReason::InvalidUnit;
    const float c = commands::requantize(kQuantAmount, cost);
    const float r = commands::requantize(kQuantDuration, respawn_seconds);
    if (c > players_[static_cast<size_t>(player)].account.credits) return RejectReason::CannotAfford;
    PlanetState& p = planets_[planet];
    if (!store_->alive(structure) || std::find(p.structures.begin(), p.structures.end(), structure) == p.structures.end())
        return RejectReason::InvalidTarget;
    destroy_structure_sabotage(planet, structure, player, true);  // story GALACTIC_SABOTAGE with the structure type
    add_credits(player, -c, true);
    if (r >= 0.0f) schedule_hero_respawn(saboteur, planet, r);  // R = 0 -> Default_Hero_Respawn_Time
    if (store_->alive(saboteur)) despawn(saboteur);
    ++p.sabotage_count;  // (Sabotage_Particle_Effect, SFXEvent_Sabotage_Success: presentation)
    return RejectReason::None;
}

bool GalacticSim::sabotage_structure_reward(PlanetIndex planet, uint32_t type) {
    if (!valid_planet(planet)) return false;
    PlanetState& p = planets_[planet];
    for (ObjectHandle s : p.structures)
        if (store_->alive(s) && static_cast<uint32_t>(store_->type(s)) == type) {
            destroy_structure_sabotage(planet, s, kNoPlayer, true);  // no cost, no saboteur, still event 0x39 [H]
            ++p.sabotage_count;
            return true;
        }
    return false;
}

// ---- hack (P4 3) ------------------------------------------------------------------------------------------------------

ObjectHandle GalacticSim::hack_find_super_weapon(PlanetIndex planet) const {
    const uint32_t req = special().hack_required_type;
    if (!valid_planet(planet) || req == kNoIndex) return {};
    for (uint32_t f : planets_[planet].orbit) {  // fleets of X in list order, members in order, any owner [H]
        if (!fleets_[f].alive) continue;
        for (ObjectHandle m : fleets_[f].contents)
            if (store_->alive(m) && static_cast<uint32_t>(store_->type(m)) == req) return m;
    }
    return {};
}

void GalacticSim::lock_type(int32_t player, uint32_t type, bool locked) {
    if (!valid_player(player)) return;
    std::vector<uint32_t>& v = players_[static_cast<size_t>(player)].locked_types;
    const auto it = std::find(v.begin(), v.end(), type);
    if (locked && it == v.end()) v.push_back(type);  // 0x27f7c0
    if (!locked && it != v.end()) v.erase(it);       // 0x286100
}

bool GalacticSim::type_locked(int32_t player, uint32_t type) const {
    if (!valid_player(player)) return false;
    const std::vector<uint32_t>& v = players_[static_cast<size_t>(player)].locked_types;
    return std::find(v.begin(), v.end(), type) != v.end();
}

void GalacticSim::unlock_type(int32_t player, uint32_t type, int32_t source) {
    if (!valid_player(player) || type >= rules_->types.size()) return;
    std::vector<uint32_t>& v = players_[static_cast<size_t>(player)].unlocked_types;
    if (std::find(v.begin(), v.end(), type) != v.end()) return;  // 0x27f860: no duplicates
    v.push_back(type);
    GalacticEvent ev;
    ev.kind = GalacticEventKind::TypeUnlocked;
    ev.player = player;
    ev.type = type;
    ev.value = source;
    emit(ev);
}

bool GalacticSim::type_unlocked(int32_t player, uint32_t type) const {
    if (!valid_player(player)) return false;
    const std::vector<uint32_t>& v = players_[static_cast<size_t>(player)].unlocked_types;
    return std::find(v.begin(), v.end(), type) != v.end();
}

void GalacticSim::remove_unlock(int32_t player, uint32_t type) {
    if (!valid_player(player)) return;
    std::vector<uint32_t>& v = players_[static_cast<size_t>(player)].unlocked_types;
    const auto it = std::find(v.begin(), v.end(), type);
    if (it != v.end()) v.erase(it);
}

void GalacticSim::unlock_tech(int32_t player, uint32_t type) {
    if (!valid_player(player) || type >= rules_->types.size()) return;
    lock_type(player, type, false);
    unlock_type(player, type, 3);
}

bool GalacticSim::type_affiliated(uint32_t type, int32_t player) const {
    if (!valid_player(player) || type >= rules_->types.size()) return false;
    const TypeInfo& t = rules_->types[type];
    if (!t.data_rules && t.affiliation.empty()) return true;  // hand-built rules: empty = any faction [port]
    const uint32_t f = players_[static_cast<size_t>(player)].faction;
    return std::find(t.affiliation.begin(), t.affiliation.end(), f) != t.affiliation.end();
}

std::vector<uint32_t> GalacticSim::slice_candidates(int32_t player) const {
    std::vector<uint32_t> out;
    if (!valid_player(player)) return out;
    const int32_t tech = players_[static_cast<size_t>(player)].tech_level;
    for (uint32_t type = 0; type < rules_->types.size(); ++type) {
        const TypeInfo& t = rules_->types[type];
        if (t.build_initially_locked && t.sliceable && t.tech_level <= tech && !type_unlocked(player, type) &&
            !type_locked(player, type) && type_affiliated(type, player))
            out.push_back(type);
    }
    return out;
}

void GalacticSim::buildable_unit_reward(int32_t player, uint32_t unlock, uint32_t obsolete) {
    if (!valid_player(player)) return;
    if (unlock < rules_->types.size()) unlock_tech(player, unlock);
    if (obsolete < rules_->types.size()) lock_type(player, obsolete, true);
    const uint32_t f = players_[static_cast<size_t>(player)].faction;
    if (f < rules_->factions.size() && iequals(rules_->factions[f].name, rules_->constants.good_side_name) &&
        slice_candidates(player).empty())
        raise_tech_level(player);
}

uint32_t GalacticSim::corruption_unlock(PlanetIndex planet, int8_t type) const {
    if (!valid_planet(planet) || type == kCorruptionNone) return kNoIndex;
    const uint32_t pt = planets_[planet].type;
    if (pt >= special().types.size()) return kNoIndex;
    for (const CorruptionChoiceDef& c : special().types[pt].corruption)
        if (c.type == type) return c.success_unlock < rules_->types.size() ? c.success_unlock : kNoIndex;
    return kNoIndex;
}

void GalacticSim::corruption_unlocks(PlanetIndex planet, int8_t old_type, int32_t old_player) {
    if (!valid_planet(planet)) return;
    const uint32_t old_unlock = corruption_unlock(planet, old_type);
    if (old_unlock != kNoIndex) remove_unlock(old_player, old_unlock);
    const PlanetState& p = planets_[planet];
    const uint32_t unlock = corruption_unlock(planet, p.corruption);
    if (unlock != kNoIndex && valid_player(p.corrupting_player) && build_cost_for(p.corrupting_player, planet, unlock) > 0)
        unlock_type(p.corrupting_player, unlock, 4);
}

void GalacticSim::launch_reveal(PlanetIndex planet, ObjectHandle unit) {
    if (!valid_planet(planet) || !store_->exists(unit)) return;
    const uint32_t type = static_cast<uint32_t>(store_->type(unit));
    if (type >= rules_->types.size()) return;
    float r = rules_->types[type].influence_range;
    for (const uint32_t m : rules_->types[type].company_units)
        if (m < rules_->types.size()) r = std::max(r, rules_->types[m].influence_range);
    const int32_t id = static_cast<int32_t>(store_->id(unit));
    const auto add = [id](PlanetState& d) {  // 0x3fd9e0: unique set
        if (std::find(d.influence.begin(), d.influence.end(), id) == d.influence.end()) d.influence.push_back(id);
    };
    if (!(r > 0.0f)) {
        add(planets_[planet]);
        return;
    }
    const Vec3 c = planets_[planet].position;
    for (PlanetState& d : planets_) {
        if (!d.is_planet) continue;
        const float dx = d.position.x - c.x, dy = d.position.y - c.y, dz = d.position.z - c.z;
        if (dx * dx + dy * dy + dz * dz <= r * r) add(d);
    }
}

void GalacticSim::set_production_disabled(int32_t player, uint32_t type, bool disabled) {
    if (!valid_player(player)) return;
    std::vector<uint32_t>& v = players_[static_cast<size_t>(player)].production_disabled;
    const auto it = std::find(v.begin(), v.end(), type);
    if (disabled && it == v.end()) v.push_back(type);  // 0x27f710
    if (!disabled && it != v.end()) v.erase(it);       // 0x2860b0
}

bool GalacticSim::production_disabled(int32_t player, uint32_t type) const {
    if (!valid_player(player)) return false;
    const std::vector<uint32_t>& v = players_[static_cast<size_t>(player)].production_disabled;
    return std::find(v.begin(), v.end(), type) != v.end();
}

bool GalacticSim::build_locked(int32_t player, uint32_t type) const {
    if (!valid_player(player)) return false;
    const PlayerState& ps = players_[static_cast<size_t>(player)];
    if (!ps.locked_types.empty() && type_locked(player, type)) return true;
    return initial_build_locks_ && type < rules_->types.size() && rules_->types[type].build_initially_locked &&
           !type_unlocked(player, type);
}

void GalacticSim::hack_effect(int32_t player, PlanetIndex planet, ObjectHandle hacker, ObjectHandle weapon) {
    const int32_t q = store_->owner(weapon);
    const uint32_t wt = static_cast<uint32_t>(store_->type(weapon));
    emit_special(GalacticEventKind::SuperWeaponHacked, player, planet, hacker, wt, 0, q);
    lock_type(q, wt, true);                     // 1. Lock_Tech for the weapon's owner
    if (store_->alive(hacker)) despawn(hacker);  // 2. no respawn [H as read]
    begin_corruption_removal(planet, 5.0f);  // 3. 0x3f6770, 5 s (GRC 4.3); step 4's planet reset cancels it at once
    planet_destroy(planet, player);             // 4./5. (removes the weapon too; owner -> neutral)
}

bool GalacticSim::apply_hack(ObjectHandle hacker, int32_t player, PlanetIndex planet) {
    const PlayerState& ps = players_[static_cast<size_t>(player)];
    if (ps.human) {  // the hack dialog (hero id, planet id); the confirmation is command kind 7
        emit_special(GalacticEventKind::SpecialDialog, player, planet, hacker, kNoIndex, 0, static_cast<int32_t>(kSpecialHack));
        return false;
    }
    if (!ps.ai) return false;
    const ObjectHandle w = hack_find_super_weapon(planet);
    if (!store_->alive(w)) return false;
    hack_effect(player, planet, hacker, w);  // AI: free [H]
    return true;
}

RejectReason GalacticSim::hack_execute(int32_t player, PlanetIndex planet, ObjectHandle hacker, float cost) {
    if (!valid_player(player)) return RejectReason::InvalidPlayer;
    if (!valid_planet(planet)) return RejectReason::InvalidTarget;
    if (!store_->alive(hacker) || store_->owner(hacker) != player) return RejectReason::InvalidUnit;
    const float c = commands::requantize(kQuantAmount, cost);
    const ObjectHandle w = hack_find_super_weapon(planet);
    if (!store_->alive(w) || c > players_[static_cast<size_t>(player)].account.credits) {  // local error SFX [H]
        emit_special(GalacticEventKind::SpecialError, player, planet, hacker, kNoIndex, 0, -1);
        return store_->alive(w) ? RejectReason::CannotAfford : RejectReason::InvalidTarget;
    }
    add_credits(player, -c, true);
    hack_effect(player, planet, hacker, w);  // + SFXEvent_Hack_Success, hack particle effect (presentation)
    return RejectReason::None;
}

bool GalacticSim::planet_destroy_core(PlanetIndex planet, int32_t destroyer, int32_t event_value) {
    return planet_destroy_impl(planet, destroyer, event_value, false);
}

bool GalacticSim::planet_destroy_impl(PlanetIndex planet, int32_t destroyer, int32_t event_value, bool hack_orbit) {
    if (!valid_planet(planet) || planets_[planet].destroyed) return false;
    const int32_t old_owner = planets_[planet].owner;
    emit_special(GalacticEventKind::PlanetDestroyed, destroyer, planet, planets_[planet].object, kNoIndex, event_value, old_owner);
    {
        PlanetState& p = planets_[planet];
        p.siphon_player = kNoPlayer;  // siphon fields cleared
        p.siphon_amount = 0.0f;
    }
    auto remove = [this, planet](ObjectHandle h) {
        if (!store_->alive(h)) return;
        if (is_hero_type(static_cast<uint32_t>(store_->type(h)))) schedule_hero_respawn(h, planet, 0.0f);  // default time
        destroy_object(h);
    };
    if (hack_orbit) {  // hack extra 0x4b7860: orbiting objects D +0xc8 (reverse; includes the hacked super weapon)
        const std::vector<uint32_t> orbit(planets_[planet].orbit.begin(), planets_[planet].orbit.end());
        for (size_t i = orbit.size(); i-- > 0;) {
            const uint32_t f = orbit[i];
            if (!fleets_[f].alive) continue;
            const std::vector<ObjectHandle> members = fleets_[f].contents;
            for (size_t k = members.size(); k-- > 0;) remove(members[k]);
            if (fleets_[f].alive) kill_fleet(f);
        }
    }
    {  // special structures (D +0x158), reverse
        const std::vector<ObjectHandle> s = planets_[planet].structures;
        for (size_t k = s.size(); k-- > 0;) remove(s[k]);
    }
    set_corruption(planet, kCorruptionNone, kNoPlayer);  // with notifications
    planets_[planet].destroyed = true;                   // income: Planet_Destroyed_Credit_Value
    {  // surface objects D +0xe0 (reverse; heroes respawn at the default time)
        const std::vector<ObjectHandle> s = planets_[planet].surface;
        for (size_t k = s.size(); k-- > 0;) remove(s[k]);
    }
    return true;
}

bool GalacticSim::planet_destroy(PlanetIndex planet, int32_t destroyer) {
    // The hack path 0x61b790: the core with the orbit removal (inside, at its old place), then starbase and owner.
    if (!planet_destroy_impl(planet, destroyer, 0, true)) return false;
    if (store_->alive(planets_[planet].starbase)) destroy_object(planets_[planet].starbase);  // 0x4bda40
    // Owner := the NEUTRAL faction's player (0x3f8060); the leaning follows so the derivation keeps it.
    PlanetState& p = planets_[planet];
    p.leaning = neutral_player_;
    if (p.owner != neutral_player_) {
        const int32_t old = p.owner;
        p.owner = neutral_player_;
        p.control = 0.0f;
        on_owner_change(planet, old);
    }
    return true;
}

// ---- income abilities (P4 4) ------------------------------------------------------------------------------------------

void GalacticSim::service_income_abilities(PlanetIndex planet) {
    if (!valid_planet(planet)) return;
    PlanetState& p = planets_[planet];
    // Sources: the planet owner's objects at the planet (structures, starbase, surface, stationary orbit fleets) whose
    // type has an income ability. Accept 0x707fc0 / 0x48df20 [M]: same planet and owner [inferred]. Applied once on
    // arrival (value fixed at apply time [H as read]); removed when the source is gone (vf31 0x708020).
    struct Want {
        uint32_t def;
        ObjectHandle src;
    };
    std::vector<Want> want;
    auto add = [&](ObjectHandle h) {
        if (!store_->alive(h) || store_->owner(h) != p.owner) return;
        const TypeSpecial& ts = special_type(h);
        if (!ts.has_income_ability) return;
        for (uint32_t ai : ts.abilities) {
            const SpecialAbilityDef& a = special().abilities[ai];
            if ((a.cls == SpecialAbilityClass::IncomeBonus || a.cls == SpecialAbilityClass::IncomeGambling) && a.initially_enabled)
                want.push_back({ai, h});
        }
    };
    if (!special().types.empty() && valid_player(p.owner)) {
        for (ObjectHandle s : p.structures) add(s);
        add(p.starbase);
        for (ObjectHandle u : p.surface) add(u);
        for (uint32_t f : p.orbit)
            if (fleets_[f].alive && !fleets_[f].moving && fleets_[f].owner == p.owner)
                for (ObjectHandle m : fleets_[f].contents) add(m);
    }
    if (want.empty()) {
        bool any = false;
        for (const IncomeEntry& e : p.income) any = any || e.ability != 0;
        if (!any) return;
    }
    auto key_of = [this](const Want& w) {
        return std::pair<uint32_t, int32_t>{special().abilities[w.def].name_crc, static_cast<int32_t>(store_->id(w.src))};
    };
    // Remove stale entries (source gone, changed owner, or moved away).
    p.income.erase(std::remove_if(p.income.begin(), p.income.end(),
                                  [&](const IncomeEntry& e) {
                                      if (e.ability == 0) return false;
                                      for (const Want& w : want)
                                          if (key_of(w) == std::pair<uint32_t, int32_t>{e.ability, e.source}) return false;
                                      return true;
                                  }),
                   p.income.end());
    const float b = static_cast<float>(planet_base(p));  // 0x3733c0: runtime base or the destroyed value; no BM multiplier
    const bool mining = special().type(p.type).mining_colony;
    for (const Want& w : want) {
        const SpecialAbilityDef& a = special().abilities[w.def];
        const auto key = key_of(w);
        IncomeEntry* e = nullptr;
        for (IncomeEntry& x : p.income)
            if (x.ability == key.first && x.source == key.second) e = &x;
        if (a.cls == SpecialAbilityClass::IncomeBonus) {
            if (e) continue;  // fixed at apply time
            IncomeEntry n;
            n.kind = kIncomeAbility;
            float v = b * a.income_percentage + a.income_absolute;  // ((base x P) + A) x M [H]
            if (mining) v *= a.mining_multiplier;
            n.value = v;
            n.ability = key.first;
            n.source = key.second;
            n.tag = a.source_tag != 0 ? a.source_tag : special_type(w.src).text_crc;
            n.def = w.def;
            p.income.push_back(n);  // appended at the tail [H]
            continue;
        }
        // Gambling (vf4 0x708430): evaluated when due (service interval = Evaluation_Interval_In_Secs [M]); the first
        // service evaluates at once [inferred]. u = Get_Float01; m = u < Chance ? Get_Float(0, Max) : Get_Float(Min, 0).
        if (e && e->next_eval > frame_) continue;
        const float u = rng_->inline_unit_float();
        const float m = u < a.chance_to_win ? rng_->inline_range_float(0.0f, a.max_income_modifier)
                                            : rng_->inline_range_float(a.min_income_modifier, 0.0f);
        const int32_t next = frame_ + econ::timer_frames(a.evaluation_interval);
        if (e) {
            e->value = m * b;
            e->next_eval = next;
            continue;
        }
        IncomeEntry n;
        n.kind = kIncomeAbility;
        n.value = m * b;
        n.ability = key.first;
        n.source = key.second;
        n.tag = special_type(w.src).text_crc;
        n.next_eval = next;
        n.def = w.def;
        p.income.push_back(n);
    }
}

std::vector<EconomyRow> GalacticSim::economy_breakdown_by_tag(int32_t player) const {
    // 0x4b1950 [H]: every breakdown entry of every owned planet summed per source tag; siphon out / in and the piracy
    // pool as their own rows. Display values of the entries as last refreshed (no recomputation here).
    std::vector<EconomyRow> rows;
    auto add = [&rows](uint32_t tag, uint8_t kind, float v) {
        for (EconomyRow& r : rows)
            if (r.tag == tag && r.kind == kind) {
                r.value += v;
                return;
            }
        EconomyRow r;
        r.tag = tag;
        r.kind = kind;
        r.value = v;
        rows.push_back(r);
    };
    if (!valid_player(player)) return rows;
    const uint32_t smuggling = special_name_crc("TEXT_ECONOMY_SMUGGLING"), smuggler = special_name_crc("TEXT_INCOME_SMUGGLER"),
                   piracy = special_name_crc("TEXT_INCOME_PIRACY");
    for (const PlanetState& p : planets_) {
        if (!p.is_planet) continue;
        if (p.owner == player) {
            for (const IncomeEntry& e : p.income) add(e.kind == kIncomeAbility ? e.tag : 0u, e.kind, e.value);
            if (valid_player(p.siphon_player) && p.siphon_player != player) add(smuggling, kIncomeAbility, -p.siphon_amount);
        }
        if (p.siphon_player == player && p.owner != player) add(smuggler, kIncomeAbility, p.siphon_amount);
        if (benefits(player) && p.piracy_pool > 0.0f) add(piracy, kIncomeAbility, p.piracy_pool);
    }
    return rows;
}

float GalacticSim::economy_income_for_tag(int32_t player, uint32_t tag) const {
    float s = 0.0f;
    for (const EconomyRow& r : economy_breakdown_by_tag(player))
        if (r.tag == tag && r.kind == kIncomeAbility) s += r.value;
    return s;
}

// ---- corruption (P4 5) ------------------------------------------------------------------------------------------------

std::span<const CorruptionChoiceDef> GalacticSim::corruption_choices(PlanetIndex planet) const {
    if (!valid_planet(planet)) return {};
    return special().type(planets_[planet].type).corruption;
}

const CorruptionChoiceDef* GalacticSim::corruption_choice(PlanetIndex planet, int8_t type) const {
    for (const CorruptionChoiceDef& c : corruption_choices(planet))
        if (c.type == type) return &c;
    return nullptr;
}

bool GalacticSim::corruption_accepts(PlanetIndex planet, ObjectHandle defiler) const {
    // vf5 0x52be20 [H]. The T(X) byte +0xBA test is not applied [inferred]: its field mapping (Planet_Retains_Residual_
    // Influence) is [M] and would refuse 62 of the 65 shipped FoC planets.
    if (!valid_planet(planet) || !store_->alive(defiler)) return false;
    const PlanetState& p = planets_[planet];
    const int32_t player = store_->owner(defiler);
    return valid_player(p.owner) && p.owner != player && p.owner != neutral_player_ && !p.corruption_protected &&
           p.corruption == kCorruptionNone && !p.destroyed && !corruption_choices(planet).empty();
}

int32_t GalacticSim::count_required_units(int32_t player, const std::vector<uint32_t>& types) const {
    // 0x523950 [H]: every object of the listed types owned by P, anywhere in the galaxy; a container counted once. [port]
    // galactic teams are single objects: an object counts when its type, or one of its Company_Units, is listed.
    int32_t n = 0;
    for (ObjectHandle h : store_->owned_by(player)) {
        if (!store_->alive(h)) continue;
        const uint32_t t = static_cast<uint32_t>(store_->type(h));
        bool match = std::find(types.begin(), types.end(), t) != types.end();
        for (uint32_t m : special().type(t).members) match = match || std::find(types.begin(), types.end(), m) != types.end();
        if (match) ++n;
    }
    return n;
}

CorruptionRequirements GalacticSim::corruption_requirements(int32_t player, PlanetIndex planet, const CorruptionChoiceDef& c) const {
    CorruptionRequirements r;
    r.cost = c.credit_cost;
    if (!valid_player(player)) {
        r.credits = r.mission = r.units = r.planets = false;
        return r;
    }
    (void)planet;
    r.credits = c.credit_cost <= 0.0f || c.credit_cost <= credits(player);  // 0x523780
    r.mission_need = c.required_mission_count;  // 0x5237b0
    if (c.required_mission >= 0 && c.required_mission < 8 && c.required_mission_count >= 1) {
        r.mission_have = players_[static_cast<size_t>(player)].corruptions_completed[static_cast<size_t>(c.required_mission)];
        r.mission = r.mission_have >= r.mission_need;
    }
    r.units_need = c.required_unit_count;  // 0x523950
    if (!c.required_unit_types.empty() && c.required_unit_count >= 1) {
        r.units_have = count_required_units(player, c.required_unit_types);
        r.units = r.units_have >= r.units_need;
    }
    if (!c.required_planets.empty()) {  // 0x523810: P owns >= 1 planet of a listed type
        r.planets_need = 1;
        for (const PlanetState& p : planets_)
            if (p.is_planet && p.owner == player && std::find(c.required_planets.begin(), c.required_planets.end(), p.type) != c.required_planets.end())
                ++r.planets_have;
        r.planets = r.planets_have >= 1;
    }
    return r;
}

int32_t GalacticSim::corruption_ai_pick(int32_t player, PlanetIndex planet) {
    const std::span<const CorruptionChoiceDef> choices = corruption_choices(planet);
    if (!valid_player(player) || choices.empty()) return -1;
    const std::array<int32_t, 8>& n = players_[static_cast<size_t>(player)].corruptions_completed;
    auto inv = [&n](size_t t) { return 1.0f / static_cast<float>(n[t] + 1); };
    std::vector<int32_t> cand;
    std::vector<float> cum;
    float total = 0.0f;
    for (size_t i = 0; i < choices.size(); ++i) {
        const CorruptionChoiceDef& c = choices[i];
        if (!corruption_requirements(player, planet, c).all()) continue;
        float w = 0.0f;  // 0x52b160 weights [H]
        switch (c.type) {
        case 0: w = 2.0f * inv(0); break;
        case 1: w = 1.0f + inv(1); break;
        case 2: w = 0.75f; break;
        case 3: w = 1.5f; break;
        case 4: w = 0.5f + inv(4); break;
        case 5: w = 0.25f; break;
        case 6: w = 2.5f; break;
        case 7: w = 0.75f; break;
        default: break;
        }
        total += w;
        cand.push_back(static_cast<int32_t>(i));
        cum.push_back(total);
    }
    if (cand.empty() || !(total > 0.0f)) return -1;
    const float x = rng_->inline_range_float(0.0f, total);
    for (size_t k = 0; k < cum.size(); ++k)
        if (cum[k] >= x) return cand[k];
    return cand.back();
}

void GalacticSim::consume_prerequisites(int32_t player, PlanetIndex planet, const CorruptionChoiceDef& c) {
    // 0x52b870 [H]: when Required_Unit_Count > 0, the first listed type with live objects of P, if that type has
    // Destroy_When_Used_As_Corruption_Prereq (vanilla: none).
    if (c.required_unit_count <= 0) return;
    for (uint32_t t : c.required_unit_types) {
        std::vector<ObjectHandle> objs;
        for (ObjectHandle h : store_->owned_by(player)) {
            if (!store_->alive(h)) continue;
            const uint32_t ht = static_cast<uint32_t>(store_->type(h));
            const std::vector<uint32_t>& mem = special().type(ht).members;
            if (ht == t || std::find(mem.begin(), mem.end(), t) != mem.end()) objs.push_back(h);
        }
        if (objs.empty()) continue;
        if (!special().type(t).destroy_when_prereq) return;
        if (special().type(t).named_hero) {  // the first object gets a default-time respawn at X and is despawned
            hero_depart(objs.front(), planet, 0.0f);
            return;
        }
        // Candidates on a planet, a Get_Float(0, 4) weight each (list order), max-heap; pop Required_Unit_Count times.
        std::vector<std::pair<float, size_t>> heap;
        for (size_t i = 0; i < objs.size(); ++i) {
            if (location_of(objs[i]) == kInvalidPlanet) continue;
            heap.push_back({rng_->inline_range_float(0.0f, 4.0f), i});
        }
        // Max weight first; equal weights: earlier candidate first [inferred: heap tie order not recovered].
        std::stable_sort(heap.begin(), heap.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        for (size_t k = 0; k < heap.size() && k < static_cast<size_t>(c.required_unit_count); ++k)
            destroy_object(objs[heap[k].second]);
        return;
    }
}

CorruptionCharge GalacticSim::corruption_charge(int32_t player, PlanetIndex planet, int8_t type, float fallback_cost) {
    if (!valid_player(player)) return CorruptionCharge::NoCredits;
    const CorruptionChoiceDef* c = corruption_choice(planet, type);
    const float cost = c ? c->credit_cost : fallback_cost;
    if (!(cost <= 0.0f || cost <= credits(player))) return CorruptionCharge::NoCredits;
    if (cost > 0.0f) add_credits(player, -cost, true);  // charged BEFORE the unit check [H as read]
    if (c) {
        if (!corruption_requirements(player, planet, *c).units) return CorruptionCharge::UnitsUnmet;  // no refund [H]
        consume_prerequisites(player, planet, *c);
    }
    return CorruptionCharge::Charged;
}

void GalacticSim::count_corruption(int32_t player, int8_t type) {
    if (valid_player(player) && type >= 0 && type < 8)
        ++players_[static_cast<size_t>(player)].corruptions_completed[static_cast<size_t>(type)];
}

int32_t GalacticSim::corruptions_completed(int32_t player, int8_t type) const {
    if (!valid_player(player) || type < 0 || type >= 8) return 0;
    return players_[static_cast<size_t>(player)].corruptions_completed[static_cast<size_t>(type)];
}

bool GalacticSim::apply_corruption(ObjectHandle defiler, int32_t player, PlanetIndex planet) {
    // vf30 0x52b160 [H].
    if (corruption_choices(planet).empty()) {
        emit_special(GalacticEventKind::SpecialError, player, planet, defiler, kNoIndex, 0, -1);
        return false;
    }
    const PlayerState& ps = players_[static_cast<size_t>(player)];
    if (ps.human) {  // the corruption dialog ("corruption_dialog" story hook); the choice is command kind 6 (apps/sim)
        emit_special(GalacticEventKind::SpecialDialog, player, planet, defiler, kNoIndex, 0, static_cast<int32_t>(kSpecialCorruption));
        return false;
    }
    if (!ps.ai) return false;
    const int32_t pick = corruption_ai_pick(player, planet);
    if (pick < 0) return false;
    const CorruptionChoiceDef c = corruption_choices(planet)[static_cast<size_t>(pick)];
    // Executor 0x52bf50, direct path (an AI never takes the mission path, L1_S4 4.5): credits then units, apply, the
    // completed counter (0x2820a0), departure: respawn at X with the default time, despawn (P4 5.5).
    if (corruption_charge(player, planet, c.type, c.credit_cost) != CorruptionCharge::Charged) return false;
    begin_corruption_gain(planet, c.type, player, 5.0f);  // GRC 4.3: direct path, 5 s transition (wrapper 0x3f6810)
    count_corruption(player, c.type);
    hero_depart(defiler, planet, 0.0f);
    return true;
}

// ---- command (event 13) -----------------------------------------------------------------------------------------------

RejectReason GalacticSim::special_command(int32_t player, const commands::EventData& ev) {
    SpecialPayload pl;
    if (!read_special_payload(ev, pl)) return RejectReason::Unhandled;  // kind 6 / others: the host's hooks
    // Execute guard (0x4b4d70 prologue) [H]: skipped while a galactic battle is pending (the galactic pause).
    if (paused()) return RejectReason::NotAllowed;
    const ObjectHandle ph = pl.planet <= 0x7FFFFFFFu ? store_->find(static_cast<objects::ObjectId>(pl.planet)) : ObjectHandle{};
    const PlanetIndex planet = planet_of(ph);
    if (planet == kInvalidPlanet) return RejectReason::InvalidTarget;
    const ObjectHandle unit = pl.unit <= 0x7FFFFFFFu ? store_->find(static_cast<objects::ObjectId>(pl.unit)) : ObjectHandle{};
    switch (pl.kind) {
    case kSpecialActivate: {
        // Setter 0x4b5320; a fleet: its first member with an accepting Ground_Activated ability at X (0x398670).
        if (!store_->alive(unit) || store_->owner(unit) != player) return RejectReason::InvalidUnit;
        std::vector<ObjectHandle> users;
        const uint32_t f = fleet_of(unit);
        if (f != kNoFleet) {
            if (fleets_[f].planet != planet || fleets_[f].moving) return RejectReason::InvalidTarget;
            for (ObjectHandle m : fleets_[f].contents) {
                if (!store_->alive(m)) continue;
                for (uint32_t ai : special_type(m).abilities) {
                    const SpecialAbilityDef& a = special().abilities[ai];
                    if (a.style == ActivationStyle::GroundActivated && a.initially_enabled && special_accepts(a, m, player, planet)) {
                        users.push_back(m);
                        break;
                    }
                }
                if (!users.empty()) break;
            }
        } else {
            if (location_of(unit) != planet) return RejectReason::InvalidTarget;  // [port] the unit must be there
            users.push_back(unit);
        }
        if (users.empty()) return RejectReason::NotAllowed;
        bool accepted = false;
        ground_activated_trigger(users.front(), planet, &accepted);
        return accepted ? RejectReason::None : RejectReason::NotAllowed;
    }
    case kSpecialBlackMarket: {
        const uint32_t item = special().find_item(pl.item_crc);
        if (item == kNoIndex) return RejectReason::Malformed;
        return bm_execute_purchase(player, planet, unit, item, pl.amount, pl.seconds);
    }
    case kSpecialSabotage: {
        const ObjectHandle s = pl.structure <= 0x7FFFFFFFu ? store_->find(static_cast<objects::ObjectId>(pl.structure)) : ObjectHandle{};
        return sabotage_execute_structure(player, planet, unit, s, pl.amount, pl.seconds);
    }
    case kSpecialHack: return hack_execute(player, planet, unit, pl.amount);
    case kSpecialSlice: {
        // Execute 0x4b4d70 case 2: price q8, chance q5 (without the x1000) and R q5 re-quantized locally (COMMANDS 3.2).
        if (!valid_player(player)) return RejectReason::InvalidPlayer;
        if (!store_->alive(unit) || store_->owner(unit) != player) return RejectReason::InvalidUnit;
        if (pl.type >= rules_->types.size()) return RejectReason::Malformed;
        SliceEntry e;
        e.type = pl.type;
        e.price = commands::requantize(kQuantAmount, pl.amount);
        e.chance = commands::requantize(kQuantDuration, pl.chance);
        if (e.price > players_[static_cast<size_t>(player)].account.credits) return RejectReason::CannotAfford;
        slice_execute(player, planet, unit, e, commands::requantize(kQuantDuration, pl.seconds), pl.only_one);
        return RejectReason::None;
    }
    default: return RejectReason::Unhandled;
    }
}

// ---- Rebel tech theft (REMAINING_GAPS 1) -------------------------------------------------------------------------------

bool GalacticSim::slice_accepts(int32_t player, PlanetIndex planet) const {
    // vf5 0x619040 [H]: a planet not held by the slicer's player or an ally, a playable owner faction, >= 1 candidate.
    if (!valid_planet(planet) || !valid_player(player)) return false;
    const int32_t owner = planets_[planet].owner;
    return valid_player(owner) && !allied(player, owner) && faction_of(owner).is_playable && !slice_candidates(player).empty();
}

std::vector<SliceEntry> GalacticSim::slice_entries(int32_t player, PlanetIndex planet, uint32_t ability) const {
    std::vector<SliceEntry> out;
    if (!valid_player(player) || !valid_planet(planet) || ability >= special().abilities.size()) return out;
    const SpecialAbilityDef& a = special().abilities[ability];
    const PlanetState& d = planets_[planet];
    const TypeSpecial& x = special().type(d.type);
    const float m = std::max(0.0001f, players_[static_cast<size_t>(player)].tech_cost_mult);  // 0x281960
    const int32_t ground_level = static_cast<int32_t>(d.structures.size());  // D +0x130 [port: structure count]
    for (const uint32_t type : slice_candidates(player)) {  // 0x619680 per candidate
        const TypeInfo& t = rules_->types[type];
        const bool affinity = std::find(x.slice_affinity.begin(), x.slice_affinity.end(), type) != x.slice_affinity.end();
        const float ca = affinity ? a.chance_affinity : 0.0f;
        const float ka = affinity ? a.cost_affinity : 0.0f;
        SliceEntry e;
        e.type = type;
        e.price = std::max(0.0f, (a.cost_by_tech_availability.eval(x.tech_availability) + ka) * special().type(type).slice_cost * m);
        float b = 0.0f;
        if (t.build_cost > 0) {
            if (t.required_star_base > 0 || (t.dummy == kDummyStarBase && t.base_level == 1))
                b = a.chance_by_base_level.eval(static_cast<float>(d.starbase_level));
            else if (t.required_ground_base > 0 || t.dummy == kDummyGroundStructure)
                b = a.chance_by_base_level.eval(static_cast<float>(ground_level));
        }
        e.chance = std::clamp(1.0f + b + a.chance_by_difficulty.eval(x.slice_difficulty) + ca, 0.0f, 1.0f);
        out.push_back(e);
    }
    return out;
}

bool GalacticSim::apply_slicer(const SpecialAbilityDef& a, ObjectHandle slicer, int32_t player, PlanetIndex planet) {
    // vf30 0x6187a0 [H]: accept re-check, the faction pairing, then (no candidates) raise the tech and apply again.
    if (!slice_accepts(player, planet)) return false;
    if (!faction_pairing(player, planets_[planet].owner)) {
        emit_special(GalacticEventKind::SpecialError, player, planet, slicer, kNoIndex, 0, -1);
        return false;
    }
    const uint32_t ability = static_cast<uint32_t>(&a - special().abilities.data());
    std::vector<SliceEntry> entries = slice_entries(player, planet, ability);
    while (entries.empty()) {  // the vf30 recursion
        if (!raise_tech_level(player)) return false;  // at max tech: message only
        if (!slice_accepts(player, planet)) return false;
        entries = slice_entries(player, planet, ability);
    }
    // 4. R, drawn now: Min/Max[t] with t = the tech level (P +0x84) when the lists exist and t <= 4; else 0 (default time).
    const PlayerState& ps = players_[static_cast<size_t>(player)];
    float r = 0.0f;
    const int32_t t = ps.tech_level;
    if (!a.slice_min_respawn.empty() && t >= 0 && t <= 4 && static_cast<size_t>(t) < a.slice_min_respawn.size())
        r = rng_->inline_range_float(a.slice_min_respawn[static_cast<size_t>(t)], a.slice_max_respawn[static_cast<size_t>(t)]);
    if (ps.human) {  // 5. the slice dialog (rows = slice_entries); a pick is command kind 2, Cancel sends nothing
        emit_special(GalacticEventKind::SpecialDialog, player, planet, slicer, kNoIndex,
                     static_cast<int32_t>(commands::quantize(kQuantDuration, r)), static_cast<int32_t>(kSpecialSlice));
        return false;
    }
    if (!ps.ai) return false;
    // 6. AI: affordable entries, weight 1 each, one Get_Float(0, count), first cumulative >= r.
    std::vector<size_t> cand;
    for (size_t i = 0; i < entries.size(); ++i)
        if (entries[i].price <= ps.account.credits) cand.push_back(i);
    if (cand.empty()) return false;
    const float x = rng_->inline_range_float(0.0f, static_cast<float>(cand.size()));
    size_t pick = cand.size() - 1;
    for (size_t k = 0; k < cand.size(); ++k)
        if (static_cast<float>(k + 1) >= x) {
            pick = k;
            break;
        }
    return slice_execute(player, planet, slicer, entries[cand[pick]], r, entries.size() == 1);
}

bool GalacticSim::slice_execute(int32_t player, PlanetIndex planet, ObjectHandle slicer, const SliceEntry& e,
                                float respawn_seconds, bool only_one) {
    // 0x619160 [H].
    if (!valid_player(player) || !valid_planet(planet) || !store_->alive(slicer)) return false;  // 1.
    if (e.price > players_[static_cast<size_t>(player)].account.credits) return false;          // 2. no draw, no despawn
    const int32_t owner = planets_[planet].owner;
    if (special().type(planets_[planet].type).planet_vigilance && special_type(slicer).generic_hero) {  // 3.
        emit_special(GalacticEventKind::Slice, player, planet, slicer, e.type, 4, owner);
        hero_depart(slicer, planet, 0.0f);
        return false;
    }
    const float s = rng_->inline_range_float(0.0f, 0.999f);  // 4. (0x1ffbb0)
    float r = respawn_seconds;
    const bool success = s < e.chance;
    if (success) {  // 5.
        add_credits(player, -e.price, true);
        unlock_type(player, e.type, 2);
        if (only_one) raise_tech_level(player);
        emit_special(GalacticEventKind::Slice, player, planet, slicer, e.type, 0, owner);
    } else {  // 6. k: 1 captured (R x 2), 2 lost effort, 3 cheated (charged)
        const int32_t k = rng_->range_int(1, 3);
        if (k == 1) r *= 2.0f;
        if (k == 3) add_credits(player, -e.price, true);
        emit_special(GalacticEventKind::Slice, player, planet, slicer, e.type, k, owner);
    }
    hero_depart(slicer, planet, r);  // 7.
    return success;
}

// ---- CRC --------------------------------------------------------------------------------------------------------------

bool GalacticSim::special_state_default() const {
    for (const PlayerState& p : players_) {
        if (!p.bm_purchased.empty() || p.bm_tutorial || p.sabotage_tutorial || !p.locked_types.empty() ||
            !p.production_disabled.empty())
            return false;
        for (int32_t c : p.corruptions_completed)
            if (c != 0) return false;
    }
    for (const PlanetState& p : planets_)
        if (p.sabotage_count != 0 || !p.respawns.empty()) return false;
    return true;
}

void GalacticSim::hash_special(StateHasher& h) const {
    h.u32(0x50345331u);  // section marker "P4S1"
    for (const PlayerState& p : players_) {
        h.u32(static_cast<uint32_t>(p.bm_purchased.size()));
        for (uint32_t i : p.bm_purchased) h.u32(i < special().items.size() ? special().items[i].name_crc : i);
        h.boolean(p.bm_tutorial);
        h.boolean(p.sabotage_tutorial);
        for (int32_t c : p.corruptions_completed) h.i32(c);
        h.u32(static_cast<uint32_t>(p.locked_types.size()));
        for (uint32_t t : p.locked_types) h.u32(t);
        h.u32(static_cast<uint32_t>(p.production_disabled.size()));
        for (uint32_t t : p.production_disabled) h.u32(t);
    }
    for (const PlanetState& p : planets_) {
        if (!p.is_planet) continue;
        h.i32(p.sabotage_count);
        h.u32(static_cast<uint32_t>(p.respawns.size()));
        for (const RespawnEntry& e : p.respawns) {
            h.i32(e.due);
            h.u32(e.type);
            h.i32(e.owner);
            h.u8(e.flag);
        }
    }
}

}  // namespace alamo::galactic
