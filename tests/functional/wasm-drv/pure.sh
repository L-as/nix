export NIX_CONFIG="store = /work/test-nix-store ; experimental-features = nix-command flakes wasm-derivations dynamic-derivations ca-derivations ; wasm-engine = /data/wasmtime-dev/lib/libwasmtime.so"
nix derivation show -f ./pure.nix
nix -L -vvvvv build -f ./pure.nix
