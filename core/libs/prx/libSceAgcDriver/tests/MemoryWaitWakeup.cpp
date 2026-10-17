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
#ifdef _WIN32
#include <windows.h>
#endif

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
            bool notified = false;
            const auto deadline = std::chrono::steady_clock::now() + 500ms;
            do {
                notified |= waiter.Wait();
            } while ((!AgcDriver::Pm4::WaitSatisfiedUnchecked(packet) || (!disabled && !notified)) && std::chrono::steady_clock::now() < deadline);
            result.set_value(AgcDriver::Pm4::WaitSatisfiedUnchecked(packet) && notified == !disabled);
        } catch (...) {
            result.set_exception(std::current_exception());
        }
    });
    entered.wait();
    const auto signaled = std::chrono::steady_clock::now();
    WriteLabel(label, 1);
    Require(done.wait_for(20ms) == std::future_status::ready, "producer store did not resume the polling wait promptly");
    Require(done.get(), "producer store failed to satisfy the wait or disabled notifications were consumed");
    thread.join();
    Require(std::chrono::steady_clock::now() - signaled < 20ms, "label wake exceeded the scheduling allowance");
    Require(AgcDriver::Pm4::WaitSatisfiedUnchecked(packet) && label == 1, "notification completed without the awaited value");
}

void CheckPendingNotification(bool disabled) {
    alignas(64) std::uint32_t label = 0;
    const auto packet = WaitPacket(label);
    MemoryWaitWakeup waiter(packet, 0x20);
    MemoryWaitWakeup::Notify(reinterpret_cast<std::uintptr_t>(&label), sizeof(label), MemoryWaitSource::Unknown, 0);
    Require(waiter.Wait() == !disabled, "a notification between poll and sleep was lost");
    Require(!AgcDriver::Pm4::WaitSatisfiedUnchecked(packet), "a notification changed the memory predicate");
    WriteLabel(label, 2);
    Require(waiter.Wait() == !disabled, "a mismatching label did not issue a wake hint");
    Require(!AgcDriver::Pm4::WaitSatisfiedUnchecked(packet), "a mismatching label satisfied the wait");
    WriteLabel(label, 1);
    Require(waiter.Wait() == !disabled && AgcDriver::Pm4::WaitSatisfiedUnchecked(packet), "a matching label was missed after a false hint");
}

void CheckFallback() {
    alignas(64) std::uint32_t label = 0;
    const auto packet = WaitPacket(label);
    MemoryWaitWakeup waiter(packet, 0x20);
    const auto started = std::chrono::steady_clock::now();
    Require(!waiter.Wait(), "a never-written label received a notification");
    const auto elapsed = std::chrono::steady_clock::now() - started;
    Require(elapsed >= 200us, "fallback returned before the poll sleep elapsed");
    Require(elapsed < 5ms, "200 us fallback exceeded the precise poll sleep allowance");
    Require(!AgcDriver::Pm4::WaitSatisfiedUnchecked(packet) && label == 0, "fallback fabricated label success");
    label = 1;
    Require(!waiter.Wait() && AgcDriver::Pm4::WaitSatisfiedUnchecked(packet), "fallback polling missed an unnotified CPU store");
}

void CheckWide(bool disabled) {
    alignas(64) std::uint64_t label = 0;
    const auto address = reinterpret_cast<std::uintptr_t>(&label);
    const std::array<std::uint32_t, 9> packet{0xc0079300u, 0x13u, static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u), 1u, 1u, 0xffffffffu, 0xffffffffu, 0x19u};
    MemoryWaitWakeup waiter(packet, 0x20);
    const std::uint32_t low = 1;
    AgcDriver::GuestMemory::Write(address, std::as_bytes(std::span(&low, 1)), 4);
    Require(waiter.Wait() == !disabled && !AgcDriver::Pm4::WaitSatisfiedUnchecked(packet), "a partial wide label satisfied the wait");
    const std::uint64_t value = 0x100000001ull;
    AgcDriver::GuestMemory::Write(address, std::as_bytes(std::span(&value, 1)), 8);
    Require(waiter.Wait() == !disabled && AgcDriver::Pm4::WaitSatisfiedUnchecked(packet), "a wide label notification was missed");
}

void CheckDriverWait(std::uint32_t queue) {
    alignas(64) static std::uint32_t label = 0;
    label = 0;
    const auto words = WaitPacket(label);
    Packet submission{const_cast<std::uint32_t*>(words.data()), static_cast<std::uint32_t>(words.size()), 0, {}};
    AgcDriver::Submit(&submission, queue);
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
    const bool blocked = done.wait_for(20ms) == std::future_status::timeout;
    const auto address = reinterpret_cast<std::uintptr_t>(&label);
    std::array<std::uint32_t, 5> write{0xc0033700u, 0x00100200u, static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u), 2u};
    Packet producer{write.data(), static_cast<std::uint32_t>(write.size()), 0, {}};
    AgcDriver::Submit(&producer, queue == 0 ? 0x20 : 0);
    const bool mismatchBlocked = done.wait_for(20ms) == std::future_status::timeout;
    write[4] = 1;
    const auto signaled = std::chrono::steady_clock::now();
    AgcDriver::Submit(&producer, queue == 0 ? 0x20 : 0);
    const bool resumed = done.wait_for(500ms) == std::future_status::ready;
    if (!resumed) AgcDriverShutdown_nid_postfix();
    idle.join();
    Require(blocked && mismatchBlocked && resumed, "driver wait failed to block on the actual value and resume after the other queue's label");
    done.get();
    Require(label == 1 && std::chrono::steady_clock::now() - signaled < 500ms, "driver wait resumed without the awaited label or exceeded the allowance");
}

void CheckCopyBack(bool disabled) {
    alignas(64) std::array<std::uint32_t, 8> labels{};
    const auto packet = WaitPacket(labels[3]);
    MemoryWaitWakeup waiter(packet, 0x20);
    const auto address = reinterpret_cast<std::uintptr_t>(labels.data());
    std::array<std::uint32_t, 8> original{}, current{};
    current[3] = 1;
    AgcDriver::GuestMemory::WriteChanged(address, std::as_bytes(std::span(current)), std::as_bytes(std::span(original)));
    Require(waiter.Wait() == !disabled && AgcDriver::Pm4::WaitSatisfiedUnchecked(packet), "selective copy-back failed to notify after its stores");
    AgcDriver::GuestMemory::WriteChanged(address, std::as_bytes(std::span(current)), std::as_bytes(std::span(current)));
    Require(!waiter.Wait(), "unchanged copy-back notified without a store");
    const std::uint32_t zero = 0;
    const auto target = reinterpret_cast<std::uintptr_t>(&labels[3]);
    AgcDriver::GuestMemory::StoreOwnBytes(target, std::as_bytes(std::span(&zero, 1)));
    Require(waiter.Wait() == !disabled && !AgcDriver::Pm4::WaitSatisfiedUnchecked(packet), "raw byte store missed notification or satisfied a mismatching wait");
    AgcDriver::GuestMemory::StoreOwnBytes(target, sizeof(labels[3]), [&] { labels[3] = 1; });
    Require(waiter.Wait() == !disabled && AgcDriver::Pm4::WaitSatisfiedUnchecked(packet), "callback store failed to notify after the label became visible");
}

#ifdef _WIN32
class TimerResolution {
public:
    TimerResolution() {
        const auto ntdll = GetModuleHandleW(L"ntdll.dll");
        setResolution = ntdll ? reinterpret_cast<SetResolution>(reinterpret_cast<void*>(GetProcAddress(ntdll, "NtSetTimerResolution"))) : nullptr;
        ULONG current = 0;
        Require(setResolution != nullptr && setResolution(5000, TRUE, &current) >= 0, "failed to raise timer resolution to the runtime setting");
    }
    ~TimerResolution() {
        ULONG current = 0;
        setResolution(5000, FALSE, &current);
    }

private:
    using SetResolution = LONG(WINAPI*)(ULONG, BOOLEAN, PULONG);
    SetResolution setResolution = nullptr;
};
#endif

void CheckDisjoint(bool disabled) {
    alignas(64) std::array<std::uint32_t, 2> labels{};
    const auto packet = WaitPacket(labels[0]);
    MemoryWaitWakeup waiter(packet, 0x20);
    WriteLabel(labels[1], 1);
    Require(!waiter.Wait(), "a disjoint label notified the waiter");
    WriteLabel(labels[0], 1);
    Require(waiter.Wait() == !disabled && AgcDriver::Pm4::WaitSatisfiedUnchecked(packet), "the overlapping label failed to notify");
}

}

int main(int argc, char** argv) {
    try {
#ifdef _WIN32
        const TimerResolution timerResolution;
#endif
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
        CheckFallback();
        CheckDisjoint(disabled);
        CheckWide(disabled);
        CheckCopyBack(disabled);
        struct Shutdown {
            ~Shutdown() { try { AgcDriverShutdown_nid_postfix(); } catch (...) {} }
        } shutdown;
        CheckDriverWait(0x20);
        CheckDriverWait(0);
        std::puts("memory wait wakeup tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
