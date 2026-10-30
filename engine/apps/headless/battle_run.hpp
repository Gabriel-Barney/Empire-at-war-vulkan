// alamo_headless --economy --battles MODE (S10): a galactic campaign whose conflicts are auto-resolved or
// fought as tactical battles (engine/apps/sim campaign_battles.hpp), with the [port] CampaignAggressor
// stand-in moving fleets so that conflicts happen.
#pragma once
#include <cstdint>
#include <string>

#include "map_run.hpp"

struct BattleRunOptions {
    std::string mode = "auto";       // auto | fight (human battles fought) | fight-all (AI vs AI fought too)
    uint32_t aggressor_period = 600; // galactic frames between stand-in fleet moves per player (0 = off)
    float retreat_ratio = 0.0f;      // stand-in tactical retreat threshold (0 = never)
    uint32_t max_frames = 27000;     // tactical battle time limit
    bool verbose = true;             // print the conflict log and battle summaries
};

int battle_run(const MapRunOptions& opt, const std::string& data_dir, const BattleRunOptions& bopt);
