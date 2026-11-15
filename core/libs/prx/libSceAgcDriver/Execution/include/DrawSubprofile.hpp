#pragma once

#include "prx/libc/include/HostThreadSlot.hpp"
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <shared_mutex>

namespace AgcDriver {

enum class DrawProfilePhase : std::size_t { Lookup, Record, ReadTarget, Count };
enum class DrawProfileOp : std::size_t {
    Other, Key, Find, Space, Revalidate, RegisteredImages, MovedBuffers, Aliases,
    PendingWrites, Invalidate, CacheTouch, Descriptors, DescriptorAllocate,
    DescriptorWrite, DrawUse, Barriers, Pass, Binds, PushConstants, DrawCommand,
    StoreDrawnLinear, Access, QueuedStores, Arguments, PassFinish, Capture,
    Resident, DepthStorage, DepthView, Cmask, RegisterClear, GuestRead,
    Multisample, FallbackAllocate, DccClear, Count
};

inline constexpr std::array DrawProfilePhaseNames{"lookup", "record", "readTarget"};
inline constexpr std::array DrawProfileOpNames{
    "other", "resource-key", "resource-cache-find", "retained-space", "revalidate",
    "registered-images", "moved-buffers", "buffer-aliases", "pending-write-proofs",
    "invalidate", "cache-touch", "descriptors", "descriptor-allocate", "descriptor-write",
    "draw-use", "barriers", "pass-begin-continue", "binds", "push-constants", "draw-command",
    "store-drawn-linear-touching", "access-hazards", "queued-stores", "arguments",
    "pass-finish", "capture", "resident-lookup-refresh", "depth-storage", "depth-view",
    "cmask-clear", "register-clear", "guest-read-detile", "multisample",
    "fallback-allocate", "dcc-clear"
};
static_assert(DrawProfilePhaseNames.size() == static_cast<std::size_t>(DrawProfilePhase::Count));
static_assert(DrawProfileOpNames.size() == static_cast<std::size_t>(DrawProfileOp::Count));

struct DrawSubprofile {
    struct Sample {
        double us = 0;
        std::uint64_t calls = 0;
        std::uint64_t locks = 0;
        std::uint64_t waits = 0;
        double waitUs = 0;
    };
    std::array<std::array<Sample, static_cast<std::size_t>(DrawProfileOp::Count)>, static_cast<std::size_t>(DrawProfilePhase::Count)> samples{};

    void Add(const DrawSubprofile& other) {
        for (std::size_t phase = 0; phase < samples.size(); ++phase) {
            for (std::size_t op = 0; op < samples[phase].size(); ++op) {
                auto& to = samples[phase][op];
                const auto& from = other.samples[phase][op];
                to.us += from.us;
                to.calls += from.calls;
                to.locks += from.locks;
                to.waits += from.waits;
                to.waitUs += from.waitUs;
            }
        }
    }

    template<typename TPrint>
    void Print(TPrint&& print) const {
        for (std::size_t phase = 0; phase < samples.size(); ++phase) {
            for (std::size_t op = 0; op < samples[phase].size(); ++op) {
                const auto& sample = samples[phase][op];
                if (sample.calls == 0) continue;
                print("[draws] subphases (10 s): %s/%s=%.3fms calls=%llu locks=%llu waits=%llu wait=%.3fms\n",
                    DrawProfilePhaseNames[phase], DrawProfileOpNames[op], sample.us / 1000.0,
                    static_cast<unsigned long long>(sample.calls), static_cast<unsigned long long>(sample.locks),
                    static_cast<unsigned long long>(sample.waits), sample.waitUs / 1000.0);
            }
        }
    }
};

class DrawSubprofileScope {
public:
    using Clock = std::chrono::steady_clock;
    using Slot = HostThreadSlot<DrawSubprofileScope*, DrawSubprofileScope>;

    DrawSubprofileScope(DrawSubprofile* totals, DrawProfilePhase phase, DrawProfileOp op) : totals(totals), phase(phase), op(op) {
        if (totals == nullptr) return;
        previous = Slot::Get();
        start();
    }

    explicit DrawSubprofileScope(DrawProfileOp op) : op(op) {
        previous = Current();
        if (previous == nullptr) return;
        totals = previous->totals;
        phase = previous->phase;
        if (previous->op != op) start();
    }

    ~DrawSubprofileScope() { Stop(); }
    DrawSubprofileScope(const DrawSubprofileScope&) = delete;
    DrawSubprofileScope& operator=(const DrawSubprofileScope&) = delete;

    static DrawSubprofileScope* Current() noexcept {
        static const bool enabled = std::getenv("APS5_PROFILE_DRAW") != nullptr;
        return enabled ? Slot::Get() : nullptr;
    }

    void Stop() {
        if (totals == nullptr || !active) return;
        const auto elapsed = std::chrono::duration<double, std::micro>(Clock::now() - began).count();
        Sample().us += elapsed - childrenUs;
        if (previous != nullptr) previous->childrenUs += elapsed;
        Slot::Set(previous);
        active = false;
    }

    void Next(DrawProfileOp next) {
        if (totals == nullptr) return;
        Stop();
        op = next;
        start();
    }

    DrawSubprofile::Sample& Sample() noexcept {
        return totals->samples[static_cast<std::size_t>(phase)][static_cast<std::size_t>(op)];
    }

private:
    void start() {
        childrenUs = 0;
        ++Sample().calls;
        began = Clock::now();
        active = true;
        Slot::Set(this);
    }

    DrawSubprofile* totals = nullptr;
    DrawSubprofileScope* previous = nullptr;
    DrawProfilePhase phase = DrawProfilePhase::Lookup;
    DrawProfileOp op;
    Clock::time_point began{};
    double childrenUs = 0;
    bool active = false;
};

template<typename TLock>
void AcquireDrawProfileLock(TLock& lock) {
    auto* scope = DrawSubprofileScope::Current();
    if (scope == nullptr) {
        lock.lock();
        return;
    }
    auto& sample = scope->Sample();
    ++sample.locks;
    if (lock.try_lock()) return;
    const auto start = DrawSubprofileScope::Clock::now();
    lock.lock();
    sample.waitUs += std::chrono::duration<double, std::micro>(DrawSubprofileScope::Clock::now() - start).count();
    ++sample.waits;
}

template<typename TMutex>
std::unique_lock<TMutex> LockDrawProfile(TMutex& mutex) {
    std::unique_lock<TMutex> lock(mutex, std::defer_lock);
    AcquireDrawProfileLock(lock);
    return lock;
}

template<typename TMutex>
std::shared_lock<TMutex> ShareDrawProfile(TMutex& mutex) {
    std::shared_lock<TMutex> lock(mutex, std::defer_lock);
    AcquireDrawProfileLock(lock);
    return lock;
}

}
