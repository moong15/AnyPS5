#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "VulkanTestDevice.hpp"
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using AgcDriver::DriverDetail::DrawPipeline;
using AgcDriver::Graphics::Require;

constexpr std::uint32_t MarkerOpcode = 0x3f;
alignas(256) std::array<std::uint32_t, 4> Label{};

void WriteLabel(std::vector<std::uint32_t>& words, std::uint32_t value) {
    const auto pointer = reinterpret_cast<std::uintptr_t>(Label.data());
    words.insert(words.end(), {0xc0033700u, 0x00100200u, static_cast<std::uint32_t>(pointer), static_cast<std::uint32_t>(pointer >> 32u), value});
}

void PredicatedIndirectBuffer(std::vector<std::uint32_t>& words, const std::vector<std::uint32_t>& target) {
    const auto pointer = reinterpret_cast<std::uintptr_t>(target.data());
    words.insert(words.end(), {0xc0023f01u, static_cast<std::uint32_t>(pointer), static_cast<std::uint32_t>(pointer >> 32u), static_cast<std::uint32_t>(target.size())});
}

void Submit(std::vector<std::uint32_t>& words) {
    Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
    AgcDriver::Submit(&packet, 0);
}

template<typename TReady>
bool WaitFor(TReady ready) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!ready()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

void Run(bool drains) {
    std::vector<std::uint32_t> warm;
    WriteLabel(warm, 1);
    Submit(warm);
    AgcDriverWaitIdle_nid_postfix();
    Require(Label[0] == 1, "the warm-up WRITE_DATA did not land");

    std::vector<std::uint32_t> target;
    WriteLabel(target, 3);
    std::vector<std::uint32_t> words;
    WriteLabel(words, 2);
    PredicatedIndirectBuffer(words, target);
    const auto drainsBefore = DrawPipeline::DrainRequestsByOpcode(MarkerOpcode);
    bool reached = false;
    {
        std::unique_lock gpuLock(AgcDriver::GuestMemory::GpuMutex());
        Submit(words);
        if (drains) reached = WaitFor([&] { return DrawPipeline::DrainRequestsByOpcode(MarkerOpcode) > drainsBefore; });
        else reached = WaitFor([] { return DrawPipeline::QueuedItems() >= 2; });
    }
    AgcDriverWaitIdle_nid_postfix();
    const auto markerDrains = DrawPipeline::DrainRequestsByOpcode(MarkerOpcode) - drainsBefore;
    Require(Label[0] == 3, "the WRITE_DATA inside the predicated indirect buffer did not land last (label " + std::to_string(Label[0]) + ")");
    if (drains) {
        Require(reached && markerDrains != 0, "with APS5_NO_INACTIVE_PREDICATE_KEEP=1 a predicated indirect buffer marker behind a queued commit did not drain the draw pipeline");
    } else {
        Require(reached, "queue 0 did not pass a predicated indirect buffer marker while predication was off and a queued commit was held");
        Require(markerDrains == 0, "a predicated indirect buffer marker drained the draw pipeline while predication was off (" + std::to_string(markerDrains) + " drains)");
    }
}

}

int main(int argc, char** argv) {
    const bool drains = argc > 1 && std::strcmp(argv[1], "--drains") == 0;
#ifdef _WIN32
    _putenv_s("APS5_PIPELINED_DRAWS", "1");
#else
    setenv("APS5_PIPELINED_DRAWS", "1", 1);
#endif
    try {
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
        }
        Run(drains);
        AgcDriverShutdown_nid_postfix();
        std::puts("pipelined inactive predicate marker tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        AgcDriverShutdown_nid_postfix();
        return 1;
    }
}
