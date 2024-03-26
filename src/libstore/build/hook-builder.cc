#include "hook-builder.hh"
#include "worker.hh"
#include "hook-instance.hh"
#include "common-protocol.hh"
#include "common-protocol-impl.hh"

namespace nix {
namespace {

class HookBuilder final : public BuilderInterface {
    public:
    ~HookBuilder() override {};
    int getChildStatus() override ;
    SingleDrvOutputs registerOutputs() override;
    void cleanupHookFinally() override {};
    void cleanupPreChildKill() override {};
    void cleanupPostChildKill() override;
    bool cleanupDecideWhetherDiskFull() override { return false; };
    void cleanupPostOutputsRegisteredModeCheck() override {};
    void cleanupPostOutputsRegisteredModeNonCheck() override {};
    void killChild() override;
    void handleChildOutput(int fd, std::string_view data) override;

    std::unique_ptr<HookInstance> hook;
    std::string currentHookLine;

    DerivationGoal& goal;
    HookBuilder(DerivationGoal& goal) : goal(goal) {}
};

void HookBuilder::handleChildOutput(int fd, std::string_view data) {
    if (fd == hook->builderOut.readSide.get()) {
        goal.writeToLog(data);
    } else if (fd == hook->fromHook.readSide.get()) {
        for (auto c : data)
            if (c == '\n') {
                auto json = parseJSONMessage(currentHookLine);
                if (json) {
                    auto s = handleJSONLogMessage(*json, goal.worker.act, hook->activities, true);
                    // ensure that logs from a builder using `ssh-ng://` as protocol
                    // are also available to `nix log`.
                    if (s && goal.logSink) {
                        const auto type = (*json)["type"];
                        const auto fields = (*json)["fields"];
                        if (type == resBuildLogLine) {
                            (*goal.logSink)((fields.size() > 0 ? fields[0].get<std::string>() : "") + "\n");
                        } else if (type == resSetPhase && ! fields.is_null()) {
                            const auto phase = fields[0];
                            if (! phase.is_null()) {
                                // nixpkgs' stdenv produces lines in the log to signal
                                // phase changes.
                                // We want to get the same lines in case of remote builds.
                                // The format is:
                                //   @nix { "action": "setPhase", "phase": "$curPhase" }
                                const auto logLine = nlohmann::json::object({
                                    {"action", "setPhase"},
                                    {"phase", phase}
                                });
                                (*goal.logSink)("@nix " + logLine.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n");
                            }
                        }
                    }
                }
                currentHookLine.clear();
            } else {
                currentHookLine += c;
            }
    }
}

void HookBuilder::killChild() {
    return hook.reset();
}

int HookBuilder::getChildStatus() {
    return hook->pid.kill();
}

void HookBuilder::cleanupPostChildKill() {
    /* Close the read side of the logger pipe. */
    hook->builderOut.readSide = -1;
    hook->fromHook.readSide = -1;
}

SingleDrvOutputs HookBuilder::registerOutputs() {
    /* When using a build hook, the build hook can register the output
       as valid (by doing `nix-store --import').  If so we don't have
       to do anything here.

       We can only early return when the outputs are known a priori. For
       floating content-addressed derivations this isn't the case.
     */
    return goal.assertPathValidity();
}

}

std::variant<RpDecline, RpPostpone, RpAccept> make_hook_builder(DerivationGoal& goal) {
    auto& worker = goal.worker;

    if (!worker.hook) worker.hook = std::make_unique<HookInstance>();

    try {

        /* Send the request to the hook. */
        worker.hook->sink
            << "try"
            << (worker.getNrLocalBuilds() < settings.maxBuildJobs ? 1 : 0)
            << goal.drv->platform
            << worker.store.printStorePath(goal.drvPath)
            << goal.parsedDrv->getRequiredSystemFeatures();
        worker.hook->sink.flush();

        /* Read the first line of input, which should be a word indicating
           whether the hook wishes to perform the build. */
        std::string reply;
        while (true) {
            auto s = [&]() {
                try {
                    return readLine(worker.hook->fromHook.readSide.get());
                } catch (Error & e) {
                    e.addTrace({}, "while reading the response from the build hook");
                    throw;
                }
            }();
            if (handleJSONLogMessage(s, worker.act, worker.hook->activities, true))
                ;
            else if (s.substr(0, 2) == "# ") {
                reply = s.substr(2);
                break;
            }
            else {
                s += "\n";
                writeToStderr(s);
            }
        }

        debug("hook reply is '%1%'", reply);

        if (reply == "decline")
            return RpDecline {};
        else if (reply == "decline-permanently") {
            worker.tryBuildHook = false;
            worker.hook = 0;
            return RpDecline {};
        }
        else if (reply == "postpone")
            return RpPostpone {};
        else if (reply != "accept")
            throw Error("bad hook reply '%s'", reply);

    } catch (SysError & e) {
        if (e.errNo == EPIPE) {
            printError(
                "build hook died unexpectedly: %s",
                chomp(drainFD(worker.hook->fromHook.readSide.get())));
            worker.hook = 0;
            return RpDecline {};
        } else
            throw;
    }

    auto hook = std::move(worker.hook);

    try {
        goal.machineName = readLine(hook->fromHook.readSide.get());
    } catch (Error & e) {
        e.addTrace({}, "while reading the machine name from the build hook");
        throw;
    }

    CommonProto::WriteConn conn { hook->sink };

    /* Tell the hook all the inputs that have to be copied to the
       remote system. */
    CommonProto::write(worker.store, conn, goal.inputPaths);

    /* Tell the hooks the missing outputs that have to be copied back
       from the remote system. */
    {
        StringSet missingOutputs;
        for (auto & [outputName, status] : goal.initialOutputs) {
            // XXX: Does this include known CA outputs?
            if (goal.buildMode != bmCheck && status.known && status.known->isValid()) continue;
            missingOutputs.insert(outputName);
        }
        CommonProto::write(worker.store, conn, missingOutputs);
    }

    hook->sink = FdSink();
    hook->toHook.writeSide = -1;

    std::set<int> fds;
    fds.insert(hook->fromHook.readSide.get());
    fds.insert(hook->builderOut.readSide.get());
    worker.childStarted(goal.shared_from_this(), fds, false, false);

    auto builder = std::make_unique<HookBuilder>(goal);
    builder->hook = std::move(hook);
    return RpAccept { std::move(builder) };
}
}
