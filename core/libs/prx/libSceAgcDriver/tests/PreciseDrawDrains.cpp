#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using AgcDriver::DriverDetail::DrawPipeline;

alignas(16) std::array<std::uint32_t, 8> Memory{};

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

template<typename TReady>
void WaitUntil(TReady ready) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!ready()) {
        Require(std::chrono::steady_clock::now() < deadline, "precise draw drains: timed out");
        std::this_thread::yield();
    }
}

std::uint64_t Address(std::size_t word) {
    return reinterpret_cast<std::uintptr_t>(Memory.data() + word);
}

std::array<std::uint32_t, 7> Fill(std::size_t word) {
    const auto destination = Address(word);
    return {0xc0055000u, 2u << 29u, 99u, 0u, static_cast<std::uint32_t>(destination), static_cast<std::uint32_t>(destination >> 32u), 4u};
}

void Exercise(std::vector<std::uint32_t> packet, bool hazard, bool bounded, bool execute = true) {
    auto& pipeline = DrawPipeline::Queue0();
    Memory.fill(7u);
    std::atomic<bool> release{false}, entered{false}, finished{false};
    std::uint32_t observed = 0;
    std::future<bool> work;
    struct Cleanup {
        std::atomic<bool>& release;
        ~Cleanup() {
            release.store(true, std::memory_order_release);
            try { DrawPipeline::Queue0().Drain(DrawPipeline::DrainReason::Submission); } catch (...) {}
        }
    } cleanup{release};
    pipeline.Enqueue([&] {
        entered.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
        observed = Memory[0];
        Memory[1] = 42u;
    }, {{Address(1), Address(2)}}, 0, {}, {{Address(0), Address(1)}}, bounded);
    WaitUntil([&] { return entered.load(std::memory_order_acquire); });
    const auto opcode = (packet[0] >> 8u) & 0xffu;
    const auto before = DrawPipeline::DrainRequestsByOpcode(opcode);
    const bool expectedDrain = hazard || !bounded || !DrawPipeline::PreciseDrains();
    work = std::async(std::launch::async, [&] {
        const bool drained = pipeline.DrainPacket(packet);
        if (execute) {
            AgcDriver::QueueState state;
            AgcDriver::Pm4::Execute(packet, state);
        }
        finished.store(true, std::memory_order_release);
        return drained;
    });
    WaitUntil([&] { return finished.load(std::memory_order_acquire) || DrawPipeline::DrainRequestsByOpcode(opcode) != before; });
    if (expectedDrain) {
        Require(!finished.load(std::memory_order_acquire), "an overlapping or unbounded packet overtook a queued read/write");
    } else {
        Require(finished.load(std::memory_order_acquire), "an unrelated packet drained a bounded queued draw");
        Require(pipeline.Busy(), "the unrelated packet waited for the queued commit");
    }
    release.store(true, std::memory_order_release);
    Require(work.get() == expectedDrain, "unexpected drain decision");
    pipeline.Drain(DrawPipeline::DrainReason::Submission);
    Require(observed == 7u, "the queued draw saw a later DMA write");
    if (execute && opcode == 0x50) {
        const auto destination = packet[4] | (static_cast<std::uint64_t>(packet[5]) << 32u);
        const auto expected = AgcDriver::Pm4::DmaSource(packet) == 2 ? 99u : 42u;
        Require(*reinterpret_cast<const std::uint32_t*>(destination) == expected, "DMA did not see the queued draw's write in order");
    }
}

}

int main() {
    try {
        GuestAllocations::Mutation().Add(Memory.data(), sizeof(Memory), true, true, false);
        for (const auto word : {2u, 0u, 1u}) {
            const auto fill = Fill(word);
            Exercise({fill.begin(), fill.end()}, word != 2, true);
        }
        auto copy = Fill(2);
        copy[1] = 0;
        copy[2] = static_cast<std::uint32_t>(Address(1));
        copy[3] = static_cast<std::uint32_t>(Address(1) >> 32u);
        Exercise({copy.begin(), copy.end()}, true, true);
        const auto unrelated = Fill(2);
        Exercise({unrelated.begin(), unrelated.end()}, false, false);
        for (const auto word : {1u, 2u}) {
            const auto address = Address(word);
            Exercise({0xc0053c00u, 0x13u, static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u), 42u, 0xffffffffu, 4u}, word == 1, true, false);
        }
        Exercise({0xc0022000u, 0u, 0u, 0u}, false, true);
        std::puts("precise draw drain tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
