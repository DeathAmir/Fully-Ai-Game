#include "gameplay/bomb_system.hpp"

#include <algorithm>

#include "gameplay/match_rules.hpp"

namespace irx::gameplay {

void BombSystem::reset(int carrier) {
    phase = BombPhase::Carried;
    carrierId = carrier;
    planterId = 0;
    defuserId = 0;
    site.clear();
    timer = 0.0f;
    actionProgress = 0.0f;
}

bool BombSystem::beginPlant(int playerId, std::string targetSite) {
    if (phase != BombPhase::Carried || playerId != carrierId) return false;
    if (targetSite != "A" && targetSite != "B") return false;
    phase = BombPhase::Planting;
    planterId = playerId;
    site = std::move(targetSite);
    actionProgress = 0.0f;
    return true;
}

bool BombSystem::beginDefuse(int playerId) {
    if (phase != BombPhase::Planted) return false;
    phase = BombPhase::Defusing;
    defuserId = playerId;
    actionProgress = 0.0f;
    return true;
}

void BombSystem::cancelAction() {
    if (phase == BombPhase::Planting) {
        phase = BombPhase::Carried;
        planterId = 0;
        actionProgress = 0.0f;
    } else if (phase == BombPhase::Defusing) {
        phase = BombPhase::Planted;
        defuserId = 0;
        actionProgress = 0.0f;
    }
}

void BombSystem::update(float dt, bool hasDefuseKit) {
    const auto& rules = competitiveRules();
    dt = std::max(0.0f, std::min(dt, 0.25f));
    if (phase == BombPhase::Planting) {
        actionProgress += dt;
        if (actionProgress >= rules.plantSeconds) {
            phase = BombPhase::Planted;
            carrierId = 0;
            timer = rules.bombSeconds;
            actionProgress = 0.0f;
        }
        return;
    }
    if (phase == BombPhase::Planted || phase == BombPhase::Defusing) {
        timer -= dt;
        if (timer <= 0.0f) {
            timer = 0.0f;
            phase = BombPhase::Exploded;
            return;
        }
    }
    if (phase == BombPhase::Defusing) {
        actionProgress += dt;
        const float required = hasDefuseKit ? rules.defuseKitSeconds : rules.defuseSeconds;
        if (actionProgress >= required) {
            phase = BombPhase::Defused;
            actionProgress = required;
        }
    }
}

bool BombSystem::terminal() const noexcept {
    return phase == BombPhase::Defused || phase == BombPhase::Exploded;
}

}