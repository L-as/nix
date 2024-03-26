#pragma once
///@file

#include "builder-interface.hh"

struct DerivationGoal;

namespace nix {
    std::unique_ptr<BuilderInterface> make_wasm_builder(DerivationGoal& goal);
}
