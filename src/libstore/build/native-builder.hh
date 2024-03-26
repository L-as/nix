#pragma once
///@file

#include "builder-interface.hh"
#include "derivation-goal.hh"

namespace nix {
    std::unique_ptr<BuilderInterface> make_native_builder(DerivationGoal& goal);
}
