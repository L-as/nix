#include "local-store.hh"
#include "machines.hh"
#include "worker.hh"
#include "substitution-goal.hh"
#include "drv-output-substitution-goal.hh"
#include "derivation-goal.hh"
#include <memory>
#ifndef _WIN32 // TODO Enable building on Windows
#  include "local-derivation-goal.hh"
#  include "hook-instance.hh"
#endif
#include "signals.hh"

namespace nix {

namespace {
    GoalPtr extractGoal(Goals goals, const GoalKey & key) {
        // FIXME: C++23 standard library supports passing key to extract
        // directly rather than going through this indirection.
        auto goal_it = goals.find(key);
        assert(goal_it != goals.end()); /* goal should be in awake */
        return std::move(goals.extract(goal_it).value());
    }
}

Worker::Worker(Store & store, Store & evalStore)
    : act(*logger, actRealise)
    , actDerivations(*logger, actBuilds)
    , actSubstitutions(*logger, actCopyPaths)
    , store(store)
    , evalStore(evalStore)
{
    nrLocalBuilds = 0;
    nrSubstitutions = 0;
    lastWokenUp = steady_time_point::min();
    permanentFailure = false;
    timedOut = false;
    hashMismatch = false;
    checkMismatch = false;
}


Worker::~Worker()
{
    /* FIXME(L-as): remove, shouldn't be necessary.
       Explicitly get rid of all strong pointers now.  After this all
       goals that refer to this worker should be gone.  (Otherwise we
       are in trouble, since goals may call childTerminated() etc. in
       their destructors). */
    awake.clear();
    waitingForAWhile.clear();
    waitingForAnyGoal.clear();
    wantingToBuild.clear();

    assert(expectedSubstitutions == 0);
    assert(expectedDownloadSize == 0);
    assert(expectedNarSize == 0);
}


std::unique_ptr<DerivationGoal> Worker::makeDerivationGoal(
    const StorePath & drvPath,
    const OutputsSpec & wantedOutputs,
    BuildMode buildMode)
{
    auto goal = std::make_unique<DerivationGoal>(drvPath, wantedOutputs, *this, buildMode);
    return goal;
}

std::unique_ptr<DerivationGoal> Worker::makeBasicDerivationGoal(const StorePath & drvPath,
    const BasicDerivation & drv, const OutputsSpec & wantedOutputs, BuildMode buildMode)
{
    auto goal = std::make_unique<DerivationGoal>(drvPath, drv, wantedOutputs, *this, buildMode);
    return goal;
}


std::shared_ptr<PathSubstitutionGoal> Worker::makePathSubstitutionGoal(const StorePath & path, RepairFlag repair, std::optional<ContentAddress> ca)
{
    std::weak_ptr<PathSubstitutionGoal> & goal_weak = substitutionGoals[path];
    auto goal = goal_weak.lock(); // FIXME
    if (!goal) {
        goal = std::make_shared<PathSubstitutionGoal>(path, *this, repair, ca);
        goal_weak = goal;
        wakeUp(goal);
    }
    return goal;
}


std::shared_ptr<DrvOutputSubstitutionGoal> Worker::makeDrvOutputSubstitutionGoal(const DrvOutput& id, RepairFlag repair, std::optional<ContentAddress> ca)
{
    std::weak_ptr<DrvOutputSubstitutionGoal> & goal_weak = drvOutputSubstitutionGoals[id];
    auto goal = goal_weak.lock(); // FIXME
    if (!goal) {
        goal = std::make_shared<DrvOutputSubstitutionGoal>(id, *this, repair, ca);
        goal_weak = goal;
        wakeUp(goal);
    }
    return goal;
}


GoalPtr Worker::makeGoal(const DerivedPath & req, BuildMode buildMode)
{
    return std::visit(overloaded {
        [&](const DerivedPath::Built & bfd) -> GoalPtr {
            if (auto bop = std::get_if<DerivedPath::Opaque>(&*bfd.drvPath))
                return makeDerivationGoal(bop->path, bfd.outputs, buildMode);
            else
                throw UnimplementedError("Building dynamic derivations in one shot is not yet implemented.");
        },
        [&](const DerivedPath::Opaque & bo) -> GoalPtr {
            return makePathSubstitutionGoal(bo.path, buildMode == bmRepair ? Repair : NoRepair);
        },
    }, req.raw());
}
)

void Worker::removeGoal(GoalKey goalKey)
{
    GoalPtr goal = extractGoal(awake, goalKey);

    // FIXME: remove references to key from memoisation maps for constructing goals.

    if (goal->getExitCode() == Goal::ecFailed && !settings.keepGoing)
        throw new Error("FIXME");

    /* All goals that were waiting for some goal to end are now awake. */
    awake.merge(waitingForAnyGoal);
    assert(waitingForAnyGoal.empty());
}


void Worker::wakeUp(const GoalKey & key)
{
    GoalPtr goal = extractGoal(pausedGoals, key);
    Goal* goal_ = goal.get();
    awake.insert(std::move(goal));
    goal_->trace("woken up");
}


unsigned Worker::getNrLocalBuilds()
{
    return nrLocalBuilds;
}


unsigned Worker::getNrSubstitutions()
{
    return nrSubstitutions;
}


void Worker::childStarted(GoalPtr goal, const std::set<MuxablePipePollState::CommChannel> & channels,
    bool inBuildSlot, bool respectTimeouts)
{
    Child child;
    child.goal_key = goal->key();
    child.channels = channels;
    child.timeStarted = child.lastOutput = steady_time_point::clock::now();
    child.inBuildSlot = inBuildSlot;
    child.respectTimeouts = respectTimeouts;
    children.emplace_back(child);
    if (inBuildSlot) {
        switch (goal->jobCategory()) {
        case JobCategory::Substitution:
            nrSubstitutions++;
            break;
        case JobCategory::Build:
            nrLocalBuilds++;
            break;
        default:
            abort();
        }
    }
}


void Worker::childTerminated(Goal * goal, bool wakeSleepers)
{
    auto i = std::find_if(children.begin(), children.end(),
        [&](const Child & child) { return child.goal2 == goal; });
    if (i == children.end()) return;

    if (i->inBuildSlot) {
        switch (goal->jobCategory()) {
        case JobCategory::Substitution:
            assert(nrSubstitutions > 0);
            nrSubstitutions--;
            break;
        case JobCategory::Build:
            assert(nrLocalBuilds > 0);
            nrLocalBuilds--;
            break;
        default:
            abort();
        }
    }

    children.erase(i);

    if (wakeSleepers) {

        /* Wake up goals waiting for a build slot. */
        for (auto & j : wantingToBuild) {
            GoalPtr goal = j.lock();
            if (goal) wakeUp(goal);
        }

        wantingToBuild.clear();
    }
}

void Worker::run()
{
#if FALSE
    std::vector<nix::DerivedPath> topPaths;

    for (auto & i : _topGoals) {
        topGoals.insert(i);
        if (auto goal = dynamic_cast<DerivationGoal *>(i.get())) {
            topPaths.push_back(DerivedPath::Built {
                .drvPath = makeConstantStorePathRef(goal->getDrvPath()),
                .outputs = goal->getWantedOutputs(),
            });
        } else
        if (auto goal = dynamic_cast<PathSubstitutionGoal *>(i.get())) {
            topPaths.push_back(DerivedPath::Opaque{goal->getStorePath()});
        }
    }

    /* Call queryMissing() to efficiently query substitutes. */
    StorePathSet willBuild, willSubstitute, unknown;
    uint64_t downloadSize, narSize;
    store.queryMissing(topPaths, willBuild, willSubstitute, unknown, downloadSize, narSize);
#endif

    debug("entered goal loop");

    while (1) {

        checkInterrupt();

        // FIXME: add autoGC to Store API
        if (auto localStore = dynamic_cast<LocalStore *>(&store))
            localStore->autoGC(false);

        for (auto it = awake.begin(); it != awake.end(); it++) {
            assert(*it);
            Goal::GoalOutput r = (*it)->work();
            std::visit(overloaded {
                [&](Goal::Nap && _) {},
                [&](Goal::WaitForAWhile && _) {
                    GoalPtr g = std::move(awake.extract(it).value());
                    waitingForAWhile.insert(std::move(g));
                },
                [&](Goal::WaitForBuildSlot && _) {
                    GoalPtr g = std::move(awake.extract(it).value());
                    wantingToBuild.insert(std::move(g));
                },
                [&](Goal::WaitWaitees && w) {
                    for (auto&& waitee : std::move(w.waitees)) {
                        auto key = waitee->key();
                        if (goalDAG.contains(key)) {
                            /* Don't insert the goal, one equivalent to it already exists. */
                        } else {
                            awake.insert(std::move(waitee));
                        }
                        goalDAG.insert({key, (*it)->key()});
                    }
                },
                [&](Goal::GoalDone && _) {
                    GoalPtr waitee = std::move(awake.extract(it).value());
                    auto waiters = goalDAG.equal_range(waitee->key());
                    for (auto it = waiters.first ; it != waiters.second ; it++) {
                        auto waiter_it = pausedGoals.find(it->second);
                        assert(waiter_it != pausedGoals.end());
                        (*waiter_it)->nrWaitees -= 1;
                        if ((*waiter_it)->nrWaitees == 0) {
                            GoalPtr waiter = std::move(pausedGoals.extract(waiter_it).value());
                            awake.insert(std::move(waiter));
                        }
                        /* We remove the link between waitee and waiter, now that
                         * waitee is gone. */
                        goalDAG.extract(it);
                    }
                },
            }, std::move(r));
        }

        if (goalDAG.empty()) {
            /* We are done! */
            break;
        }

        /* There must still be work left to do. */
        assert(!waitingForAWhile.empty() || !awake.empty() || !children.empty());

        /* Wait for input. */
        if (!children.empty() || !waitingForAWhile.empty())
            waitForInput();
        else if (awake.empty() && 0U == settings.maxBuildJobs) {
            /* FIXME: This logic shouldn't be here. */
            if (getMachines().empty())
               throw Error(
                    R"(
                    Unable to start any build;
                    either increase '--max-jobs' or enable remote builds.

                    For more information run 'man nix.conf' and search for '/machines'.
                    )"
                );
            else
               throw Error(
                    R"(
                    Unable to start any build;
                    remote machines may not have all required system features.

                    For more information run 'man nix.conf' and search for '/machines'.
                    )"
                );

        } else assert(!awake.empty());
    }

    /* If --keep-going is not set, it's possible that the main goal
       exited while some of its subgoals were still active.  But if
       --keep-going *is* set, then they must all be finished now. */
    assert(!settings.keepGoing || awake.empty());
    assert(!settings.keepGoing || wantingToBuild.empty());
    assert(!settings.keepGoing || children.empty());
}

void Worker::waitForInput()
{
    printMsg(lvlVomit, "waiting for children");

    /* Process output from the file descriptors attached to the
       children, namely log output and output path creation commands.
       We also use this to detect child termination: if we get EOF on
       the logger pipe of a build, we assume that the builder has
       terminated. */

    bool useTimeout = false;
    long timeout = 0;
    auto before = steady_time_point::clock::now();

    /* If we're monitoring for silence on stdout/stderr, or if there
       is a build timeout, then wait for input until the first
       deadline for any child. */
    auto nearest = steady_time_point::max(); // nearest deadline
    if (settings.minFree.get() != 0)
        // Periodicallty wake up to see if we need to run the garbage collector.
        nearest = before + std::chrono::seconds(10);
    for (auto & i : children) {
        if (!i.respectTimeouts) continue;
        if (0 != settings.maxSilentTime)
            nearest = std::min(nearest, i.lastOutput + std::chrono::seconds(settings.maxSilentTime));
        if (0 != settings.buildTimeout)
            nearest = std::min(nearest, i.timeStarted + std::chrono::seconds(settings.buildTimeout));
    }
    if (nearest != steady_time_point::max()) {
        timeout = std::max(1L, (long) std::chrono::duration_cast<std::chrono::seconds>(nearest - before).count());
        useTimeout = true;
    }

    /* If we are polling goals that are waiting for a lock, then wake
       up after a few seconds at most.
       If there is still more work to do, we also set a timeout. */
    if (!waitingForAWhile.empty() || !awake.empty()) {
        useTimeout = true;
        if (lastWokenUp == steady_time_point::min() || lastWokenUp > before) lastWokenUp = before;
        timeout = std::max(1L,
            (long) std::chrono::duration_cast<std::chrono::seconds>(
                lastWokenUp + std::chrono::seconds(settings.pollInterval) - before).count());
    } else lastWokenUp = steady_time_point::min();

    if (useTimeout)
        vomit("sleeping %d seconds", timeout);

    MuxablePipePollState state;

#ifndef _WIN32
    /* Use select() to wait for the input side of any logger pipe to
       become `available'.  Note that `available' (i.e., non-blocking)
       includes EOF. */
    for (auto & i : children) {
        for (auto & j : i.channels) {
            state.pollStatus.push_back((struct pollfd) { .fd = j, .events = POLLIN });
            state.fdToPollStatus[j] = state.pollStatus.size() - 1;
        }
    }
#endif

    state.poll(
#ifdef _WIN32
        ioport.get(),
#endif
        useTimeout ? (std::optional { timeout * 1000 }) : std::nullopt);

    auto after = steady_time_point::clock::now();

    /* Process all available file descriptors. FIXME: this is
       O(children * fds). */
    decltype(children)::iterator i;
    for (auto j = children.begin(); j != children.end(); j = i) {
        i = std::next(j);

        checkInterrupt();

        GoalPtr goal = j->goal.lock();
        assert(goal);

        state.iterate(
            j->channels,
            [&](Descriptor k, std::string_view data) {
                printMsg(lvlVomit, "%1%: read %2% bytes",
                    goal->getName(), data.size());
                j->lastOutput = after;
                goal->handleChildOutput(k, data);
            },
            [&](Descriptor k) {
                debug("%1%: got EOF", goal->getName());
                goal->handleEOF(k);
            });

        if (goal->getExitCode() == Goal::ecBusy &&
            0 != settings.maxSilentTime &&
            j->respectTimeouts &&
            after - j->lastOutput >= std::chrono::seconds(settings.maxSilentTime))
        {
            goal->timedOut(Error(
                    "%1% timed out after %2% seconds of silence",
                    goal->getName(), settings.maxSilentTime));
        }

        else if (goal->getExitCode() == Goal::ecBusy &&
            0 != settings.buildTimeout &&
            j->respectTimeouts &&
            after - j->timeStarted >= std::chrono::seconds(settings.buildTimeout))
        {
            goal->timedOut(Error(
                    "%1% timed out after %2% seconds",
                    goal->getName(), settings.buildTimeout));
        }
    }

    if (!waitingForAWhile.empty() && lastWokenUp + std::chrono::seconds(settings.pollInterval) <= after) {
        lastWokenUp = after;
        for (auto & i : waitingForAWhile) {
            GoalPtr goal = i.lock();
            if (goal) wakeUp(goal);
        }
        waitingForAWhile.clear();
    }
}


unsigned int Worker::failingExitStatus()
{
    // See API docs in header for explanation
    unsigned int mask = 0;
    bool buildFailure = permanentFailure || timedOut || hashMismatch;
    if (buildFailure)
        mask |= 0x04;  // 100
    if (timedOut)
        mask |= 0x01;  // 101
    if (hashMismatch)
        mask |= 0x02;  // 102
    if (checkMismatch) {
        mask |= 0x08;  // 104
    }

    if (mask)
        mask |= 0x60;
    return mask ? mask : 1;
}


bool Worker::pathContentsGood(const StorePath & path)
{
    auto i = pathContentsGoodCache.find(path);
    if (i != pathContentsGoodCache.end()) return i->second;
    printInfo("checking path '%s'...", store.printStorePath(path));
    auto info = store.queryPathInfo(path);
    bool res;
    if (!pathExists(store.printStorePath(path)))
        res = false;
    else {
        auto current = hashPath(
            {store.getFSAccessor(), CanonPath(store.printStorePath(path))},
            FileIngestionMethod::NixArchive, info->narHash.algo).first;
        Hash nullHash(HashAlgorithm::SHA256);
        res = info->narHash == nullHash || info->narHash == current;
    }
    pathContentsGoodCache.insert_or_assign(path, res);
    if (!res)
        printError("path '%s' is corrupted or missing!", store.printStorePath(path));
    return res;
}


void Worker::markContentsGood(const StorePath & path)
{
    pathContentsGoodCache.insert_or_assign(path, true);
}


GoalPtr upcast_goal(std::shared_ptr<PathSubstitutionGoal> subGoal)
{
    return subGoal;
}

GoalPtr upcast_goal(std::shared_ptr<DrvOutputSubstitutionGoal> subGoal)
{
    return subGoal;
}

}
