#include "current-process.hh"
#include "file-system.hh"
#include "globals.hh"
#include "bin-dir.hh"

namespace nix {

const Path & getNixBinDir()
{
    static const Path nixBinDir = canonPath(getEnvNonEmpty("NIX_BIN_DIR").value_or(NIX_BIN_DIR));

    return nixBinDir;
}

void setNixBuildRemoteLocation()
{
    std::string nixExePath = getNixBinDir() + "/nix";
    if (!pathExists(nixExePath)) {
        nixExePath = getSelfExe().value_or("nix");
    }
    settings.buildHook = {
        nixExePath,
        "__build-remote",
    };
}


}
