{ lib
, stdenv
, mkMesonDerivation
, releaseTools

, meson
, ninja
, pkg-config
, rsync

, jq
, git
, mercurial

, nix-store
, nix-expr
, nix-ng

, rapidcheck
, gtest
, runCommand

# Configuration Options

, version

# For running the functional tests against a different pre-built Nix.
, test-daemon ? null
}:

let
  inherit (lib) fileset;
in

mkMesonDerivation (finalAttrs: {
  pname = "nix-functional-tests";
  inherit version;

  workDir = ./.;
  fileset = fileset.unions [
    ../../.version
    ../../tests/functional
    ./.
  ];

  nativeBuildInputs = [
    meson
    ninja
    pkg-config
    rsync

    jq
    git
    mercurial

    nix-ng
  ];

  buildInputs = [
    nix-store
    nix-expr
  ];

  preConfigure =
    # "Inline" .version so it's not a symlink, and includes the suffix.
    # Do the meson utils, without modification.
    ''
      chmod u+w ./.version
      echo ${version} > ../../../.version
    '';

  mesonCheckFlags = [
    "--verbose"
  ];

  enableParallelBuilding = true;

  doCheck = true;

  strictDeps = true;

  meta = {
    platforms = lib.platforms.unix;
  };

} // lib.optionalAttrs (test-daemon != null) {
  NIX_DAEMON_PACKAGE = test-daemon;
})
