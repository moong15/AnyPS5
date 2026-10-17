#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;

constexpr std::uint32_t Width = 192;
constexpr std::uint32_t Height = 128;
constexpr std::uint32_t DistanceVector = 0x00400000u;
constexpr std::uint32_t SetContextReg = 0x69u;
constexpr std::uint32_t SetShReg = 0x76u;
constexpr std::uint32_t SetUconfigReg = 0x79u;
constexpr std::uint32_t DrawIndexOffset2Header = 0xc0033500u;

constexpr std::size_t Block = 65536;
constexpr std::size_t PoolBytes = 2 * Block;
constexpr std::size_t ColorBytes = Width * Height * 4;
constexpr std::uint8_t Sentinel = 0xc7;
constexpr std::array<std::uint32_t, 3> Indices{0, 1, 2};
constexpr std::size_t VertexOffset = 0x2000;

std::byte* ColorMemory = nullptr;
std::byte* Pool = nullptr;

alignas(256) constexpr std::array<std::uint32_t, 10> VertexCode{
    0xe0382000, 0x80020005, 0xe0382010, 0x80020405, 0xbf8c3f70,
    0xf80000cf, 0x03020100, 0xf80008df, 0x07060504, 0xbf810000,
};

alignas(256) constexpr std::array<std::uint32_t, 7> PixelCode{
    0x7e0002f2, 0x7e0202f2, 0x7e0402f2, 0x7e0602f2, 0xf800080f, 0x03020100, 0xbf810000,
};

struct Vertex {
    std::array<float, 4> position;
    std::array<float, 4> distances;
};

std::array<Vertex, 3> Vertices{};

constexpr std::array<float, 3> PositionX{-3.0f, 1.0f, 1.0f};
constexpr std::array<float, 3> PositionY{-1.0f, -1.0f, 3.0f};
constexpr std::array<float, 3> Negative{-1.0f, -1.0f, -1.0f};

struct RegisteredShader {
    Shader shader;
    ShaderUserData userData;
};
RegisteredShader VertexShader{};
RegisteredShader PixelShader{};

const std::vector<std::pair<std::uint32_t, std::uint32_t>> ContextBaseline{
    {0x2d5, 0x2000},
    {0x1b6, 0}, {0x200, 0}, {0x203, 0x800},
    {0x2dc, 0xaa00}, {0x2f8, 0}, {0x292, 2}, {0x293, 0},
    {0x80, 0}, {0x8d, 0}, {0x83, 0xffff}, {0x8c, 0xa},
    {0x2f9, 0x2d}, {0x313, 0x6000}, {0x30e, 0xffffffff}, {0x30f, 0xffffffff},
    {0x206, 0x43f}, {0x204, 0x80000}, {0x205, 0x240},
    {0x8e, 0xf}, {0x8f, 0xf}, {0x202, 0xcc0010},
    {0x1c4, 0}, {0x1c5, 9}, {0x1c3, 4}, {0x31c, 0x28028},
    {0x31b, 0}, {0x31d, 0}, {0x3b8, 0x9000000}, {0x1e0, 0},
    {0xc, 0},
    {0x81, 0x80000000}, {0x90, 0x80000000}, {0x94, 0x80000000},
    {0xb4, 0}, {0xb5, std::bit_cast<std::uint32_t>(1.0f)}, {0x114, 0}, {0x113, std::bit_cast<std::uint32_t>(1.0f)},
};

std::byte* AllocateWatched(std::size_t bytes) {
#ifdef _WIN32
    void* block = GuestArena::GuestArenaAllocate_nid_postfix(bytes, Block);
    GuestArena::GuestArenaCommit_nid_postfix(block, bytes, PAGE_READWRITE, bytes);
#else
    void* raw = mmap(nullptr, bytes + Block, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) throw std::runtime_error("dispatch miss items: cannot map the watched block");
    const auto begin = reinterpret_cast<std::uintptr_t>(raw);
    const auto aligned = (begin + Block - 1) & ~static_cast<std::uintptr_t>(Block - 1);
    if (aligned != begin) munmap(raw, aligned - begin);
    if (aligned + bytes != begin + bytes + Block) munmap(reinterpret_cast<void*>(aligned + bytes), begin + Block - aligned);
    void* block = reinterpret_cast<void*>(aligned);
    GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, bytes);
#endif
    return static_cast<std::byte*>(block);
}

void Register(RegisteredShader& registered, const void* code, std::uint32_t bytes, std::uint8_t type) {
    registered.shader.file_header = 0x34333231u;
    registered.shader.version = 0x18u;
    registered.shader.user_data = &registered.userData;
    registered.shader.code = code;
    registered.shader.header_size = sizeof(RegisteredShader);
    registered.shader.shader_size = bytes;
    registered.shader.type = type;
    AgcDriverRegisterShader_nid_postfix(&registered.shader);
}

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t stride, std::uint32_t count) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (stride << 16u), count, 0x01016facu};
}

void SetRegister(std::vector<std::uint32_t>& words, std::uint32_t opcode, std::uint32_t offset, std::uint32_t value) {
    words.push_back(0xc0010000u | (opcode << 8u));
    words.push_back(offset);
    words.push_back(value);
}

std::vector<std::uint32_t> Draw(const void* vertices, std::size_t count) {
    const auto colorAddress = reinterpret_cast<std::uintptr_t>(ColorMemory);
    const auto poolAddress = reinterpret_cast<std::uintptr_t>(Pool);
    const auto vertexAddress = reinterpret_cast<std::uintptr_t>(VertexCode.data());
    const auto pixelAddress = reinterpret_cast<std::uintptr_t>(PixelCode.data());
    std::vector<std::uint32_t> words;
    for (const auto& [offset, value] : ContextBaseline) SetRegister(words, SetContextReg, offset, value);
    SetRegister(words, SetContextReg, 0x207, DistanceVector);
    SetRegister(words, SetContextReg, 0x318, static_cast<std::uint32_t>(colorAddress >> 8u));
    SetRegister(words, SetContextReg, 0x390, static_cast<std::uint32_t>(colorAddress >> 40u));
    SetRegister(words, SetContextReg, 0x3b0, ((Width - 1u) << 14u) | (Height - 1u));
    SetRegister(words, SetContextReg, 0x10f, std::bit_cast<std::uint32_t>(Width / 2.0f));
    SetRegister(words, SetContextReg, 0x110, std::bit_cast<std::uint32_t>(Width / 2.0f));
    SetRegister(words, SetContextReg, 0x111, std::bit_cast<std::uint32_t>(-(Height / 2.0f)));
    SetRegister(words, SetContextReg, 0x112, std::bit_cast<std::uint32_t>(Height / 2.0f));
    const auto extent = (Height << 16u) | Width;
    SetRegister(words, SetContextReg, 0xd, extent);
    SetRegister(words, SetContextReg, 0x82, extent);
    SetRegister(words, SetContextReg, 0x91, extent);
    SetRegister(words, SetContextReg, 0x95, extent);
    SetRegister(words, SetUconfigReg, 0x242, 4u);
    SetRegister(words, SetShReg, 0x008, static_cast<std::uint32_t>(pixelAddress >> 8u));
    SetRegister(words, SetShReg, 0x009, static_cast<std::uint32_t>(pixelAddress >> 40u));
    SetRegister(words, SetShReg, 0x00b, 0u);
    SetRegister(words, SetShReg, 0x0c8, static_cast<std::uint32_t>(vertexAddress >> 8u));
    SetRegister(words, SetShReg, 0x0c9, static_cast<std::uint32_t>(vertexAddress >> 40u));
    SetRegister(words, SetShReg, 0x08b, 4u << 1u);
    const auto descriptor = BufferDescriptor(vertices, sizeof(Vertex), static_cast<std::uint32_t>(count));
    for (std::uint32_t i = 0; i < descriptor.size(); ++i) SetRegister(words, SetShReg, 0x08c + i, descriptor[i]);
    words.insert(words.end(), {0xc0012600u, static_cast<std::uint32_t>(poolAddress), static_cast<std::uint32_t>(poolAddress >> 32u)});
    words.insert(words.end(), {0xc0001300u, static_cast<std::uint32_t>(Indices.size())});
    SetRegister(words, SetUconfigReg, 0x243, 0x401u);
    words.insert(words.end(), {DrawIndexOffset2Header, static_cast<std::uint32_t>(Indices.size()), 0u, static_cast<std::uint32_t>(Indices.size()), 0u});
    return words;
}

void Submit(const std::vector<std::uint32_t>& words, const char* what) {
    Packet packet{const_cast<std::uint32_t*>(words.data()), static_cast<std::uint32_t>(words.size()), 0, {}};
    Require(sceAgcDriverSubmitDcb(&packet) == 0, std::string("dispatch miss items: the submission failed for ") + what);
}

std::size_t WrittenPixels() {
    std::vector<std::byte> colors(ColorBytes);
    AgcDriver::GuestMemory::Read(reinterpret_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(ColorMemory)), colors);
    std::size_t written = 0;
    for (std::uint32_t pixel = 0; pixel < Width * Height; ++pixel) {
        const auto value = std::to_integer<std::uint8_t>(colors[pixel * 4u]);
        Require(value == 0u || value == 255u, "dispatch miss items: pixel " + std::to_string(pixel) + " stored " + std::to_string(value));
        written += value == 255u ? 1u : 0u;
    }
    return written;
}

void RequireTriangle(const char* phase) {
    const auto written = WrittenPixels();
    Require(written == Width * Height, std::string("dispatch miss items: ") + phase + ": the draw left " + std::to_string(Width * Height - written) + " pixels cleared, expected the triangle its indices and vertices name");
}

using AgcDriver::DriverDetail::DrawPipeline;
using AgcDriver::DriverDetail::Driver;

constexpr std::uint32_t DispatchOpcode = 0x15;
constexpr std::uint32_t Lanes = 32;
constexpr std::size_t DispatchBytes = Lanes * sizeof(std::uint32_t);

alignas(256) constexpr std::array<std::uint32_t, 8> BufferCode{0x34020082u, 0xe0301000u, 0x80000201u, 0xbf8c3f70u, 0x4a060481u, 0xe0701000u, 0x80010301u, 0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 9> ScalarCode{0xf4000200u, 0xfa000000u, 0xbf8cc07fu, 0x34020082u, 0x7e040208u, 0x4a040481u, 0xe0701000u, 0x80010201u, 0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 9> GdsCode{0x34060082u, 0xbefc03ffu, 0x0000ffffu, 0xd8fa0010u, 0x04000000u, 0xbf8cc07fu, 0xe0702000u, 0x80000403u, 0xbf810000u};

std::vector<std::uint32_t> Dispatch(const void* code, const void* input, const void* output, bool gds) {
    const auto address = reinterpret_cast<std::uintptr_t>(code);
    auto in = BufferDescriptor(input, 0, DispatchBytes);
    auto out = BufferDescriptor(output, gds ? 4u : 0u, gds ? Lanes : DispatchBytes);
    in[3] = 0x31016facu;
    out[3] = gds ? 0x11016facu : 0x31016facu;
    const auto& first = gds ? out : in;
    return {
        0xc0027600u, 0x20cu, static_cast<std::uint32_t>(address >> 8u), static_cast<std::uint32_t>(address >> 40u),
        0xc0037600u, 0x207u, Lanes, 1u, 1u,
        0xc0017600u, 0x213u, 8u << 1u,
        0xc0087600u, 0x240u, first[0], first[1], first[2], first[3], out[0], out[1], out[2], out[3],
        0xc0031500u, 1u, 1u, 1u, 0x8041u,
    };
}

template<typename TReady>
void WaitUntil(TReady ready, const char* what) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!ready()) {
        Require(std::chrono::steady_clock::now() < deadline, std::string("dispatch miss items: timed out waiting for ") + what);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void Run(const void* code, const void* input, void* output, bool drain, bool gds, bool miss, std::uint32_t expected) {
    std::fill_n(ColorMemory, ColorBytes, std::byte{0});
    std::memset(Pool, Sentinel, PoolBytes);
    std::memcpy(Pool, Indices.data(), sizeof(Indices));
    std::memcpy(Pool + VertexOffset, Vertices.data(), sizeof(Vertices));
    const auto before = Driver::Get().DispatchCacheCounters();
    const auto released = std::make_shared<std::atomic<bool>>(false);
    struct Release {
        std::shared_ptr<std::atomic<bool>> flag;
        ~Release() { flag->store(true, std::memory_order_release); }
    } release{released};
    DrawPipeline::Queue0().Enqueue([released] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!released->load(std::memory_order_acquire)) {
            Require(std::chrono::steady_clock::now() < deadline, "dispatch miss items: gate was never released");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }, {}, 0, {}, {}, true);
    Submit(Draw(Pool + VertexOffset, Vertices.size()), "the queued draw");
    WaitUntil([] { return DrawPipeline::HeldReads(reinterpret_cast<std::uintptr_t>(Pool), sizeof(Indices)); }, "the draw to hold its indices");
    const auto drainsBefore = DrawPipeline::DrainRequestsByOpcode(DispatchOpcode);
    Submit(Dispatch(code, input, output, gds), "the dispatch behind the draw");
    WaitUntil([&] { return DrawPipeline::QueuedItems() >= 3 || DrawPipeline::DrainRequestsByOpcode(DispatchOpcode) > drainsBefore; }, "a dispatch item or fallback drain");
    released->store(true, std::memory_order_release);
    AgcDriverWaitIdle_nid_postfix();
    const auto drains = DrawPipeline::DrainRequestsByOpcode(DispatchOpcode) - drainsBefore;
    Require(drains == (drain ? 1u : 0u), "dispatch miss items: expected " + std::to_string(drain ? 1 : 0) + " dispatch drains, got " + std::to_string(drains));
    const auto after = Driver::Get().DispatchCacheCounters();
    Require(miss ? after.absent > before.absent : after.equal > before.equal, "dispatch miss items: the intended cache lookup outcome was not exercised");
    RequireTriangle("dispatch behind queued draw");
    if (!gds) {
        AgcDriver::GuestMemory::FlushGpuWrites(reinterpret_cast<std::uintptr_t>(output), DispatchBytes);
        const auto* words = static_cast<const std::uint32_t*>(output);
        for (std::uint32_t lane = 0; lane < Lanes; ++lane) Require(words[lane] == expected, "dispatch miss items: stale or missing output in lane " + std::to_string(lane));
    }
}

}

int main() {
#ifdef _WIN32
    _putenv_s("APS5_PIPELINED_DRAWS", "1");
    _putenv_s("APS5_PIPELINE_DISPATCH", "1");
#else
    setenv("APS5_PIPELINED_DRAWS", "1", 1);
    setenv("APS5_PIPELINE_DISPATCH", "1", 1);
#endif
    try {
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
        }
        ColorMemory = AllocateWatched(2 * Block);
        Pool = AllocateWatched(PoolBytes);
        auto* input = AllocateWatched(Block);
        auto* output = AllocateWatched(Block);
        for (auto* block : {ColorMemory, Pool, input, output}) GuestAllocations::Mutation().Add(block, block == input || block == output ? Block : PoolBytes, true, true, true);
        Register(VertexShader, VertexCode.data(), sizeof(VertexCode), 2);
        Register(PixelShader, PixelCode.data(), sizeof(PixelCode), 1);
        RegisteredShader bufferShader{}, scalarShader{}, gdsShader{};
        Register(bufferShader, BufferCode.data(), sizeof(BufferCode), 0);
        Register(scalarShader, ScalarCode.data(), sizeof(ScalarCode), 0);
        Register(gdsShader, GdsCode.data(), sizeof(GdsCode), 0);
        for (std::size_t i = 0; i < Vertices.size(); ++i) Vertices[i] = {{PositionX[i], PositionY[i], 0.5f, 1.0f}, {Negative[i], Negative[i], Negative[i], 0.0f}};
        std::fill_n(reinterpret_cast<std::uint32_t*>(input), Lanes, 0x12340000u);
        Run(BufferCode.data(), input, output, false, false, true, 0x12340001u);
        Run(BufferCode.data(), input, output, false, false, false, 0x12340001u);
        Run(BufferCode.data(), ColorMemory, output, false, false, true, 0u);
        Run(ScalarCode.data(), ColorMemory, output, true, false, true, 0u);
        Run(BufferCode.data(), input, Pool, false, false, true, 0x12340001u);
        Run(GdsCode.data(), input, output, true, true, true, 0u);
        AgcDriverShutdown_nid_postfix();
        std::puts("dispatch miss item tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        AgcDriverShutdown_nid_postfix();
        return 1;
    }
}
