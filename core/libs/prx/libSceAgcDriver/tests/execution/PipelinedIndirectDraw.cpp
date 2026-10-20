#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Memory/DrawWriteRanges.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "VulkanTestDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include <cstring>
#include <limits>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <future>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using AgcDriver::DriverDetail::DrawPipeline;

constexpr std::byte Kept{0x40};
constexpr std::uint32_t BaseVertexLocation = 0x8c;
constexpr std::uint32_t Clipped = 0x40000000u;
constexpr std::uint32_t IndexCount = 3;
constexpr std::chrono::seconds WaitSeconds{10};

alignas(256) constexpr std::array<std::uint32_t, 20> VertexCode{
    0x7e000d05u, 0x080200f4u, 0x10020300u, 0x100202f4u, 0x060202f3u, 0x060400f3u, 0x10040500u, 0x060404f3u,
    0x7e060208u, 0x7e0802f2u, 0x7e0a02ffu, 0x44800000u, 0xf80000d1u, 0x00000005u, 0xf80008cfu, 0x04030201u, 0xbf810000u, 0xbf810000u, 0xbf810000u, 0xbf810000u,
};
alignas(256) constexpr std::array<std::uint32_t, 8> PixelCode{0x7e000200u, 0x7e020280u, 0xf800180fu, 0x00010100u, 0xbf810000u, 0xbf810000u, 0xbf810000u, 0xbf810000u};
alignas(4096) std::array<std::byte, 131072> ImageArgumentMemory{};
alignas(256) constexpr std::array<std::uint32_t, 11> ImageWriteCode{
    0xd7460004u, 0x0401060eu, 0xd7460005u, 0x0405060fu, 0x7e100d04u, 0x7e120d05u,
    0x7e140c0du, 0x7e1602f2u, 0xf0200f08u, 0x00000804u, 0xbf810000u,
};
alignas(256) std::array<std::byte, 1024> DirectMemory{};
alignas(256) std::array<std::byte, 1024> FirstMemory{};
alignas(256) std::array<std::byte, 1024> SecondMemory{};
alignas(256) std::array<std::byte, 1024> WrittenMemory{};
alignas(256) std::array<std::byte, 1024> InlineMemory{};
alignas(256) std::array<std::byte, 1024> IndexedDirectMemory{};
alignas(256) std::array<std::byte, 1024> IndexedMemory{};
alignas(256) std::array<std::uint32_t, 4> Records{3, 1, 0, 0};
alignas(256) std::array<std::uint32_t, 4> LateRecords{0, 1, 0, 0};
alignas(256) std::array<std::uint32_t, 8> TrailingZeroRecords{3, 1, 0, 0, 0, 0, 0, 0};
alignas(256) std::array<std::uint32_t, 10> IndexedRecords{3, 1, 0, 0, 0, 3, 1, 8, 0, 0};
alignas(256) std::array<std::uint16_t, 4> Indices{0, 1, 2, 0};
alignas(256) std::array<std::uint32_t, 4> StoredRecords{0, 1, 0, 0};
alignas(256) std::array<std::uint32_t, 4> RewrittenRecords{3, 1, 0, 0};
alignas(256) std::array<std::uint32_t, 4> ZeroRecords{0, 1, 0, 0};
alignas(256) std::array<std::uint32_t, 8> CountedRecords{3, 1, 0, 0, 3, 1, 0, 0};
alignas(256) std::array<std::uint32_t, 4> DrawCount{0, 0, 0, 0};

struct Header {
    Shader shader{};
    ShaderUserData userData{};
};

alignas(8) Header VertexHeader{};
alignas(8) Header PixelHeader{};

void Registered(Header& header, const std::uint32_t* code, std::size_t bytes, std::uint8_t type) {
    header.shader.file_header = 0x34333231u;
    header.shader.version = 0x18u;
    header.shader.header_size = sizeof(Header);
    header.shader.shader_size = static_cast<std::uint32_t>(bytes);
    header.shader.code = code;
    header.shader.type = type;
    header.shader.user_data = &header.userData;
    AgcDriverRegisterShader_nid_postfix(&header.shader);
}

void Append(std::vector<std::uint32_t>& commands, std::uint32_t opcode, std::initializer_list<std::uint32_t> payload) {
    commands.push_back(0xc0000000u | (static_cast<std::uint32_t>(payload.size() - 1) << 16u) | (opcode << 8u));
    commands.insert(commands.end(), payload);
}

void AppendState(std::vector<std::uint32_t>& commands) {
    const auto vertex = reinterpret_cast<std::uintptr_t>(VertexCode.data());
    const auto pixel = reinterpret_cast<std::uintptr_t>(PixelCode.data());
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> context{
        {0x2d5, 0x2000},
        {0x1b6, 0}, {0x207, 0x01200000}, {0x200, 0}, {0x203, 0x800},
        {0x2dc, 0xaa00}, {0x2f8, 0}, {0x292, 2}, {0x293, 0},
        {0x80, 0}, {0x8d, 0}, {0x83, 0xffff}, {0x8c, 0xa},
        {0x2f9, 0x2d}, {0x313, 0x6000}, {0x30e, 0xffffffff}, {0x30f, 0xffffffff},
        {0x206, 0x43f}, {0x204, 0x80000}, {0x205, 0x240},
        {0x8e, 0xf}, {0x8f, 0xf}, {0x202, 0xcc0010},
        {0x1c4, 0}, {0x1c5, 9}, {0x1c3, 4}, {0x31c, 0x28028},
        {0x31b, 0}, {0x31d, 0}, {0x3b0, (63u << 14u) | 3u},
        {0x3b8, 0x9000000}, {0x1e0, 0},
        {0xc, 0}, {0xd, 0x40040},
        {0x81, 0x80000000}, {0x82, 0x40040},
        {0x90, 0x80000000}, {0x91, 0x40040},
        {0x94, 0x80000000}, {0x95, 0x40040},
        {0x10f, std::bit_cast<std::uint32_t>(32.0f)}, {0x110, std::bit_cast<std::uint32_t>(32.0f)},
        {0x111, std::bit_cast<std::uint32_t>(-2.0f)}, {0x112, std::bit_cast<std::uint32_t>(2.0f)},
        {0x113, std::bit_cast<std::uint32_t>(1.0f)}, {0x114, 0},
        {0xb4, 0}, {0xb5, std::bit_cast<std::uint32_t>(1.0f)},
        {0x2fe, 0}, {0x302, 0}, {0x306, 0}, {0x30a, 0},
    };
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> shader{
        {0xc8, static_cast<std::uint32_t>(vertex >> 8u)}, {0xc9, static_cast<std::uint32_t>(vertex >> 40u)}, {0x8b, 1u << 1u},
        {0x008, static_cast<std::uint32_t>(pixel >> 8u)}, {0x009, static_cast<std::uint32_t>(pixel >> 40u)}, {0x00b, 1u << 1u}, {0x00c, std::bit_cast<std::uint32_t>(1.0f)},
    };
    Append(commands, 0x79, {0x242, 4});
    Append(commands, 0x79, {0x24a, 0});
    for (const auto& [offset, value] : context) Append(commands, 0x69, {offset, value});
    for (const auto& [offset, value] : shader) Append(commands, 0x76, {offset, value});
}

void AppendTarget(std::vector<std::uint32_t>& commands, std::span<std::byte> memory, std::uint32_t userWord) {
    const auto color = reinterpret_cast<std::uintptr_t>(memory.data());
    Append(commands, 0x69, {0x318, static_cast<std::uint32_t>(color >> 8u)});
    Append(commands, 0x69, {0x390, static_cast<std::uint32_t>(color >> 40u)});
    Append(commands, 0x76, {BaseVertexLocation, userWord});
}

void AppendRecords(std::vector<std::uint32_t>& commands, const void* records) {
    const auto address = reinterpret_cast<std::uintptr_t>(records);
    Append(commands, 0x11, {1, static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u)});
}

void AppendIndirect(std::vector<std::uint32_t>& commands) {
    Append(commands, 0x24, {0, BaseVertexLocation, 0, 2});
}

void AppendIndirectMulti(std::vector<std::uint32_t>& commands, std::uint32_t count) {
    Append(commands, 0x2c, {0, BaseVertexLocation, 0, 0, count, 0, 0, 16, 2});
}

void AppendGpuSideIndirect(std::vector<std::uint32_t>& commands) {
    Append(commands, 0x24, {0, 0, 0, 2});
}

void AppendGpuSideIndirectCounted(std::vector<std::uint32_t>& commands, std::uint32_t maxCount, const void* count) {
    const auto address = reinterpret_cast<std::uintptr_t>(count);
    Append(commands, 0x2c, {0, 0, 0, 1u << 30u, maxCount, static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u), 16, 2});
}

void AppendDma(std::vector<std::uint32_t>& commands, const void* source, void* destination, std::uint32_t bytes) {
    const auto from = reinterpret_cast<std::uintptr_t>(source);
    const auto to = reinterpret_cast<std::uintptr_t>(destination);
    Append(commands, 0x50, {0, static_cast<std::uint32_t>(from), static_cast<std::uint32_t>(from >> 32u), static_cast<std::uint32_t>(to), static_cast<std::uint32_t>(to >> 32u), bytes});
}

bool PipelinedIndirectDisabled() {
    return std::getenv("APS5_NO_PIPELINED_INDIRECT") != nullptr;
}

void AppendIndexBuffer(std::vector<std::uint32_t>& commands) {
    const auto address = reinterpret_cast<std::uintptr_t>(Indices.data());
    Append(commands, 0x2a, {0});
    Append(commands, 0x26, {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u)});
    Append(commands, 0x13, {IndexCount});
}

void AppendIndexedDirect(std::vector<std::uint32_t>& commands) {
    Append(commands, 0x35, {IndexCount, 0, IndexCount, 0});
}

void AppendIndexedIndirectMulti(std::vector<std::uint32_t>& commands, std::uint32_t count) {
    Append(commands, 0x38, {0, BaseVertexLocation, 0, 0, count, 0, 0, 20, 0});
}

void AppendLabel(std::vector<std::uint32_t>& commands, std::uint32_t* target, std::uint32_t value) {
    const auto address = reinterpret_cast<std::uintptr_t>(target);
    commands.insert(commands.end(), {0xc0064900u, 0x514u, (1u << 29u) | (2u << 24u), static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u), value, 0, 0});
}

std::size_t Covered(const std::array<std::byte, 1024>& memory) {
    return static_cast<std::size_t>(std::count_if(memory.begin(), memory.end(), [](std::byte value) { return value != Kept; }));
}

void SameAsDirect(const std::array<std::byte, 1024>& memory, const std::array<std::byte, 1024>& direct, const std::string& what) {
    for (std::size_t index = 0; index < direct.size(); ++index) {
        Require(memory[index] == direct[index], what + ": byte " + std::to_string(index) + " is " + std::to_string(static_cast<unsigned>(memory[index])) + ", the direct draw wrote " + std::to_string(static_cast<unsigned>(direct[index])));
    }
}

void Flush(const std::array<std::byte, 1024>& memory) {
    AgcDriver::GuestMemory::FlushGpuWrites(reinterpret_cast<std::uintptr_t>(memory.data()), memory.size());
}

struct Blocker {
    std::atomic<bool> release{false};
    void Enqueue() {
        DrawPipeline::Queue0().Enqueue([this] {
            while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            std::lock_guard gpuLock(AgcDriver::GuestMemory::GpuMutex());
        }, {}, 0, {}, {}, true);
    }
};

void SubmitBehind(Blocker& blocker, std::vector<std::uint32_t>& commands) {
    std::unique_lock gpuLock(AgcDriver::GuestMemory::GpuMutex());
    Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
    AgcDriver::Submit(&packet, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    blocker.Enqueue();
}

void Finish() {
    auto idle = std::async(std::launch::async, [] { AgcDriverWaitIdle_nid_postfix(); });
    if (idle.wait_for(std::chrono::seconds(20)) != std::future_status::ready) {
        std::fputs("pipelined indirect draw: the queue worker and the draw pipeline deadlocked\n", stderr);
        std::fflush(stderr);
        std::_Exit(1);
    }
    idle.get();
}

void WaitForItems(std::size_t items, Blocker& blocker, const std::string& what) {
    const auto start = std::chrono::steady_clock::now();
    while (DrawPipeline::Queue0().Outstanding() < items) {
        if (std::chrono::steady_clock::now() - start > WaitSeconds) {
            const auto outstanding = DrawPipeline::Queue0().Outstanding();
            blocker.release = true;
            Finish();
            Require(false, what + " (" + std::to_string(outstanding) + " items outstanding, expected " + std::to_string(items) + ")");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    blocker.release = true;
    Finish();
}

void DrawDirectReference() {
    DirectMemory.fill(Kept);
    std::vector<std::uint32_t> commands;
    AppendState(commands);
    AppendTarget(commands, DirectMemory, 0);
    Append(commands, 0x2d, {3, 2});
    Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
    Require(sceAgcDriverSubmitDcb(&packet) == 0, "the direct draw submission failed");
    AgcDriverWaitIdle_nid_postfix();
    Flush(DirectMemory);
    const auto direct = Covered(DirectMemory);
    Require(direct != 0 && direct != DirectMemory.size(), "the direct draw covered " + std::to_string(direct) + " of " + std::to_string(DirectMemory.size()) + " bytes, expected an edge through the target");
}

void DrawIndexedDirectReference() {
    IndexedDirectMemory.fill(Kept);
    std::vector<std::uint32_t> commands;
    AppendState(commands);
    AppendIndexBuffer(commands);
    AppendTarget(commands, IndexedDirectMemory, 0);
    AppendIndexedDirect(commands);
    Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
    Require(sceAgcDriverSubmitDcb(&packet) == 0, "the indexed direct draw submission failed");
    AgcDriverWaitIdle_nid_postfix();
    Flush(IndexedDirectMemory);
    const auto direct = Covered(IndexedDirectMemory);
    Require(direct != 0 && direct != IndexedDirectMemory.size(), "the indexed direct draw covered " + std::to_string(direct) + " of " + std::to_string(IndexedDirectMemory.size()) + " bytes, expected an edge through the target");
}

void IndirectDrawsQueueBehindTheCommitter() {
    FirstMemory.fill(Kept);
    SecondMemory.fill(Kept);
    std::vector<std::uint32_t> commands;
    AppendState(commands);
    AppendRecords(commands, Records.data());
    AppendTarget(commands, FirstMemory, Clipped);
    AppendIndirect(commands);
    AppendTarget(commands, SecondMemory, Clipped);
    AppendIndirect(commands);
    Blocker blocker;
    SubmitBehind(blocker, commands);
    if (std::getenv("APS5_NO_PIPELINED_INDIRECT") != nullptr) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const auto outstanding = DrawPipeline::Queue0().Outstanding();
        blocker.release = true;
        Finish();
        Require(outstanding == 1, "disabled pipelined indirect draws did not drain before reading records");
    } else {
        WaitForItems(3, blocker, "the indirect draws drained the pipeline instead of queuing behind it");
    }
    Flush(FirstMemory);
    Flush(SecondMemory);
    SameAsDirect(FirstMemory, DirectMemory, "the first queued indirect draw");
    SameAsDirect(SecondMemory, DirectMemory, "the second queued indirect draw");
}

void IndirectDrawWaitsForItsQueuedRecordWrite() {
    WrittenMemory.fill(Kept);
    LateRecords = {0, 1, 0, 0};
    std::vector<std::uint32_t> commands;
    AppendState(commands);
    AppendLabel(commands, &LateRecords[0], 3);
    AppendRecords(commands, LateRecords.data());
    AppendTarget(commands, WrittenMemory, Clipped);
    AppendIndirect(commands);
    Blocker blocker;
    SubmitBehind(blocker, commands);
    const auto start = std::chrono::steady_clock::now();
    while (DrawPipeline::Queue0().Outstanding() < 2 && std::chrono::steady_clock::now() - start < WaitSeconds) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    blocker.release = true;
    Finish();
    Flush(WrittenMemory);
    Require(LateRecords[0] == 3, "the queued label did not store the record's vertex count");
    SameAsDirect(WrittenMemory, DirectMemory, "the indirect draw whose record a queued label wrote");
}

void IndirectDrawWaitsForQueuedDma() {
    WrittenMemory.fill(Kept);
    LateRecords = {0, 1, 0, 0};
    GuestAllocations::Mutation().Add(LateRecords.data(), sizeof(LateRecords), true, true, true);
    GuestAllocations::Mutation().Add(Records.data(), sizeof(Records), true, true, true);
    const auto source = reinterpret_cast<std::uintptr_t>(Records.data());
    const auto destination = reinterpret_cast<std::uintptr_t>(LateRecords.data());
    std::vector<std::uint32_t> commands;
    AppendState(commands);
    Append(commands, 0x50, {0, static_cast<std::uint32_t>(source), static_cast<std::uint32_t>(source >> 32u), static_cast<std::uint32_t>(destination), static_cast<std::uint32_t>(destination >> 32u), sizeof(Records)});
    AppendRecords(commands, LateRecords.data());
    AppendTarget(commands, WrittenMemory, Clipped);
    AppendIndirect(commands);
    Blocker blocker;
    SubmitBehind(blocker, commands);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    blocker.release = true;
    Finish();
    Flush(WrittenMemory);
    Require(LateRecords == Records, "queued DMA did not publish indirect records");
    SameAsDirect(WrittenMemory, DirectMemory, "indirect records written by queued DMA");
}

void HeldImageArgumentsDrainBeforeRead(bool gpuSide = false) {
    ImageArgumentMemory.fill(std::byte{0});
    std::memcpy(ImageArgumentMemory.data(), Records.data(), sizeof(Records));
    GuestAllocations::Mutation().Add(ImageArgumentMemory.data(), ImageArgumentMemory.size(), true, true, true);
    const auto address = reinterpret_cast<std::uintptr_t>(ImageArgumentMemory.data());
    const auto code = reinterpret_cast<std::uintptr_t>(ImageWriteCode.data());
    std::vector<std::uint32_t> prepare;
    Append(prepare, 0x76, {0x207, 8, 8, 1});
    Append(prepare, 0x76, {0x20c, static_cast<std::uint32_t>(code >> 8u), static_cast<std::uint32_t>(code >> 40u)});
    Append(prepare, 0x76, {0x213, (14u << 1u) | (7u << 7u) | (1u << 11u)});
    Append(prepare, 0x76, {0x240, static_cast<std::uint32_t>(address >> 8u), static_cast<std::uint32_t>(address >> 40u) | (71u << 20u) | (3u << 30u), 31u | (119u << 14u), 0xfacu | (9u << 20u) | (9u << 28u), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    Append(prepare, 0x15, {8, 8, 1, 0x41});
    Packet packet{prepare.data(), static_cast<std::uint32_t>(prepare.size()), 0, {}};
    Require(sceAgcDriverSubmitDcb(&packet) == 0, "pending argument image dispatch failed");
    AgcDriverWaitIdle_nid_postfix();
    const std::array<DrawPipeline::Range, 1> range{{{address, address + sizeof(Records)}}};
    Require(AgcDriver::Graphics::StorageTexture::AnyPendingOverlaps(range), "argument fixture has no pending image");
    std::uint32_t staleCount = 0;
    std::memcpy(&staleCount, ImageArgumentMemory.data(), sizeof(staleCount));
    Require(staleCount == Records[0], "argument fixture already published its GPU results");
    WrittenMemory.fill(Kept);
    Blocker blocker;
    DrawPipeline::Queue0().Enqueue([&] {
        while (!blocker.release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }, {}, 0, {}, gpuSide ? std::vector<DrawPipeline::Range>{} : std::vector<DrawPipeline::Range>{{0, std::numeric_limits<std::uint64_t>::max()}}, true);
    if (!gpuSide) {
        Require(DrawPipeline::HeldReads(address, sizeof(Records)), "dispatch-miss read hold was not registered");
        Require(!AgcDriver::Graphics::StorageTexture::FlushPending(address, sizeof(Records)), "pending image flush ignored held reads");
    }
    const auto pendingImages = AgcDriver::Graphics::IndirectDrawCounts()[static_cast<std::size_t>(AgcDriver::Graphics::IndirectDrawPath::PendingImage)];
    std::vector<std::uint32_t> commands;
    AppendState(commands);
    AppendRecords(commands, ImageArgumentMemory.data());
    AppendTarget(commands, WrittenMemory, gpuSide ? 0 : Clipped);
    if (gpuSide) {
        AppendGpuSideIndirect(commands);
        AppendGpuSideIndirect(commands);
    } else {
        AppendIndirect(commands);
    }
    const auto drains = DrawPipeline::DrainRequestsByOpcode(0x100);
    Packet indirect{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
    Require(sceAgcDriverSubmitDcb(&indirect) == 0, "pending argument indirect submission failed");
    const auto start = std::chrono::steady_clock::now();
    while (DrawPipeline::DrainRequestsByOpcode(0x100) == drains && DrawPipeline::Queue0().Outstanding() < (gpuSide ? 3u : 2u) && std::chrono::steady_clock::now() - start < WaitSeconds) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const bool requestedDrain = DrawPipeline::DrainRequestsByOpcode(0x100) > drains;
    const auto outstanding = DrawPipeline::Queue0().Outstanding();
    blocker.release = true;
    Finish();
    Flush(WrittenMemory);
    Require(requestedDrain, "pending image argument read did not request an inline drain");
    Require(outstanding == 1, "pending image argument read did not drain its read hold");
    if (gpuSide) {
        Require(AgcDriver::Graphics::IndirectDrawCounts()[static_cast<std::size_t>(AgcDriver::Graphics::IndirectDrawPath::PendingImage)] > pendingImages, "GPU-side indirect draw did not flush its pending argument image");
        Require(!AgcDriver::Graphics::StorageTexture::AnyPendingOverlaps(range), "GPU-side indirect draw left its argument image pending");
    }
    AgcDriver::GuestMemory::FlushGpuWrites(address, sizeof(Records));
    std::array<std::uint32_t, 4> actual{};
    std::memcpy(actual.data(), ImageArgumentMemory.data(), sizeof(actual));
    Require(actual[0] == 0, "pending argument image did not publish its zero vertex count");
    Require(Covered(WrittenMemory) == 0, "indirect draw consumed stale arguments instead of the pending image's zero count");
}

void WriteCompletenessControlsIndirectDrain(bool gds) {
    using namespace AgcDriver::DriverDetail;
    ShaderRecompiler::RecompileResult program{};
    ShaderRecompiler::DescriptorBinding binding{};
    binding.kind = gds ? ShaderRecompiler::DescriptorKind::StorageBuffer : ShaderRecompiler::DescriptorKind::SampledImage;
    binding.role = gds ? ShaderRecompiler::DescriptorRole::Gds : ShaderRecompiler::DescriptorRole::GuestImages;
    binding.count = 1;
    program.bindings.push_back(binding);
    const std::array<AgcDriver::Graphics::CompiledShader, 1> stages{{{ShaderRecompiler::ShaderStage::Vertex, &program, 0}}};
    const AgcDriver::Graphics::State graphics{};
    bool bounded = true;
    auto reads = DrawReadRanges(graphics, stages, bounded);
    auto writes = DrawWriteRanges(graphics, stages, ExactDrawWrites());
    const bool complete = DrawWritesComplete(graphics, stages);
    Require(!bounded, "synthetic queued draw must be unbounded by reads");
    Require(complete == !gds, "write completeness confused unknown image reads with GDS writes");
    WrittenMemory.fill(Kept);
    LateRecords = gds ? std::array<std::uint32_t, 4>{0, 1, 0, 0} : Records;
    Blocker blocker;
    DrawPipeline::Queue0().Enqueue([&] {
        while (!blocker.release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (gds) LateRecords = Records;
    }, std::move(writes), 0, {}, std::move(reads), bounded, false, complete);
    Require(!DrawPipeline::Queue0().Overlaps(reinterpret_cast<std::uintptr_t>(LateRecords.data()), sizeof(LateRecords)), "ordinary overlap query changed for unbounded reads or incomplete writes");
    std::vector<std::uint32_t> commands;
    AppendState(commands);
    AppendRecords(commands, LateRecords.data());
    AppendTarget(commands, WrittenMemory, Clipped);
    AppendIndirect(commands);
    const auto drains = DrawPipeline::DrainRequestsByOpcode(0x100);
    Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
    Require(sceAgcDriverSubmitDcb(&packet) == 0, "write completeness indirect submission failed");
    const bool mustDrain = gds || std::getenv("APS5_NO_PIPELINED_INDIRECT") != nullptr;
    const auto start = std::chrono::steady_clock::now();
    while ((mustDrain ? DrawPipeline::DrainRequestsByOpcode(0x100) == drains : DrawPipeline::Queue0().Outstanding() < 2) && std::chrono::steady_clock::now() - start < WaitSeconds) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const bool drained = DrawPipeline::DrainRequestsByOpcode(0x100) > drains;
    const auto outstanding = DrawPipeline::Queue0().Outstanding();
    blocker.release = true;
    Finish();
    Require(drained == mustDrain, "indirect drain did not depend only on write completeness");
    Require(mustDrain || outstanding >= 2, "complete writes with unbounded reads prevented indirect queuing");
    Flush(WrittenMemory);
    SameAsDirect(WrittenMemory, DirectMemory, "indirect draw behind a synthetic write-completeness item");
}

void TrailingZeroCountRecordIsNotQueued() {
    InlineMemory.fill(Kept);
    std::vector<std::uint32_t> commands;
    AppendState(commands);
    AppendRecords(commands, TrailingZeroRecords.data());
    AppendTarget(commands, InlineMemory, Clipped);
    AppendIndirectMulti(commands, 2);
    Blocker blocker;
    const auto drains = DrawPipeline::DrainRequestsByOpcode(0x100);
    SubmitBehind(blocker, commands);
    if (std::getenv("APS5_NO_PIPELINED_INDIRECT") != nullptr) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const auto drained = DrawPipeline::DrainRequestsByOpcode(0x100) > drains;
        blocker.release = true;
        Finish();
        Require(drained, "disabled multi-record indirect draw did not request a drain");
    } else {
        WaitForItems(2, blocker, "a draw whose last record has no vertices queued nothing");
    }
    Flush(InlineMemory);
    SameAsDirect(InlineMemory, DirectMemory, "the draw whose trailing record has a zero count");
}

void OutOfRangeIndexedRecordIsNotQueued() {
    IndexedMemory.fill(Kept);
    std::vector<std::uint32_t> commands;
    AppendState(commands);
    AppendIndexBuffer(commands);
    AppendRecords(commands, IndexedRecords.data());
    AppendTarget(commands, IndexedMemory, Clipped);
    AppendIndexedIndirectMulti(commands, 2);
    Blocker blocker;
    const auto drains = DrawPipeline::DrainRequestsByOpcode(0x100);
    SubmitBehind(blocker, commands);
    if (std::getenv("APS5_NO_PIPELINED_INDIRECT") != nullptr) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const auto drained = DrawPipeline::DrainRequestsByOpcode(0x100) > drains;
        blocker.release = true;
        Finish();
        Require(drained, "disabled indexed indirect draw did not request a drain");
    } else {
        WaitForItems(2, blocker, "an indexed draw whose second record is out of range queued nothing");
    }
    Flush(IndexedMemory);
    SameAsDirect(IndexedMemory, IndexedDirectMemory, "the indexed draw whose second record is out of range");
}

void GpuSideIndirectDrawsQueueBehindTheCommitter() {
    FirstMemory.fill(Kept);
    SecondMemory.fill(Kept);
    std::vector<std::uint32_t> commands;
    AppendState(commands);
    AppendRecords(commands, Records.data());
    AppendTarget(commands, FirstMemory, 0);
    AppendGpuSideIndirect(commands);
    AppendTarget(commands, SecondMemory, 0);
    AppendGpuSideIndirect(commands);
    Blocker blocker;
    SubmitBehind(blocker, commands);
    if (PipelinedIndirectDisabled()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const auto outstanding = DrawPipeline::Queue0().Outstanding();
        blocker.release = true;
        Finish();
        Require(outstanding == 1, "disabled pipelined indirect draws queued a GPU-side indirect draw");
    } else {
        WaitForItems(3, blocker, "the GPU-side indirect draws drained the pipeline instead of queuing behind it");
    }
    Flush(FirstMemory);
    Flush(SecondMemory);
    SameAsDirect(FirstMemory, DirectMemory, "the first queued GPU-side indirect draw");
    SameAsDirect(SecondMemory, DirectMemory, "the second queued GPU-side indirect draw");
}

void GpuSideIndirectDrawQueuesBehindTheStoreOfItsArguments() {
    WrittenMemory.fill(Kept);
    StoredRecords = {0, 1, 0, 0};
    std::vector<std::uint32_t> commands;
    AppendState(commands);
    AppendLabel(commands, &StoredRecords[0], 3);
    AppendRecords(commands, StoredRecords.data());
    AppendTarget(commands, WrittenMemory, 0);
    AppendGpuSideIndirect(commands);
    Blocker blocker;
    SubmitBehind(blocker, commands);
    if (PipelinedIndirectDisabled()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const auto outstanding = DrawPipeline::Queue0().Outstanding();
        blocker.release = true;
        Finish();
        Require(outstanding == 1, "disabled pipelined indirect draws queued a GPU-side indirect draw behind the store of its arguments");
    } else {
        WaitForItems(3, blocker, "a GPU-side indirect draw drained for the queued store of its arguments instead of queuing behind it");
    }
    Flush(WrittenMemory);
    Require(StoredRecords[0] == 3, "the queued label did not store the GPU-side record's vertex count");
    SameAsDirect(WrittenMemory, DirectMemory, "the GPU-side indirect draw whose record a queued label wrote");
}

void GpuSideIndirectArgumentsRewrittenAfterEnqueue() {
    WrittenMemory.fill(Kept);
    RewrittenRecords = Records;
    GuestAllocations::Mutation().Add(RewrittenRecords.data(), sizeof(RewrittenRecords), true, true, true);
    GuestAllocations::Mutation().Add(ZeroRecords.data(), sizeof(ZeroRecords), true, true, true);
    const auto address = reinterpret_cast<std::uintptr_t>(RewrittenRecords.data());
    std::vector<std::uint32_t> commands;
    AppendState(commands);
    AppendRecords(commands, RewrittenRecords.data());
    AppendTarget(commands, WrittenMemory, 0);
    AppendGpuSideIndirect(commands);
    AppendDma(commands, ZeroRecords.data(), RewrittenRecords.data(), sizeof(RewrittenRecords));
    bool held = false;
    {
        std::unique_lock gpuLock(AgcDriver::GuestMemory::GpuMutex());
        Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
        AgcDriver::Submit(&packet, 0);
        if (PipelinedIndirectDisabled()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        } else {
            const auto start = std::chrono::steady_clock::now();
            while (!(held = DrawPipeline::HeldReads(address, sizeof(RewrittenRecords))) && std::chrono::steady_clock::now() - start < WaitSeconds) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    Finish();
    Flush(WrittenMemory);
    Require(PipelinedIndirectDisabled() || held, "a queued GPU-side indirect draw did not hold the read of its arguments");
    Require(RewrittenRecords == ZeroRecords, "the DMA after the GPU-side indirect draw did not rewrite its arguments");
    SameAsDirect(WrittenMemory, DirectMemory, "the GPU-side indirect draw whose arguments a later DMA rewrote");
}

void GpuSideIndirectCountQueuesBehindItsStore() {
    WrittenMemory.fill(Kept);
    DrawCount = {0, 0, 0, 0};
    const auto count = reinterpret_cast<std::uintptr_t>(DrawCount.data());
    const auto arguments = reinterpret_cast<std::uintptr_t>(CountedRecords.data());
    std::vector<std::uint32_t> commands;
    AppendState(commands);
    AppendLabel(commands, &DrawCount[0], 1);
    AppendRecords(commands, CountedRecords.data());
    AppendTarget(commands, WrittenMemory, 0);
    AppendGpuSideIndirectCounted(commands, 2, DrawCount.data());
    bool held = false;
    std::size_t outstanding = 0;
    {
        std::unique_lock gpuLock(AgcDriver::GuestMemory::GpuMutex());
        Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
        AgcDriver::Submit(&packet, 0);
        if (PipelinedIndirectDisabled()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        } else {
            const auto start = std::chrono::steady_clock::now();
            const auto queued = [&] { return DrawPipeline::Queue0().Outstanding() >= 2 && DrawPipeline::HeldReads(count, 4) && DrawPipeline::HeldReads(arguments, sizeof(CountedRecords)); };
            while (!(held = queued()) && std::chrono::steady_clock::now() - start < WaitSeconds) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        outstanding = DrawPipeline::Queue0().Outstanding();
    }
    Finish();
    Flush(WrittenMemory);
    Require(DrawCount[0] == 1, "the queued label did not store the draw count");
    Require(PipelinedIndirectDisabled() || (held && outstanding >= 2), "a GPU-side indirect draw drained for the queued store of its count instead of queuing with its count and arguments held");
    SameAsDirect(WrittenMemory, DirectMemory, "the GPU-side indirect draw whose count a queued label wrote");
}

void GpuSideIndirectDisjointDmaDoesNotDrain() {
    WrittenMemory.fill(Kept);
    std::vector<std::uint32_t> commands;
    AppendState(commands);
    AppendRecords(commands, Records.data());
    AppendTarget(commands, WrittenMemory, 0);
    AppendGpuSideIndirect(commands);
    std::vector<std::uint32_t> dma;
    AppendDma(dma, ZeroRecords.data(), RewrittenRecords.data(), sizeof(RewrittenRecords));
    const auto address = reinterpret_cast<std::uintptr_t>(Records.data());
    bool held = false;
    bool ready = false;
    std::future<bool> drained;
    const auto drains = DrawPipeline::DrainRequestsByOpcode(0x50);
    {
        std::unique_lock gpuLock(AgcDriver::GuestMemory::GpuMutex());
        Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
        AgcDriver::Submit(&packet, 0);
        const auto start = std::chrono::steady_clock::now();
        while (!(held = DrawPipeline::HeldReads(address, sizeof(Records)) && DrawPipeline::Queue0().Outstanding() >= 2) && std::chrono::steady_clock::now() - start < WaitSeconds) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        drained = std::async(std::launch::async, [&] { return DrawPipeline::Queue0().DrainPacket(dma); });
        ready = drained.wait_for(std::chrono::milliseconds(200)) == std::future_status::ready;
    }
    Finish();
    const bool requested = drained.get();
    Require(held, "disjoint DMA control did not enqueue its GPU-side indirect draw and submission completion");
    Require(ready && !requested && DrawPipeline::DrainRequestsByOpcode(0x50) == drains, "a DMA to disjoint memory drained the queued GPU-side indirect draw");
    Flush(WrittenMemory);
    SameAsDirect(WrittenMemory, DirectMemory, "the GPU-side indirect draw held across a disjoint DMA control");
}

void IndirectBatch(const std::string& mode) {
    using Recorder = AgcDriver::Graphics::Recorder;
    struct alignas(65536) Slot {
        std::array<std::byte, 65536> pixels;
        std::array<std::uint32_t, 16385> records;
    };
    static auto referenceStorage = std::make_unique<Slot>();
    static auto actualStorage = std::make_unique<Slot>();
    static auto producerStorage = std::make_unique<Slot>();
    auto& reference = *referenceStorage;
    auto& actual = *actualStorage;
    auto& produced = *producerStorage;
    const bool committer = mode == "committer";
    const bool indexed = mode == "indexed";
    const bool counted = mode == "count" || mode == "count_producer";
    const bool strided = mode == "strided" || indexed || counted;
    const bool producer = mode == "producer" || mode == "count_producer";
    const bool label = mode == "label";
    const bool bound = mode == "bound";
    const bool following = mode == "following";
    Require(mode == "alias" || strided || producer || label || bound || following || committer, "unknown indirect batch case");
    GuestAllocations::Mutation().Add(&reference, sizeof(reference), true, true, true);
    GuestAllocations::Mutation().Add(&actual, sizeof(actual), true, true, true);
    GuestAllocations::Mutation().Add(&produced, sizeof(produced), true, true, true);
    reference.pixels.fill(Kept);
    actual.pixels.fill(Kept);
    reference.records[0] = 2;
    actual.records.fill(Clipped);
    const auto recordWords = indexed ? 5u : 4u;
    const auto strideWords = strided ? 8u : recordWords;
    const auto count = strided ? 2u : 1u;
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto offset = index * strideWords;
        actual.records[offset] = strided && index == 0 ? 0u : 3u;
        actual.records[offset + 1] = 1u;
        if (indexed) actual.records[offset + 2] = 0;
    }
    produced.records.fill(0);
    produced.records[1] = 1;
    const auto submit = [](std::vector<std::uint32_t>& commands) {
        Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
        Require(sceAgcDriverSubmitDcb(&packet) == 0, "indirect batch submission failed");
        AgcDriverWaitIdle_nid_postfix();
    };
    const auto state = [&](std::vector<std::uint32_t>& commands, Slot& slot) {
        AppendState(commands);
        Append(commands, 0x69, {0x3b8, 0x9000000u | (0x1bu << 14u)});
        AppendTarget(commands, slot.pixels, 0);
        if (indexed) AppendIndexBuffer(commands);
    };
    const auto draws = bound ? static_cast<std::uint32_t>(Recorder::IndirectBatchDraws + 1) : producer ? 4u : 2u;
    const auto drawState = [&](std::vector<std::uint32_t>& commands, std::uint32_t index) {
        Append(commands, 0x79, {0x24a, indexed || producer || committer ? 0u : index % 2u});
        if (producer || committer) Append(commands, 0x76, {0x00c, std::bit_cast<std::uint32_t>(1.0f - 0.2f * static_cast<float>(index))});
    };
    std::vector<std::uint32_t> direct;
    state(direct, reference);
    for (std::uint32_t index = 0; index < draws; ++index) {
        if ((producer && index == 2) || (label && index == 1)) break;
        drawState(direct, index);
        if (indexed) AppendIndexedDirect(direct);
        else Append(direct, 0x2d, {3, 2});
    }
    if (following) {
        Append(direct, 0x79, {0x24a, 2});
        Append(direct, 0x2d, {3, 2});
    }
    submit(direct);
    AgcDriver::GuestMemory::FlushGpuWrites(reinterpret_cast<std::uintptr_t>(reference.pixels.data()), reference.pixels.size());
    Require(std::any_of(reference.pixels.begin(), reference.pixels.end(), [](std::byte value) { return value != Kept; }), "indirect batch reference drew no pixels");
    if (producer) {
        produced.pixels.fill(Kept);
        std::vector<std::uint32_t> stale;
        state(stale, produced);
        for (std::uint32_t index = 0; index < draws; ++index) {
            drawState(stale, index);
            Append(stale, 0x2d, {3, 2});
        }
        submit(stale);
        AgcDriver::GuestMemory::FlushGpuWrites(reinterpret_cast<std::uintptr_t>(produced.pixels.data()), produced.pixels.size());
        Require(produced.pixels != reference.pixels, "indirect batch producer fixture cannot distinguish stale arguments");
    }
    const auto before = Recorder::IndirectBatchCounts();
    std::vector<std::uint32_t> commands;
    state(commands, actual);
    AppendRecords(commands, actual.records.data());
    for (std::uint32_t index = 0; index < draws; ++index) {
        if (index == 2 && producer) {
            const auto source = reinterpret_cast<std::uintptr_t>(produced.records.data());
            const auto destination = reinterpret_cast<std::uintptr_t>(counted ? reference.records.data() : actual.records.data());
            Append(commands, 0x50, {0, static_cast<std::uint32_t>(source), static_cast<std::uint32_t>(source >> 32u), static_cast<std::uint32_t>(destination), static_cast<std::uint32_t>(destination >> 32u), static_cast<std::uint32_t>(sizeof(produced.records))});
        }
        if (index == 1 && label) AppendLabel(commands, actual.records.data(), 0);
        drawState(commands, index);
        const auto countAddress = counted ? reinterpret_cast<std::uintptr_t>(reference.records.data()) : 0;
        if (strided) Append(commands, indexed ? 0x38u : 0x2cu, {0, 0, 0, counted ? 1u << 30u : 0u, count, static_cast<std::uint32_t>(countAddress), static_cast<std::uint32_t>(countAddress >> 32u), strideWords * 4u, indexed ? 0u : 2u});
        else Append(commands, 0x24, {0, 0, 0, 2});
    }
    if (following) {
        Append(commands, 0x79, {0x24a, 2});
        Append(commands, 0x2d, {3, 2});
    }
    if (committer) {
        Blocker blocker;
        SubmitBehind(blocker, commands);
        if (PipelinedIndirectDisabled()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            const auto outstanding = DrawPipeline::Queue0().Outstanding();
            blocker.release = true;
            Finish();
            Require(outstanding == 1, "disabled pipelined indirect batch queued draw items");
        } else {
            WaitForItems(3, blocker, "indirect batch did not queue two separate committer items");
        }
    } else {
        submit(commands);
    }
    AgcDriver::GuestMemory::FlushGpuWrites(reinterpret_cast<std::uintptr_t>(actual.pixels.data()), actual.pixels.size());
    Require(actual.pixels == reference.pixels, "indirect batch output differs from the same direct draw sequence");
    if (producer) {
        auto& destination = counted ? reference.records : actual.records;
        AgcDriver::GuestMemory::FlushGpuWrites(reinterpret_cast<std::uintptr_t>(destination.data()), sizeof(destination));
        Require(destination == produced.records, "indirect batch producer did not publish its records");
    }
    if (!producer && !label) {
        for (std::uint32_t index = 0; index < count; ++index) {
            const auto offset = index * strideWords;
            Require(actual.records[offset + (indexed ? 3u : 2u)] == Clipped && actual.records[offset + (indexed ? 4u : 3u)] == Clipped, "indirect normalization modified an aliased source record");
            for (std::uint32_t word = recordWords; word < strideWords; ++word) Require(actual.records[offset + word] == Clipped, "indirect normalization modified source stride padding");
        }
    }
    const auto after = Recorder::IndirectBatchCounts();
    const bool disabled = std::getenv("APS5_NO_INDIRECT_BATCH") != nullptr;
    if (disabled) Require(after.draws == before.draws && after.batches == before.batches, "disabled indirect batching still queued draws");
    else if (label) Require(after.batches > before.batches, "label boundary did not complete the first indirect draw batch");
    else {
        Require(after.draws - before.draws == draws, "indirect batch fixture did not use the GPU batching path");
        const auto minimumBatches = producer || bound || (committer && PipelinedIndirectDisabled()) ? 2u : 1u;
        const auto maximumBatches = producer && !PipelinedIndirectDisabled() ? 3u : minimumBatches;
        const auto batches = after.batches - before.batches;
        Require(batches >= minimumBatches && batches <= maximumBatches, "indirect draws did not form the expected dependency batches");
        Require(after.avoided - before.avoided == draws - batches, "indirect batch did not preserve compatible rendering");
    }
    std::uint64_t digest = 14695981039346656037ull;
    for (const auto value : actual.pixels) digest = (digest ^ static_cast<unsigned>(value)) * 1099511628211ull;
    std::printf("indirect-batch-output %016llx\n", static_cast<unsigned long long>(digest));
}

}

int main(int argc, char** argv) {
#ifdef _WIN32
    _putenv_s("APS5_PIPELINED_DRAWS", "1");
    _putenv_s("APS5_PIPELINE_DMA", "1");
#else
    setenv("APS5_PIPELINED_DRAWS", "1", 1);
    setenv("APS5_PIPELINE_DMA", "1", 1);
#endif
    try {
        const bool countOnly = argc == 2 && std::string(argv[1]) == "--count";
        const bool pendingImageOnly = argc == 2 && std::string(argv[1]) == "--pending-image";
        const bool disjointDmaOnly = argc == 2 && std::string(argv[1]) == "--disjoint-dma";
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
            if (countOnly || (argc == 3 && std::string(argv[1]) == "--indirect-batch" && (std::string(argv[2]) == "count" || std::string(argv[2]) == "count_producer"))) {
                const auto support = device->DrawIndirectSupport();
                if (!support.count || !support.multi) {
                    std::puts("pipelined indirect count requires indirect-count and multi-draw support");
                    return VulkanTestSkipped;
                }
            }
        }
        Registered(VertexHeader, VertexCode.data(), sizeof(VertexCode), 2);
        Registered(PixelHeader, PixelCode.data(), sizeof(PixelCode), 1);
        if (argc == 3 && std::string(argv[1]) == "--indirect-batch") {
            IndirectBatch(argv[2]);
            AgcDriverShutdown_nid_postfix();
            return 0;
        }
        DrawDirectReference();
        if (countOnly || pendingImageOnly || disjointDmaOnly) {
            if (countOnly) GpuSideIndirectCountQueuesBehindItsStore();
            if (pendingImageOnly) HeldImageArgumentsDrainBeforeRead(true);
            if (disjointDmaOnly) GpuSideIndirectDisjointDmaDoesNotDrain();
            AgcDriverShutdown_nid_postfix();
            return 0;
        }
        DrawIndexedDirectReference();
        IndirectDrawsQueueBehindTheCommitter();
        IndirectDrawWaitsForItsQueuedRecordWrite();
        IndirectDrawWaitsForQueuedDma();
        HeldImageArgumentsDrainBeforeRead();
        WriteCompletenessControlsIndirectDrain(true);
        WriteCompletenessControlsIndirectDrain(false);
        TrailingZeroCountRecordIsNotQueued();
        OutOfRangeIndexedRecordIsNotQueued();
        GpuSideIndirectDrawsQueueBehindTheCommitter();
        GpuSideIndirectDrawQueuesBehindTheStoreOfItsArguments();
        GpuSideIndirectArgumentsRewrittenAfterEnqueue();
        AgcDriverShutdown_nid_postfix();
        std::puts("pipelined indirect draw tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        AgcDriverShutdown_nid_postfix();
        return 1;
    }
}
