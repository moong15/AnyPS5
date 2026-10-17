#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/WaitMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Queues/SubmissionRing.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/SynchronizationStatistics.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libc/include/HostThreadLocal.hpp"
#include <charconv>
#include <cstring>
#include <stdexcept>
#include <string>
#include <cstdlib>
#include <mutex>
#include <set>
#ifdef _WIN32
#include <windows.h>
#endif
#include <immintrin.h>
#include <algorithm>
#include <tuple>
#include <vector>

namespace AgcDriver::DriverDetail {

namespace {

std::optional<std::uint64_t> LongWaitThreshold() {
    static const auto threshold = []() -> std::optional<std::uint64_t> {
        const char* text = std::getenv("APS5_TRACE_LONG_WAITS");
        if (text == nullptr) return std::nullopt;
        const auto* end = text + std::strlen(text);
        std::uint64_t value = 0;
        const auto [last, error] = std::from_chars(text, end, value);
        if (error != std::errc{} || last != end) throw std::runtime_error("APS5_TRACE_LONG_WAITS must be a whole number of milliseconds");
        return value;
    }();
    return threshold;
}

void ProfileLongWait(std::uint32_t queue, double ms) {
    struct Totals {
        std::atomic<std::uint64_t> count{0}, microseconds{0};
    };
    static std::array<Totals, SubmissionQueueEnd> totals;
    static std::atomic<std::int64_t> reported{std::chrono::steady_clock::now().time_since_epoch().count()};
    if (ms >= 100.0 && queue < totals.size()) {
        totals[queue].count.fetch_add(1, std::memory_order_relaxed);
        totals[queue].microseconds.fetch_add(static_cast<std::uint64_t>(ms * 1000.0), std::memory_order_relaxed);
    }
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    auto previous = reported.load(std::memory_order_relaxed);
    if (now - std::chrono::steady_clock::duration(previous) < std::chrono::seconds(10) || !reported.compare_exchange_strong(previous, now.count(), std::memory_order_relaxed)) return;
    std::string line = "[long-wait] (10 s):";
    for (std::size_t index = 0; index < totals.size(); ++index) {
        const auto count = totals[index].count.exchange(0, std::memory_order_relaxed);
        const auto us = totals[index].microseconds.exchange(0, std::memory_order_relaxed);
        if (count == 0 && us == 0 && index != 0 && index != 0x20) continue;
        char part[160];
        std::snprintf(part, sizeof(part), " queue 0x%x: %llu waits >= 100 ms, %.1f ms;", static_cast<unsigned>(index), static_cast<unsigned long long>(count), static_cast<double>(us) / 1000.0);
        line += part;
    }
    line.back() = '\n';
    AgcDriver::ProfilePrint_nid_no_patch("%s", line.c_str());
}

}

struct LongMemoryWait {
    std::span<const std::uint32_t> packet;
    std::uint32_t queue;
    std::uint64_t serial;
    std::size_t cursor;
    std::optional<std::uint64_t> threshold;
    bool profile;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    std::uint64_t first = 0, initialGeneration = 0, historyStamp = 0;
    std::uint32_t historyQueue = 0;
    bool checked = false, stillReported = false, lockedEntry = false, entryTryFailed = false, timedOut = false;
    const char* resolved = "cancelled/error";
    std::optional<Graphics::Recorder::LabelHit> resolvingLabel;
    std::optional<Graphics::Recorder::WaitProducer> write;
    std::chrono::steady_clock::time_point writeObserved{};
    bool openWrite = false;
    std::chrono::steady_clock::time_point nextObservation{};

    std::uint64_t Address() const { return packet[2] | (static_cast<std::uint64_t>(packet[3]) << 32u); }
    std::uint64_t Value() const {
        return ((packet[0] >> 8u) & 0xffu) == 0x93u ? *reinterpret_cast<const volatile std::uint64_t*>(Address()) : *reinterpret_cast<const volatile std::uint32_t*>(Address());
    }
    void Checked() {
        if (!threshold.has_value()) return;
        checked = true;
        first = Value();
        initialGeneration = Graphics::Recorder::WriteGeneration();
    }
    void Observe(const std::shared_ptr<VulkanDevice>& localDevice) noexcept {
        if (!threshold.has_value()) return;
        const auto now = std::chrono::steady_clock::now();
        if (now < nextObservation) return;
        nextObservation = now + std::chrono::milliseconds(100);
        try {
            if (const auto* recorder = Graphics::Recorder::Active()) {
                write = recorder->DescribeWaitWrite(Address(), Pm4::WaitAwaitedBytes(packet));
                writeObserved = now;
                openWrite = localDevice->OpenWriteOverlaps(Address(), Pm4::WaitAwaitedBytes(packet));
            }
        } catch (...) {}
    }
    void Print(double ms, bool stillWaiting, std::size_t pending) const {
        const bool wide = ((packet[0] >> 8u) & 0xffu) == 0x93u;
        const auto reference = wide ? packet[4] | (static_cast<std::uint64_t>(packet[5]) << 32u) : packet[4];
        const auto mask = wide ? packet[6] | (static_cast<std::uint64_t>(packet[7]) << 32u) : packet[5];
        constexpr std::array names{"always", "lt", "le", "eq", "ne", "ge", "gt", "invalid"};
        char head[768];
        std::snprintf(head, sizeof(head), "[long-wait] queue 0x%x serial %llu cursor %zu opcode 0x%x function %s address 0x%llx bytes %zu reference 0x%llx mask 0x%llx first 0x%llx last 0x%llx waited %.3f ms resolved by %s lockedEntry %d entryTryFailed %d generations %llu->%llu%s timeoutWarned %d pendingWaits %zu snapshotWriteOverlaps %d",
                      queue, static_cast<unsigned long long>(serial), cursor, (packet[0] >> 8u) & 0xffu, names[packet[1] & 7u], static_cast<unsigned long long>(Address()), Pm4::WaitAwaitedBytes(packet), static_cast<unsigned long long>(reference), static_cast<unsigned long long>(mask), static_cast<unsigned long long>(first), static_cast<unsigned long long>(Value()), ms, stillWaiting ? "pending" : resolved, lockedEntry ? 1 : 0, entryTryFailed ? 1 : 0,
                      static_cast<unsigned long long>(initialGeneration), static_cast<unsigned long long>(Graphics::Recorder::WriteGeneration()), stillWaiting ? " still waiting" : "", timedOut ? 1 : 0, pending, Graphics::Recorder::SnapshotWriteOverlaps(Address(), Pm4::WaitAwaitedBytes(packet)) ? 1 : 0);
        std::string line = head;
        const auto producer = [&](const char* kind, const Graphics::Recorder::WaitProducer& item, double age) {
            char part[320];
            std::snprintf(part, sizeof(part), "; %s queue 0x%x batchSerial %llu %s %llu state %s %s %d observed %.3f ms ago", kind, item.queue, static_cast<unsigned long long>(item.serial), std::strcmp(kind, "write") == 0 ? "writeNote" : "recordStamp", static_cast<unsigned long long>(item.stamp), item.signaled ? "GPU-executed" : item.submitted ? "submitted" : item.serial != 0 ? "recorded-only" : "queued-only", std::strcmp(kind, "write") == 0 ? "batchHasCompletions" : "completionPending", item.completion ? 1 : 0, age);
            line += part;
        };
        char completions[192];
        std::snprintf(completions, sizeof(completions), "; pendingCompletionLabels %llu pendingWriteBackCompletions %llu", static_cast<unsigned long long>(Graphics::Recorder::PendingCompletionLabels()), static_cast<unsigned long long>(Graphics::Recorder::PendingWriteBackCompletions()));
        line += completions;
        const auto labels = Graphics::Recorder::DescribeWaitLabels(Address(), wide ? 8 : 4);
        for (const auto& label : labels) producer("label", label, 0.0);
        if (resolvingLabel.has_value()) {
            char part[192];
            std::snprintf(part, sizeof(part), "; resolvingLabel queue 0x%x recordStamp %llu value 0x%llx", resolvingLabel->queue, static_cast<unsigned long long>(resolvingLabel->stamp), static_cast<unsigned long long>(resolvingLabel->value));
            line += part;
        }
        if (historyStamp != 0) {
            char part[192];
            std::snprintf(part, sizeof(part), "; labelHistory queue 0x%x recordStamp %llu state memory-landed", historyQueue, static_cast<unsigned long long>(historyStamp));
            line += part;
        }
        if (write.has_value()) producer("write", *write, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - writeObserved).count());
        if (writeObserved != std::chrono::steady_clock::time_point{}) line += openWrite ? "; observedOpenWrite 1" : "; observedOpenWrite 0";
        line += '\n';
        if (GuestMemory::GpuMutex().HeldByThisThread()) AgcDriver::ProfileOutput_nid_no_patch().Write(std::move(line));
        else std::fputs(line.c_str(), stderr);
    }
    void Tick(std::size_t pending) {
        if (profile) ProfileLongWait(queue, 0.0);
        if (!threshold.has_value() || stillReported) return;
        const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        if (ms < static_cast<double>(*threshold) * 5.0) return;
        stillReported = true;
        Print(ms, true, pending);
    }
    void Finish(std::size_t pending) {
        const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        if (profile) ProfileLongWait(queue, ms);
        if (checked && threshold.has_value() && ms >= static_cast<double>(*threshold)) Print(ms, false, pending);
    }
};

void PollSleep() {
#ifdef _WIN32
    thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (timer != nullptr) {
        LARGE_INTEGER due{};
        due.QuadPart = -2000;
        if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
            WaitForSingleObject(timer, INFINITE);
            return;
        }
    }
#endif
    std::this_thread::sleep_for(std::chrono::microseconds(200));
}

int Driver::waitTimeoutMs() {
    static const int value = [] { const char* text = std::getenv("APS5_GPU_WAIT_TIMEOUT_MS"); return text ? std::atoi(text) : 1000; }();
    return value;
}

bool Driver::pollReapAll() {
    static const bool all = std::getenv("APS5_POLL_REAP") != nullptr;
    return all;
}

bool Driver::pollTryEach() {
    static const bool each = std::getenv("APS5_POLL_TRY_EACH") != nullptr;
    return each;
}

bool Driver::traceLateLabels() {
    static const bool trace = std::getenv("APS5_TRACE_LATE_LABELS") != nullptr;
    return trace;
}

std::vector<Driver::PendingWait>& Driver::pendingWaits() {
    struct WaitsTag {};
    return HostThreadLocal<std::vector<PendingWait>, WaitsTag>();
}

void Driver::notePendingWait(std::span<const std::uint32_t> packet, std::uint32_t queue, std::uint64_t received, std::uint64_t serial, std::size_t cursor) {
    if (packet.size() > std::tuple_size_v<decltype(PendingWait::words)>) return;
    static const bool allQueues = std::getenv("APS5_LAND_WAITS_ALL_QUEUES") != nullptr;
    if (!allQueues && queue == 0) return;
    if (Pm4::WaitSatisfiedUnchecked(packet)) return;
    auto& waits = pendingWaits();
    for (const auto& wait : waits) {
        if (wait.count == packet.size() && std::equal(packet.begin(), packet.end(), wait.words.begin())) return;
    }
    PendingWait wait{};
    std::copy(packet.begin(), packet.end(), wait.words.begin());
    wait.count = packet.size();
    wait.received = received;
    wait.serial = serial;
    wait.cursor = cursor;
    waits.push_back(wait);
}

void Driver::landPendingWaits(std::uint32_t queue) {
    auto& waits = pendingWaits();
    if (waits.empty()) return;
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    struct LandStats {
        std::uint64_t lands = 0, waited = 0;
        double waitedMs = 0, maxMs = 0;
        std::chrono::steady_clock::time_point reported = std::chrono::steady_clock::now();
    };
    static thread_local LandStats stats;
    const PacketHistory none{};
    for (const auto& wait : waits) {
        const auto packet = std::span<const std::uint32_t>(wait.words).first(wait.count);
        ++stats.lands;
        if (Pm4::WaitSatisfiedUnchecked(packet)) continue;
        const auto started = std::chrono::steady_clock::now();
        waitMemory(packet, queue, none, wait.received, false, true, wait.serial, wait.cursor);
        const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        ++stats.waited;
        stats.waitedMs += ms;
        stats.maxMs = std::max(stats.maxMs, ms);
    }
    waits.clear();
    if (profile && std::chrono::steady_clock::now() - stats.reported > std::chrono::seconds(10)) {
        AgcDriver::ProfilePrint_nid_no_patch("[landwait] queue 0x%x (10 s): %llu waits landed before a guest read, %llu had to wait for the GPU: %.1f ms in total, max %.2f ms\n", queue, static_cast<unsigned long long>(stats.lands), static_cast<unsigned long long>(stats.waited), stats.waitedMs, stats.maxMs);
        stats = LandStats{};
    }
}

void Driver::waitMemory(std::span<const std::uint32_t> packet, std::uint32_t queue, const PacketHistory& context, std::uint64_t received, bool heldAtSubmit, bool requireMemory, std::uint64_t serial, std::size_t cursor) {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    static const auto threshold = LongWaitThreshold();
    if (!profile && !threshold.has_value()) {
        waitMemoryImpl<false>(packet, queue, context, received, heldAtSubmit, requireMemory, serial, cursor, nullptr);
        return;
    }
    LongMemoryWait diagnostic{packet, queue, serial, cursor, threshold, profile};
    try {
        waitMemoryImpl<true>(packet, queue, context, received, heldAtSubmit, requireMemory, serial, cursor, &diagnostic);
    } catch (...) {
        try { diagnostic.Finish(pendingWaits().size()); } catch (...) {}
        throw;
    }
    try { diagnostic.Finish(pendingWaits().size()); } catch (...) {}
}

template<bool Diagnose>
void Driver::waitMemoryImpl(std::span<const std::uint32_t> packet, std::uint32_t queue, const PacketHistory& context, std::uint64_t received, bool heldAtSubmit, bool requireMemory, std::uint64_t serial, std::size_t cursor, LongMemoryWait* diagnostic) {

    auto start = std::chrono::steady_clock::now();
    auto lastDone = packetsDone.load();

    GuestMemory::TagGpuLockThread(queue);
    workerQueue() = queue;
    auto& outcomes = waitOutcomes();
    const std::uint64_t awaited = packet[2] | (static_cast<std::uint64_t>(packet[3]) << 32u);
    const std::size_t awaitedBytes = Pm4::WaitAwaitedBytes(packet);
    static const bool traceGpu = std::getenv("APS5_TRACE_GPU") != nullptr;
    if (traceGpu) std::fprintf(stderr, "[gpu] %.1f queue 0x%x waits 0x%llx == 0x%x (now 0x%x)\n", TraceMs(), queue, static_cast<unsigned long long>(packet[2] | (static_cast<std::uint64_t>(packet[3]) << 32u)), packet[4],
                               *reinterpret_cast<const volatile std::uint32_t*>(packet[2] | (static_cast<std::uint64_t>(packet[3]) << 32u)));
    struct WaitTrace {
        bool enabled; std::uint32_t queue; std::uint64_t address; std::chrono::steady_clock::time_point begin;
        ~WaitTrace() {
            if (!enabled) return;
            const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
            if (ms >= 0.5) std::fprintf(stderr, "[gpu] %.1f queue 0x%x wait on 0x%llx done after %.1f ms\n", TraceMs(), queue, static_cast<unsigned long long>(address), ms);
        }
    } waitTrace{traceGpu, queue, packet[2] | (static_cast<std::uint64_t>(packet[3]) << 32u), std::chrono::steady_clock::now()};
    bool warned = false;

    struct EpochPoint {
        bool bump = true;
        ~EpochPoint() {
            if (bump) bumpEpoch(&EpochBumps::waits);
        }
    } epochPoint;
    GuestMemory::CheckRange(reinterpret_cast<const void*>(awaited), awaitedBytes, awaitedBytes);
    if constexpr (Diagnose) diagnostic->Checked();
    if (Pm4::WaitSatisfiedUnchecked(packet)) {
        if constexpr (Diagnose) diagnostic->resolved = "memory at entry";
        ++outcomes.atEntry;
        return;
    }
    if (heldAtSubmit) {
        if constexpr (Diagnose) diagnostic->resolved = "held at submit";
        return;
    }
    if (std::uint32_t writer = queue; !requireMemory && storedSince(packet, awaited, awaitedBytes, received, &writer, Diagnose ? &diagnostic->historyStamp : nullptr)) {
        if constexpr (Diagnose) {
            diagnostic->resolved = "label history";
            diagnostic->historyQueue = writer;
        }
        if (writer != queue) notePendingWait(packet, queue, received, serial, cursor);
        else epochPoint.bump = false;
        return;
    }

    static const bool labelShortcut = std::getenv("APS5_NO_LABEL_SHORTCUT") == nullptr;
    const bool labelTable = labelShortcut && !requireMemory;
    static const bool overlapSubmit = std::getenv("APS5_NO_WAIT_OVERLAP_SUBMIT") == nullptr;

    static const bool waitLock = std::getenv("APS5_WAIT_LOCK") != nullptr;

    std::uint64_t refusedGeneration = 0;

    bool behindCompletion = false;
    const auto traceLate = [&](const char* outcome, const std::optional<Graphics::Recorder::LabelHit>& hit, Graphics::Recorder::LabelRefusal refusal) {
        static std::atomic<int> shown{0};
        if (shown.fetch_add(1) >= 200) return;
        std::fprintf(stderr, "[late] queue 0x%x wait on 0x%llx (function %u ref 0x%x mask 0x%x) received %llu: %s; entry value 0x%llx stamp %llu generation %llu, refusal %d\n", queue, static_cast<unsigned long long>(awaited), packet[1] & 7u, packet[4], packet[5], static_cast<unsigned long long>(received), outcome,
                     static_cast<unsigned long long>(hit ? hit->value : 0), static_cast<unsigned long long>(hit ? hit->stamp : 0), static_cast<unsigned long long>(hit ? hit->generation : 0), static_cast<int>(refusal));
    };
    const auto takeLabel = [&](const std::optional<Graphics::Recorder::LabelHit>& hit, Graphics::Recorder::LabelRefusal refusal, bool polling, bool unlockedHit) {
        behindCompletion = refusal == Graphics::Recorder::LabelRefusal::BehindCompletion;
        const bool trace = traceLateLabels() && (refusal != Graphics::Recorder::LabelRefusal::None || (hit.has_value() && hit->late));
        if (!hit.has_value() || !Pm4::WaitComparesValue(packet, hit->value)) {
            if (trace) traceLate(hit.has_value() ? "value mismatch" : "refused by the table", hit, refusal);
            return false;
        }
        if (hit->late) {
            if (hit->generation == refusedGeneration) return false;
            if (!GuestMemory::UnchangedSinceCollected(awaited, awaitedBytes, hit->generation)) {
                ++outcomes.lateRefusedCpuStore;
                refusedGeneration = hit->generation;
                if (trace) traceLate("refused, CPU store since the group closed", hit, refusal);
                return false;
            }
            ++outcomes.fromRecorderLate;
            if (trace) traceLate("trusted", hit, refusal);
        }
        if constexpr (Diagnose) {
            diagnostic->resolved = polling ? "recorder polling" : "recorder label";
            diagnostic->resolvingLabel = hit;
        }
        ++(polling ? outcomes.fromRecorderPolling : outcomes.fromRecorder);
        if (unlockedHit) ++outcomes.fromRecorderUnlocked;
        if (hit->queue == queue) {
            ++outcomes.fromRecorderSameQueue;
            if (!hit->late) epochPoint.bump = false;
        } else {
            notePendingWait(packet, queue, received, serial, cursor);
        }
        return true;
    };

    std::uint64_t seenGeneration = Graphics::Recorder::WriteGeneration();

    const bool unlocked = !waitLock && overlapSubmit;
    if (unlocked) {
        if (labelTable) {
            Graphics::Recorder::LabelRefusal refusal{};
            if (takeLabel(Graphics::Recorder::LookupLabel(awaited, awaitedBytes, received, &refusal), refusal, false, true)) return;
        }
    }
    const bool lockedEntry = !unlocked || Graphics::Recorder::SnapshotWriteOverlaps(awaited, awaitedBytes);
    if constexpr (Diagnose) diagnostic->lockedEntry = lockedEntry;
    if (!lockedEntry) ++outcomes.entriesUnlocked;

    static const bool blockingEntry = std::getenv("APS5_WAIT_LOCK_ENTRY") != nullptr;
    bool recheck = !lockedEntry;
    bool entryTryFailed = false;
    if (lockedEntry) {
        GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Wait);
        std::unique_lock gpuLock(GuestMemory::GpuMutex(), std::defer_lock);
        if (blockingEntry || !unlocked) {
            gpuLock.lock();
        } else if (!gpuLock.try_lock()) {
            ++outcomes.entryTriesFailed;
            recheck = true;
            entryTryFailed = true;
            if constexpr (Diagnose) diagnostic->entryTryFailed = true;
        }
        if (const auto localDevice = gpuLock.owns_lock() ? device.Load() : std::shared_ptr<VulkanDevice>{}) {
            if constexpr (Diagnose) diagnostic->Observe(localDevice);

            if (labelTable) {
                Graphics::Recorder::LabelRefusal refusal{};
                if (takeLabel(localDevice->PendingLabel(awaited, awaitedBytes, received, &refusal), refusal, false, false)) return;
            }

            if (!overlapSubmit || localDevice->OpenWriteOverlaps(awaited, awaitedBytes)) localDevice->SubmitRecorded(false);
        }
    }

    static const bool pollProfile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    auto& poll = pollStats();
    const auto pollRefused = [&](Graphics::Recorder::LabelRefusal refusal) {
        if (pollProfile && refusal != Graphics::Recorder::LabelRefusal::None && static_cast<std::size_t>(refusal) < poll.refusals.size()) ++poll.refusals[static_cast<std::size_t>(refusal)];
    };

    const bool reapAll = pollReapAll() || !labelShortcut || queue == 0;
    std::chrono::steady_clock::time_point lastTry{};
    const auto pollService = [&](bool sleeping) {
        if (pollProfile) ++poll.ticks;
        const auto generation = Graphics::Recorder::WriteGeneration();
        const bool changed = generation != seenGeneration || recheck;
        const bool labelDue = labelFlushDue();
        const bool reap = sleeping && (behindCompletion || ((reapAll || queue0Dormant.load(std::memory_order_relaxed)) && completionsPending()));
        if (!changed && !labelDue && !reap) return false;

        if (!changed && !pollTryEach()) {
            const auto now = std::chrono::steady_clock::now();
            if (now - lastTry < PollTryInterval) return false;
            lastTry = now;
        }
        if (changed && unlocked && labelTable) {

            Graphics::Recorder::LabelRefusal refusal{};
            if (takeLabel(Graphics::Recorder::LookupLabel(awaited, awaitedBytes, received, &refusal), refusal, true, true)) {
                if (pollProfile) ++poll.tableHits;
                return true;
            }
            pollRefused(refusal);
        }
        if (pollProfile) ++poll.tries;
        std::unique_lock gpuLock(GuestMemory::GpuMutex(), std::try_to_lock);
        if (!gpuLock.owns_lock()) {
            ++triesFailed[TryPoll];
            if (pollProfile) ++poll.triesFailed;
            return false;
        }

        struct TryHold {
            bool enabled;
            PollStats& poll;
            std::chrono::steady_clock::time_point start;
            ~TryHold() {
                if (!enabled) return;
                const auto us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
                poll.tryHoldUs += us;
                poll.longestTryHoldUs = std::max(poll.longestTryHoldUs, us);
            }
        } tryHold{pollProfile, poll, pollProfile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}};
        const auto localDevice = device.Load();
        if (localDevice == nullptr) {
            seenGeneration = generation;
            recheck = false;
            return false;
        }
        if constexpr (Diagnose) diagnostic->Observe(localDevice);
        bool submitted = false;
        if (changed) {
            seenGeneration = generation;
            recheck = false;
            if (labelTable) {
                Graphics::Recorder::LabelRefusal refusal{};
                if (takeLabel(localDevice->PendingLabel(awaited, awaitedBytes, received, &refusal), refusal, true, false)) {
                    if (pollProfile) ++poll.tableHits;
                    return true;
                }
                pollRefused(refusal);
            }
            if (localDevice->OpenWriteOverlaps(awaited, awaitedBytes)) {
                localDevice->SubmitRecorded(false);
                submitted = true;
            }
        }
        if (labelDue && !submitted && Graphics::Recorder::PendingLabelSince().has_value()) {
            localDevice->SubmitRecorded(false);
            submitted = true;
        }
        if (submitted) {
            ++outcomes.pollSubmits;
            if (pollProfile) ++poll.submits;
        }
        if (reap) {
            const auto withWorkBefore = pollProfile ? Graphics::Recorder::ReapsWithWork() : 0;
            localDevice->ReapRecorded();
            ++outcomes.pollReaps;
            if (pollProfile) {
                ++poll.reaps;
                ++(behindCompletion ? poll.reapsBehindCompletion : poll.reapsOther);
                poll.reapsWithWork += Graphics::Recorder::ReapsWithWork() - withWorkBefore;
            }
        }
        return false;
    };

    if (entryTryFailed && pollService(false)) return;

    noteWaitBlocked(queue, awaited, true);
    struct BlockedWait {
        Driver& driver;
        std::uint32_t queue;
        ~BlockedWait() { driver.noteWaitBlocked(queue, 0, false); }
    } blocked{*this, queue};
    static const bool pauseSpin = std::getenv("APS5_NO_PAUSE_SPIN") == nullptr;
    const auto spinLimit = queue == 0 ? std::chrono::microseconds(1500) : std::chrono::microseconds(100);
    const auto spinStart = std::chrono::steady_clock::now();
    bool spinning = pauseSpin;
    std::uint32_t polls = 0;
    StallWatch stall(queue);
    while (!Pm4::WaitSatisfiedUnchecked(packet)) {
        if (std::uint32_t writer = queue; !requireMemory && storedSince(packet, awaited, awaitedBytes, received, &writer, Diagnose ? &diagnostic->historyStamp : nullptr)) {
            if constexpr (Diagnose) {
                diagnostic->resolved = "label history";
                diagnostic->historyQueue = writer;
            }
            if (writer != queue) notePendingWait(packet, queue, received, serial, cursor);
            else epochPoint.bump = false;
            return;
        }
        ++polls;
        if (spinning) {
            _mm_pause();
            if ((polls & 63u) != 0) continue;
            if (std::chrono::steady_clock::now() - spinStart > spinLimit) spinning = false;
        } else if (pauseSpin || polls >= 4000) {
            PollSleep();
        } else {
            std::this_thread::yield();
        }
        CheckFailure();
        if constexpr (Diagnose) {
            try { diagnostic->Tick(pendingWaits().size()); } catch (...) {}
        }
        if (pollService(!spinning)) return;
        noteAwaitingTitle(queue, awaited);
        if (requireMemory && (polls & 15u) == 0) {
            std::unique_lock settleLock(GuestMemory::GpuMutex(), std::try_to_lock);
            if (settleLock.owns_lock()) {
                if (const auto localDevice = device.Load(); localDevice != nullptr) {
                    if constexpr (Diagnose) diagnostic->Observe(localDevice);
                    if (localDevice->RecordedWritesSettled(awaited, awaitedBytes)) {
                        if constexpr (Diagnose) diagnostic->resolved = "recorded writes settled";
                        return;
                    }
                }
            }
        }
        if (const auto done = packetsDone.load(); done != lastDone || (!requireMemory && packetsInFlight.load() != 0)) {
            lastDone = done;
            start = std::chrono::steady_clock::now();
            stall.Progressed();
        }
        stall.Check(((packet[0] >> 8u) & 0xffu) == 0x93u ? "WAIT_REG_MEM_64 at" : "WAIT_REG_MEM at", awaited);

        if (!warned && std::chrono::steady_clock::now() - start > std::chrono::milliseconds(waitTimeoutMs())) {
            warned = true;
            if constexpr (Diagnose) diagnostic->timedOut = true;
            ++outcomes.timedOut;
            {
                static std::mutex reportedMutex;
                static std::set<std::uint64_t> reported;
                static std::uint64_t timeouts = 0;
                const std::lock_guard lock(reportedMutex);
                if (++timeouts % 20 == 0) std::fprintf(stderr, "[gpu] %llu GPU waits have timed out\n", static_cast<unsigned long long>(timeouts));
                if (!reported.insert(packet[2] | (static_cast<std::uint64_t>(packet[3]) << 32u)).second) continue;
            }
            const bool wide = ((packet[0] >> 8u) & 0xffu) == 0x93u;
            const std::uint64_t reference = wide ? packet[4] | (static_cast<std::uint64_t>(packet[5]) << 32u) : packet[4];
            const std::uint64_t mask = wide ? packet[6] | (static_cast<std::uint64_t>(packet[7]) << 32u) : packet[5];
            const std::uint64_t current = wide ? *reinterpret_cast<const volatile std::uint64_t*>(awaited) : *reinterpret_cast<const volatile std::uint32_t*>(awaited);
            std::fprintf(stderr, "[gpu] queue 0x%x WAIT_REG_MEM%s%s at 0x%llx timed out after %dms (function %u ref 0x%llx mask 0x%llx value 0x%llx)\n", queue, wide ? "_64" : "", requireMemory ? " (landing)" : "",
                         static_cast<unsigned long long>(awaited), waitTimeoutMs(), packet[1] & 7u, static_cast<unsigned long long>(reference), static_cast<unsigned long long>(mask), static_cast<unsigned long long>(current));
            const auto address = packet[2] | (static_cast<std::uint64_t>(packet[3]) << 32u);
            for (const auto& record : writeHistory()) {
                if (record.length != 0 && record.target <= address && address < record.target + std::max<std::uint64_t>(record.length, 4))
                    std::fprintf(stderr, "[gpu]   earlier write by queue 0x%x opcode 0x%x at 0x%llx+0x%llx\n", record.queue, record.opcode, static_cast<unsigned long long>(record.target), static_cast<unsigned long long>(record.length));
            }

            if (const auto reportDevice = device.Load(); reportDevice != nullptr) {
                Graphics::Recorder::LabelRefusal refusal{};
                const auto hit = reportDevice->PendingLabel(awaited, awaitedBytes, received, &refusal);
                const auto since = Graphics::Recorder::PendingLabelSince();
                const auto age = since.has_value() ? std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - *since).count() : -1.0;
                std::fprintf(stderr, "[gpu]   recorder: table %s (refusal %u), open batch writes it %d, snapshot write overlaps %d, pending completion labels %llu, write-back completions %llu, pending label age %.1f ms, write generation %llu seen %llu, queue 0 dormant %d, completions pending %d, label due %d\n",
                             hit.has_value() ? "hit" : "miss", static_cast<unsigned>(refusal), reportDevice->OpenWriteOverlaps(awaited, awaitedBytes) ? 1 : 0, Graphics::Recorder::SnapshotWriteOverlaps(awaited, awaitedBytes) ? 1 : 0,
                             static_cast<unsigned long long>(Graphics::Recorder::PendingCompletionLabels()), static_cast<unsigned long long>(Graphics::Recorder::PendingWriteBackCompletions()), age,
                             static_cast<unsigned long long>(Graphics::Recorder::WriteGeneration()), static_cast<unsigned long long>(seenGeneration), queue0Dormant.load(std::memory_order_relaxed) ? 1 : 0, completionsPending() ? 1 : 0, labelFlushDue() ? 1 : 0);
            }
            context.Print(stderr, "[gpu]   preceding packet %s\n");
        }
    }
    if constexpr (Diagnose) diagnostic->resolved = "memory";
    ++outcomes.polled;
}

}
