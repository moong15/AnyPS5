#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using AgcDriver::Graphics::StorageTexture;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Side = 512;
constexpr std::uint32_t Format32UInt = 20;
constexpr std::uint32_t TileR64KBX = 0x1b;
constexpr std::uint32_t Type2D = 9;
constexpr std::uint64_t SurfaceBytes = std::uint64_t{Side} * Side * 4u;
constexpr std::uint64_t Stride = 0x100000;
constexpr std::uint64_t Budget = 32ull << 20u;
constexpr std::uint32_t MaxSurfaces = 40;

alignas(256) constexpr std::array<std::uint32_t, 8> StoreCode{
    0x7e020280, 0x7e0402f2, 0x7e0602f2, 0x7e0802f2, 0x7e0a02f2, 0xf0201f08, 0x00010200, 0xbf810000,
};

std::array<std::uint32_t, 8> TextureDescriptor(std::uint64_t address) {
    return {
        static_cast<std::uint32_t>(address >> 8u),
        static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (Format32UInt << 20u) | (((Side - 1u) & 3u) << 30u),
        ((Side - 1u) >> 2u) | ((Side - 1u) << 14u),
        0xfacu | (TileR64KBX << 20u) | (Type2D << 28u),
        0u,
        0u,
        0u,
        0u,
    };
}

std::shared_ptr<StorageTexture> Store(AgcDriver::VulkanDevice& device, std::uint64_t address) {
    std::vector<std::uint32_t> userData(16, 0u);
    const auto texture = TextureDescriptor(address);
    std::copy(texture.begin(), texture.end(), userData.begin() + 4);
    const std::span<const std::uint32_t> code(StoreCode);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{32, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {32, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = true;
    const auto result = ShaderRecompiler::Recompile(request);
    device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
    device.WaitIdle();
    StorageTexture::FlushPending(address, static_cast<std::size_t>(SurfaceBytes), nullptr, "test");
    device.WaitIdle();
    return StorageTexture::FindLive(address, SurfaceBytes);
}

}

int main() {
    try {
#ifdef _WIN32
        Require(_putenv_s("APS5_TEXTURE_CACHE_MIB", "32") == 0, "cannot set the test cache budget");
#else
        Require(setenv("APS5_TEXTURE_CACHE_MIB", "32", 1) == 0, "cannot set the test cache budget");
#endif
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        std::vector<std::uint32_t> storage((MaxSurfaces * Stride + Stride) / 4u, 0u);
        auto* texels = reinterpret_cast<std::uint32_t*>((reinterpret_cast<std::uintptr_t>(storage.data()) + Stride - 1u) & ~std::uintptr_t{Stride - 1u});
        const auto base = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(texels));
        const auto blockBytes = static_cast<std::size_t>((MaxSurfaces - 1u) * Stride + SurfaceBytes);
        {
            GuestAllocations::Mutation mutation;
            mutation.Add(texels, blockBytes, true, true, true);
        }
        const auto release = [&] {
            AgcDriver::Graphics::ClearCachedTextures(device->Device());
            GuestAllocations::Mutation mutation;
            mutation.Remove(texels);
        };
        try {
            auto first = Store(*device, base);
            Require(first != nullptr, "the copy source has no cached storage image");
            const auto held = std::max<std::uint64_t>(first->AllocationBytes(), first->GuestBytes());
            first.reset();
            const auto capacity = static_cast<std::uint32_t>(Budget / held);
            Require(capacity >= 2u && capacity < MaxSurfaces, "the test storage does not fit " + std::to_string(capacity) + " cached surfaces");
            for (std::uint32_t surface = 1; surface < capacity; ++surface) Require(Store(*device, base + surface * Stride) != nullptr, "surface " + std::to_string(surface) + " is not cached");
            Require(StorageTexture::FindLive(base, SurfaceBytes) != nullptr, "the copy source was evicted before the copy");
            const auto destination = base + capacity * Stride;
            Require(StorageTexture::FindLive(destination, SurfaceBytes) == nullptr, "the copy destination already has an image");
            const auto before = AgcDriver::VulkanDevice::CopyAliasCounts();
            AgcDriver::VulkanDevice::CopyOutcome outcome{};
            {
                std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
                outcome = device->CopyBuffer(destination, base, static_cast<std::size_t>(SurfaceBytes), 0, std::numeric_limits<std::size_t>::max(), 0, 0, 0, [](std::span<const std::byte>, std::uint64_t) {});
            }
            device->WaitIdle();
            const auto after = AgcDriver::VulkanDevice::CopyAliasCounts();
            Require(after.created == before.created + 1u, "the copy did not make a destination image for a device copy");
            Require(StorageTexture::FindLive(base, SurfaceBytes) == nullptr, "making the destination image did not evict the copy source");
            Require(after.refusedLate == before.refusedLate + 1u, "a device copy whose source was evicted after its check was not counted as refused");
            Require(after.aliased == before.aliased && outcome.path != 3, "a device copy ran from an evicted source");
        } catch (...) {
            release();
            throw;
        }
        release();
        std::puts("copy alias eviction tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
