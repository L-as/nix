#pragma once
///@file

#include "parsed-derivations.hh"
#include "lock.hh"
#include "outputs-spec.hh"
#include "store-api.hh"
#include "pathlocks.hh"
#include "goal.hh"
#include "builder-interface.hh"

namespace nix {

using std::map;

struct HookInstance;

/**
 * Unless we are repairing, we don't both to test validity and just assume it,
 * so the choices are `Absent` or `Valid`.
 */
enum struct PathStatus {
    Corrupt,
    Absent,
    Valid,
};

struct InitialOutputStatus {
    StorePath path;
    PathStatus status;
    /**
     * Valid in the store, and additionally non-corrupt if we are repairing
     */
    bool isValid() const {
        return status == PathStatus::Valid;
    }
    /**
     * Merely present, allowed to be corrupt
     */
    bool isPresent() const {
        return status == PathStatus::Corrupt
            || status == PathStatus::Valid;
    }
};

struct InitialOutput {
    bool wanted;
    Hash outputHash;
    std::optional<InitialOutputStatus> known;
};

// FIXME: hide all implementation details
/**
 * A goal for building some or all of the outputs of a derivation.
 */
struct DerivationGoal final : public Goal
{
    public:

    /**
     * The sort of derivation we are building.
     */
    std::optional<DerivationType> derivationType;

    /**
     * The derivation stored at drvPath.
     */
    std::unique_ptr<Derivation> drv;

    std::unique_ptr<ParsedDerivation> parsedDrv;

    /**
     * Whether to use an on-disk .drv file.
     */
    bool useDerivation;

    /* Write to log */
    void writeToLog(std::string_view data);

    /* Write to log directly */
    std::shared_ptr<BufferedSink> logSink;

    /** The path of the derivation. */
    StorePath drvPath;

    /**
     * The specific outputs that we need to build.
     * Accessed by worker.
     */
    OutputsSpec wantedOutputs;


    /**
     * Add wanted outputs to an already existing derivation goal.
     * Used by worker.
     */
    void addWantedOutputs(const OutputsSpec & outputs);

    /**
     * Aborts if any output is not valid or corrupt, and otherwise
     * returns a 'SingleDrvOutputs' structure containing all outputs.
     */
    SingleDrvOutputs assertPathValidity();

    /**
     * All input paths (that is, the union of FS closures of the
     * immediate input paths).
     */
    StorePathSet inputPaths;

    /**
     * Activity that denotes waiting for a lock.
     */
    std::unique_ptr<Activity> actLock;

    void done(
        BuildResult::Status status,
        SingleDrvOutputs builtOutputs = {},
        std::optional<Error> ex = {});

    /**
     * The remote machine on which we're building.
     */
    std::string machineName;

    std::map<std::string, InitialOutput> initialOutputs;

    BuildMode buildMode;

    /**
     * Sign the newly built realisation if the store allows it
     */
    void signRealisation(Realisation&);

    DerivationGoal(const StorePath & drvPath,
        const OutputsSpec & wantedOutputs, Worker & worker,
        BuildMode buildMode = bmNormal);
    DerivationGoal(const StorePath & drvPath, const BasicDerivation & drv,
        const OutputsSpec & wantedOutputs, Worker & worker,
        BuildMode buildMode = bmNormal);
    ~DerivationGoal();

    private:

    void started();

    /**
     * The goal for the corresponding resolved derivation
     */
    std::shared_ptr<DerivationGoal> resolvedDrvGoal;

    /**
     * Mapping from input derivations + output names to actual store
     * paths. This is filled in by waiteeDone() as each dependency
     * finishes, before inputsRealised() is reached.
     */
    std::map<std::pair<StorePath, std::string>, StorePath> inputDrvOutputs;

    /**
     * See `needRestart`; just for that field.
     */
    enum struct NeedRestartForMoreOutputs {
        /**
         * The goal state machine is progressing based on the current value of
         * `wantedOutputs. No actions are needed.
         */
        OutputsUnmodifedDontNeed,
        /**
         * `wantedOutputs` has been extended, but the state machine is
         * proceeding according to its old value, so we need to restart.
         */
        OutputsAddedDoNeed,
        /**
         * The goal state machine has progressed to the point of doing a build,
         * in which case all outputs will be produced, so extensions to
         * `wantedOutputs` no longer require a restart.
         */
        BuildInProgressWillNotNeed,
    };

    /**
     * Whether additional wanted outputs have been added.
     */
    NeedRestartForMoreOutputs needRestart = NeedRestartForMoreOutputs::OutputsUnmodifedDontNeed;

    /**
     * See `retrySubstitution`; just for that field.
     */
    enum RetrySubstitution {
        /**
         * No issues have yet arose, no need to restart.
         */
        NoNeed,
        /**
         * Something failed and there is an incomplete closure. Let's retry
         * substituting.
         */
        YesNeed,
        /**
         * We are current or have already retried substitution, and whether or
         * not something goes wrong we will not retry again.
         */
        AlreadyRetried,
    };

    /**
     * Whether to retry substituting the outputs after building the
     * inputs. This is done in case of an incomplete closure.
     */
    RetrySubstitution retrySubstitution = RetrySubstitution::NoNeed;

    /**
     * The remainder is state held during the build.
     */

    /**
     * Locks on (fixed) output paths.
     */
    PathLocks outputLocks;

    /**
     * File descriptor for the log file.
     */
    AutoCloseFD fdLogFile;
    std::shared_ptr<BufferedSink> logFileSink;

    /**
     * Number of bytes received from the builder's stdout/stderr.
     */
    unsigned long logSize;

    /**
     * The most recent log lines.
     */
    std::list<std::string> logTail;

    std::string currentLogLine;
    size_t currentLogLinePos = 0; // to handle carriage return

    typedef void (DerivationGoal::*GoalState)();
    GoalState state;

    std::unique_ptr<MaintainCount<uint64_t>> mcExpectedBuilds, mcRunningBuilds;

    std::unique_ptr<Activity> act;

    std::map<ActivityId, Activity> builderActivities;

    std::unique_ptr<BuilderInterface> builder;

    void timedOut(Error && ex) override;

    std::string key() override;

    void work() override;

    /**
     * The states.
     */
    void getDerivation();
    void loadDerivation();
    void haveDerivation();
    void outputsSubstitutionTried();
    void gaveUpOnSubstitution();
    void closureRepaired();
    void inputsRealised();
    void tryToBuild();
    void tryLocalBuild();
    void buildDone();

    void resolvedFinished();

    int getChildStatus();

    /**
     * Check that the derivation outputs all exist and register them
     * as valid.
     */
    SingleDrvOutputs registerOutputs();

    /**
     * Close the log file.
     */
    void closeLogFile();

    /**
     * Cleanup hooks for buildDone()
     */
    void cleanupHookFinally();
    void cleanupPreChildKill();
    void cleanupPostChildKill();
    bool cleanupDecideWhetherDiskFull();
    void cleanupPostOutputsRegisteredModeCheck();
    void cleanupPostOutputsRegisteredModeNonCheck();

    /**
     * Callback used by the worker to write to the log.
     */
    void handleChildOutput(int fd, std::string_view data) override;
    void handleEOF(int fd) override;
    void flushLine();

    /**
     * Wrappers around the corresponding Store methods that first consult the
     * derivation.  This is currently needed because when there is no drv file
     * there also is no DB entry.
     */
    std::map<std::string, std::optional<StorePath>> queryPartialDerivationOutputMap();
    OutputPathMap queryDerivationOutputMap();

    /**
     * Update 'initialOutputs' to determine the current status of the
     * outputs of the derivation. Also returns a Boolean denoting
     * whether all outputs are valid and non-corrupt, and a
     * 'SingleDrvOutputs' structure containing the valid outputs.
     */
    std::pair<bool, SingleDrvOutputs> checkPathValidity();

    /**
     * Forcibly kill the child process, if any.
     */
    void killChild();

    void repairClosure();

    void waiteeDone(GoalPtr waitee, ExitCode result) override;

    StorePathSet exportReferences(const StorePathSet & storePaths);

    JobCategory jobCategory() const override {
        return JobCategory::Build;
    };

    /**
     * Open a log file and a pipe to it.
     */
    void openLogFile();
};

MakeError(NotDeterministic, BuildError);

}
