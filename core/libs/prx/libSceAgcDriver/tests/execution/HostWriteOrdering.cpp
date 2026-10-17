#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <array>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <memory>
#include <span>
#include <vector>

namespace AgcDriver {

struct VulkanDeviceTestAccess {
    static Graphics::Context ContextOf(const VulkanDevice& device) { return device.graphicsContext(); }
};

}

namespace {

using AgcDriver::Graphics::Require;
using AgcDriver::Graphics::Recorder;
using AgcDriver::Graphics::StorageTexture;
constexpr std::size_t BlockBytes = 65536;
constexpr std::size_t TexelBytes = 8192;
constexpr std::uint8_t HostByte = 0xa5;

alignas(256) constexpr std::array<std::uint32_t, 24> StoreCode{
    0x4a500081, 0xd5690014, 0x0201ff28, 0x9e3779b1, 0x4a2a28ff, 0x7f4a7c15, 0x4a2c28ff, 0xfe94f82a,
    0x4a2e28ff, 0x7ddf743f, 0x7e3c0300, 0x7e3e0280, 0xf0281f08, 0x0001141e, 0x7e3e0281, 0xf0281508,
    0x0001141e, 0x7e3e0282, 0xf0281a08, 0x0001141e, 0x7e3e0283, 0xf0281808, 0x0001141e, 0xbf810000,
};

class GuestBlock {
public:
    GuestBlock() {
        data = GuestArena::GuestArenaAllocate_nid_postfix(BlockBytes, BlockBytes);
        GuestArena::GuestArenaCommit_nid_postfix(data, BlockBytes, 0x04, BlockBytes);
        GuestAllocations::Mutation().Add(data, BlockBytes, true, true, true);
        std::memset(data, 0x55, BlockBytes);
    }
    ~GuestBlock() {
        GuestAllocations::Mutation().Remove(data);
        GuestArena::GuestArenaReset_nid_postfix(data, BlockBytes);
        GuestArena::GuestArenaRelease_nid_postfix(data, BlockBytes);
    }
    void* Data() const { return data; }
    std::uintptr_t Address() const { return reinterpret_cast<std::uintptr_t>(data); }
    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;

private:
    void* data = nullptr;
};

void Dispatch(AgcDriver::VulkanDevice& device, const GuestBlock& block) {
    const auto address = block.Address();
    const std::array<std::uint32_t, 8> texture{
        static_cast<std::uint32_t>(address >> 8u),
        static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (60u << 20u) | (3u << 30u),
        (31u >> 2u) | (7u << 14u), 0xfacu | (9u << 28u), 0u, 0u, 0u, 0u,
    };
    std::vector<std::uint32_t> userData(16, 0);
    std::copy(texture.begin(), texture.end(), userData.begin() + 4);
    const auto code = std::span(StoreCode);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{32, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderRecompiler::ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {32, 0, userData, compute, std::nullopt, std::nullopt, memory}, device.Target(), {0, 0, 0, 128}
    };
    const auto shader = ShaderRecompiler::Recompile(request);
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    device.Dispatch(shader, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
}

bool RecordedOverlaps(const GuestBlock& block) {
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    const auto* recorder = Recorder::Active();
    return recorder != nullptr && recorder->PendingWriteOverlaps(block.Address(), TexelBytes, false);
}

void Fill(const GuestBlock& block, bool ordered, std::uint8_t value = HostByte) {
    Require(!AgcDriver::GuestMemory::GpuMutex().HeldByThisThread(), "host write entered while holding the GPU mutex");
    const GuestArena::HostWrite destination(block.Data(), TexelBytes);
    Require(destination.Open(), "host write could not open the destination");
    if (ordered) Require(!RecordedOverlaps(block), "host write left a recorded store pending");
    std::memset(block.Data(), value, TexelBytes);
}

bool Holds(const GuestBlock& block, std::uint8_t value) {
    const auto* data = static_cast<const std::uint8_t*>(block.Data());
    return std::all_of(data, data + TexelBytes, [value](std::uint8_t byte) { return byte == value; });
}

void Finish(AgcDriver::VulkanDevice& device) {
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    device.WaitIdle();
}

void DeferredImage(AgcDriver::VulkanDevice& device, bool ordered, std::uint8_t value) {
    GuestBlock block;
    Dispatch(device, block);
    const std::array<std::pair<std::uint64_t, std::uint64_t>, 1> range{{{block.Address(), block.Address() + TexelBytes}}};
    Require(StorageTexture::AnyPendingOverlaps(range), "dispatch did not leave a deferred storage image");
    Fill(block, ordered, value);
    Require(StorageTexture::AnyPendingOverlaps(range) != ordered, "host write used the wrong deferred image order");
    StorageTexture::FlushPending(block.Address(), TexelBytes, nullptr, "host ordering test");
    Finish(device);
    if (ordered) Require(Holds(block, value), "deferred image overwrote host bytes");
    else if (value == 0x55) Require(!Holds(block, value), "same-baseline case did not expose the old overwrite");
}

bool RecordedImage(AgcDriver::VulkanDevice& device, bool ordered) {
    GuestBlock block;
    Dispatch(device, block);
    StorageTexture::FlushPending(block.Address(), TexelBytes, nullptr, "host ordering test");
    if (!RecordedOverlaps(block)) {
        Finish(device);
        std::puts("recorded storage write-back unavailable without host imports");
        return false;
    }
    Fill(block, ordered);
    if (!ordered) Require(RecordedOverlaps(block), "disabled ordering retired the recorded store");
    Finish(device);
    Require(Holds(block, HostByte) == ordered, "recorded storage write-back used the wrong host order");
    return true;
}

void CopiedCompletion(AgcDriver::VulkanDevice& device, bool ordered, bool submitted, bool foreign) {
    GuestBlock block;
    const auto copy = std::make_shared<std::vector<std::byte>>(TexelBytes, std::byte{0x39});
    std::unique_ptr<Recorder> other;
    Recorder* primary = nullptr;
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        primary = Recorder::Active();
        Require(primary != nullptr, "missing active recorder");
        if (foreign) other = std::make_unique<Recorder>(AgcDriver::VulkanDeviceTestAccess::ContextOf(device));
        auto* recorder = foreign ? other.get() : primary;
        recorder->Activate();
        recorder->Commands();
        if (submitted) recorder->Submit();
        const std::array<std::byte, 4> label{};
        if (submitted) recorder->AfterCompletions(block.Address(), label, 1, 0, false);
        recorder->NotePendingWrite(block.Address(), TexelBytes / 2);
        recorder->NotePendingWrite(block.Address(), TexelBytes);
        recorder->OnComplete([copy, address = block.Address()] {
            AgcDriver::GuestMemory::StoreOwnBytes(address, copy->size(), [&] {
                std::memcpy(reinterpret_cast<void*>(address), copy->data(), copy->size());
            });
        });
        if (submitted) recorder->Submit();
        if (foreign) {
            primary->Activate();
            primary->Commands();
            primary->NotePendingWrite(block.Address() + BlockBytes / 2, 4);
        }
    }
    Fill(block, ordered);
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        if (other) {
            if (ordered) Require(!other->PendingWriteOverlaps(block.Address(), TexelBytes, false), "host write left a foreign recorder pending");
            other->Sync();
            other.reset();
        }
        primary->Activate();
    }
    Finish(device);
    Require(Holds(block, ordered ? HostByte : 0x39), "copied completion used the wrong host order");
}

void UnrelatedWrite(AgcDriver::VulkanDevice& device, bool ordered) {
    GuestBlock block;
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        auto* recorder = Recorder::Active();
        recorder->Commands();
        recorder->NotePendingWrite(block.Address() + BlockBytes / 2, 4);
    }
    Fill(block, ordered);
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        auto* recorder = Recorder::Active();
        Require(recorder->OpenWriteOverlaps(block.Address() + BlockBytes / 2, 4), "host write drained unrelated work");
    }
    Finish(device);
}

void NoPendingWrite() {
    GuestBlock block;
    const auto before = Recorder::PendingWriteTests();
    {
        const GuestArena::HostWrite destination(block.Data(), TexelBytes);
        Require(destination.Open(), "idle host write could not open the destination");
        std::memset(block.Data(), HostByte, TexelBytes);
    }
    Require(Recorder::PendingWriteTests() == before, "idle host write walked recorder ranges");
    Require(Holds(block, HostByte), "idle host write lost bytes");
}

}

std::array<std::uint8_t, 32> helperBytes{};
bool helperPending = false;
std::size_t helperCalls = 0;

void CompleteHelperWrite() {
    if (!helperPending) return;
    helperBytes.fill(0x39);
    helperPending = false;
}

void OrderHelperWrite(std::uintptr_t address, std::size_t bytes) {
    Require(address == reinterpret_cast<std::uintptr_t>(helperBytes.data()) && bytes == helperBytes.size(), "host write callback received the wrong range");
    ++helperCalls;
    CompleteHelperWrite();
}

int HelperOnly() {
    const bool ordered = std::getenv("APS5_NO_HOST_WRITE_ORDER") == nullptr;
    GuestArena::GuestArenaSetHostWriteOrder_nid_no_patch(&OrderHelperWrite);
    helperPending = true;
    {
        const GuestArena::HostWrite destination(helperBytes.data(), helperBytes.size());
        Require(destination.Open(), "host write helper refused writable memory");
        helperBytes.fill(HostByte);
    }
    CompleteHelperWrite();
    Require(helperCalls == (ordered ? 1u : 0u), "host write helper used the wrong switch policy");
    Require(std::all_of(helperBytes.begin(), helperBytes.end(), [ordered](auto byte) { return byte == (ordered ? HostByte : 0x39); }), "host write helper used the wrong completion order");
    GuestArena::GuestArenaSetHostWriteOrder_nid_no_patch(nullptr);
    std::puts("host write helper tests passed");
    return 0;
}

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::strcmp(argv[1], "--helper") == 0) return HelperOnly();
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        const bool ordered = std::getenv("APS5_NO_HOST_WRITE_ORDER") == nullptr;
        DeferredImage(*device, ordered, HostByte);
        DeferredImage(*device, ordered, 0x55);
        const bool recorded = RecordedImage(*device, ordered);
        CopiedCompletion(*device, ordered, false, false);
        CopiedCompletion(*device, ordered, true, false);
        CopiedCompletion(*device, ordered, false, true);
        UnrelatedWrite(*device, ordered);
        NoPendingWrite();
        std::puts("host write ordering tests passed");
        return recorded ? 0 : VulkanTestSkipped;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
