#pragma once
///@file

#include "builder-interface.hh"
#include "derivation-goal.hh"

namespace nix {
    struct RpDecline {};
    struct RpPostpone {};;
    struct RpAccept { std::unique_ptr<BuilderInterface> builder; };

    std::variant<RpDecline, RpPostpone, RpAccept> make_hook_builder(DerivationGoal& goal);
}
