#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/MemoryWaitWakeup.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <span>
#include <stdexcept>
#include <thread>

namespace {

using namespace AgcDriver::DriverDetail;
using namespace std::chrono_literals;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::array<std::uint32_t, 7> WaitPacket(std::uint32_t& label) {
    const auto address = reinterpret_cast<std::uintptr_t>(&label);
    return {0xc0053c00u, 0x13u, static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u), 1u, 0xffffffffu, 0x19u};
}

void WriteLabel(std::uint32_t& label, std::uint32_t value) {
    AgcDriver::GuestMemory::SetCurrentPacket(0x37, 0);
    AgcDriver::GuestMemory::Write(reinterpret_cast<std::uintptr_t>(&label), std::as_bytes(std::span(&value, 1)), 4);
}

void CheckWake(bool disabled) {
    alignas(64) std::uint32_t label = 0;
    const auto packet = WaitPacket(label);
    MemoryWaitWakeup waiter(packet, 0x20);
    std::promise<void> ready;
    auto entered = ready.get_future();
    std::promise<bool> result;
    auto done = result.get_future();
    std::jthread thread([&] {
        try {
            ready.set_value();
            result.set_value(waiter.Wait(2s));
        } catch (...) {
            result.set_exception(std::current_exception());
        }
    });
    entered.wait();
    const auto signaled = std::chrono::steady_clock::now();
    WriteLabel(label, 1);
    Require(done.wait_for(500ms) == std::future_status::ready, "label notification failed to interrupt the long fallback");
    Require(done.get() == !disabled, "wakeup switch did not select the expected sleep path");
    thread.join();
    Require(std::chrono::steady_clock::now() - signaled < 500ms, "label wake exceeded the scheduling allowance");
    Require(AgcDriver::Pm4::WaitSatisfiedUnchecked(packet) && label == 1, "notification completed without the awaited value");
}

void CheckPendingNotification(bool disabled) {
    alignas(64) std::uint32_t label = 0;
    const auto packet = WaitPacket(label);
    MemoryWaitWakeup waiter(packet, 0x20);
    MemoryWaitWakeup::Notify(reinterpret_cast<std::uintptr_t>(&label), sizeof(label), MemoryWaitSource::Unknown, 0);
    Require(waiter.Wait(20ms) == !disabled, "a notification between poll and sleep was lost");
    Require(!AgcDriver::Pm4::WaitSatisfiedUnchecked(packet), "a notification changed the memory predicate");
    WriteLabel(label, 2);
    Require(waiter.Wait(20ms) == !disabled, "a mismatching label did not issue a wake hint");
    Require(!AgcDriver::Pm4::WaitSatisfiedUnchecked(packet), "a mismatching label satisfied the wait");
    WriteLabel(label, 1);
    Require(waiter.Wait(20ms) == !disabled && AgcDriver::Pm4::WaitSatisfiedUnchecked(packet), "a matching label was missed after a false hint");
}

void CheckFallback(bool disabled) {
    alignas(64) std::uint32_t label = 0;
    const auto packet = WaitPacket(label);
    MemoryWaitWakeup waiter(packet, 0x20);
    const auto started = std::chrono::steady_clock::now();
    Require(!waiter.Wait(10ms), "a never-written label received a notification");
    if (!disabled) Require(std::chrono::steady_clock::now() - started >= 10ms, "bounded fallback returned before its deadline");
    Require(!AgcDriver::Pm4::WaitSatisfiedUnchecked(packet) && label == 0, "fallback fabricated label success");
    label = 1;
    Require(!waiter.Wait(1ms) && AgcDriver::Pm4::WaitSatisfiedUnchecked(packet), "fallback polling missed an unnotified CPU store");
}

void CheckWide(bool disabled) {
    alignas(64) std::uint64_t label = 0;
    const auto address = reinterpret_cast<std::uintptr_t>(&label);
    const std::array<std::uint32_t, 9> packet{0xc0079300u, 0x13u, static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u), 1u, 1u, 0xffffffffu, 0xffffffffu, 0x19u};
    MemoryWaitWakeup waiter(packet, 0x20);
    const std::uint32_t low = 1;
    AgcDriver::GuestMemory::Write(address, std::as_bytes(std::span(&low, 1)), 4);
    Require(waiter.Wait(20ms) == !disabled && !AgcDriver::Pm4::WaitSatisfiedUnchecked(packet), "a partial wide label satisfied the wait");
    const std::uint64_t value = 0x100000001ull;
    AgcDriver::GuestMemory::Write(address, std::as_bytes(std::span(&value, 1)), 8);
    Require(waiter.Wait(20ms) == !disabled && AgcDriver::Pm4::WaitSatisfiedUnchecked(packet), "a wide label notification was missed");
}

void CheckDriverWait() {
    alignas(64) static std::uint32_t label = 0;
    const auto words = WaitPacket(label);
    Packet submission{const_cast<std::uint32_t*>(words.data()), static_cast<std::uint32_t>(words.size()), 0, {}};
    AgcDriver::Submit(&submission, 0x20);
    std::promise<void> result;
    auto done = result.get_future();
    std::jthread idle([&] {
        try {
            AgcDriverWaitIdle_nid_postfix();
            result.set_value();
        } catch (...) {
            result.set_exception(std::current_exception());
        }
    });
    struct Shutdown {
        ~Shutdown() { try { AgcDriverShutdown_nid_postfix(); } catch (...) {} }
    } shutdown;
    const bool blocked = done.wait_for(20ms) == std::future_status::timeout;
    const auto address = reinterpret_cast<std::uintptr_t>(&label);
    std::array<std::uint32_t, 5> write{0xc0033700u, 0x00100200u, static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u), 2u};
    Packet producer{write.data(), static_cast<std::uint32_t>(write.size()), 0, {}};
    AgcDriver::Submit(&producer, 0);
    const bool mismatchBlocked = done.wait_for(20ms) == std::future_status::timeout;
    write[4] = 1;
    const auto signaled = std::chrono::steady_clock::now();
    AgcDriver::Submit(&producer, 0);
    const bool resumed = done.wait_for(500ms) == std::future_status::ready;
    if (!resumed) AgcDriverShutdown_nid_postfix();
    idle.join();
    Require(blocked && mismatchBlocked && resumed, "driver wait failed to block on the actual value and resume after the other queue's label");
    done.get();
    Require(label == 1 && std::chrono::steady_clock::now() - signaled < 500ms, "driver wait resumed without the awaited label or exceeded the allowance");
}

void CheckDisjoint(bool disabled) {
    alignas(64) std::array<std::uint32_t, 2> labels{};
    const auto packet = WaitPacket(labels[0]);
    MemoryWaitWakeup waiter(packet, 0x20);
    WriteLabel(labels[1], 1);
    Require(!waiter.Wait(1ms), "a disjoint label notified the waiter");
    WriteLabel(labels[0], 1);
    Require(waiter.Wait(20ms) == !disabled && AgcDriver::Pm4::WaitSatisfiedUnchecked(packet), "the overlapping label failed to notify");
}

}

int main(int argc, char** argv) {
    try {
        const bool disabled = argc > 1 && std::strcmp(argv[1], "--disabled") == 0;
        Require(MemoryWaitWakeupEnabled() == !disabled, "unexpected initial wakeup switch");
#ifdef _WIN32
        _putenv_s("APS5_NO_MEMORY_WAIT_WAKEUP", disabled ? "" : "1");
#else
        if (disabled) unsetenv("APS5_NO_MEMORY_WAIT_WAKEUP");
        else setenv("APS5_NO_MEMORY_WAIT_WAKEUP", "1", 1);
#endif
        Require(MemoryWaitWakeupEnabled() == !disabled, "wakeup switch was not cached");
        CheckWake(disabled);
        CheckPendingNotification(disabled);
        CheckFallback(disabled);
        CheckDisjoint(disabled);
        CheckWide(disabled);
        CheckDriverWait();
        std::puts("memory wait wakeup tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
