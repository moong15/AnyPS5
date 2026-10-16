#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;

constexpr std::uint32_t Threads = 64;
alignas(256) constexpr std::array<std::uint32_t, 16> Code{
    0xf4000100u, 0xfa000000u, 0xbf8cc07fu, 0x7e020204u, 0x34040082u,
    0xe0701000u, 0x80020102u, 0xbf810000u, 0xbf810000u,
};
alignas(256) constexpr std::array<std::uint32_t, 16> ValueCode{
    0xf4000100u, 0xfa000000u, 0xbf8cc07fu, 0x7e020200u, 0x4a020204u, 0x34040082u,
    0xe0701000u, 0x80020102u, 0xbf810000u, 0xbf810000u,
};
alignas(256) constexpr std::array<std::uint32_t, 16> NestedCode{
    0xf4040100u, 0xfa000000u, 0xbf8cc07fu, 0xf4000182u, 0xfa000000u, 0xbf8cc07fu,
    0x7e020206u, 0x34040082u, 0xe0701000u, 0x80020102u, 0xbf810000u, 0xbf810000u,
};
alignas(16) std::uint32_t NestedValue = 0x10203040u;
alignas(256) std::array<std::uint32_t, Threads> Output{};
alignas(256) std::array<std::uint32_t, Threads> OtherOutput{};
alignas(16) std::array<std::array<std::uint32_t, 4>, 9> Input{};

struct Header {
    Shader shader{};
    ShaderUserData userData{};
};

alignas(8) Header ComputeHeader{};
alignas(8) Header ValueHeader{};
alignas(8) Header NestedHeader{};

enum class Program { Read, Value, Nested };

void Register(Header& header, std::span<const std::uint32_t> code) {
    header.shader.file_header = 0x34333231u;
    header.shader.version = 0x18u;
    header.shader.header_size = sizeof(Header);
    header.shader.shader_size = static_cast<std::uint32_t>(code.size_bytes());
    header.shader.code = code.data();
    header.shader.user_data = &header.userData;
    AgcDriverRegisterShader_nid_postfix(&header.shader);
}

AgcDriver::DriverDetail::EntryCounters Dispatch(const std::uint32_t* input, Program kind = Program::Read, std::uint32_t* destination = Output.data()) {
    const auto words = std::span(destination, Threads);
    std::fill(words.begin(), words.end(), 0xcafebabeu);
    const auto source = reinterpret_cast<std::uintptr_t>(input);
    const auto output = reinterpret_cast<std::uintptr_t>(destination);
    const auto code = reinterpret_cast<std::uintptr_t>(kind == Program::Value ? ValueCode.data() : kind == Program::Nested ? NestedCode.data() : Code.data());
    const std::array<std::uint32_t, 12> userData{
        static_cast<std::uint32_t>(source), static_cast<std::uint32_t>(source >> 32u), 0, 0, 0, 0, 0, 0,
        static_cast<std::uint32_t>(output), static_cast<std::uint32_t>((output >> 32u) & 0xffffu), sizeof(Output), 0x31016facu,
    };
    std::vector<std::uint32_t> commands;
    const auto registers = [&](std::uint32_t first, std::span<const std::uint32_t> values) {
        commands.push_back(0xc0007600u | (static_cast<std::uint32_t>(values.size()) << 16u));
        commands.push_back(first);
        commands.insert(commands.end(), values.begin(), values.end());
    };
    const std::array<std::uint32_t, 3> shape{Threads, 1, 1};
    const std::array<std::uint32_t, 2> program{static_cast<std::uint32_t>(code >> 8u), static_cast<std::uint32_t>(code >> 40u)};
    const std::array<std::uint32_t, 1> resources{static_cast<std::uint32_t>(userData.size()) << 1u};
    registers(0x207, shape);
    registers(0x20c, program);
    registers(0x213, resources);
    registers(0x240, userData);
    commands.insert(commands.end(), {0xc0031500u, 1, 1, 1, 0x41u});
    Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
    Require(sceAgcDriverSubmitDcb(&packet) == 0, "the compute submission failed");
    AgcDriverWaitIdle_nid_postfix();
    AgcDriver::GuestMemory::FlushGpuWrites(output, sizeof(Output));
    const auto expected = kind == Program::Nested ? NestedValue : input[0] + (kind == Program::Value ? static_cast<std::uint32_t>(source) : 0u);
    for (const auto word : words) Require(word == expected, "the relocated dispatch wrote " + std::to_string(word) + ", expected " + std::to_string(expected));
    return AgcDriver::DriverDetail::Driver::Get().DispatchCacheCounters();
}

void MovedDataHitsTheCache() {
    Register(ComputeHeader, Code);
    Register(ValueHeader, ValueCode);
    Register(NestedHeader, NestedCode);
    for (auto& input : Input) input = {0x12345678u, 0, 0, 0};
    const bool disabled = std::getenv("APS5_NO_DISPATCH_RELOCATION") != nullptr;
    auto counters = Dispatch(Input[0].data());
    Require(counters.lookups == 1 && counters.inserts == 1 && counters.aliasHits == 0, "the first dispatch did not teach the cache");
    for (std::size_t i = 1; i < 8; ++i) counters = Dispatch(Input[i].data());
    Require(counters.lookups == 8 && counters.aliasHits == (disabled ? 0u : 7u) && counters.inserts == (disabled ? 8u : 1u), "equal contents at moved pointers did not follow the expected cache path: " + std::to_string(counters.aliasHits) + " alias hits, " + std::to_string(counters.aliasLookups) + " alias lookups, " + std::to_string(counters.inserts) + " inserts");
    Input[8][0] = 0x87654321u;
    const auto different = Dispatch(Input[8].data());
    Require(different.aliasHits == counters.aliasHits && different.inserts == counters.inserts + 1, "different contents hit a relocated dispatch");
    const auto again = Dispatch(Input[0].data());
    Require(again.equal == 1 && again.aliasHits == different.aliasHits, "an exact-key dispatch did not use the ordinary cache path");
    Input[0][0] = 0xabcdef01u;
    const auto changed = Dispatch(Input[0].data());
    const auto descriptor = Dispatch(Input[1].data(), Program::Read, OtherOutput.data());
    Require(descriptor.aliasHits == changed.aliasHits && descriptor.inserts == changed.inserts + 1, "a changed output descriptor reused a relocated result");
    const auto firstValue = Dispatch(Input[1].data(), Program::Value);
    const auto nextValue = Dispatch(Input[2].data(), Program::Value);
    Require(nextValue.aliasHits == firstValue.aliasHits && nextValue.inserts == firstValue.inserts + 1, "a pointer also used as a value was removed from the key");
    const auto nested = reinterpret_cast<std::uintptr_t>(&NestedValue);
    for (std::size_t i = 3; i < 6; ++i) Input[i] = {static_cast<std::uint32_t>(nested), static_cast<std::uint32_t>(nested >> 32u), 0, 0};
    const auto firstNested = Dispatch(Input[3].data(), Program::Nested);
    NestedValue = 0x50607080u;
    const auto changedNested = Dispatch(Input[4].data(), Program::Nested);
    Require(changedNested.aliasHits == firstNested.aliasHits && changedNested.inserts == firstNested.inserts + 1 && changedNested.aliasValidation == firstNested.aliasValidation + (disabled ? 0u : 1u), "an alias with changed nested contents was not refused by value validation");
    const auto nextNested = Dispatch(Input[5].data(), Program::Nested);
    Require(nextNested.aliasHits == changedNested.aliasHits + (disabled ? 0u : 1u), "equal nested contents did not reuse the relocated dispatch");
}

}

int main() {
    try {
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
        }
        MovedDataHitsTheCache();
        AgcDriverShutdown_nid_postfix();
        std::cout << "Dispatch relocation and output readback passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        AgcDriverShutdown_nid_postfix();
        return 1;
    }
}
