#include "current-process.hh"
#include "file-system.hh"
#include "globals.hh"
#include "bin-dir.hh"

namespace nix {

namespace fs = std::filesystem;

fs::path getNixBin(std::string_view binary_name)
{
    // If the environment variable is set, use it unconditionally
    if (auto envOpt = getEnvNonEmpty("NIX_BIN_DIR"))
        return fs::path{*envOpt} / std::string{binary_name};

    // Use some-times avaiable OS tricks to get to the path of this Nix, and try that
    if (auto selfOpt = getSelfExe()) {
        auto path = fs::path{*selfOpt}.parent_path() / std::string{binary_name};
        if (fs::exists(path))
            return path;
    }

    // If `nix` exists at the hardcoded fallback path, use it.
    {
        auto path = fs::path{NIX_BIN_DIR} / std::string{binary_name};
        if (fs::exists(path))
            return path;
    }

    // return just the name, hoping the exe is on the `PATH`
    return binary_name;
}

void setNixBuildRemoteLocation()
{
    settings.buildHook = {
        getNixBin("nix"),
        "__build-remote",
    };
}

}
