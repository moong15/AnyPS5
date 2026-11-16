#pragma once

#include "prx/libc/include/HostThreadSlot.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Dispatch/DispatchTiming.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <utility>

namespace AgcDriver {

enum class DrawProfilePhase : std::size_t { Lookup, Record, ReadTarget, Vertex, Descriptors, Count };
enum class DrawProfileOp : std::size_t {
    Other, Key, Find, Space, Revalidate, RegisteredImages, MovedBuffers, Aliases,
    PendingWrites, Invalidate, CacheTouch, Descriptors, DescriptorAllocate,
    DescriptorWrite, DrawUse, Barriers, Pass, Binds, PushConstants, DrawCommand,
    StoreDrawnLinear, Access, QueuedStores, Arguments, PassFinish, Capture,
    Resident, DepthStorage, DepthView, Cmask, RegisterClear, GuestRead,
    Multisample, FallbackAllocate, DccClear, InputSnapshot, InPlaceProof, IndexRange,
    VertexPlan, WatchChecks, ScanFilter, DataUpload, Snapshot, Retain, BatchOpen, Count
};

inline constexpr std::array DrawProfilePhaseNames{"lookup", "record", "readTarget", "vertex", "record/descriptors"};
inline constexpr std::array DrawProfileOpNames{
    "other", "resource-key", "resource-cache-find", "retained-space", "revalidate",
    "registered-images", "moved-buffers", "buffer-aliases", "pending-write-proofs",
    "invalidate", "cache-touch", "descriptors", "descriptor-allocate", "descriptor-write",
    "draw-use", "barriers", "pass-begin-continue", "binds", "push-constants", "draw-command",
    "store-drawn-linear-touching", "access-hazards", "queued-stores", "arguments",
    "pass-finish", "capture", "resident-lookup-refresh", "depth-storage", "depth-view",
    "cmask-clear", "register-clear", "guest-read-detile", "multisample",
    "fallback-allocate", "dcc-clear", "input-snapshot-copy", "in-place-proof", "index-range",
    "plan-build", "watch-checks", "scan-filter", "data-word-upload", "snapshot", "retain", "batch-open"
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
                if (phase == static_cast<std::size_t>(DrawProfilePhase::Descriptors)) {
                    print("[draws] subphases (10 s): %s/%s=%.3fms calls=%llu\n",
                        DrawProfilePhaseNames[phase], DrawProfileOpNames[op], sample.us / 1000.0,
                        static_cast<unsigned long long>(sample.calls));
                    continue;
                }
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

    DrawSubprofileScope(DrawProfileOp op, DrawProfilePhase required) : op(op) {
        previous = Current();
        if (previous == nullptr || previous->phase != required) return;
        totals = previous->totals;
        phase = required;
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

    DrawSubprofile* Totals() const noexcept { return totals; }

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

class DrawDescriptorProfileScope {
public:
    using Clock = DrawSubprofileScope::Clock;
    using Slot = HostThreadSlot<DrawDescriptorProfileScope*, DrawDescriptorProfileScope>;

    explicit DrawDescriptorProfileScope(DrawProfileOp op, bool root = false) : op(op) {
        auto* draw = DrawSubprofileScope::Current();
        if (draw == nullptr) return;
        previous = Slot::Get();
        if (previous == nullptr && !root) return;
        totals = previous != nullptr ? previous->totals : draw->Totals();
        if (previous == nullptr || previous->op != op) start();
    }

    ~DrawDescriptorProfileScope() { Stop(); }
    DrawDescriptorProfileScope(const DrawDescriptorProfileScope&) = delete;
    DrawDescriptorProfileScope& operator=(const DrawDescriptorProfileScope&) = delete;

    void Stop() {
        if (!active) return;
        const auto elapsed = std::chrono::duration<double, std::micro>(Clock::now() - began).count();
        sample().us += elapsed - childrenUs;
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

private:
    DrawSubprofile::Sample& sample() noexcept {
        return totals->samples[static_cast<std::size_t>(DrawProfilePhase::Descriptors)][static_cast<std::size_t>(op)];
    }

    void start() {
        childrenUs = 0;
        ++sample().calls;
        began = Clock::now();
        active = true;
        Slot::Set(this);
    }

    DrawSubprofile* totals = nullptr;
    DrawDescriptorProfileScope* previous = nullptr;
    DrawProfileOp op;
    Clock::time_point began{};
    double childrenUs = 0;
    bool active = false;
};

enum class ProofBreakSite : std::size_t {
    Explicit, NonDrawCommit, Stored, StoreOwn, MarkWritten, GpuAcquire,
    WriteChanged, WriteChangedIfHeld, ShaderWrites, CopiedDrawWriter,
    CompletionLabel, RecordedWork, CompletionLabelLate, Count
};

enum class DrawProfileCount : std::size_t {
    PendingScans, PendingSizeSum, PendingSizeMax, PendingFlushingSum, PendingFlushingMax,
    RevalidateDraws, SourceViews, UniqueSources, DuplicateSourceViews,
    TargetMemoMiss, TargetPartialMask, TargetRecipe, ReadTargetCollectedBytes, ReadTargetCollectNs, Count
};

inline constexpr std::array DrawProfileBreakNames{
    "explicit", "non-draw-commit", "stored", "store-own", "mark-written", "gpu-acquire",
    "write-changed", "write-changed-if-held", "shader-writes", "copied-draw-writer",
    "completion-label", "recorded-work", "completion-label-late"
};
inline constexpr std::array DrawProfileCommitNames{
    "other", "draw", "indirect", "colormeta", "dispatch", "sampledump", "dmafill", "dmawrite",
    "dmacopy", "store", "endofpipe", "seal", "submitend", "completion", "bufferfill"
};
inline constexpr std::array DrawProfileCountNames{
    "pending/scan-calls", "pending/size-sum", "pending/size-max", "pending/flushing-sum", "pending/flushing-max",
    "revalidate/draw-calls", "revalidate/source-views", "revalidate/unique-sources", "revalidate/duplicate-source-views",
    "readTarget/lookups-memo-miss", "readTarget/lookups-partial-mask", "readTarget/lookups-recipe",
    "readTarget/collected-bytes", "readTarget/collect-ns"
};
static_assert(DrawProfileBreakNames.size() == static_cast<std::size_t>(ProofBreakSite::Count));
static_assert(DrawProfileCountNames.size() == static_cast<std::size_t>(DrawProfileCount::Count));

class DrawProfileMeasurements {
public:
    static bool Enabled() noexcept {
        static const bool enabled = std::getenv("APS5_PROFILE_DRAW") != nullptr;
        return enabled;
    }

    static void MarkCommitter() noexcept {
        if (Enabled()) HostThreadSlot<bool, DrawProfileMeasurements>::Set(true);
    }

    static bool OnCommitter() noexcept {
        return Enabled() && HostThreadSlot<bool, DrawProfileMeasurements>::Get();
    }

    static void Break(ProofBreakSite site) noexcept {
        if (Enabled()) Get().breaks[static_cast<std::size_t>(site)].fetch_add(1, std::memory_order_relaxed);
    }

    static void Commit(std::size_t kind) noexcept {
        if (Enabled()) Get().commits[kind].fetch_add(1, std::memory_order_relaxed);
    }

    static void Count(DrawProfileCount which, std::uint64_t value = 1) noexcept {
        if (Enabled()) Get().counts[static_cast<std::size_t>(which)].fetch_add(value, std::memory_order_relaxed);
    }

    static void Maximum(DrawProfileCount which, std::uint64_t value) noexcept {
        if (!Enabled()) return;
        auto& counter = Get().counts[static_cast<std::size_t>(which)];
        auto previous = counter.load(std::memory_order_relaxed);
        while (previous < value && !counter.compare_exchange_weak(previous, value, std::memory_order_relaxed)) {}
    }

    static void Dispatch(const std::array<std::uint64_t, DriverDetail::DriverPhaseCount>& ns,
        const std::array<std::uint64_t, DriverDetail::DriverPhaseCount>& calls, std::uint64_t totalNs) {
        if (!Enabled()) return;
        auto& totals = Get();
        std::lock_guard lock(totals.dispatchMutex);
        for (std::size_t i = 0; i < ns.size(); ++i) {
            totals.dispatchNs[i] += ns[i];
            totals.dispatchCalls[i] += calls[i];
        }
        totals.dispatchNs.back() += totalNs;
        ++totals.dispatchCalls.back();
    }

    template<typename TPrint>
    static void Print(TPrint&& print) {
        if (!Enabled()) return;
        auto& totals = Get();
        const auto counter = [&](const char* group, const char* name, auto& value) {
            print("[draws] counts (10 s): %s%s=%llu\n", group, name,
                static_cast<unsigned long long>(value.exchange(0, std::memory_order_relaxed)));
        };
        for (std::size_t i = 0; i < totals.breaks.size(); ++i) counter("breaks/", DrawProfileBreakNames[i], totals.breaks[i]);
        for (std::size_t i = 0; i < totals.commits.size(); ++i) counter("commits/", DrawProfileCommitNames[i], totals.commits[i]);
        for (std::size_t i = 0; i < static_cast<std::size_t>(DrawProfileCount::ReadTargetCollectedBytes); ++i) counter("", DrawProfileCountNames[i], totals.counts[i]);
        print("[draws] counts (10 s): readTarget/collected-bytes=%llu readTarget/collect-ns=%llu\n",
            static_cast<unsigned long long>(totals.counts[static_cast<std::size_t>(DrawProfileCount::ReadTargetCollectedBytes)].exchange(0, std::memory_order_relaxed)),
            static_cast<unsigned long long>(totals.counts[static_cast<std::size_t>(DrawProfileCount::ReadTargetCollectNs)].exchange(0, std::memory_order_relaxed)));
        std::lock_guard lock(totals.dispatchMutex);
        for (std::size_t i = 0; i < totals.dispatchNs.size(); ++i) {
            const auto ns = std::exchange(totals.dispatchNs[i], 0);
            const auto calls = std::exchange(totals.dispatchCalls[i], 0);
            const auto* name = i == DriverDetail::DriverPhaseCount ? "total" : DriverDetail::DriverPhaseNames[i];
            print("[draws] subphases (10 s): dispatch/%s=%.3fms calls=%llu\n", name,
                static_cast<double>(ns) / 1e6, static_cast<unsigned long long>(calls));
        }
    }

private:
    static DrawProfileMeasurements& Get() noexcept {
        static DrawProfileMeasurements totals;
        return totals;
    }

    std::array<std::atomic<std::uint64_t>, DrawProfileBreakNames.size()> breaks{};
    std::array<std::atomic<std::uint64_t>, DrawProfileCommitNames.size()> commits{};
    std::array<std::atomic<std::uint64_t>, DrawProfileCountNames.size()> counts{};
    std::mutex dispatchMutex;
    std::array<std::uint64_t, DriverDetail::DriverPhaseCount + 1> dispatchNs{}, dispatchCalls{};
};

class DrawDispatchProfile {
public:
    using Clock = std::chrono::steady_clock;

    DrawDispatchProfile() : enabled(DrawProfileMeasurements::OnCommitter()) {
        if (!enabled) return;
        ns.fill(0);
        calls.fill(0);
        began = lap = Clock::now();
    }

    ~DrawDispatchProfile() {
        if (!enabled) return;
        Phase(DriverDetail::PhaseEpilogue);
        DrawProfileMeasurements::Dispatch(ns, calls,
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(lap - began).count()));
    }

    DrawDispatchProfile(const DrawDispatchProfile&) = delete;
    DrawDispatchProfile& operator=(const DrawDispatchProfile&) = delete;

    void Phase(DriverDetail::DriverPhase phase) {
        if (!enabled) return;
        const auto now = Clock::now();
        ns[phase] += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - lap).count());
        ++calls[phase];
        lap = now;
    }

    void Prepare(std::span<const double, 5> ms) {
        if (!enabled) return;
        const auto now = Clock::now();
        const auto elapsed = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - lap).count());
        auto remaining = elapsed;
        for (std::size_t i = 0; i < ms.size(); ++i) {
            const auto part = std::min(remaining, static_cast<std::uint64_t>(std::max(0.0, ms[i]) * 1e6));
            ns[DriverDetail::PhasePrepareKey + i] += part;
            ++calls[DriverDetail::PhasePrepareKey + i];
            remaining -= part;
        }
        ns[DriverDetail::PhasePrepareOther] += remaining;
        ++calls[DriverDetail::PhasePrepareOther];
        lap = now;
    }

private:
    bool enabled;
    Clock::time_point began{}, lap{};
    std::array<std::uint64_t, DriverDetail::DriverPhaseCount> ns, calls;
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
