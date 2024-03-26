#include "experimental-features.hh"
#include "fmt.hh"
#include "util.hh"

#include "nlohmann/json.hpp"

namespace nix {

struct ExperimentalFeatureDetails
{
    ExperimentalFeature tag;
    std::string_view name;
    std::string_view description;
    std::string_view trackingUrl;
};

/**
 * If two different PRs both add an experimental feature, and we just
 * used a number for this, we *woudln't* get merge conflict and the
 * counter will be incremented once instead of twice, causing a build
 * failure.
 *
 * By instead defining this instead as 1 + the bottom experimental
 * feature, we either have no issue at all if few features are not added
 * at the end of the list, or a proper merge conflict if they are.
 */
constexpr size_t numXpFeatures = 1 + static_cast<size_t>(Xp::VerifiedFetches);

constexpr std::array<ExperimentalFeatureDetails, numXpFeatures> xpFeatureDetails = {{
    {
        .tag = Xp::CaDerivations,
        .name = "ca-derivations",
        .description = R"(
            Allow derivations to be content-addressed in order to prevent
            rebuilds when changes to the derivation do not result in changes to
            the derivation's output. See
            [__contentAddressed](@docroot@/language/advanced-attributes.md#adv-attr-__contentAddressed)
            for details.
        )",
        .trackingUrl = "https://github.com/NixOS/nix/milestone/35",
    },
    {
        .tag = Xp::ImpureDerivations,
        .name = "impure-derivations",
        .description = R"(
            Allow derivations to produce non-fixed outputs by setting the
            `__impure` derivation attribute to `true`. An impure derivation can
            have differing outputs each time it is built.

            Example:

            ```
            derivation {
              name = "impure";
              builder = /bin/sh;
              __impure = true; # mark this derivation as impure
              args = [ "-c" "read -n 10 random < /dev/random; echo $random > $out" ];
              system = builtins.currentSystem;
            }
            ```

            Each time this derivation is built, it can produce a different
            output (as the builder outputs random bytes to `$out`).  Impure
            derivations also have access to the network, and only fixed-output
            or other impure derivations can rely on impure derivations. Finally,
            an impure derivation cannot also be
            [content-addressed](#xp-feature-ca-derivations).

            This is a more explicit alternative to using [`builtins.currentTime`](@docroot@/language/builtin-constants.md#builtins-currentTime).
        )",
        .trackingUrl = "https://github.com/NixOS/nix/milestone/42",
    },
    {
        .tag = Xp::Flakes,
        .name = "flakes",
        .description = R"(
            Enable flakes. See the manual entry for [`nix
            flake`](@docroot@/command-ref/new-cli/nix3-flake.md) for details.
        )",
        .trackingUrl = "https://github.com/NixOS/nix/milestone/27",
    },
    {
        .tag = Xp::FetchTree,
        .name = "fetch-tree",
        .description = R"(
            Enable the use of the [`fetchTree`](@docroot@/language/builtins.md#builtins-fetchTree) built-in function in the Nix language.

            `fetchTree` exposes a generic interface for fetching remote file system trees from different types of remote sources.
            The [`flakes`](#xp-feature-flakes) feature flag always enables `fetch-tree`.
            This built-in was previously guarded by the `flakes` experimental feature because of that overlap.

            Enabling just this feature serves as a "release candidate", allowing users to try it out in isolation.
        )",
        .trackingUrl = "https://github.com/NixOS/nix/milestone/31",
    },
    {
        .tag = Xp::NixCommand,
        .name = "nix-command",
        .description = R"(
            Enable the new `nix` subcommands. See the manual on
            [`nix`](@docroot@/command-ref/new-cli/nix.md) for details.
        )",
        .trackingUrl = "https://github.com/NixOS/nix/milestone/28",
    },
    {
        .tag = Xp::GitHashing,
        .name = "git-hashing",
        .description = R"(
            Allow creating (content-addressed) store objects which are hashed via Git's hashing algorithm.
            These store objects will not be understandable by older versions of Nix.
        )",
        .trackingUrl = "https://github.com/NixOS/nix/milestone/41",
    },
    {
        .tag = Xp::RecursiveNix,
        .name = "recursive-nix",
        .description = R"(
            Allow derivation builders to call Nix, and thus build derivations
            recursively.

            Example:

            ```
            with import <nixpkgs> {};

            runCommand "foo"
              {
                 buildInputs = [ nix jq ];
                 NIX_PATH = "nixpkgs=${<nixpkgs>}";
              }
              ''
                hello=$(nix-build -E '(import <nixpkgs> {}).hello.overrideDerivation (args: { name = "recursive-hello"; })')

                mkdir -p $out/bin
                ln -s $hello/bin/hello $out/bin/hello
              ''
            ```

            An important restriction on recursive builders is disallowing
            arbitrary substitutions. For example, running

            ```
            nix-store -r /nix/store/kmwd1hq55akdb9sc7l3finr175dajlby-hello-2.10
            ```

            in the above `runCommand` script would be disallowed, as this could
            lead to derivations with hidden dependencies or breaking
            reproducibility by relying on the current state of the Nix store. An
            exception would be if
            `/nix/store/kmwd1hq55akdb9sc7l3finr175dajlby-hello-2.10` were
            already in the build inputs or built by a previous recursive Nix
            call.
        )",
        .trackingUrl = "https://github.com/NixOS/nix/milestone/47",
    },
    {
        .tag = Xp::NoUrlLiterals,
        .name = "no-url-literals",
        .description = R"(
            Disallow unquoted URLs as part of the Nix language syntax. The Nix
            language allows for URL literals, like so:

            ```
            $ nix repl
            Welcome to Nix 2.15.0. Type :? for help.

            nix-repl> http://foo
            "http://foo"
            ```

            But enabling this experimental feature will cause the Nix parser to
            throw an error when encountering a URL literal:

            ```
            $ nix repl --extra-experimental-features 'no-url-literals'
            Welcome to Nix 2.15.0. Type :? for help.

            nix-repl> http://foo
            error: URL literals are disabled

            at «string»:1:1:

            1| http://foo
             | ^

            ```

            While this is currently an experimental feature, unquoted URLs are
            being deprecated and their usage is discouraged.

            The reason is that, as opposed to path literals, URLs have no
            special properties that distinguish them from regular strings, URLs
            containing parameters have to be quoted anyway, and unquoted URLs
            may confuse external tooling.
        )",
        .trackingUrl = "https://github.com/NixOS/nix/milestone/44",
    },
    {
        .tag = Xp::FetchClosure,
        .name = "fetch-closure",
        .description = R"(
            Enable the use of the [`fetchClosure`](@docroot@/language/builtins.md#builtins-fetchClosure) built-in function in the Nix language.
        )",
        .trackingUrl = "https://github.com/NixOS/nix/milestone/40",
    },
    {
        .tag = Xp::ReplFlake,
        .name = "repl-flake",
        .description = R"(
            *Enabled with [`flakes`](#xp-feature-flakes) since 2.19*

            Allow passing [installables](@docroot@/command-ref/new-cli/nix.md#installables) to `nix repl`, making its interface consistent with the other experimental commands.
        )",
        .trackingUrl = "https://github.com/NixOS/nix/milestone/32",
    },
    {
        .tag = Xp::AutoAllocateUids,
        .name = "auto-allocate-uids",
        .description = R"(
            Allows Nix to automatically pick UIDs for builds, rather than creating
            `nixbld*` user accounts. See the [`auto-allocate-uids`](@docroot@/command-ref/conf-file.md#conf-auto-allocate-uids) setting for details.
        )",
        .trackingUrl = "https://github.com/NixOS/nix/milestone/34",
    },
    {
        .tag = Xp::Cgroups,
        .name = "cgroups",
        .description = R"(
            Allows Nix to execute builds inside cgroups. See
            the [`use-cgroups`](@docroot@/command-ref/conf-file.md#conf-use-cgroups) setting for details.
        )",
        .trackingUrl = "https://github.com/NixOS/nix/milestone/36",
    },
    {
        .tag = Xp::DaemonTrustOverride,
        .name = "daemon-trust-override",
        .description = R"(
            Allow forcing trusting or not trusting clients with
            `nix-daemon`. This is useful for testing, but possibly also
            useful for various experiments with `nix-daemon --stdio`
            networking.
        )",
        .trackingUrl = "https://github.com/NixOS/nix/milestone/38",
    },
    {
        .tag = Xp::DynamicDerivations,
        .name = "dynamic-derivations",
        .description = R"(
            Allow the use of a few things related to dynamic derivations:

              - "text hashing" derivation outputs, so we can build .drv
                files.

              - dependencies in derivations on the outputs of
                derivations that are themselves derivations outputs.
        )",
        .trackingUrl = "https://github.com/NixOS/nix/milestone/39",
    },
    {
        .tag = Xp::WasmDerivations,
        .name = "wasm-derivations",
        .description = R"(
            Enables building derivations defined via WASM modules.
            Such derivations have the system set to `wasm`.

            NB: WASM derivations can only be content addressed,
            hence wasm-derivations requires ca-derivations.

            Such derivations are similar to ordinary derivations but different
            in important ways. They do not make use of OS-level sandboxing features,
            nor are the builders given a POSIX-y environment, nor are they subject
            to the restrictions that come with that.

            The goal of WASM derivations is to have **completely pure** derivations,
            such that derivations always result in the same outputs,
            regardless of your system, and of course, be sandboxed such
            that building a derivation is always safe.

            The combination of Nix and WASM provides unique opportunities for rethinking
            the host/derivation boundary, notably, we provide recursive-nix-esque functionalities
            as part of the core interface. In fact, it becomes necessary for doing anything useful
            with WASM derivations.

            WASM derivations specify a path to a builder that must be a WASM module
            (either in binary format or textual format).

            At build-time, the module is loaded, the imports are supplied, the module instantiated,
            the exports of the instantiation read, and the exported function
            (which would be of type `IO ()` in Haskell) is run.

            Any failures in running the WASM module will make the derivation fail.
            Any failures from using the imports incorrectly will also make the derivation fail,
            that is to say, you can not handle errors.

            Paths do not begin with `/nix/store/`, since the only files you can open
            are all in the store, hence use the paths you'd usually use but strip `/nix/store/`.
            Neither do they begin with `/`, since relative paths do not exist.
            `..`, `.`, are invalid too.

            Until type imports are more stable,
            the interface will make use of the unityped `externref`,
            hence all types that go across the border form a sum type in essence.

            Passing a value of the wrong variant/constructor will as already
            outlined above make the derivation fail irrecoverably.

            Files in the interface are heavily simplified; all unnecessary features are
            removed.
            They are essentially byte vectors, and nothing more than that.

            Temporary files may exist only in memory, or may be
            represented as a true file by the implementation,
            of no consequence to the derivation builder.

            The following imports are available:
            ```c
            // Log message.
            void log(const char* msg, i64 msg_len);
            
            // Create a file descriptor that represents a temporary "file"
            // of the specified size. The file is initially filled with zeros.
            fd mktemp(i64 size);

            // Open file in store.
            // The store path must either be a dependency or have been made by
            // this derivation.
            fd open(const char* storepath, i64 storepath_len);

            // Close file. If it's a temporary file, it will be deleted.
            void close(fd);

            // Read from a file from the specified index `count` bytes to `out`.
            void read(fd, char* out, i64 count, i64 idx);

            // Get the size of the file.
            i64 size(fd);

            // Resize a temporary file.
            // New bytes are zero.
            void resize(fd, i64);

            // Write to the file at the specified index the data given.
            // If the offset is out of bounds, it fails.
            // However, if the offset is within the file,
            // then any bytes written outside the bounds will extend the file size.
            void write(fd, const char* data, i64 data_len, i64 offset); 

            // Add to the store. This is a thin wrapper around `Store::addToStoreFromDump`.
            Sink add(const char* name, i64 name_len, bool is_nar, int hashMethod, int hashAlgo);

            // Write to the sink. This always writes the full data, blocking until it's done.
            void sink_write(Sink, const char* data, i64 data_len);

            // Write to the sink from a file descriptor. This always writes the full data, blocking until it's done.
            void sink_write_fd(Sink, fd, i64 offset, i64 count);

            // FIXME: remove
            StorePath add_dumb(const char* name, size_t name_len, DumpMethod, HashMethod, HashAlgo, fd);

            // Mark a store path as an output.
            // The store path must either be a dependency or made by this derivation.
            void output(const char* name, i64 name_len, StorePath);

            // Ends the derivation prematurely
            void exit();

            // Fails the derivation
            void fail();

            // Build a derivation at the specified path.
            drv build(const char* storepath, i64 storepath_len, const char* subpath, i64 subpath_len);
            ```

            All external types are represented as externrefs currently.
            If you pass a value of the incorrect type, the derivation will fail.
            In the future, the interface will likely use typed references to push
            the responsibility of ensuring the values are of the correct type
            onto the program.

            These are enough to do useful work. Future extensions could expose more functionality,
            e.g. access to GPUs (or rather, a WebGPU API).

            One very big notable limitation is that you _can not_ JIT-compile WASM.
            That would necessitate embedding something like the WASM C API in this interface,
            which would be quite complex.

            This means you can e.g. not open an "executable" file (WASM module) and execute it efficiently.
            You can of course interpret it, but really, you should be making a derivation for it
            and executing the derivation instead, leveraging Nix's caching functionality.
            What if the program does not use the above interface? You modify the module to
            use the interface, and shim its existing imports in a way that uses the provided imports.

            This naturally creates a need for dynamic derivations, derivations that build derivations.
            You are thus expected to use `wasm-derivations` with `dynamic-derivations` enabled.
            `recursive-nix`, however, does _not_ apply, since it runs a Nix daemon "recursively"
            in the builder environment. That doesn't make sense in this case.

            Another solution is to semi-JIT-compile things by essentially creating a continuation
            consisting of the original WASM module patched with new code, making a derivation of that,
            and then outputting the results of that derivation.

            FIXME: expose mmap somehow
        )",
    },
    {
        .tag = Xp::ParseTomlTimestamps,
        .name = "parse-toml-timestamps",
        .description = R"(
            Allow parsing of timestamps in builtins.fromTOML.
        )",
        .trackingUrl = "https://github.com/NixOS/nix/milestone/45",
    },
    {
        .tag = Xp::ReadOnlyLocalStore,
        .name = "read-only-local-store",
        .description = R"(
            Allow the use of the `read-only` parameter in [local store](@docroot@/store/types/local-store.md) URIs.
        )",
        .trackingUrl = "https://github.com/NixOS/nix/milestone/46",
    },
    {
        .tag = Xp::ConfigurableImpureEnv,
        .name = "configurable-impure-env",
        .description = R"(
            Allow the use of the [impure-env](@docroot@/command-ref/conf-file.md#conf-impure-env) setting.
        )",
        .trackingUrl = "https://github.com/NixOS/nix/milestone/37",
    },
    {
        .tag = Xp::MountedSSHStore,
        .name = "mounted-ssh-store",
        .description = R"(
            Allow the use of the [`mounted SSH store`](@docroot@/command-ref/new-cli/nix3-help-stores.html#experimental-ssh-store-with-filesytem-mounted).
        )",
        .trackingUrl = "https://github.com/NixOS/nix/milestone/43",
    },
    {
        .tag = Xp::VerifiedFetches,
        .name = "verified-fetches",
        .description = R"(
            Enables verification of git commit signatures through the [`fetchGit`](@docroot@/language/builtins.md#builtins-fetchGit) built-in.
        )",
        .trackingUrl = "https://github.com/NixOS/nix/milestone/48",
    },
}};

static_assert(
    []() constexpr {
        for (auto [index, feature] : enumerate(xpFeatureDetails))
            if (index != (size_t)feature.tag)
                return false;
        return true;
    }(),
    "array order does not match enum tag order");

const std::optional<ExperimentalFeature> parseExperimentalFeature(const std::string_view & name)
{
    using ReverseXpMap = std::map<std::string_view, ExperimentalFeature>;

    static std::unique_ptr<ReverseXpMap> reverseXpMap = []() {
        auto reverseXpMap = std::make_unique<ReverseXpMap>();
        for (auto & xpFeature : xpFeatureDetails)
            (*reverseXpMap)[xpFeature.name] = xpFeature.tag;
        return reverseXpMap;
    }();

    if (auto feature = get(*reverseXpMap, name))
        return *feature;
    else
        return std::nullopt;
}

std::string_view showExperimentalFeature(const ExperimentalFeature tag)
{
    assert((size_t)tag < xpFeatureDetails.size());
    return xpFeatureDetails[(size_t)tag].name;
}

nlohmann::json documentExperimentalFeatures()
{
    StringMap res;
    for (auto & xpFeature : xpFeatureDetails) {
        std::stringstream docOss;
        docOss << stripIndentation(xpFeature.description);
        docOss << fmt("\nRefer to [%1% tracking issue](%2%) for feature tracking.", xpFeature.name, xpFeature.trackingUrl);
        res[std::string{xpFeature.name}] = trim(docOss.str());
    }
    return (nlohmann::json) res;
}

std::set<ExperimentalFeature> parseFeatures(const std::set<std::string> & rawFeatures)
{
    std::set<ExperimentalFeature> res;
    for (auto & rawFeature : rawFeatures)
        if (auto feature = parseExperimentalFeature(rawFeature))
            res.insert(*feature);
    return res;
}

MissingExperimentalFeature::MissingExperimentalFeature(ExperimentalFeature feature)
    : Error("experimental Nix feature '%1%' is disabled; add '--extra-experimental-features %1%' to enable it", showExperimentalFeature(feature))
    , missingFeature(feature)
{}

std::ostream & operator <<(std::ostream & str, const ExperimentalFeature & feature)
{
    return str << showExperimentalFeature(feature);
}

void to_json(nlohmann::json & j, const ExperimentalFeature & feature)
{
    j = showExperimentalFeature(feature);
}

void from_json(const nlohmann::json & j, ExperimentalFeature & feature)
{
    const std::string input = j;
    const auto parsed = parseExperimentalFeature(input);

    if (parsed.has_value())
        feature = *parsed;
    else
        throw Error("Unknown experimental feature '%s' in JSON input", input);
}

}
