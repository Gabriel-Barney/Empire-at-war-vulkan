// alamo_headless --economy --galactic-ai (A2): a headless galactic campaign driven by the galactic AI. Every
// playable faction is a galactic AI player (engine/ai galactic_ai.hpp) serviced at T7 through SimHost's
// per-player controllers; its orders go through SimHost::execute_direct; conflicts are auto-resolved (or fought,
// with --battles fight-all) by the S10 CampaignBattles glue. Replaces the S10 [port] CampaignAggressor.
//
// --human FACTION makes that faction's player a human (no AI) whose orders come from a fixed recorded command
// stream (--human-script FILE, see ai_run.cpp for the format), submitted through the scheduled command pipeline
// like a real player's. The Human-rooted perception paths then bind (AI_GALACTIC_2 §4.3), so the conquest
// desires that need a human run.
#pragma once
#include <string>

#include "map_run.hpp"

struct AiRunOptions {
    std::string battles = "auto";  // CampaignBattles mode: auto | fight | fight-all
    std::string ai_log;            // write the AI decision log here
    std::string ai_golden;         // compare the AI decision log with this file ('#' lines ignored)
    std::string human;             // faction played by the scripted human (empty = AI vs AI)
    std::string human_script;      // the human's recorded command stream (empty = the human gives no orders)
    bool verbose = true;
};

int ai_campaign_run(const MapRunOptions& opt, const AiRunOptions& aopt);
