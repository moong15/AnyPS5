#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/RuntimeSwitches.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/Shutdown.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

int main(int argc, char** argv) {
    using AgcDriver::DriverDetail::DispatchVariant;
    using AgcDriver::DriverDetail::Driver;
    using AgcDriver::Graphics::RuntimeSwitch;
    namespace RuntimeSwitches = AgcDriver::Graphics::RuntimeSwitches;
    try {
        const auto check = [](bool condition, const std::string& what) {
            if (!condition) throw std::runtime_error("dispatch cache relocation: " + what);
        };
        const std::array<std::uint32_t, 5> from{0x1000u, 0x1234u, 0x3000u, 0x1234u, 42u};
        auto to = from;
        to[0] = 0x2000u;
        to[2] = 0x2000u;
        DispatchVariant donor;
        donor.runs = {{0x123400001000ull, 0x123400001008ull}, {0x9000u, 0x9004u}, {0x123400003000ull, 0x123400003004ull}};
        donor.runBases = {0, -1, 2};
        donor.relocatableWords = 5u;
        donor.words = {1u, 2u, 3u, 4u};
        donor.shared.emplace_back();
        donor.dataPositions = {0u};
        donor.recipe.store(std::make_shared<AgcDriver::Recipe>(), std::memory_order_release);
        auto compiled = std::make_shared<ShaderRecompiler::RecompileResult>();
        compiled->shaderDataUserWords = {0u, 2u, 4u, 0u};
        const std::array<std::uint32_t, 4> pushed{from[0], from[2], from[4], from[0]};
        compiled->pushConstants.resize(sizeof(pushed));
        std::memcpy(compiled->pushConstants.data(), pushed.data(), sizeof(pushed));
        ShaderRecompiler::DescriptorBinding data{};
        data.role = ShaderRecompiler::DescriptorRole::ShaderData;
        data.guestDescriptor.assign(pushed.begin(), pushed.end());
        compiled->bindings.push_back(std::move(data));
        donor.compiled = compiled;
        const auto moved = Driver::RelocateDispatch(donor, from, to);
        check(moved != nullptr, "two independent pointer moves were refused");
        check(moved->runs == std::vector<std::pair<std::uint64_t, std::uint64_t>>{{0x123400002000ull, 0x123400002008ull}, {0x9000u, 0x9004u}, {0x123400002000ull, 0x123400002004ull}}, "a backward move or a fixed run was rebased incorrectly");
        const std::array<std::uint32_t, 4> expected{to[0], to[2], to[4], to[0]};
        check(std::memcmp(moved->compiled->pushConstants.data(), expected.data(), sizeof(expected)) == 0 && moved->compiled->bindings.front().guestDescriptor == std::vector<std::uint32_t>(expected.begin(), expected.end()), "forwarded pointers were not patched in both push and stored shader data");
        check(donor.runs.front().first == 0x123400001000ull && std::memcmp(compiled->pushConstants.data(), pushed.data(), sizeof(pushed)) == 0, "relocation mutated its donor");
        check(moved->shared.empty() && moved->dataPositions.empty() && moved->recipe.load(std::memory_order_acquire) == nullptr, "address-bound validation, data masks or a stale recipe survived relocation");
        auto refused = to;
        refused[4] = 43u;
        check(Driver::RelocateDispatch(donor, from, refused) == nullptr, "a scalar change was masked");
        refused = to;
        refused[1] ^= 1u;
        check(Driver::RelocateDispatch(donor, from, refused) == nullptr, "an untracked high address word was masked");
        refused = to;
        refused[0] = 0u;
        check(Driver::RelocateDispatch(donor, from, refused) == nullptr, "a null root was relocated");
        refused = to;
        refused[0] |= 1u;
        check(Driver::RelocateDispatch(donor, from, refused) == nullptr, "changed address flags were masked");
        check(Driver::RelocateDispatch(donor, from, from) == nullptr, "an exact key was claimed as relocation");
        donor.runs.front() = {(std::uint64_t{1} << 48u) - 8u, std::uint64_t{1} << 48u};
        check(Driver::RelocateDispatch(donor, from, to) == nullptr, "a pointer move escaped the guest address width");

        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
        }
        constexpr std::size_t BlockBytes = 65536;
        constexpr std::uint32_t Lanes = 32;
#ifdef _WIN32
        auto* block = static_cast<std::uint32_t*>(VirtualAlloc(nullptr, BlockBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
        auto* block = static_cast<std::uint32_t*>(std::aligned_alloc(BlockBytes, BlockBytes));
#endif
        check(block != nullptr, "could not allocate guest data");
        std::fill_n(block, BlockBytes / sizeof(std::uint32_t), 0u);
        GuestAllocations::Mutation().Add(block, BlockBytes, true, true, true);
        auto* output = block + 2048;
        alignas(256) static constexpr std::array<std::uint32_t, 9> code{0xf4000200u, 0xfa000000u, 0xbf8cc07fu, 0x34020082u, 0x7e040208u, 0x4a040481u, 0xe0701000u, 0x80010201u, 0xbf810000u};
        Shader shader{};
        shader.file_header = 0x34333231u;
        shader.version = 0x18u;
        shader.header_size = sizeof(Shader);
        shader.shader_size = sizeof(code);
        shader.code = code.data();
        AgcDriverRegisterShader_nid_postfix(&shader);
        const auto enabled = RuntimeSwitches::Set(RuntimeSwitch::DispatchRelocation);
        const bool evicted = argc > 1 && std::string_view(argv[1]) == "--evicted";
        const auto round = [&](std::size_t offset, std::uint32_t value, std::uint32_t scalar = 0) {
            block[offset] = value;
            std::fill_n(output, Lanes, 0xcafebabeu);
            const auto address = reinterpret_cast<std::uintptr_t>(code.data());
            const auto input = reinterpret_cast<std::uintptr_t>(block + offset);
            const auto out = reinterpret_cast<std::uintptr_t>(output);
            std::vector<std::uint32_t> words{
                0xc0027600u, 0x20cu, static_cast<std::uint32_t>(address >> 8u), static_cast<std::uint32_t>(address >> 40u),
                0xc0037600u, 0x207u, Lanes, 1u, 1u,
                0xc0017600u, 0x213u, 8u << 1u,
                0xc0087600u, 0x240u, static_cast<std::uint32_t>(input), static_cast<std::uint32_t>(input >> 32u), scalar, 0u,
                static_cast<std::uint32_t>(out), static_cast<std::uint32_t>((out >> 32u) & 0xffffu), Lanes * 4u, 0x31016facu,
                0xc0031500u, 1u, 1u, 1u, 0x8041u,
            };
            Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
            check(sceAgcDriverSubmitDcb(&packet) == 0, "submission failed");
            AgcDriverWaitIdle_nid_postfix();
            AgcDriver::GuestMemory::FlushGpuWrites(out, Lanes * sizeof(std::uint32_t));
            for (std::size_t lane = 0; lane < Lanes; ++lane) check(output[lane] == value + 1u, "a moved pointer produced stale output in lane " + std::to_string(lane));
            return Driver::Get().DispatchCacheCounters();
        };
        const auto first = round(0, 41u);
        const auto second = round(32, 41u);
        check(second.relocationHits - first.relocationHits == (enabled ? 1u : 0u), "moving to identical contents did not follow the switch");
        const auto exact = round(0, 41u);
        check(exact.relocationHits == second.relocationHits && exact.inserts == second.inserts, "a relocated hit damaged the exact donor");
        const auto changed = round(64, 71u);
        check(changed.relocationHits == exact.relocationHits && changed.inserts == exact.inserts + 1u, "changed contents did not fall back to a fresh capture");
        const auto again = round(96, 71u);
        check(again.relocationHits - changed.relocationHits == (enabled ? 1u : 0u), "an inserted entry did not learn relocation");
        if (evicted) {
            const auto back = round(0, 41u);
            check(back.relocationHits == again.relocationHits && back.inserts == again.inserts + 1u, "an evicted donor served an alias");
        }
        const auto beforeScalar = Driver::Get().DispatchCacheCounters();
        const auto scalar = round(128, 71u, 17u);
        check(scalar.relocationHits == beforeScalar.relocationHits && scalar.inserts == beforeScalar.inserts + 1u, "a changed non-address word was ignored");
        RuntimeSwitches::Assign(RuntimeSwitch::DispatchRelocation, false);
        const auto disabled = round(160, 71u, 17u);
        check(disabled.relocationHits == scalar.relocationHits && disabled.inserts == scalar.inserts + 1u, "turning relocation off still served an alias");
        LibcRunShutdown_nid_postfix();
        GuestAllocations::Mutation().Remove(block);
#ifdef _WIN32
        VirtualFree(block, 0, MEM_RELEASE);
#else
        std::free(block);
#endif
        std::puts("dispatch cache relocation tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        try { LibcRunShutdown_nid_postfix(); } catch (...) {}
        return 1;
    }
}
