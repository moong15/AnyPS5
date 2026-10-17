#include "prx/libSceAgcDriver/Execution/include/Driver/DeviceAccess.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>

using AgcDriver::DriverDetail::NarrowDeviceLock_nid_no_patch;
using AgcDriver::DriverDetail::WithDevicePreparation_nid_no_patch;
using AgcDriver::GuestMemory::GpuMutex;

namespace {

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void Contention(bool narrow) {
    std::promise<void> preparation, commit, preparationProbed, commitProbed;
    auto preparationReady = preparation.get_future();
    auto commitReady = commit.get_future();
    auto preparationDone = preparationProbed.get_future();
    auto commitDone = commitProbed.get_future();
    bool preparationAcquired = false, commitAcquired = false;
    bool preparedLocked = false, validatedLocked = false, committedLocked = false;
    bool preparationTimedOut = false, commitTimedOut = false;
    unsigned shared = 0;
    std::thread contender([&] {
        preparationReady.wait();
        {
            std::unique_lock lock(GpuMutex(), std::try_to_lock);
            preparationAcquired = lock.owns_lock();
            if (preparationAcquired) ++shared;
        }
        preparationProbed.set_value();
        commitReady.wait();
        {
            std::unique_lock lock(GpuMutex(), std::try_to_lock);
            commitAcquired = lock.owns_lock();
            if (commitAcquired) ++shared;
        }
        commitProbed.set_value();
    });
    WithDevicePreparation_nid_no_patch([&] {
        preparedLocked = GpuMutex().HeldByThisThread();
        preparation.set_value();
        preparationTimedOut = preparationDone.wait_for(std::chrono::seconds(2)) != std::future_status::ready;
    }, [&] {
        validatedLocked = GpuMutex().DepthOnThisThread() == 1;
    }, [&] {
        committedLocked = GpuMutex().DepthOnThisThread() == 1;
        ++shared;
        commit.set_value();
        commitTimedOut = commitDone.wait_for(std::chrono::seconds(2)) != std::future_status::ready;
    });
    contender.join();
    Require(!preparationTimedOut && !commitTimedOut, "contender did not finish its probes");
    Require(preparationAcquired == narrow && preparedLocked != narrow, "preparation did not follow the selected lock policy");
    Require(validatedLocked && committedLocked && !commitAcquired, "shared device state was not serialized");
    Require(shared == (narrow ? 2u : 1u), "shared state changed outside its hold");
    Require(!GpuMutex().HeldByThisThread(), "device preparation leaked its lock");
}

void Serialize() {
    unsigned shared = 0;
    std::atomic<bool> failed{false};
    const auto run = [&] {
        for (unsigned i = 0; i < 1000; ++i) {
            WithDevicePreparation_nid_no_patch([] {}, [&] {
                if (!GpuMutex().HeldByThisThread()) failed.store(true);
            }, [&] {
                const auto before = shared;
                std::this_thread::yield();
                shared = before + 1;
                std::lock_guard nested(GpuMutex());
                if (GpuMutex().DepthOnThisThread() != 2) failed.store(true);
            });
        }
    };
    std::thread first(run), second(run);
    first.join();
    second.join();
    Require(shared == 2000 && !failed.load(), "concurrent device work lost serialization or recursion");
}

void ExceptionRelease() {
    for (unsigned phase = 0; phase < 3; ++phase) {
        bool caught = false;
        try {
            WithDevicePreparation_nid_no_patch([&] {
                if (phase == 0) throw std::runtime_error("prepare");
            }, [&] {
                if (phase == 1) throw std::runtime_error("validate");
            }, [&] {
                throw std::runtime_error("commit");
            });
        } catch (const std::runtime_error&) {
            caught = true;
        }
        bool acquired = false;
        std::thread probe([&] {
            std::unique_lock lock(GpuMutex(), std::try_to_lock);
            acquired = lock.owns_lock();
        });
        probe.join();
        Require(caught && acquired && !GpuMutex().HeldByThisThread(), "exception leaked the GPU mutex");
    }
}

}

int main(int argc, char**) {
    try {
        const bool expectedNarrow = argc == 1;
        Require(NarrowDeviceLock_nid_no_patch() == expectedNarrow, "switch did not select the expected path");
#ifdef _WIN32
        Require(_putenv_s("APS5_NO_NARROW_DEVICE_LOCK", expectedNarrow ? "1" : "0") == 0, "cannot change test switch");
#else
        Require(setenv("APS5_NO_NARROW_DEVICE_LOCK", expectedNarrow ? "1" : "0", 1) == 0, "cannot change test switch");
#endif
        Require(NarrowDeviceLock_nid_no_patch() == expectedNarrow, "switch was read more than once");
        Contention(expectedNarrow);
        Serialize();
        ExceptionRelease();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
