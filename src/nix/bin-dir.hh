#pragma once
///@file

#include <filesystem>

namespace nix {

/**
 * Get a path to the given Nix binary
 */
std::filesystem::path getNixBin(std::string_view binary_name);

/**
 * @TODO update docs to match status quo, where NIX_BIN_DIR is not defined for libraries
 *
 * Set the build hook location
 *
 * For builds we perform a self-invocation, so Nix has to be self-aware.
 * That is, it has to know where it is installed. We don't think it's sentient.
 *
 * Normally, nix is installed according to `nixBinDir`, which is set at compile time,
 * but can be overridden. This makes for a great default that works even if this
 * code is linked as a library into some other program whose main is not aware
 * that it might need to be a build remote hook.
 *
 * However, it may not have been installed at all. For example, if it's a static build,
 * there's a good chance that it has been moved out of its installation directory.
 * That makes `nixBinDir` useless. Instead, we'll query the OS for the path to the
 * current executable, using `getSelfExe()`.
 *
 * As a last resort, we resort to `PATH`. Hopefully we find a `nix` there that's compatible.
 * If you're porting Nix to a new platform, that might be good enough for a while, but
 * you'll want to improve `getSelfExe()` to work on your platform.
 */
void setNixBuildRemoteLocation();

}
