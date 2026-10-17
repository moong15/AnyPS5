#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_MEMORYWAITWAKEUP_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_MEMORYWAITWAKEUP_HPP

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <span>

namespace AgcDriver::DriverDetail {

enum class MemoryWaitSource { Unknown, CpuLabel, GpuFence, DeferredStore, GuestCpu, FirstPoll, Held, Recorded, Cancelled, CpuSameQueue, Count };

bool MemoryWaitWakeupEnabled();

class MemoryWaitWakeup {
public:
    MemoryWaitWakeup(std::span<const std::uint32_t> packet, std::uint32_t queue);
    ~MemoryWaitWakeup();
    MemoryWaitWakeup(const MemoryWaitWakeup&) = delete;
    MemoryWaitWakeup& operator=(const MemoryWaitWakeup&) = delete;
    bool Wait(std::chrono::microseconds fallback = std::chrono::microseconds(200));
    void FirstPollSatisfied();
    void HeldAtSubmit();
    static void Notify(std::uint64_t address, std::size_t bytes, MemoryWaitSource source, std::uint32_t queue, std::span<const std::byte> stored = {});

private:
    std::span<const std::uint32_t> packet;
    std::uint32_t queue;
    std::uint64_t address;
    std::size_t bytes;
    bool profile;
    bool observedExact = false;
    bool linked;
    int exceptions;
    std::condition_variable changed;
    MemoryWaitWakeup* next = nullptr;
    std::uint64_t generation = 0, seen = 0, notifications = 0, fallbacks = 0;
    double fallbackTotalUs = 0, fallbackMaxUs = 0;
    MemoryWaitSource source = MemoryWaitSource::Unknown;
    std::uint64_t observedValue = 0;
    std::chrono::steady_clock::time_point started{}, observed{};
};

}

#endif
