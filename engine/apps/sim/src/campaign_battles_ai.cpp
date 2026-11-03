// A1 stage 2 (owner: A1 agent): CampaignBattles' tactical AI hookup (campaign_battles.hpp, battle_ai.hpp).
#include "alamo/sim/campaign_battles.hpp"

#include "alamo/core/profile.hpp"

#ifdef ALAMO_SIM_HAVE_TACTICAL_AI
namespace alamo::sim {

void CampaignBattles::ai_begin(SimHost& host, GameMode& mode) {
    ai_.reset();
    if (!opt_.tactical_ai || !battle_ || !opt_.tactical_ai->defs || !opt_.tactical_ai->equations) return;
    ai_ = std::make_unique<BattleAi>(*battle_, static_cast<TacticalMode&>(mode), host, *opt_.tactical_ai);
    std::vector<std::string> diag;
    if (!ai_->start(&diag)) {
        log_.push_back("tactical AI: no AI participant in " + battle_->request().map);
        ai_.reset();
    }
    for (std::string& d : diag) log_.push_back("tactical AI: " + d);
}

void CampaignBattles::ai_update() {
    ALAMO_PROFILE_SCOPE("tac.ai");  // OPT2 t08: the battle's tactical AI (inside T5.battles)
    if (ai_) ai_->update();
}

void CampaignBattles::ai_end() {
    if (!ai_) return;
    ai_->end();
    ai_log_.push_back(battle_ ? battle_->request().map + ": " + ai_->summary() : ai_->summary());
    for (const std::string& e : ai_->world().lua_errors()) ai_log_.push_back("  lua: " + e);
    for (const ai::tactical::TacticalAiDecision& d : ai_->world().decision_log())
        ai_log_.push_back("  " + std::to_string(d.frame) + " p" + std::to_string(d.player) + " " + d.text);
    for (const auto& [k, n] : ai_->world().stats()) ai_log_.push_back("  stat " + k + " " + std::to_string(n));
    ai_.reset();
}

}  // namespace alamo::sim
#endif
