#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_SLIMMUTEX_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_SLIMMUTEX_HPP

#include "prx/libSceAgcDriver/Execution/include/DrawSubprofile.hpp"
#include <atomic>
#include <thread>
#include "prx/libc/include/PreciseWait.hpp"

namespace AgcDriver::Graphics {

class SlimMutex {
public:
    SlimMutex() = default;
    SlimMutex(const SlimMutex&) = delete;
    SlimMutex& operator=(const SlimMutex&) = delete;

    void lock() {
        auto* scope = DrawSubprofileScope::Current();
        if (scope == nullptr) {
            lockUnprofiled();
            return;
        }
        auto& sample = scope->Sample();
        ++sample.locks;
        if (try_lock()) return;
        const auto start = DrawSubprofileScope::Clock::now();
        lockUnprofiled();
        sample.waitUs += std::chrono::duration<double, std::micro>(DrawSubprofileScope::Clock::now() - start).count();
        ++sample.waits;
    }
    bool try_lock() { return !locked.load(std::memory_order_relaxed) && !locked.exchange(true, std::memory_order_acquire); }
    void unlock() { locked.store(false, std::memory_order_release); }

private:
    void lockUnprofiled() {
        for (unsigned spins = 0;;) {
            if (!locked.exchange(true, std::memory_order_acquire)) return;
            while (locked.load(std::memory_order_relaxed)) {
                if (spins < 128) {
                    ++spins;
                    PreciseSpinPause();
                } else {
                    std::this_thread::yield();
                }
            }
        }
    }
    std::atomic<bool> locked{false};
};

}

#endif
