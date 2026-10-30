// P host: human unit orders into the running S10 battle (SimHost::set_order_hook). Runs at the command phase (T2) of
// the tick that drains the order, in drain order, on the synced RNG: deterministic and replay-safe.
//   MoveToPosition            the issuer's selected movable battle units -> MovementSystem::move (destination clamped to
//                             the mode bounds); their attack orders are cleared (as BattleAi's MoveToPositionFacing).
//   MoveToPositionFacing / MoveToRay / MoveToRayFacing   the same move (ray events: destination = the plane point b;
//                             facing events: the final facing). Every move event's 8-bit flags go unchanged into
//                             MoveOrder::flags (P1_DOUBLE_CLICK_MOVE.md §3.2); flag 0x01 (attack-move) keeps attack orders.
//   Facing                    rotate only: TacticalBattle::order_facing.
//   MoveToObject / Escort     move to the target's current position (movers = the event's list, else the selection)
//                             [port: guard = a one-shot move to the guarded object; no follow behaviour yet].
//   Attack                    enemy target only: CombatSystem::order_attack per attacker, plus a move into 70 % of the
//                             attacker's range when it is beyond 80 % (BattleAi's Attack rule).
//   StopMovement              MovementSystem::stop + CombatSystem::clear_order.
// Units that are not live battle units of the issuer are skipped (COMMANDS.md 6.2); nothing ordered -> InvalidUnit.
// Without a running battle: Unhandled (the G0 stub path applies).
//
// The orders go through TacticalBattle::order_move / order_move_to_object / order_attack / order_stop (integration
// fixes; the former const_cast stopgap is gone).
#pragma once
#include <cstdint>
#include <vector>

#include "alamo/commands/command.hpp"
#include "alamo/sim/campaign_battles.hpp"

namespace alamo::game {

struct BattleOrderStats;

commands::RejectReason route_battle_order(sim::CampaignBattles& battles, sim::SimHost& host, uint8_t player,
                                          const commands::EventData& ev, const std::vector<objects::ObjectId>& selection,
                                          BattleOrderStats* stats = nullptr);

}  // namespace alamo::game
