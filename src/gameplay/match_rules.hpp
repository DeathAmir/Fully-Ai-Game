#pragma once

namespace irx::gameplay {

enum class Team { Spectator, Terrorist, Counter };
enum class RoundPhase { Warmup, Freeze, Live, Ended };

struct MatchRules {
    int maxRounds = 24;
    int roundsToWin = 13;
    float freezeSeconds = 5.0f;
    float roundSeconds = 115.0f;
    float bombSeconds = 40.0f;
    float plantSeconds = 3.2f;
    float defuseSeconds = 10.0f;
    float defuseKitSeconds = 5.0f;
    float buySeconds = 25.0f;
    int startMoney = 800;
    int maxMoney = 16000;
};

const MatchRules& competitiveRules();

}