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

, busybox-sandbox-shell ? null

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

  # Hack for sake of the dev shell
  passthru.baseNativeBuildInputs = [
    meson
    ninja
    pkg-config
    rsync

    jq
    git
    mercurial
  ] ++ lib.optionals stdenv.hostPlatform.isLinux [
    busybox-sandbox-shell
  ];

  nativeBuildInputs = finalAttrs.passthru.baseNativeBuildInputs ++ [
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
