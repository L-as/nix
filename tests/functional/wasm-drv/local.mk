wasm-drv-tests := \
  $(d)/pure.sh

install-tests-groups += wasm-drv

clean-files += \
  $(d)/config.nix

test-deps += \
  tests/functional/dyn-drv/config.nix
