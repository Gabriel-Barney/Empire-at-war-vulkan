#include "alamo/ai/tactical/tactical_perception.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <string>

#include "alamo/core/sim_crc.hpp"
#include "alamo/objects/enums.hpp"

namespace alamo::ai::tactical {

namespace {

// ---------------------------------------------------------------------------------- leaves
enum class Leaf : uint16_t {
    None = 0,
    // sub-evaluator tokens
    Location, Owner, Hints, Type,
    // unit (TacticalObject / TacticalUnit / LandUnit / SpaceShip)
    Health, Shield, Energy, ForceUnnormalized, Force, ForceNBTP, ForceNBTD, HasTakenDamage, IsBuildPad, HasBuiltObject,
    NearbyOpenBuildPadCount, ContainsHero, DistanceToNearestFriendly, DistanceToNearestEnemy, IsContestable,
    GarrisonSlotsAvailable, AdditionalPopulationCapacity, IsCashPoint, CashPointValue, IsInsideEnemyShield,
    IsInsideFriendlyShield, IsValidBombingTarget, HardPointHealth, AreEnginesOnline,
    // location
    FriendlyForceUnnormalized, EnemyForceUnnormalized, FriendlyForce, EnemyForce, FriendlyForceNBTD, EnemyForceNBTD,
    FriendlyUnitConcentrationUnnormalized, EnemyUnitConcentrationUnnormalized, FriendlyUnitConcentration,
    EnemyUnitConcentration, FriendlyUnitConcentrationNBTD, EnemyUnitConcentrationNBTD, FriendlyUnitsBitfield,
    EnemyUnitsBitfield, TimeLastSeenUnnormalized, TimeLastSeen, IsFriendlyStartLocation, IsEnemyStartLocation,
    // player
    LandedForceUnnormalized, IsDefender, CanRetreat, IsRetreating, BaseLevel, OpenBuildPadCount,
    TacticalBuiltStructureCount, ReinforcementsUnnormalized, UnitSpaceAvailable, IsBombingRunAvailable,
    IsOrbitalBombardmentAvailable, HasUnit, IsFaction, IsDifficulty, CreditsUnnormalized, TechLevel, BudgetAllocation,
    StoryTrigger, ActiveGoals,
    // game
    Age, IsCampaignGame, IsLandControlGame, IsSetupPhase, ForceVisibility, Timeline, InterventionsEnabled,
    TimeSinceStoryPopup, ActiveStoryGoalCount, IsStoryCampaign,
    // type / hints
    IsType, HintValue,
};

struct LeafName {
    std::string_view name;
    Leaf leaf;
};
#define ALAMO_LEAF(x) {#x, Leaf::x}
constexpr LeafName kLeafNames[] = {
    ALAMO_LEAF(Location), ALAMO_LEAF(Owner), ALAMO_LEAF(Hints),
    ALAMO_LEAF(Health), ALAMO_LEAF(Shield), ALAMO_LEAF(Energy), ALAMO_LEAF(ForceUnnormalized), ALAMO_LEAF(Force),
    ALAMO_LEAF(ForceNBTP), ALAMO_LEAF(ForceNBTD), ALAMO_LEAF(HasTakenDamage), ALAMO_LEAF(IsBuildPad),
    ALAMO_LEAF(HasBuiltObject), ALAMO_LEAF(NearbyOpenBuildPadCount), ALAMO_LEAF(ContainsHero),
    ALAMO_LEAF(DistanceToNearestFriendly), ALAMO_LEAF(DistanceToNearestEnemy), ALAMO_LEAF(IsContestable),
    ALAMO_LEAF(GarrisonSlotsAvailable), ALAMO_LEAF(AdditionalPopulationCapacity), ALAMO_LEAF(IsCashPoint),
    ALAMO_LEAF(CashPointValue), ALAMO_LEAF(IsInsideEnemyShield), ALAMO_LEAF(IsInsideFriendlyShield),
    ALAMO_LEAF(IsValidBombingTarget), ALAMO_LEAF(HardPointHealth), ALAMO_LEAF(AreEnginesOnline),
    ALAMO_LEAF(FriendlyForceUnnormalized), ALAMO_LEAF(EnemyForceUnnormalized), ALAMO_LEAF(FriendlyForce),
    ALAMO_LEAF(EnemyForce), ALAMO_LEAF(FriendlyForceNBTD), ALAMO_LEAF(EnemyForceNBTD),
    ALAMO_LEAF(FriendlyUnitConcentrationUnnormalized), ALAMO_LEAF(EnemyUnitConcentrationUnnormalized),
    ALAMO_LEAF(FriendlyUnitConcentration), ALAMO_LEAF(EnemyUnitConcentration), ALAMO_LEAF(FriendlyUnitConcentrationNBTD),
    ALAMO_LEAF(EnemyUnitConcentrationNBTD), ALAMO_LEAF(FriendlyUnitsBitfield), ALAMO_LEAF(EnemyUnitsBitfield),
    ALAMO_LEAF(TimeLastSeenUnnormalized), ALAMO_LEAF(TimeLastSeen), ALAMO_LEAF(IsFriendlyStartLocation),
    ALAMO_LEAF(IsEnemyStartLocation), ALAMO_LEAF(LandedForceUnnormalized), ALAMO_LEAF(IsDefender), ALAMO_LEAF(CanRetreat),
    ALAMO_LEAF(IsRetreating), ALAMO_LEAF(BaseLevel), ALAMO_LEAF(OpenBuildPadCount), ALAMO_LEAF(TacticalBuiltStructureCount),
    ALAMO_LEAF(ReinforcementsUnnormalized), ALAMO_LEAF(UnitSpaceAvailable), ALAMO_LEAF(IsBombingRunAvailable),
    ALAMO_LEAF(IsOrbitalBombardmentAvailable), ALAMO_LEAF(HasUnit), ALAMO_LEAF(IsFaction), ALAMO_LEAF(IsDifficulty),
    ALAMO_LEAF(CreditsUnnormalized), ALAMO_LEAF(TechLevel), ALAMO_LEAF(BudgetAllocation), ALAMO_LEAF(StoryTrigger),
    ALAMO_LEAF(ActiveGoals), ALAMO_LEAF(Age), ALAMO_LEAF(IsCampaignGame), ALAMO_LEAF(IsLandControlGame),
    ALAMO_LEAF(IsSetupPhase), ALAMO_LEAF(ForceVisibility), ALAMO_LEAF(Timeline), ALAMO_LEAF(InterventionsEnabled),
    ALAMO_LEAF(TimeSinceStoryPopup), ALAMO_LEAF(ActiveStoryGoalCount), ALAMO_LEAF(IsStoryCampaign), ALAMO_LEAF(IsType),
};
#undef ALAMO_LEAF

const std::vector<Leaf>& registry_leaves() {
    static const std::vector<Leaf> table = [] {
        std::vector<Leaf> r(TokenTable::registry_size(), Leaf::None);
        for (const LeafName& ln : kLeafNames) {
            const TokenId id = registry_token(ln.name);
            if (id != kNoToken && id < r.size()) r[id] = ln.leaf;
        }
        return r;
    }();
    return table;
}

// Several leaf names the evaluators register (Health, Age, Type, ...) are not in the 0x5b43d0 name
// list S2 seeded the token table with; those are matched by name.
Leaf leaf_of(TokenId id, const TokenTable* tokens) {
    if (TokenTable::is_registered(id)) {
        const Leaf l = registry_leaves()[id];
        if (l != Leaf::None) return l;
    }
    std::string_view name;
    if (tokens && id < tokens->size()) name = tokens->name(id);
    else if (TokenTable::is_registered(id)) name = registry_token_names()[id];
    if (name.empty()) return Leaf::None;
    if (iequal(name, "Type")) return Leaf::Type;
    for (const LeafName& ln : kLeafNames)
        if (iequal(ln.name, name)) return ln.leaf;
    return Leaf::None;
}

// ---------------------------------------------------------------------------------- evaluators
enum class Ev : uint8_t { Game = 0, Player = 1, Unit = 2, UnitLocation = 3, Zone = 4, Cell = 5, Type = 6, Hints = 7 };

struct Node {
    Ev kind = Ev::Game;
    int32_t a = -1;  // Player: player index; Unit/UnitLocation/Zone/Cell/Type: target handle; Hints: zone index
};

// Leaves registered on an evaluator class and their cache lifetimes (section 3). False = not registered.
bool leaf_info(Ev ev, Leaf l, TacticalMode mode, int32_t& life) {
    const bool land = mode == TacticalMode::Land;
    auto r = [&](int32_t v) {
        life = v;
        return true;
    };
    switch (ev) {
        case Ev::Unit:
            switch (l) {
                case Leaf::Health: case Leaf::Shield: case Leaf::Energy: case Leaf::ForceUnnormalized: case Leaf::Force:
                case Leaf::ForceNBTP: case Leaf::ForceNBTD: case Leaf::HasTakenDamage: case Leaf::HasBuiltObject:
                case Leaf::NearbyOpenBuildPadCount: case Leaf::GarrisonSlotsAvailable:
                    return r(0);
                case Leaf::IsBuildPad: case Leaf::ContainsHero: case Leaf::IsContestable: case Leaf::AdditionalPopulationCapacity:
                    return r(kCacheForever);
                case Leaf::DistanceToNearestFriendly: case Leaf::DistanceToNearestEnemy:
                    return r(2);
                case Leaf::IsCashPoint: case Leaf::CashPointValue:
                    return land && r(kCacheForever);
                case Leaf::IsInsideEnemyShield: case Leaf::IsInsideFriendlyShield: case Leaf::IsValidBombingTarget:
                    return land && r(5);
                case Leaf::HardPointHealth: case Leaf::AreEnginesOnline:
                    return !land && r(0);
                default: return false;
            }
        case Ev::UnitLocation: case Ev::Zone: case Ev::Cell:
            switch (l) {
                case Leaf::FriendlyForceUnnormalized: return r(10);
                case Leaf::EnemyForceUnnormalized: return r(9);
                case Leaf::FriendlyForce: case Leaf::EnemyForce: case Leaf::FriendlyForceNBTD: case Leaf::EnemyForceNBTD:
                case Leaf::ForceUnnormalized: case Leaf::FriendlyUnitConcentration: case Leaf::EnemyUnitConcentration:
                case Leaf::FriendlyUnitConcentrationNBTD: case Leaf::EnemyUnitConcentrationNBTD:
                    return r(0);
                case Leaf::FriendlyUnitConcentrationUnnormalized: return r(11);
                case Leaf::EnemyUnitConcentrationUnnormalized: return r(8);
                case Leaf::FriendlyUnitsBitfield: case Leaf::EnemyUnitsBitfield: return r(10);
                case Leaf::DistanceToNearestFriendly: case Leaf::DistanceToNearestEnemy: return r(2);
                case Leaf::TimeLastSeenUnnormalized: case Leaf::TimeLastSeen: return r(7);
                case Leaf::IsFriendlyStartLocation: case Leaf::IsEnemyStartLocation: return r(kCacheForever);
                case Leaf::IsInsideEnemyShield: case Leaf::IsInsideFriendlyShield: case Leaf::IsValidBombingTarget:
                    return land && r(5);
                default: return false;
            }
        case Ev::Player:
            switch (l) {
                case Leaf::ForceUnnormalized: case Leaf::Force: return r(1);
                case Leaf::FriendlyForceUnnormalized: case Leaf::EnemyForceUnnormalized: case Leaf::ForceNBTD:
                case Leaf::BudgetAllocation:
                    return r(2);
                case Leaf::FriendlyForce: case Leaf::EnemyForce: return r(3);
                case Leaf::IsDefender: case Leaf::IsFaction: return r(kCacheForever);
                case Leaf::HasUnit: return r(8);
                case Leaf::CanRetreat: case Leaf::IsRetreating: case Leaf::BaseLevel: case Leaf::OpenBuildPadCount:
                case Leaf::TacticalBuiltStructureCount: case Leaf::ReinforcementsUnnormalized: case Leaf::UnitSpaceAvailable:
                case Leaf::IsDifficulty: case Leaf::CreditsUnnormalized: case Leaf::TechLevel: case Leaf::StoryTrigger:
                case Leaf::ActiveGoals:
                    return r(0);
                case Leaf::LandedForceUnnormalized: return land && r(1);
                case Leaf::IsBombingRunAvailable: case Leaf::IsOrbitalBombardmentAvailable: return land && r(0);
                default: return false;
            }
        case Ev::Game:
            switch (l) {
                case Leaf::Age: case Leaf::IsSetupPhase: case Leaf::Timeline: case Leaf::InterventionsEnabled:
                case Leaf::TimeSinceStoryPopup: case Leaf::ActiveStoryGoalCount:
                    return r(0);
                case Leaf::IsCampaignGame: case Leaf::IsLandControlGame: case Leaf::IsStoryCampaign: return r(kCacheForever);
                case Leaf::ForceUnnormalized: return r(3);
                case Leaf::ForceVisibility: return r(2);
                default: return false;
            }
        case Ev::Type: return l == Leaf::IsType && r(0);
        case Ev::Hints: return l == Leaf::HintValue && r(0);
    }
    return false;
}

// ---------------------------------------------------------------------------------- parameters
const Param* find_param(std::span<const Param> ps, std::string_view name) {
    for (const Param& p : ps)
        if (iequal(p.name, name)) return &p;
    return nullptr;
}

double param_number(std::span<const Param> ps, std::string_view name, double fallback, bool* present = nullptr) {
    const Param* p = find_param(ps, name);
    if (present) *present = p != nullptr;
    if (!p) return fallback;
    if (p->value.kind == ParamValue::Kind::Number) return p->value.number;
    const std::string s(p->value.kind == ParamValue::Kind::Enum && !p->value.enum_values.empty() ? p->value.enum_values[0]
                                                                                                 : p->value.text);
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    return end != s.c_str() ? v : fallback;
}

// Every name of every `name` parameter (String / Identifier text split at '|' / ',' / spaces, Enum values).
void param_names(std::span<const Param> ps, std::string_view name, std::vector<std::string_view>& out) {
    out.clear();
    for (const Param& p : ps) {
        if (!iequal(p.name, name)) continue;
        if (p.value.kind == ParamValue::Kind::Enum) {
            for (const std::string& v : p.value.enum_values) out.emplace_back(v);
            continue;
        }
        if (p.value.kind == ParamValue::Kind::Number) continue;
        const std::string_view t = p.value.text;
        size_t i = 0;
        while (i < t.size()) {
            while (i < t.size() && (t[i] == '|' || t[i] == ',' || t[i] == ' ' || t[i] == '\t')) ++i;
            size_t j = i;
            while (j < t.size() && !(t[j] == '|' || t[j] == ',' || t[j] == ' ' || t[j] == '\t')) ++j;
            if (j > i) out.push_back(t.substr(i, j - i));
            i = j;
        }
    }
}

std::string canonical_params(std::span<const Param> ps, std::string_view hint) {
    std::string s(hint);
    char buf[32];
    for (const Param& p : ps) {
        s += ';';
        s += p.name;
        s += '=';
        switch (p.value.kind) {
            case ParamValue::Kind::Number:
                std::snprintf(buf, sizeof buf, "#%016llx", static_cast<unsigned long long>(std::bit_cast<uint64_t>(p.value.number)));
                s += buf;
                break;
            case ParamValue::Kind::String:
                s += '"';
                s += p.value.text;
                s += '"';
                break;
            case ParamValue::Kind::Enum:
                s += p.value.text;
                s += '[';
                for (size_t i = 0; i < p.value.enum_values.size(); ++i) {
                    if (i) s += '|';
                    s += p.value.enum_values[i];
                }
                s += ']';
                break;
            case ParamValue::Kind::Identifier:
                s += p.value.text;
                break;
        }
    }
    return s;
}

bool listed(std::span<const std::string_view> names, std::string_view n) {
    for (const std::string_view x : names)
        if (iequal(x, n)) return true;
    return false;
}

float dist2(const Vec3f& a, const Vec3f& b) {
    const float dx = a.x - b.x, dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}
float dist3(const Vec3f& a, const Vec3f& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// ---------------------------------------------------------------------------------- evaluation
struct Area {
    bool zero = false;             // land unit outside every zone
    std::vector<uint32_t> own;     // rect cells
    std::span<const uint32_t> cells;
    Vec3f center;
    int32_t attached = -1;         // attached object id
    float attached_radius = 0.0f;
    int32_t zone = -1;             // zone of the location (land)
    bool zone_total = false;       // land region: ForceUnnormalized = zone danger
    bool point = false;            // ForceUnnormalized = point danger at `center`
};

struct Eval {
    const TacticalPerception& p;
    const PerceptionWorld& w;
    const EvalContext& ctx;
    const TokenTable* tokens;
    const std::vector<PerceptionTarget>& targets;
    const std::vector<std::vector<uint32_t>>& zone_cells;

    TacticalMode mode() const { return p.mode(); }
    bool land() const { return mode() == TacticalMode::Land; }

    const PerceptionTarget* target_of(int32_t h) const {
        if (h < 0 || static_cast<size_t>(h) >= targets.size()) return nullptr;
        return &targets[static_cast<size_t>(h)];
    }
    const PerceptionObject* unit_object(int32_t h) const {
        const PerceptionTarget* t = target_of(h);
        if (!t || t->kind != PerceptionTarget::Kind::Unit || t->released) return nullptr;
        return w.object(t->object_id);
    }
    uint64_t category(std::span<const Param> ps) const {
        if (!find_param(ps, "Parameter_Category")) return ~0ull;
        std::vector<std::string_view> names;
        param_names(ps, "Parameter_Category", names);
        uint64_t m = 0;
        for (const std::string_view n : names) m |= p.config().category_mask ? p.config().category_mask(n) : 0;
        return m;
    }
    float attenuator(std::span<const Param> ps) const {
        return static_cast<float>(param_number(ps, "Parameter_Attenuator", 0.0));
    }
    std::span<const uint32_t> zcells(int32_t z) const {
        if (z < 0 || static_cast<size_t>(z) >= zone_cells.size()) return {};
        return zone_cells[static_cast<size_t>(z)];
    }
    int32_t type_index(std::string_view name) const {
        for (size_t i = 0; i < w.types.size(); ++i)
            if (iequal(w.types[i].name, name)) return static_cast<int32_t>(i);
        return -1;
    }
    std::string_view type_name(int32_t t) const {
        if (t < 0 || static_cast<size_t>(t) >= w.types.size()) return {};
        return w.types[static_cast<size_t>(t)].name;
    }

    // ---- path walk ----
    bool start(Root root, Node& n) const {
        switch (root) {
            case Root::Game: n = {Ev::Game, -1}; return true;
            case Root::Self: n = {Ev::Player, ctx.self}; return w.is_player(ctx.self);
            case Root::Enemy: n = {Ev::Player, ctx.enemy}; return w.is_player(ctx.enemy);
            case Root::Human: n = {Ev::Player, ctx.human}; return w.is_player(ctx.human);
            case Root::Target: case Root::Location: {
                const int32_t h = root == Root::Target ? ctx.target : ctx.location;
                const PerceptionTarget* t = target_of(h);
                if (!t) return false;
                switch (t->kind) {
                    case PerceptionTarget::Kind::Unit: n = {Ev::Unit, h}; return true;
                    case PerceptionTarget::Kind::LandZone: n = {Ev::Zone, h}; return true;
                    case PerceptionTarget::Kind::SpaceCell: n = {Ev::Cell, h}; return true;
                }
                return false;
            }
        }
        return false;
    }

    bool step(Node& n, Leaf sub) const {
        switch (sub) {
            case Leaf::Location:
                if (n.kind != Ev::Unit) return false;
                n.kind = Ev::UnitLocation;
                return true;
            case Leaf::Owner: {
                if (n.kind != Ev::Unit) return false;
                const PerceptionTarget* t = target_of(n.a);
                if (!t || !w.is_player(t->owner_at_registration)) return false;
                n = {Ev::Player, t->owner_at_registration};
                return true;
            }
            case Leaf::Type:
                if (n.kind != Ev::Unit) return false;
                n.kind = Ev::Type;
                return true;
            case Leaf::Hints: {
                if (!land()) return false;
                if (n.kind == Ev::Zone) {
                    n = {Ev::Hints, target_of(n.a)->zone};
                    return true;
                }
                if (n.kind == Ev::UnitLocation) {
                    const PerceptionObject* o = unit_object(n.a);
                    if (!o) return false;
                    n = {Ev::Hints, o->zone};
                    return true;
                }
                return false;
            }
            default: return false;
        }
    }

    bool resolve(Root root, std::span<const TokenId> path, std::span<const Param> params, double& out) const {
        if (path.empty()) return false;
        Node n;
        if (!start(root, n)) return false;
        for (size_t i = 0; i + 1 < path.size(); ++i)
            if (!step(n, leaf_of(path[i], tokens))) return false;
        if (n.kind == Ev::Hints) {
            const TokenId id = path.back();
            std::string_view name;
            if (tokens && id < tokens->size()) name = tokens->name(id);
            else if (TokenTable::is_registered(id)) name = registry_token_names()[id];
            return value(n, Leaf::HintValue, params, name, out);
        }
        return value(n, leaf_of(path.back(), tokens), params, {}, out);
    }

    // ---- cached leaf ----
    bool value(const Node& n, Leaf l, std::span<const Param> params, std::string_view hint, double& out) const {
        int32_t life = 0;
        if (l == Leaf::None || !leaf_info(n.kind, l, mode(), life)) return false;
        LeafCacheKey key;
        if (life != kCacheNone) {
            key.eval_kind = static_cast<uint8_t>(n.kind);
            key.eval_a = n.a;
            key.leaf = static_cast<uint16_t>(l);
            key.self = ctx.self;
            key.target = ctx.target;
            key.enemy = ctx.enemy;
            key.human = ctx.human;
            key.location = ctx.location;
            key.params = canonical_params(params, hint);
            if (p.cache().lookup(key, life, w.frame, out)) return true;
        }
        double v = 0.0;
        if (!compute(n, l, params, hint, v)) return false;
        if (life != kCacheNone) p.cache().store(key, life, w.frame, v);
        out = v;
        return true;
    }

    bool ratio(const Node& nn, Leaf nl, std::span<const Param> np, const Node& dn, Leaf dl, double& out) const {
        double num = 0.0;
        if (!value(nn, nl, np, {}, num)) return false;
        if (num == 0.0) {
            out = 0.0;
            return true;
        }
        double den = 0.0;
        if (!value(dn, dl, {}, {}, den)) return false;
        out = num <= den ? num / den : 1.0;
        return true;
    }

    // ---- shared bodies ----
    // 0x656400 (+ the land slot-13 transport rule): P = -1 -> every player.
    float player_force(int32_t P, bool friendly, uint64_t cat, float a, bool land_transports) const {
        if (P >= 0 && w.players[static_cast<size_t>(P)].neutral_faction) return 0.0f;
        float sum = 0.0f;
        const auto n = static_cast<int32_t>(w.players.size());
        for (int32_t q = 0; q < n; ++q) {
            if (P >= 0 && (w.rel(P, q) == 0) != friendly) continue;
            for (const int32_t id : p.grid().player_objects(q)) {
                const PerceptionObject* o = w.object(id);
                if (!o) continue;
                const PerceptionType* t = w.type_of(*o);
                if (!t || (t->category & cat) == 0) continue;
                sum += o->combat_power * (1.0f - (1.0f - o->health) * a);
            }
        }
        if (land_transports && P >= 0) {
            for (const PerceptionObject& o : w.objects) {  // [M] reinforcements still in the air
                if (!o.transport_landing || !o.alive || !w.is_player(o.owner)) continue;
                if ((w.rel(P, o.owner) == 0) != friendly) continue;
                const PerceptionType* t = w.type_of(o);
                if (!t || (t->category & cat) == 0) continue;
                sum += o.combat_power * (1.0f - (1.0f - o.health) * a);
            }
        }
        return sum;
    }

    bool qualifies(int32_t self, int32_t q, bool friendly) const {
        if (friendly) return w.rel(self, q) == 0;
        return w.rel(self, q) != 0 && !w.players[static_cast<size_t>(q)].neutral_faction;
    }

    // DistanceToNearest* scan (0x730df0 object variant / 0x72d000 location variant).
    bool nearest(const Vec3f& c, bool three_d, float own_radius, int32_t exclude, bool friendly, std::span<const Param> ps,
                 double& out) const {
        if (!w.is_player(ctx.self)) return false;
        const uint64_t cat = category(ps);
        std::vector<std::string_view> types;
        param_names(ps, "Parameter_Type", types);
        float best = 1.0e18f;
        const auto n = static_cast<int32_t>(w.players.size());
        for (int32_t q = 0; q < n; ++q) {
            if (!qualifies(ctx.self, q, friendly)) continue;
            for (const int32_t id : p.grid().player_objects(q)) {
                if (id == exclude) continue;
                const PerceptionObject* o = w.object(id);
                if (!o || !o->alive) continue;
                const PerceptionType* t = w.type_of(*o);
                if (!t || (t->category & cat) == 0) continue;
                if (!types.empty()) {
                    const PerceptionType* rt = w.rep_type_of(*o);
                    if (!rt || !listed(types, rt->name)) continue;
                }
                if (w.fow_active && w.object_fogged && w.object_fogged(ctx.self, id)) continue;
                const float d = (three_d ? dist3(c, o->position) : dist2(c, o->position)) - own_radius - o->radius;
                if (d < best) best = d;
            }
        }
        out = static_cast<double>(best);
        return true;
    }

    bool location_area(const Node& n, Area& a) const {
        const TacticalPerceptionGrid& g = p.grid();
        if (n.kind == Ev::Zone) {
            const PerceptionTarget* t = target_of(n.a);
            a.cells = zcells(t->zone);
            a.center = t->center;
            a.zone = t->zone;
            a.zone_total = true;
            return true;
        }
        if (n.kind == Ev::Cell) {
            const PerceptionTarget* t = target_of(n.a);
            g.rect_cells(t->rect, a.own);
            a.cells = a.own;
            a.center = t->center;
            a.point = true;
            return true;
        }
        const PerceptionObject* o = unit_object(n.a);
        if (!o) return false;
        a.center = o->position;
        a.attached = o->id;
        a.attached_radius = o->radius;
        a.point = true;
        if (land()) {
            a.zone = o->zone;
            if (o->zone < 0) {
                a.zero = true;
                return true;
            }
            a.cells = zcells(o->zone);
            return true;
        }
        const float r = o->max_attack_range;
        g.rect_cells({o->position.x - r, o->position.y - r, o->position.x + r, o->position.y + r}, a.own);
        a.cells = a.own;
        return true;
    }

    float area_scale() const {
        return land() ? p.constants().land_area_threat_scale : p.constants().space_area_threat_scale;
    }

    bool time_last_seen(const Area& a, double& out) const {
        if (a.zero || a.cells.empty()) {
            out = 0.0;
            return true;
        }
        const TacticalPerceptionGrid& g = p.grid();
        float sum = 0.0f;
        for (const uint32_t cell : a.cells) {
            const auto col = static_cast<int32_t>(cell % static_cast<uint32_t>(g.nx()));
            const auto row = static_cast<int32_t>(cell / static_cast<uint32_t>(g.nx()));
            const Vec3f c = g.cell_center(col, row);
            if (land() && w.point_blocked && w.point_blocked(c.x, c.y)) continue;  // counts as 0 [M]
            const bool fogged = w.fow_active && w.point_fogged && w.point_fogged(ctx.self, c.x, c.y);
            if (fogged) sum += static_cast<float>(static_cast<int64_t>(w.frame) - g.last_seen(ctx.self, cell));
        }
        const float mean = sum / static_cast<float>(a.cells.size());
        out = static_cast<double>(mean / static_cast<float>(kFramesPerSecond));
        return true;
    }

    bool start_location(const Area& a, bool friendly, double& out) const {
        if (!w.is_player(ctx.self)) return false;
        const PerceptionPlayerInfo& self = w.players[static_cast<size_t>(ctx.self)];
        StartMarker::Kind kind = StartMarker::Kind::PlayerPosition;
        if (w.campaign) {
            const bool defender = !self.attacker;
            const bool want_def = friendly ? defender : !defender;
            kind = want_def ? StartMarker::Kind::DefendingForces : StartMarker::Kind::AttackerEntry;
        } else if (!friendly) {
            double f = 0.0;
            if (!start_location(a, true, f)) return false;
            out = 1.0 - f;
            return true;
        }
        const float wdt = w.map.max_x - w.map.min_x, hgt = w.map.max_y - w.map.min_y;
        const float D = std::sqrt(wdt * wdt + hgt * hgt) / 2.0f;
        bool found = false;
        float best = 0.0f;
        for (const StartMarker& m : w.markers) {
            if (m.kind != kind) continue;
            if (kind == StartMarker::Kind::PlayerPosition && m.player_id != self.id) continue;
            const float v = 1.0f - dist2(m.position, a.center) / (2.0f * D);
            if (!found || v > best) best = v;
            found = true;
        }
        if (!found) return false;
        out = static_cast<double>(best);
        return true;
    }

    bool hero_or_listed(int32_t type, std::span<const std::string_view> types) const {
        if (type < 0 || static_cast<size_t>(type) >= w.types.size()) return false;
        const PerceptionType& t = w.types[static_cast<size_t>(type)];
        return types.empty() ? t.is_hero : listed(types, t.name);
    }

    bool type_listed_or_builds(int32_t type, std::span<const std::string_view> types, bool builds) const {
        if (type < 0 || static_cast<size_t>(type) >= w.types.size()) return false;
        const PerceptionType& t = w.types[static_cast<size_t>(type)];
        if (listed(types, t.name)) return true;
        return builds && t.builds_into >= 0 && listed(types, type_name(t.builds_into));
    }

    // ---- leaf bodies ----
    bool compute(const Node& n, Leaf l, std::span<const Param> ps, std::string_view hint, double& out) const {
        switch (n.kind) {
            case Ev::Unit: return unit_leaf(n, l, ps, out);
            case Ev::UnitLocation: case Ev::Zone: case Ev::Cell: return location_leaf(n, l, ps, out);
            case Ev::Player: return player_leaf(n, l, ps, out);
            case Ev::Game: return game_leaf(l, ps, out);
            case Ev::Type: {
                const PerceptionObject* o = unit_object(n.a);
                if (!o) return false;
                std::vector<std::string_view> types;
                param_names(ps, "Parameter_Type", types);
                const PerceptionType* t = w.type_of(*o);
                out = (t && listed(types, t->name)) ? 1.0 : 0.0;
                return true;
            }
            case Ev::Hints:
                out = (w.zone_hint && n.a >= 0) ? w.zone_hint(n.a, hint) : 0.0;
                return true;
        }
        return false;
    }

    bool unit_leaf(const Node& n, Leaf l, std::span<const Param> ps, double& out) const {
        const PerceptionObject* o = unit_object(n.a);
        if (!o) return false;
        const PerceptionTarget* tg = target_of(n.a);
        const Node game{Ev::Game, -1};
        const Node owner{Ev::Player, tg->owner_at_registration};
        switch (l) {
            case Leaf::Health: out = static_cast<double>(o->health); return true;
            case Leaf::Shield: out = static_cast<double>(o->shield); return true;
            case Leaf::Energy: out = static_cast<double>(o->energy); return true;
            case Leaf::ForceUnnormalized: {
                const PerceptionType* t = w.type_of(*o);
                const uint64_t cat = category(ps);
                const float a = attenuator(ps);
                out = (t && (t->category & cat) != 0) ? static_cast<double>(o->combat_power * (1.0f - (1.0f - o->health) * a)) : 0.0;
                return true;
            }
            case Leaf::Force: return ratio(n, Leaf::ForceUnnormalized, ps, game, Leaf::ForceUnnormalized, out);
            case Leaf::ForceNBTP:
                if (!w.is_player(owner.a)) return false;
                return ratio(n, Leaf::ForceUnnormalized, ps, owner, Leaf::ForceUnnormalized, out);
            case Leaf::ForceNBTD:
                if (!w.is_player(owner.a)) return false;
                return ratio(n, Leaf::ForceUnnormalized, ps, owner, Leaf::FriendlyForceUnnormalized, out);
            case Leaf::HasTakenDamage: {
                const double t = param_number(ps, "Parameter_Time", 0.0);
                out = (o->has_damage_tracking && o->last_damage_frame != -1 &&
                       static_cast<double>(w.frame) <= static_cast<double>(o->last_damage_frame) + t * kFramesPerSecond)
                          ? 1.0
                          : 0.0;
                return true;
            }
            case Leaf::IsBuildPad: out = o->is_build_pad ? 1.0 : 0.0; return true;
            case Leaf::HasBuiltObject: {
                if (!o->is_build_pad || o->built_type < 0) {
                    out = 0.0;
                    return true;
                }
                std::vector<std::string_view> types;
                param_names(ps, "Parameter_Type", types);
                out = (types.empty() || listed(types, type_name(o->built_type))) ? 1.0 : 0.0;
                return true;
            }
            case Leaf::NearbyOpenBuildPadCount: {
                bool has = false;
                const double r = param_number(ps, "Parameter_Range", 0.0, &has);
                if (!has) return false;
                if (w.nearby_open_pads) {
                    out = static_cast<double>(w.nearby_open_pads(o->id, static_cast<float>(r)));
                    return true;
                }
                uint32_t c = 0;  // [port] objects scan when the free-store table is not wired
                for (const PerceptionObject& x : w.objects)
                    if (x.id != o->id && x.alive && x.is_build_pad && x.built_type < 0 &&
                        static_cast<double>(dist3(x.position, o->position)) <= r)
                        ++c;
                out = static_cast<double>(c);
                return true;
            }
            case Leaf::ContainsHero: {
                std::vector<std::string_view> types;
                param_names(ps, "Parameter_Type", types);
                bool hit = hero_or_listed(o->type, types);
                for (size_t i = 0; !hit && i < o->contained_types.size(); ++i) hit = hero_or_listed(o->contained_types[i], types);
                out = hit ? 1.0 : 0.0;
                return true;
            }
            case Leaf::DistanceToNearestFriendly: case Leaf::DistanceToNearestEnemy:
                return nearest(o->position, true, o->radius, o->id, l == Leaf::DistanceToNearestFriendly, ps, out);
            case Leaf::IsContestable: out = o->contestable ? 1.0 : 0.0; return true;
            case Leaf::GarrisonSlotsAvailable: out = static_cast<double>(o->garrison_capacity - o->garrison_occupants); return true;
            case Leaf::AdditionalPopulationCapacity: out = static_cast<double>(o->additional_population_capacity); return true;
            case Leaf::IsCashPoint: out = o->is_cash_point ? 1.0 : 0.0; return true;
            case Leaf::CashPointValue: out = o->is_cash_point ? static_cast<double>(o->cash_point_value) : 0.0; return true;
            case Leaf::IsInsideEnemyShield: case Leaf::IsInsideFriendlyShield:
                out = (w.inside_shield && w.inside_shield(o->position, ctx.self, l == Leaf::IsInsideEnemyShield)) ? 1.0 : 0.0;
                return true;
            case Leaf::IsValidBombingTarget: out = o->valid_bombing_target ? 1.0 : 0.0; return true;
            case Leaf::HardPointHealth: {  // [L] mean health of the hard points of that type
                bool has = false;
                const double ht = param_number(ps, "Parameter_Hard_Point_Type", 0.0, &has);
                std::vector<PerceptionHardPoint> hps;
                w.object_hard_points(*o, hps);
                float sum = 0.0f;
                uint32_t cnt = 0;
                for (const PerceptionHardPoint& hp : hps) {
                    if (has && static_cast<double>(hp.type) != ht) continue;
                    sum += hp.destroyed ? 0.0f : hp.health;
                    ++cnt;
                }
                out = cnt ? static_cast<double>(sum / static_cast<float>(cnt)) : 0.0;
                return true;
            }
            case Leaf::AreEnginesOnline: out = o->engines_online ? 1.0 : 0.0; return true;
            default: return false;
        }
    }

    bool location_leaf(const Node& n, Leaf l, std::span<const Param> ps, double& out) const {
        Area a;
        if (!location_area(n, a)) return false;
        const Node game{Ev::Game, -1};
        const Node self{Ev::Player, ctx.self};
        const TacticalPerceptionGrid& g = p.grid();
        GridQuery q;
        q.self = ctx.self;
        q.exclude = a.attached;
        q.memory_counted_when_not_fogged = p.config().memory_counted_when_not_fogged;
        switch (l) {
            case Leaf::FriendlyForceUnnormalized: case Leaf::EnemyForceUnnormalized: {
                if (!w.is_player(ctx.self)) return false;
                if (a.zero) {
                    out = 0.0;
                    return true;
                }
                q.ally = l == Leaf::FriendlyForceUnnormalized;
                q.category = category(ps);
                q.attenuator = attenuator(ps);
                const Param* f = find_param(ps, "Parameter_Faction");
                std::string faction;
                if (f) faction = f->value.kind == ParamValue::Kind::Enum && !f->value.enum_values.empty() ? f->value.enum_values[0] : f->value.text;
                q.faction = faction;
                out = static_cast<double>(g.cells_force(w, a.cells, q) * area_scale());
                return true;
            }
            case Leaf::FriendlyForce: return ratio(n, Leaf::FriendlyForceUnnormalized, ps, game, Leaf::ForceUnnormalized, out);
            case Leaf::EnemyForce: return ratio(n, Leaf::EnemyForceUnnormalized, ps, game, Leaf::ForceUnnormalized, out);
            case Leaf::FriendlyForceNBTD:
                return ratio(n, Leaf::FriendlyForceUnnormalized, ps, self, Leaf::FriendlyForceUnnormalized, out);
            case Leaf::EnemyForceNBTD: return ratio(n, Leaf::EnemyForceUnnormalized, ps, self, Leaf::EnemyForceUnnormalized, out);
            case Leaf::ForceUnnormalized: {
                if (!w.is_player(ctx.self)) return false;
                if (a.zero) {
                    out = 0.0;
                    return true;
                }
                out = static_cast<double>(a.zone_total ? g.cells_danger(w, ctx.self, a.cells)
                                                       : g.point_danger(w, ctx.self, a.center.x, a.center.y));
                return true;
            }
            case Leaf::FriendlyUnitConcentrationUnnormalized: case Leaf::EnemyUnitConcentrationUnnormalized: {
                if (!w.is_player(ctx.self)) return false;
                if (a.zero) {
                    out = 0.0;
                    return true;
                }
                q.ally = l == Leaf::FriendlyUnitConcentrationUnnormalized;
                q.category = category(ps);
                out = static_cast<double>(g.cells_concentration(w, a.cells, q));
                return true;
            }
            case Leaf::FriendlyUnitConcentration:
                return ratio(n, Leaf::FriendlyUnitConcentrationUnnormalized, ps, game, Leaf::ForceUnnormalized, out);
            case Leaf::EnemyUnitConcentration:
                return ratio(n, Leaf::EnemyUnitConcentrationUnnormalized, ps, game, Leaf::ForceUnnormalized, out);
            case Leaf::FriendlyUnitConcentrationNBTD:
                return ratio(n, Leaf::FriendlyUnitConcentrationUnnormalized, ps, self, Leaf::FriendlyForceUnnormalized, out);
            case Leaf::EnemyUnitConcentrationNBTD:
                return ratio(n, Leaf::EnemyUnitConcentrationUnnormalized, ps, self, Leaf::EnemyForceUnnormalized, out);
            case Leaf::FriendlyUnitsBitfield: case Leaf::EnemyUnitsBitfield: {
                if (!w.is_player(ctx.self)) return false;
                q.ally = l == Leaf::FriendlyUnitsBitfield;
                std::vector<std::string_view> types;
                param_names(ps, "Parameter_Type", types);
                std::vector<BitfieldMatcher> m;
                for (const std::string_view t : types) m.push_back({t, 0});
                for (const Param& prm : ps) {  // one matcher per Parameter_Category entry, in order
                    if (!iequal(prm.name, "Parameter_Category")) continue;
                    const std::span<const Param> one(&prm, 1);
                    m.push_back({{}, category(one)});
                }
                out = static_cast<double>(g.cells_bitfield(w, a.cells, q, m));
                return true;
            }
            case Leaf::DistanceToNearestFriendly: case Leaf::DistanceToNearestEnemy:
                return nearest(a.center, false, a.attached_radius, a.attached, l == Leaf::DistanceToNearestFriendly, ps, out);
            case Leaf::TimeLastSeenUnnormalized:
                if (!w.is_player(ctx.self)) return false;
                return time_last_seen(a, out);
            case Leaf::TimeLastSeen: {
                if (!w.is_player(ctx.self)) return false;
                double u = 0.0;
                if (!time_last_seen(a, u)) return false;
                out = u * static_cast<double>(kFramesPerSecond) / static_cast<double>(w.frame);
                return true;
            }
            case Leaf::IsFriendlyStartLocation: return start_location(a, true, out);
            case Leaf::IsEnemyStartLocation: return start_location(a, false, out);
            case Leaf::IsInsideEnemyShield: case Leaf::IsInsideFriendlyShield: {
                Vec3f c = a.center;
                if (a.zone >= 0 && static_cast<size_t>(a.zone) < p.zone_count() && a.attached < 0) {
                    const HintZone& z = p.zone(static_cast<uint32_t>(a.zone));
                    c = {z.center[0], z.center[1], z.center[2]};
                }
                out = (w.inside_shield && w.inside_shield(c, ctx.self, l == Leaf::IsInsideEnemyShield)) ? 1.0 : 0.0;
                return true;
            }
            case Leaf::IsValidBombingTarget: out = 0.0; return true;  // [L] body not traced
            default: return false;
        }
    }

    bool player_leaf(const Node& n, Leaf l, std::span<const Param> ps, double& out) const {
        const int32_t P = n.a;
        if (!w.is_player(P)) return false;
        const PerceptionPlayerInfo& pl = w.players[static_cast<size_t>(P)];
        const Node game{Ev::Game, -1};
        switch (l) {
            case Leaf::ForceUnnormalized: case Leaf::FriendlyForceUnnormalized: case Leaf::LandedForceUnnormalized:
            case Leaf::EnemyForceUnnormalized:
                out = static_cast<double>(player_force(P, l != Leaf::EnemyForceUnnormalized, category(ps), attenuator(ps), land()));
                return true;
            case Leaf::Force: return ratio(n, Leaf::ForceUnnormalized, ps, game, Leaf::ForceUnnormalized, out);
            case Leaf::FriendlyForce: return ratio(n, Leaf::FriendlyForceUnnormalized, ps, game, Leaf::ForceUnnormalized, out);
            case Leaf::EnemyForce: return ratio(n, Leaf::EnemyForceUnnormalized, ps, game, Leaf::ForceUnnormalized, out);
            case Leaf::ForceNBTD: return ratio(n, Leaf::ForceUnnormalized, ps, n, Leaf::FriendlyForceUnnormalized, out);
            case Leaf::IsDefender: out = (w.campaign && !pl.attacker) ? 1.0 : 0.0; return true;
            case Leaf::CanRetreat: out = pl.can_retreat ? 1.0 : 0.0; return true;
            case Leaf::IsRetreating: out = pl.is_retreating ? 1.0 : 0.0; return true;
            case Leaf::BaseLevel: {
                if (land()) {
                    if (w.campaign) {
                        out = static_cast<double>(w.conflict_ground_structures);
                        return true;
                    }
                    int32_t c = 0;
                    for (const PerceptionObject& o : w.objects)
                        if (o.owner == P && o.alive && o.dummy_ground_structure) ++c;
                    out = static_cast<double>(std::clamp(c, 0, 5));
                    return true;
                }
                out = 0.0;  // [M] first star base among players allied with P, GOM order
                for (const PerceptionObject& o : w.objects) {
                    if (!o.dummy_star_base || o.contestable || !w.is_player(o.owner) || w.rel(P, o.owner) != 0) continue;
                    out = static_cast<double>(o.starbase_level);
                    break;
                }
                return true;
            }
            case Leaf::OpenBuildPadCount: {
                if (P != ctx.self) return false;
                std::vector<std::string_view> types;
                param_names(ps, "Parameter_Type", types);
                uint32_t c = 0;
                for (const PerceptionObject& o : w.objects) {
                    if (!o.alive || !o.is_build_pad || o.built_type >= 0) continue;
                    if (!types.empty()) {
                        bool ok = listed(types, type_name(o.type));
                        for (size_t i = 0; !ok && i < types.size(); ++i) {
                            const int32_t ti = type_index(types[i]);
                            ok = ti >= 0 && w.pad_accepts && w.pad_accepts(o.type, ti, pl.faction);
                        }
                        if (!ok) continue;
                    }
                    ++c;
                }
                out = static_cast<double>(c);
                return true;
            }
            case Leaf::TacticalBuiltStructureCount: {
                if (P != ctx.self) return false;
                std::vector<std::string_view> types;
                param_names(ps, "Parameter_Type", types);
                uint32_t c = 0;
                for (const PerceptionObject& o : w.objects)
                    if (o.owner == P && o.alive && o.built_structure && (types.empty() || listed(types, type_name(o.type)))) ++c;
                out = static_cast<double>(c);
                return true;
            }
            case Leaf::ReinforcementsUnnormalized: {
                const uint64_t cat = category(ps);
                float sum = 0.0f;
                for (const int32_t t : pl.reinforcements) {
                    if (t < 0 || static_cast<size_t>(t) >= w.types.size()) continue;
                    const PerceptionType& ty = w.types[static_cast<size_t>(t)];
                    if (ty.category & cat) sum += ty.combat_power;
                }
                out = static_cast<double>(sum);
                return true;
            }
            case Leaf::UnitSpaceAvailable:
                out = static_cast<double>(std::max(0, std::min(pl.pop_cap - pl.pop_used, pl.pop_cap)));
                return true;
            case Leaf::IsBombingRunAvailable: out = pl.bombing_run_available ? 1.0 : 0.0; return true;
            case Leaf::IsOrbitalBombardmentAvailable: out = pl.orbital_bombardment_available ? 1.0 : 0.0; return true;
            case Leaf::HasUnit: {
                std::vector<std::string_view> types;
                param_names(ps, "Parameter_Type", types);
                const double want = param_number(ps, "Parameter_Count", 1.0);
                const double complete = param_number(ps, "Parameter_Only_Consider_Complete", 0.0);
                const bool builds = complete <= 0.0;
                double count = 0.0;
                auto done = [&] { return count >= want; };
                for (const PerceptionObject& o : w.objects) {
                    if (done()) break;
                    if (o.owner == P && o.alive && type_listed_or_builds(o.type, types, builds)) count += 1.0;
                }
                for (const std::string_view tn : types) {  // community property: allied players' objects
                    if (done()) break;
                    const int32_t ti = type_index(tn);
                    if (ti < 0 || !w.types[static_cast<size_t>(ti)].community_property) continue;
                    for (const PerceptionObject& o : w.objects) {
                        if (done()) break;
                        if (o.alive && o.type == ti && o.owner != P && w.is_player(o.owner) && w.rel(P, o.owner) == 0) count += 1.0;
                    }
                }
                for (size_t i = 0; !done() && i < pl.reinforcements.size(); ++i)
                    if (listed(types, type_name(pl.reinforcements[i]))) count += 1.0;
                if (builds)
                    for (size_t i = 0; !done() && i < pl.production_queue.size(); ++i)
                        if (listed(types, type_name(pl.production_queue[i]))) count += 1.0;
                out = done() ? 1.0 : 0.0;
                return true;
            }
            case Leaf::IsFaction: {
                std::vector<std::string_view> f;
                param_names(ps, "Parameter_Faction", f);
                out = listed(f, pl.faction) ? 1.0 : 0.0;
                return true;
            }
            case Leaf::IsDifficulty: {
                if (pl.is_human || pl.ai_difficulty.empty()) {
                    out = 0.0;
                    return true;
                }
                std::vector<std::string_view> d;
                param_names(ps, "Parameter_Difficulty_Level_Type", d);
                out = listed(d, pl.ai_difficulty) ? 1.0 : 0.0;
                return true;
            }
            case Leaf::CreditsUnnormalized: out = P == ctx.self ? static_cast<double>(pl.credits) : 0.0; return true;
            case Leaf::TechLevel: out = static_cast<double>(pl.tech_level); return true;
            case Leaf::BudgetAllocation: case Leaf::StoryTrigger: case Leaf::ActiveGoals: out = 0.0; return true;  // [M] galactic
            default: return false;
        }
    }

    bool game_leaf(Leaf l, std::span<const Param> ps, double& out) const {
        switch (l) {
            case Leaf::Age: out = static_cast<double>(static_cast<float>(w.frame) * (1.0f / 30.0f)); return true;
            case Leaf::IsCampaignGame: out = w.campaign ? 1.0 : 0.0; return true;
            case Leaf::IsLandControlGame: out = w.land_control ? 1.0 : 0.0; return true;
            case Leaf::IsSetupPhase: out = w.setup_phase ? 1.0 : 0.0; return true;
            case Leaf::IsStoryCampaign: out = w.story_campaign ? 1.0 : 0.0; return true;
            case Leaf::ForceUnnormalized:
                out = static_cast<double>(player_force(-1, true, category(ps), attenuator(ps), false));
                return true;
            case Leaf::ForceVisibility: {
                if (!w.is_player(ctx.self)) return false;
                const uint64_t cat = category(ps);
                float total = 0.0f, visible = 0.0f;
                const auto n = static_cast<int32_t>(w.players.size());
                for (int32_t q = 0; q < n; ++q) {
                    if (q == ctx.self) continue;
                    for (const int32_t id : p.grid().player_objects(q)) {
                        const PerceptionObject* o = w.object(id);
                        if (!o) continue;
                        const PerceptionType* t = w.type_of(*o);
                        if (!t || (t->category & cat) == 0) continue;
                        total += 1.0f;
                        if (ctx.self < 64 && ((o->visible_to >> ctx.self) & 1u)) visible += 1.0f;
                    }
                }
                out = static_cast<double>(visible / total);  // 0/0 -> NaN (vanilla)
                return true;
            }
            case Leaf::Timeline: case Leaf::InterventionsEnabled: case Leaf::TimeSinceStoryPopup: case Leaf::ActiveStoryGoalCount:
                out = 0.0;  // [L] story / galactic
                return true;
            default: return false;
        }
    }
};

uint64_t default_category_mask(std::string_view name) {
    static const objects::EnumRegistry reg;
    const auto v = reg.get(objects::EnumId::GameObjectCategory).lookup(name);
    return v ? static_cast<uint64_t>(*v) : 0;
}

}  // namespace

// ---------------------------------------------------------------------------------- resolver
bool TacticalTokenResolver::resolve(const EvalContext& ctx, const TokenRef& token, double& out) const {
    const Eval e{p_, w_, ctx, token.tokens, p_.targets_, p_.zone_cells_};
    return e.resolve(token.root, token.path, token.params, out);
}

bool TacticalTokenResolver::resolve_script(const EvalContext& ctx, std::string_view name, std::span<const Param> params,
                                           double& out) const {
    return p_.script_ && p_.script_(ctx, name, params, out);
}

bool TacticalTokenResolver::implements(Root root, std::span<const TokenId> path, const TokenTable& tokens) const {
    if (path.empty()) return false;
    std::vector<Ev> kinds;
    switch (root) {
        case Root::Game: kinds = {Ev::Game}; break;
        case Root::Self: case Root::Enemy: case Root::Human: kinds = {Ev::Player}; break;
        case Root::Target: case Root::Location: kinds = {Ev::Unit, Ev::Zone, Ev::Cell}; break;
    }
    for (size_t i = 0; i + 1 < path.size(); ++i) {
        const Leaf sub = leaf_of(path[i], &tokens);
        std::vector<Ev> next;
        for (const Ev k : kinds) {
            if (sub == Leaf::Location && k == Ev::Unit) next.push_back(Ev::UnitLocation);
            if (sub == Leaf::Owner && k == Ev::Unit) next.push_back(Ev::Player);
            if (sub == Leaf::Type && k == Ev::Unit) next.push_back(Ev::Type);
            if (sub == Leaf::Hints && (k == Ev::Zone || k == Ev::UnitLocation)) next.push_back(Ev::Hints);
        }
        kinds = std::move(next);
        if (kinds.empty()) return false;
    }
    const Leaf l = leaf_of(path.back(), &tokens);
    for (const Ev k : kinds) {
        if (k == Ev::Hints) return true;
        int32_t life = 0;
        if (leaf_info(k, l, TacticalMode::Land, life) || leaf_info(k, l, TacticalMode::Space, life)) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------- perception
TacticalPerception::TacticalPerception() { cfg_.category_mask = default_category_mask; }

void TacticalPerception::init(TacticalMode mode, const TacticalAiConstants& c, const Rect2& map, uint32_t players,
                              std::span<const HintZone> zones, const TacticalPerceptionConfig& cfg) {
    mode_ = mode;
    c_ = c;
    cfg_ = cfg;
    if (!cfg_.category_mask) cfg_.category_mask = default_category_mask;
    grid_.init(mode, map, players, threat_grid_config(mode, c));
    zones_.assign(zones.begin(), zones.end());
    zone_cells_.assign(zones_.size(), {});
    for (size_t i = 0; i < zones_.size(); ++i) grid_.zone_cells(zones_[i], zone_cells_[i]);
    targets_.clear();
    unit_handles_.clear();
    zone_handles_.clear();
    cell_handles_.clear();
    cache_.clear();
}

std::span<const uint32_t> TacticalPerception::zone_threat_cells(int32_t zone) const {
    if (zone < 0 || static_cast<size_t>(zone) >= zone_cells_.size()) return {};
    return zone_cells_[static_cast<size_t>(zone)];
}

int32_t TacticalPerception::handle_of_object(int32_t object_id) const {
    const auto it = std::lower_bound(unit_handles_.begin(), unit_handles_.end(), std::pair<int32_t, int32_t>(object_id, INT32_MIN));
    return (it != unit_handles_.end() && it->first == object_id) ? it->second : -1;
}

int32_t TacticalPerception::register_unit(const PerceptionWorld& w, int32_t object_id) {
    const int32_t existing = handle_of_object(object_id);
    if (existing >= 0) return existing;
    PerceptionTarget t;
    t.kind = PerceptionTarget::Kind::Unit;
    t.object_id = object_id;
    if (const PerceptionObject* o = w.object(object_id)) t.owner_at_registration = w.is_player(o->owner) ? o->owner : -1;
    const auto h = static_cast<int32_t>(targets_.size());
    targets_.push_back(t);
    unit_handles_.insert(std::upper_bound(unit_handles_.begin(), unit_handles_.end(), std::pair<int32_t, int32_t>(object_id, h)),
                         {object_id, h});
    return h;
}

int32_t TacticalPerception::register_zone(int32_t zone) {
    for (const auto& [z, h] : zone_handles_)
        if (z == zone) return h;
    PerceptionTarget t;
    t.kind = PerceptionTarget::Kind::LandZone;
    t.zone = zone;
    if (zone >= 0 && static_cast<size_t>(zone) < zones_.size()) {
        const HintZone& hz = zones_[static_cast<size_t>(zone)];
        t.center = {hz.center[0], hz.center[1], hz.center[2]};
    }
    const auto h = static_cast<int32_t>(targets_.size());
    targets_.push_back(t);
    zone_handles_.emplace_back(zone, h);
    return h;
}

int32_t TacticalPerception::register_cell(int32_t x, int32_t y, const Rect2& rect) {
    const int64_t key = (static_cast<int64_t>(x) << 32) | static_cast<int64_t>(static_cast<uint32_t>(y));
    for (const auto& [k, h] : cell_handles_)
        if (k == key) return h;
    PerceptionTarget t;
    t.kind = PerceptionTarget::Kind::SpaceCell;
    t.cell_x = x;
    t.cell_y = y;
    t.rect = rect;
    t.center = {(rect.min_x + rect.max_x) * 0.5f, (rect.min_y + rect.max_y) * 0.5f, 0.0f};
    const auto h = static_cast<int32_t>(targets_.size());
    targets_.push_back(t);
    cell_handles_.emplace_back(key, h);
    return h;
}

void TacticalPerception::register_targets(const PerceptionWorld& w, const TacticalTargetList& list, std::vector<int32_t>* handles) {
    if (handles) handles->clear();
    for (const TacticalTarget& t : list.targets()) {
        int32_t h = -1;
        if (t.is_unit()) h = register_unit(w, t.object_id);
        else if (t.zone >= 0) h = register_zone(t.zone);
        else h = register_cell(t.cell_x, t.cell_y, t.eval_rect);
        if (handles) handles->push_back(h);
    }
}

int32_t TacticalPerception::handle_for(const TacticalTarget& t) const {
    if (t.is_unit()) return handle_of_object(t.object_id);
    if (t.zone >= 0) {
        for (const auto& [z, h] : zone_handles_)
            if (z == t.zone) return h;
        return -1;
    }
    const int64_t key = (static_cast<int64_t>(t.cell_x) << 32) | static_cast<int64_t>(static_cast<uint32_t>(t.cell_y));
    for (const auto& [k, h] : cell_handles_)
        if (k == key) return h;
    return -1;
}

void TacticalPerception::on_object_destroyed(int32_t object_id) {
    const int32_t h = handle_of_object(object_id);
    if (h >= 0) targets_[static_cast<size_t>(h)].released = true;
}

EvalResult TacticalPerception::evaluate(const EquationSet& set, uint32_t equation, const EvalContext& ctx,
                                        const PerceptionWorld& w) const {
    const TacticalTokenResolver r(*this, w);
    return ai::evaluate(set, equation, ctx, r);
}

EvalResult TacticalPerception::evaluate(const EquationSet& set, const Expression& expr, const EvalContext& ctx,
                                        const PerceptionWorld& w) const {
    const TacticalTokenResolver r(*this, w);
    return ai::evaluate(set, expr, ctx, r);
}

ZoneThreatFn TacticalPerception::zone_threat_fn(const PerceptionWorld& w, int32_t player) const {
    return [this, &w, player](uint32_t zone) {
        return static_cast<double>(grid_.cells_danger(w, player, zone_threat_cells(static_cast<int32_t>(zone))));
    };
}

void TacticalPerception::hash(StateHasher& h) const {
    grid_.hash(h);
    h.u32(static_cast<uint32_t>(targets_.size()));
    for (const PerceptionTarget& t : targets_) {
        h.u8(static_cast<uint8_t>(t.kind));
        h.i32(t.object_id);
        h.i32(t.owner_at_registration);
        h.boolean(t.released);
        h.i32(t.zone);
        h.i32(t.cell_x);
        h.i32(t.cell_y);
    }
    cache_.hash(h);
}

}  // namespace alamo::ai::tactical
