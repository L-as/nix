source ../common.sh

requireDaemonNewerThan "2.16.0pre20230419"

enableFeatures "wasm-derivations ca-derivations dynamic-derivations"

export NIX_CONFIG="wasm-engine = $NIX_WASM_ENGINE"

restartDaemon
