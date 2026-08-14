#include "gameplay/match_rules.hpp"

namespace irx::gameplay {

const MatchRules& competitiveRules() {
    static const MatchRules rules{};
    return rules;
}

}