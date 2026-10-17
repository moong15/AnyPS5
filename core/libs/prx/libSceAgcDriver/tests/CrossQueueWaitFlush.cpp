#include "execution/VulkanTestDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <span>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::uint32_t Low(const volatile void* pointer) {
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(pointer));
}

std::uint32_t High(const volatile void* pointer) {
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(pointer) >> 32u);
}

void WarmDevice() {
    alignas(256) static const std::array<std::uint32_t, 64> code{0xbf810000u};
    const auto address = reinterpret_cast<std::uintptr_t>(code.data());
    std::vector<std::uint32_t> words{
        0xc0027600u, 0x20cu, static_cast<std::uint32_t>(address >> 8u), static_cast<std::uint32_t>(address >> 40u),
        0xc0037600u, 0x207u, 1u, 1u, 1u,
        0xc0017600u, 0x212u, 0u,
        0xc0017600u, 0x213u, 0u,
        0xc0031500u, 1u, 1u, 1u, 0x8041u,
    };
    Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
    AgcDriver::Submit(&packet, 0);
    AgcDriverWaitIdle_nid_postfix();
}

void CheckWait(bool disabled, bool polling, bool wide, bool generalWrite) {
    struct alignas(64) State {
        volatile std::uint64_t label = 0;
        volatile std::uint32_t gate = 0;
    };
    static State state;
    const auto address = reinterpret_cast<std::uintptr_t>(&state.label);
    const std::uint64_t value = wide ? 0x100000001ull : 1ull;
    const auto bytes = std::as_bytes(std::span(&value, 1)).first(wide ? 8 : 4);
    std::vector<std::uint32_t> words;
    if (wide) words.insert(words.end(), {0xc0079300u, 0x13u, Low(&state.label), High(&state.label), 1u, 1u, 0xffffffffu, 0xffffffffu, 0x19u});
    else words.insert(words.end(), {0xc0053c00u, 0x13u, Low(&state.label), High(&state.label), 1u, 0xffffffffu, 0x19u});
    words.insert(words.end(), {0xc0053c00u, 0x13u, Low(&state.gate), High(&state.gate), 1u, 0xffffffffu, 0x19u});
    Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
    if (polling) {
        AgcDriver::Submit(&packet, 0x20);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::uint64_t before = 0;
    {
        AgcDriver::GuestMemory::TagGpuLockThread(0);
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        auto* recorder = AgcDriver::Graphics::Recorder::Active();
        Require(recorder != nullptr, "the driver has no recorder");
        before = recorder->Submissions();
        recorder->Commands();
        if (generalWrite) {
            recorder->OnComplete([address, value] {
                AgcDriver::GuestMemory::StoreOwnBytes(address, std::as_bytes(std::span(&value, 1)).first(4));
            });
            recorder->NotePendingWrite(address, 4);
        } else {
            recorder->AfterCompletions(address, bytes, std::numeric_limits<std::uint64_t>::max(), 0, false);
        }
        Require(recorder->OpenCrossQueueWriteOverlaps(address, bytes.size(), 0x20), "the producer was not recorded on the other queue");
        if (!polling) AgcDriver::Submit(&packet, 0x20);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        Require(recorder->Submissions() == before, "another path submitted the producer while its mutex was held");
    }
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + std::chrono::milliseconds(disabled ? 250 : 500);
    bool submitted = false;
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::unique_lock lock(AgcDriver::GuestMemory::GpuMutex(), std::try_to_lock);
            if (lock.owns_lock()) {
                const auto* recorder = AgcDriver::Graphics::Recorder::Active();
                submitted = recorder != nullptr && recorder->Submissions() > before;
            }
        }
        if (submitted && state.label == value) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool landed = state.label == value;
    state.gate = 1;
    AgcDriverWaitIdle_nid_postfix();
    {
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        if (auto* recorder = AgcDriver::Graphics::Recorder::Active()) recorder->Sync();
    }
    Require(disabled ? !submitted && !landed : submitted && landed, disabled ? "the disabled cross-queue shortcut flushed the producer" : "the cross-queue wait did not submit and land the producer within 500 ms");
    Require(state.label == value, "the producer label did not land during cleanup");
}

}

int main(int argc, char** argv) {
    try {
        {
            const auto probe = OpenVulkanTestDevice();
            if (!probe) return VulkanTestSkipped;
        }
        WarmDevice();
        const char* mode = argc > 1 ? argv[1] : "";
        CheckWait(std::strcmp(mode, "--disabled") == 0, std::strcmp(mode, "--poll") == 0, std::strcmp(mode, "--wide") == 0, std::strcmp(mode, "--write") == 0);
        AgcDriverShutdown_nid_postfix();
        std::puts("cross-queue wait flush tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        try { AgcDriverShutdown_nid_postfix(); } catch (...) {}
        return 1;
    }
}
