#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "VulkanTestDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using AgcDriver::DriverDetail::DrawMiss;

alignas(256) constexpr std::array<std::uint32_t, 6> VertexCode{0xe0382000u, 0x80020005u, 0xbf8c3f70u, 0xf80008cfu, 0x03020100u, 0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 8> PixelCode{0xf4000100u, 0xfa000000u, 0x7e000204u, 0x7e020280u, 0xf800180fu, 0x00010100u, 0xbf810000u, 0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 6> InlinePixelCode{0x7e000200u, 0x7e020280u, 0xf800180fu, 0x00010100u, 0xbf810000u, 0xbf810000u};
bool InlineData = false;
alignas(4096) std::array<std::array<std::byte, 65536>, 2> ColorMemory{};
alignas(4096) std::array<std::array<std::byte, 65536>, 2> DepthMemory{};
alignas(16) constexpr std::array<std::array<float, 4>, 3> Vertices{{{-3, -1, 0.5f, 1}, {1, -1, 0.5f, 1}, {1, 3, 0.5f, 1}}};

struct Header {
    Shader shader{};
    ShaderUserData userData{};
};

alignas(8) Header VertexHeader{};
alignas(8) Header PixelHeader{};

const Shader* Registered(Header& header, const std::uint32_t* code, std::size_t bytes, std::uint8_t type) {
    header.shader.file_header = 0x34333231u;
    header.shader.version = 0x18u;
    header.shader.header_size = sizeof(Header);
    header.shader.shader_size = static_cast<std::uint32_t>(bytes);
    header.shader.code = code;
    header.shader.type = type;
    header.shader.user_data = &header.userData;
    AgcDriverRegisterShader_nid_postfix(&header.shader);
    return &header.shader;
}

void Append(std::vector<std::uint32_t>& commands, std::uint32_t opcode, std::initializer_list<std::uint32_t> payload) {
    commands.push_back(0xc0000000u | (static_cast<std::uint32_t>(payload.size() - 1) << 16u) | (opcode << 8u));
    commands.insert(commands.end(), payload);
}

std::vector<std::uint32_t> DrawFrom(std::size_t target, std::uint32_t left, const float* source, bool smaller = false, bool otherFormat = false) {
    const auto color = reinterpret_cast<std::uintptr_t>(ColorMemory[target].data());
    const auto depth = reinterpret_cast<std::uintptr_t>(DepthMemory[target].data());
    const auto vertex = reinterpret_cast<std::uintptr_t>(VertexCode.data());
    const auto pixel = reinterpret_cast<std::uintptr_t>(InlineData ? InlinePixelCode.data() : PixelCode.data());
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> context{
        {0x2d5, 0x2000},
        {0x1b6, 0}, {0x207, 0}, {0x200, 0x36}, {0x203, 0x800},
        {0x2dc, 0xaa00}, {0x2f8, 0}, {0x292, 2}, {0x293, 0},
        {0x80, 0}, {0x8d, 0}, {0x83, 0xffff}, {0x8c, 0xa},
        {0x2f9, 0x2d}, {0x313, 0x6000}, {0x30e, 0xffffffff}, {0x30f, 0xffffffff},
        {0x206, 0x43f}, {0x204, 0x80000}, {0x205, 0x240},
        {0x8e, 0xf}, {0x8f, 0xf}, {0x202, 0xcc0010},
        {0x1c4, 0}, {0x1c5, 9}, {0x1c3, 4}, {0x31c, otherFormat ? 0x28128u : 0x28028u},
        {0x31b, 0}, {0x31d, 0}, {0x3b0, ((smaller ? 31u : 63u) << 14u) | 3u},
        {0x3b8, 0x9000000}, {0x1e0, 0},
        {0xc, 0}, {0xd, 0x40040},
        {0x81, 0x80000000}, {0x82, 0x40040},
        {0x90, 0x80000000}, {0x91, 0x40040},
        {0x94, 0x80000000}, {0x95, 0x40040},
        {0x318, static_cast<std::uint32_t>(color >> 8u)}, {0x390, static_cast<std::uint32_t>(color >> 40u)},
        {0x10f, std::bit_cast<std::uint32_t>(32.0f)}, {0x110, std::bit_cast<std::uint32_t>(32.0f)},
        {0x111, std::bit_cast<std::uint32_t>(-2.0f)}, {0x112, std::bit_cast<std::uint32_t>(2.0f)},
        {0x113, std::bit_cast<std::uint32_t>(1.0f)}, {0x114, 0},
        {0xb4, 0}, {0xb5, std::bit_cast<std::uint32_t>(1.0f)},
    };
    const auto vertices = reinterpret_cast<std::uintptr_t>(Vertices.data());
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> shader{
        {0xc8, static_cast<std::uint32_t>(vertex >> 8u)}, {0xc9, static_cast<std::uint32_t>(vertex >> 40u)}, {0x8b, 4u << 1u}, {0x8c, static_cast<std::uint32_t>(vertices)}, {0x8d, static_cast<std::uint32_t>(vertices >> 32u) | (16u << 16u)}, {0x8e, 3}, {0x8f, 0x01016facu},
        {0x008, static_cast<std::uint32_t>(pixel >> 8u)}, {0x009, static_cast<std::uint32_t>(pixel >> 40u)}, {0x00b, 2u << 1u},
    };
    std::vector<std::uint32_t> commands;
    Append(commands, 0x79, {0x242, 4});
    for (const auto& [offset, value] : context) Append(commands, 0x69, {offset, value});
    for (const auto& [offset, value] : shader) Append(commands, 0x76, {offset, value});
    for (const auto& [offset, value] : std::vector<std::pair<std::uint32_t, std::uint32_t>>{
        {0x000, 0}, {0x002, 0}, {0x007, (3u << 16u) | 63u}, {0x010, 3}, {0x011, 0},
        {0x012, static_cast<std::uint32_t>(depth >> 8u)}, {0x014, static_cast<std::uint32_t>(depth >> 8u)},
        {0x01a, static_cast<std::uint32_t>(depth >> 40u)}, {0x01c, static_cast<std::uint32_t>(depth >> 40u)},
        {0x00a, 0}, {0x00b, std::bit_cast<std::uint32_t>(1.0f)},
        {0x090, 0x80000000u | left}, {0x091, 0x40020u},
        {0x10f, std::bit_cast<std::uint32_t>(16.0f)}, {0x110, std::bit_cast<std::uint32_t>(16.0f + left)}
    }) Append(commands, 0x69, {offset, value});
    const auto address = reinterpret_cast<std::uintptr_t>(source);
    Append(commands, 0x76, {0x00c, static_cast<std::uint32_t>(address)});
    Append(commands, 0x76, {0x00d, static_cast<std::uint32_t>(address >> 32u)});
    if (InlineData) {
        Append(commands, 0x76, {0x00b, 1u << 1u});
        Append(commands, 0x76, {0x00c, std::bit_cast<std::uint32_t>(1.0f)});
    }
    Append(commands, 0x2d, {3, 2});
    return commands;
}

void MovedDataHitsTheCache(bool libraries) {
    static_cast<void>(libraries);
    Registered(VertexHeader, VertexCode.data(), sizeof(VertexCode), 2);
    Registered(PixelHeader, InlineData ? InlinePixelCode.data() : PixelCode.data(), InlineData ? sizeof(InlinePixelCode) : sizeof(PixelCode), 1);
    const bool disabled = std::getenv("APS5_NO_DRAW_TARGET_RELOCATION") != nullptr;
    static std::array<std::array<float, 4>, 12> same{};
    for (auto& entry : same) entry = {1, 0, 0, 0};
    const auto draw = [&](std::size_t target, std::uint32_t left, std::size_t data, bool smaller = false, bool format = false) {
        auto commands = DrawFrom(target, left, same[data].data(), smaller, format);
        Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
        Require(sceAgcDriverSubmitDcb(&packet) == 0, "the target draw submission failed");
        AgcDriverWaitIdle_nid_postfix();
        return AgcDriver::DriverDetail::Driver::Get().DrawCacheCounters();
    };
    for (std::size_t i = 0; i < 4; ++i) draw(0, static_cast<std::uint32_t>(i), i);
    const auto before = AgcDriver::DriverDetail::Driver::Get().DrawCacheCounters();
    const auto moved = draw(1, 8, 4);
    Require(moved.hits == before.hits + (disabled ? 0u : 1u), "the moved target did not obey target relocation mode");
    Require(disabled || moved.targetHits != 0, "the moved draw was not counted as a target hit");
    draw(0, 4, 5);
    for (std::size_t target = 0; target < 2; ++target) {
        std::vector<std::byte> pixels(1024);
        AgcDriver::GuestMemory::Read(reinterpret_cast<std::uintptr_t>(ColorMemory[target].data()), pixels);
        for (std::uint32_t y = 0; y < 4; ++y) {
            for (std::uint32_t x = 0; x < 64; ++x) {
                const bool red = target == 0 ? x < 32 : x >= 8 && x < 32;
                Require(std::to_integer<unsigned>(pixels[(y * 64 + x) * 4]) == (red ? 255u : 0u), "incorrect pixels in target " + std::to_string(target) + " at " + std::to_string(x) + "," + std::to_string(y));
            }
        }
    }
    const auto prior = AgcDriver::DriverDetail::Driver::Get().DrawCacheCounters();
    const auto resized = draw(1, 0, 6, true);
    Require(resized.hits == prior.hits, "a different target size hit");
    const auto reformatted = draw(1, 0, 7, false, true);
    Require(reformatted.hits == resized.hits, "a different target format hit");

}

}

int main(int argc, char** argv) {
    try {
        InlineData = argc > 1 && std::string(argv[1]) == "inline";
        bool libraries = false;
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
            libraries = device->GraphicsPipelineLibraries();
        }
        if (!libraries && std::getenv("APS5_NO_GPL") == nullptr) return VulkanTestSkipped;
        MovedDataHitsTheCache(libraries);
        AgcDriverShutdown_nid_postfix();
        std::cout << "A draw whose data moved reused the cached draw " << (libraries ? "with" : "without") << " graphics pipeline libraries\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        AgcDriverShutdown_nid_postfix();
        return 1;
    }
}
