#pragma once

#include <string>

namespace irx::gameplay {

enum class BombPhase { Carried, Dropped, Planting, Planted, Defusing, Defused, Exploded };

struct BombSystem {
    BombPhase phase = BombPhase::Carried;
    int carrierId = 0;
    int planterId = 0;
    int defuserId = 0;
    std::string site;
    float timer = 0.0f;
    float actionProgress = 0.0f;

    void reset(int carrier);
    bool beginPlant(int playerId, std::string targetSite);
    bool beginDefuse(int playerId);
    void cancelAction();
    void update(float dt, bool hasDefuseKit = false);
    bool terminal() const noexcept;
};

}