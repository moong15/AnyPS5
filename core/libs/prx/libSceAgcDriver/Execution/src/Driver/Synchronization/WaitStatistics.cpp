#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/MemoryWaitWakeup.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include <cstdlib>
#include <cstring>
#include <exception>
#include <mutex>
#include <algorithm>
#include <array>
#include <atomic>
#include <map>

namespace AgcDriver::DriverDetail {

namespace {

std::mutex memoryWaitMutex;
MemoryWaitWakeup* memoryWaitHead = nullptr;
std::atomic<std::size_t> memoryWaiters{0};

bool MemoryWaitProfiled() {
    static const bool enabled = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    return enabled;
}

std::uint64_t MemoryWaitValue(std::uint64_t address, std::size_t bytes) {
    return bytes == 8 ? *reinterpret_cast<const volatile std::uint64_t*>(address) : *reinterpret_cast<const volatile std::uint32_t*>(address);
}

void RecordMemoryWait(std::uint32_t queue, MemoryWaitSource source, double us, double delayUs, std::uint64_t notifications, std::uint64_t fallbacks, double fallbackTotalUs, double fallbackMaxUs) {
    struct Total {
        std::uint64_t count = 0, delaySamples = 0, notified = 0, timedPolls = 0;
        double totalUs = 0, maxUs = 0, delayTotalUs = 0, delayMaxUs = 0, sleepTotalUs = 0, sleepMaxUs = 0;
    };
    struct Queue {
        std::array<Total, static_cast<std::size_t>(MemoryWaitSource::Count)> sources{};
        std::chrono::steady_clock::time_point reported = std::chrono::steady_clock::now();
    };
    static thread_local std::map<std::uint32_t, Queue> queues;
    auto& stats = queues[queue];
    auto& total = stats.sources[static_cast<std::size_t>(source)];
    ++total.count;
    total.totalUs += us;
    total.maxUs = std::max(total.maxUs, us);
    total.notified += notifications;
    total.timedPolls += fallbacks;
    total.sleepTotalUs += fallbackTotalUs;
    total.sleepMaxUs = std::max(total.sleepMaxUs, fallbackMaxUs);
    if (delayUs >= 0) {
        ++total.delaySamples;
        total.delayTotalUs += delayUs;
        total.delayMaxUs = std::max(total.delayMaxUs, delayUs);
    }
    if (std::chrono::steady_clock::now() - stats.reported < std::chrono::seconds(10)) return;
    static constexpr const char* names[] = {"unknown-memory", "cpu-other-queue", "gpu-fence-observed", "deferred-store", "guest-cpu", "first-poll", "held-shortcut", "recorded-shortcut", "cancelled", "cpu-same-queue"};
    for (std::size_t i = 0; i < stats.sources.size(); ++i) {
        const auto& item = stats.sources[i];
        if (item.count == 0) continue;
        AgcDriver::ProfilePrint_nid_no_patch("[memory-wait] queue 0x%x source %s: count %llu total_us %.1f max_us %.1f observed_delay_samples %llu observed_delay_total_us %.1f observed_delay_max_us %.1f notified %llu fallback_polls %llu fallback_total_us %.1f fallback_max_us %.1f\n", queue, names[i], static_cast<unsigned long long>(item.count), item.totalUs, item.maxUs, static_cast<unsigned long long>(item.delaySamples), item.delayTotalUs, item.delayMaxUs, static_cast<unsigned long long>(item.notified), static_cast<unsigned long long>(item.timedPolls), item.sleepTotalUs, item.sleepMaxUs);
    }
    stats = Queue{};
}

}

bool MemoryWaitWakeupEnabled() {
    static const bool enabled = std::getenv("APS5_NO_MEMORY_WAIT_WAKEUP") == nullptr;
    return enabled;
}

MemoryWaitWakeup::MemoryWaitWakeup(std::span<const std::uint32_t> packet, std::uint32_t queue)
    : packet(packet), queue(queue), address(packet[2] | (static_cast<std::uint64_t>(packet[3]) << 32u)), bytes(Pm4::WaitAwaitedBytes(packet)), profile(MemoryWaitProfiled()), linked(MemoryWaitWakeupEnabled() || profile), exceptions(std::uncaught_exceptions()) {
    if (profile) started = std::chrono::steady_clock::now();
    if (!linked) return;
    const std::lock_guard lock(memoryWaitMutex);
    next = memoryWaitHead;
    memoryWaitHead = this;
    memoryWaiters.fetch_add(1, std::memory_order_release);
}

MemoryWaitWakeup::~MemoryWaitWakeup() {
    if (linked) {
        const std::lock_guard lock(memoryWaitMutex);
        auto** entry = &memoryWaitHead;
        while (*entry != this) entry = &(*entry)->next;
        *entry = next;
        memoryWaiters.fetch_sub(1, std::memory_order_release);
    }
    if (!profile) return;
    try {
        const auto now = std::chrono::steady_clock::now();
        double delayUs = -1;
        if (std::uncaught_exceptions() != exceptions) source = MemoryWaitSource::Cancelled;
        else if (source != MemoryWaitSource::FirstPoll && source != MemoryWaitSource::Held && !Pm4::WaitSatisfiedUnchecked(packet)) source = MemoryWaitSource::Recorded;
        else if (source != MemoryWaitSource::FirstPoll && source != MemoryWaitSource::Held) {
            if (observed == std::chrono::steady_clock::time_point{} || (observedExact && MemoryWaitValue(address, bytes) != observedValue)) source = MemoryWaitSource::Unknown;
            else if (observedExact) delayUs = std::chrono::duration<double, std::micro>(now - observed).count();
        }
        RecordMemoryWait(queue, source, std::chrono::duration<double, std::micro>(now - started).count(), delayUs, notifications, fallbacks, fallbackTotalUs, fallbackMaxUs);
    } catch (...) {}
}

void MemoryWaitWakeup::FirstPollSatisfied() {
    if (!profile) return;
    const std::lock_guard lock(memoryWaitMutex);
    source = MemoryWaitSource::FirstPoll;
}

void MemoryWaitWakeup::HeldAtSubmit() {
    if (!profile) return;
    const std::lock_guard lock(memoryWaitMutex);
    source = MemoryWaitSource::Held;
}

bool MemoryWaitWakeup::Wait(std::chrono::microseconds fallback) {
    const auto began = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const auto countFallback = [&] {
        if (!profile) return;
        ++fallbacks;
        const auto us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - began).count();
        fallbackTotalUs += us;
        fallbackMaxUs = std::max(fallbackMaxUs, us);
    };
    if (!MemoryWaitWakeupEnabled()) {
        PollSleep();
        countFallback();
        return false;
    }
    std::unique_lock lock(memoryWaitMutex);
    const bool notified = changed.wait_for(lock, fallback, [&] { return generation != seen; });
    seen = generation;
    if (!notified) countFallback();
    else if (profile) ++notifications;
    return notified;
}

void MemoryWaitWakeup::Notify(std::uint64_t address, std::size_t bytes, MemoryWaitSource source, std::uint32_t queue, std::span<const std::byte> stored) {
    if ((!MemoryWaitWakeupEnabled() && !MemoryWaitProfiled()) || bytes == 0 || memoryWaiters.load(std::memory_order_acquire) == 0) return;
    const std::lock_guard lock(memoryWaitMutex);
    for (auto* waiter = memoryWaitHead; waiter != nullptr; waiter = waiter->next) {
        const bool overlaps = address <= waiter->address ? waiter->address - address < bytes : address - waiter->address < waiter->bytes;
        if (!overlaps) continue;
        ++waiter->generation;
        if (waiter->profile && waiter->source != MemoryWaitSource::FirstPoll && address <= waiter->address && waiter->address - address <= stored.size() && waiter->bytes <= stored.size() - (waiter->address - address)) {
            std::uint64_t value = 0;
            std::memcpy(&value, stored.data() + (waiter->address - address), waiter->bytes);
            if (Pm4::WaitComparesValue(waiter->packet, value)) {
                if (!waiter->observedExact || waiter->observedValue != value) waiter->observed = std::chrono::steady_clock::now();
                waiter->source = source == MemoryWaitSource::CpuLabel && queue == waiter->queue ? MemoryWaitSource::CpuSameQueue : source;
                waiter->observedValue = value;
                waiter->observedExact = true;
            } else {
                waiter->source = MemoryWaitSource::Unknown;
                waiter->observed = {};
                waiter->observedExact = false;
            }
        }
        if (waiter->profile && waiter->source != MemoryWaitSource::FirstPoll && !stored.empty() && (address > waiter->address || waiter->address - address > stored.size() || waiter->bytes > stored.size() - (waiter->address - address))) {
            waiter->source = MemoryWaitSource::Unknown;
            waiter->observed = {};
            waiter->observedExact = false;
        }
        if (waiter->profile && waiter->source != MemoryWaitSource::FirstPoll && source == MemoryWaitSource::GpuFence && !waiter->observedExact && address <= waiter->address && waiter->address - address <= bytes && waiter->bytes <= bytes - (waiter->address - address)) {
            const auto value = MemoryWaitValue(waiter->address, waiter->bytes);
            if (Pm4::WaitComparesValue(waiter->packet, value)) {
                waiter->source = source;
                waiter->observedValue = value;
                waiter->observed = std::chrono::steady_clock::now();
                waiter->observedExact = true;
            }
        }
        if (MemoryWaitWakeupEnabled()) waiter->changed.notify_one();
    }
}


PollStats& Driver::pollStats() {
    static thread_local PollStats stats;
    return stats;
}

WaitOutcomes& Driver::waitOutcomes() {
    static thread_local WaitOutcomes outcomes;
    return outcomes;
}

Graphics::Recorder::LateStatistics& Driver::lateCountsSeen() {
    static thread_local Graphics::Recorder::LateStatistics seen{};
    return seen;
}

EpochBumps& Driver::epochBumps() {
    static thread_local EpochBumps bumps;
    return bumps;
}

std::chrono::steady_clock::time_point& Driver::lastEpochBump() {
    static thread_local std::chrono::steady_clock::time_point at{};
    return at;
}

std::uint32_t& Driver::workerQueue() {
    static thread_local std::uint32_t queue = Driver::QueueUnknown;
    return queue;
}

void Driver::bumpEpoch(std::uint64_t EpochBumps::*counter) {
    lastEpochBump() = std::chrono::steady_clock::now();
    GuestMemory::BumpCollectEpoch();
    DrawPipeline::BumpEpoch(workerQueue());
    ++(epochBumps().*counter);
}

bool Driver::packetEpoch() {
    static const bool packet = std::getenv("APS5_PACKET_EPOCH") != nullptr;
    return packet;
}

}
