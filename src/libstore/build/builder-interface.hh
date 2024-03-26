#pragma once
///@file

#include <realisation.hh>

namespace nix {

class BuilderInterface {
    public:
    virtual ~BuilderInterface() {};
    virtual int getChildStatus() = 0;
    /**
     * Check that the derivation outputs all exist and register them
     * as valid.
     */
    virtual SingleDrvOutputs registerOutputs() = 0;

    /**
     * Cleanup hooks for buildDone()
     */
    virtual void cleanupHookFinally() = 0;
    virtual void cleanupPreChildKill() = 0;
    virtual void cleanupPostChildKill() = 0;
    virtual bool cleanupDecideWhetherDiskFull() = 0;
    virtual void cleanupPostOutputsRegisteredModeCheck() = 0;
    virtual void cleanupPostOutputsRegisteredModeNonCheck() = 0;

    virtual void handleChildOutput(int fd, std::string_view data) = 0;

    /**
     * Forcibly kill the child process, if any.
     */
    virtual void killChild() = 0;
};

}
