#include "alamo/game/battle_orders.hpp"

#include "alamo/game/game_session.hpp"
#include "alamo/sim/tactical_battle.hpp"
#include "alamo/tactical/movement/movement_system.hpp"

namespace alamo::game {

namespace cmd = alamo::commands;

namespace {
bool finite(float f) { return f - f == 0.0f; }

// Wire ids -> object ids (kNoObject / out-of-range ids dropped). TacticalBattle::order_* filters ownership.
std::vector<objects::ObjectId> ids_of(const std::vector<uint32_t>& wire) {
    std::vector<objects::ObjectId> out;
    out.reserve(wire.size());
    for (const uint32_t w : wire)
        if (w != cmd::kNoObject && w <= 0x7FFFFFFFu) out.push_back(static_cast<objects::ObjectId>(w));
    return out;
}

objects::ObjectId target_id(uint32_t w) {
    return (w == cmd::kNoObject || w > 0x7FFFFFFFu) ? objects::kNoObjectId : static_cast<objects::ObjectId>(w);
}
}  // namespace

// Integration fixes: routed through TacticalBattle's human order API (order_move / order_attack /
// order_move_to_object / order_stop); the former const_cast stopgap on movement() / combat() is gone.
cmd::RejectReason route_battle_order(sim::CampaignBattles& battles, sim::SimHost& host, uint8_t player, const cmd::EventData& ev,
                                     const std::vector<objects::ObjectId>& selection, BattleOrderStats* stats) {
    using R = cmd::RejectReason;
    using T = cmd::EventType;
    sim::TacticalBattle* battle = battles.battle();
    sim::TacticalMode* tm = host.tactical();
    if (!battle || !tm || battle->finished()) return R::Unhandled;
    SimRng& rng = host.random().synced();
    auto result = [&](R r) {
        if (stats) {
            if (r == R::None) ++stats->routed;
            else ++stats->rejected;
        }
        return r;
    };

    // P1_DOUBLE_CLICK_MOVE.md §3.2: the move events' 8-bit flags go unchanged into MoveOrder::flags; attack-move (orders
    // kept) = flag 0x01. Units: the event's list when it has one (MoveToPositionFacing / Facing), else the selection.
    const auto move = [&](const std::vector<objects::ObjectId>& list, float x, float y, float facing, bool switch_locomotor) {
        if (!finite(x) || !finite(y)) return result(R::InvalidPosition);
        const uint8_t flags = static_cast<uint8_t>(ev.u("flags") & 0xFFu);
        const bool attack_move = (flags & tactical::movement::kMoveFlagAttackMove) != 0;
        const R r = battle->order_move(*tm, player, list, x, y, rng, finite(facing) ? facing : tactical::movement::kNoFacing,
                                       attack_move, ev.queued(), flags, switch_locomotor);
        if (r == R::None && stats) ++stats->moves;
        return result(r);
    };
    const auto units_or_selection = [&] { return ev.ids("units").empty() ? selection : ids_of(ev.ids("units")); };
    // MoveToRay / MoveToRayFacing: a = camera ray origin, b = that ray's intersection with the move plane (space: z = 0,
    // done by the UI, spec §2.2); the destination is b as sent (its quantised z is not re-intersected: a z off by one
    // quantisation step would move the point along the ray).
    const auto ray_point = [&](float& x, float& y) {
        x = ev.f("bx");
        y = ev.f("by");
    };
    switch (ev.type()) {
        case T::MoveToPosition:  // S7 t10: the 1-bit swaps Uses_Multiple_Locomotors units first (MOVEMENT §6.2)
            return move(selection, ev.f("x"), ev.f("y"), tactical::movement::kNoFacing, ev.u("switch_locomotor") != 0);
        case T::MoveToPositionFacing:  // id 47: point + final facing (angle in degrees), units list (empty: the selection)
            return move(units_or_selection(), ev.f("x"), ev.f("y"), ev.f("angle"), false);
        case T::MoveToRay: {  // id 14
            float x = 0.0f, y = 0.0f;
            ray_point(x, y);
            return move(selection, x, y, tactical::movement::kNoFacing, false);
        }
        case T::MoveToRayFacing: {  // id 19: the anchor's ray + plane point, final facing
            float x = 0.0f, y = 0.0f;
            ray_point(x, y);
            return move(selection, x, y, ev.f("angle"), false);
        }
        case T::Facing: {  // id 20: rotate only (a land drag that started on a selected unit)
            const R r = battle->order_facing(*tm, player, units_or_selection(), ev.f("angle"), rng);
            if (r == R::None && stats) ++stats->moves;
            return result(r);
        }
        case T::MoveToObject:
        case T::Escort: {
            const std::vector<objects::ObjectId> list =
                (ev.type() == T::MoveToObject && !ev.ids("movers").empty()) ? ids_of(ev.ids("movers")) : selection;
            const R r = battle->order_move_to_object(*tm, player, list, target_id(ev.u("target")), rng, false, ev.queued());
            if (r == R::None && stats) ++stats->moves;
            return result(r);
        }
        case T::Attack: {
            const std::vector<objects::ObjectId> list = ev.ids("attackers").empty() ? selection : ids_of(ev.ids("attackers"));
            // Attack ref2 = 1 + HardPointType (COMMANDS.md 2): the target's first targetable hard point of that type.
            const objects::ObjectId target = target_id(ev.u("target"));
            const uint32_t ref2 = ev.u("ref2");
            const int32_t hp = ref2 > 0 ? battle->first_hardpoint_of_type(target, static_cast<int32_t>(ref2) - 1) : -1;
            // flag0 ("double", movement byte +0x5d: attackers close at their own speed in formation type 2) and flag1 (Ctrl:
            // the approach move's flags = 0x01) have no consumer in movement / combat yet [port]: not carried.
            const R r = battle->order_attack(*tm, player, list, target, rng, hp, ev.queued());
            if (r == R::None && stats) ++stats->attacks;
            return result(r);
        }
        case T::StopMovement: {
            const R r = battle->order_stop(*tm, player, selection);
            if (r == R::None && stats) ++stats->stops;
            return result(r);
        }
        case T::Reinforce: {  // the reinforcement pad + drop point (TacticalHud BeginReinforcement -> reinforce_event)
            const uint32_t key = ev.u("unit");
            if (key == cmd::kNoObject || key > 0x7FFFFFFFu) return result(R::InvalidUnit);
            const R r = battle->order_reinforce(*tm, player, static_cast<objects::ObjectId>(key), ev.f("x"), ev.f("y"), rng);
            if (r == R::None && stats) ++stats->reinforcements;
            return result(r);
        }
        case T::Retreat:  // the retreat dialog's confirm (GameScreens): the retreat coordinator (BT 3.4)
            return result(ev.u("request") != 0 && battle->request_retreat(static_cast<int32_t>(player)) ? R::None : R::NotAllowed);
        default: return R::Unhandled;
    }
}

}  // namespace alamo::game
