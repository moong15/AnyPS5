#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
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
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using AgcDriver::Graphics::StorageTexture;
constexpr std::uint32_t Side = 128;
constexpr std::uint32_t Format32UInt = 20;
constexpr std::uint32_t TileR64KBX = 0x1b;
constexpr std::uint32_t Type2D = 9;
constexpr std::size_t Block = 0x10000;
constexpr std::size_t BlockWords = Block / sizeof(std::uint32_t);
constexpr std::uint32_t Untouched = 0xdeadbeefu;
constexpr std::uint32_t Sentinel = 0xcdcdcdcdu;
constexpr std::uint32_t FirstStored = 0x13570000u;
constexpr std::uint32_t UserSgprs = 16;
constexpr std::uint32_t TgidYEnable = 1u << 8u;
constexpr std::uint32_t ReadThreads = 64;
constexpr std::uint32_t ReadStrideShift = 10;
constexpr int PlainRounds = 4;

static_assert((static_cast<std::size_t>(ReadThreads - 1u) << ReadStrideShift) + sizeof(std::uint32_t) <= Block);

constexpr std::uint32_t ShiftLeftThreadId(std::uint32_t destination, std::uint32_t shift) {
    return (0x1au << 25u) | (destination << 17u) | (0x80u + shift);
}

alignas(256) constexpr std::array<std::uint32_t, 6> StoreRowsCode{
    0x7e040300, 0x7e060210, 0x7e020200, 0xf0200108, 0x00020102, 0xbf810000,
};

alignas(256) constexpr std::array<std::uint32_t, 8> ReadSurfaceCode{
    ShiftLeftThreadId(1, ReadStrideShift), ShiftLeftThreadId(3, 2), 0xe0301000, 0x80010201, 0xbf8c3f70, 0xe0701000, 0x80000203, 0xbf810000,
};

alignas(256) constexpr std::array<std::uint32_t, 5> AddressStoreCode{
    ShiftLeftThreadId(1, 2), 0x7e040202, 0xdc708000, 0x00000201, 0xbf810000,
};

enum class Mode { Enabled, Disabled, Unimported };

std::uint64_t AddressOf(const void* data) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(data));
}

std::string Hex(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08x", value);
    return text;
}

std::array<std::uint32_t, 8> TextureDescriptor(const std::uint32_t* texels) {
    const auto address = AddressOf(texels);
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

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t bytes) {
    const auto address = AddressOf(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x31016facu};
}

void Dispatch(std::vector<std::uint32_t>& words, std::span<const std::uint32_t> code, std::uint32_t threads, std::uint32_t resource, std::span<const std::uint32_t> userData, std::uint32_t groups, std::uint32_t rows = 1) {
    const auto registers = [&](const std::uint32_t first, const std::span<const std::uint32_t> values) {
        words.push_back(0xc0007600u | (static_cast<std::uint32_t>(values.size()) << 16u));
        words.push_back(first);
        words.insert(words.end(), values.begin(), values.end());
    };
    const std::array<std::uint32_t, 3> shape{threads, 1, 1};
    registers(0x207, shape);
    const auto address = AddressOf(code.data());
    const std::array<std::uint32_t, 2> program{static_cast<std::uint32_t>(address >> 8u), static_cast<std::uint32_t>(address >> 40u)};
    registers(0x20c, program);
    const std::array<std::uint32_t, 1> layout{resource};
    registers(0x213, layout);
    registers(0x240, userData);
    words.insert(words.end(), {0xc0031500u, groups, rows, 1, 0x41u});
}

class GuestBlock {
public:
    explicit GuestBlock(std::uint32_t fill) {
#ifdef _WIN32
        region = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(Block, Block));
        if (region != nullptr) GuestArena::GuestArenaCommit_nid_postfix(region, Block, PAGE_READWRITE, Block);
#else
        void* mapped = mmap(nullptr, Block + Block, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapped != MAP_FAILED) {
            const auto begin = reinterpret_cast<std::uintptr_t>(mapped);
            const auto aligned = (begin + Block - 1) & ~std::uintptr_t{Block - 1};
            if (aligned != begin) munmap(mapped, aligned - begin);
            if (aligned != begin + Block) munmap(reinterpret_cast<void*>(aligned + Block), begin + Block - aligned);
            region = reinterpret_cast<std::uint8_t*>(aligned);
            GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(region, Block);
        }
#endif
        Require(region != nullptr, "region ordered flush: cannot allocate a guest block");
        words = reinterpret_cast<std::uint32_t*>(region);
        std::fill(words, words + BlockWords, fill);
        GuestAllocations::Mutation().Add(region, Block, true, true, true);
    }

    ~GuestBlock() {
        GuestAllocations::Mutation().Remove(region);
#ifdef _WIN32
        GuestArena::GuestArenaReset_nid_postfix(region, Block);
        GuestArena::GuestArenaRelease_nid_postfix(region, Block);
#else
        munmap(region, Block);
        GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(region, Block);
#endif
    }

    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;

    std::uint32_t* Words() const { return words; }
    std::uint64_t Address() const { return AddressOf(region); }

private:
    std::uint8_t* region = nullptr;
    std::uint32_t* words = nullptr;
};

struct Blocks {
    const GuestBlock& surface;
    const GuestBlock& output;
    const GuestBlock& scratch;
};

void StoreRows(std::vector<std::uint32_t>& words, const GuestBlock& surface, std::uint32_t value) {
    std::array<std::uint32_t, UserSgprs> userData{};
    userData[0] = value;
    const auto texture = TextureDescriptor(surface.Words());
    std::copy(texture.begin(), texture.end(), userData.begin() + 8);
    Dispatch(words, StoreRowsCode, Side, (UserSgprs << 1u) | TgidYEnable, userData, 1, Side);
}

void StoreThroughAddress(std::vector<std::uint32_t>& words, const GuestBlock& scratch, std::uint32_t value) {
    std::array<std::uint32_t, UserSgprs> userData{};
    userData[0] = static_cast<std::uint32_t>(scratch.Address());
    userData[1] = static_cast<std::uint32_t>(scratch.Address() >> 32u);
    userData[2] = value;
    Dispatch(words, AddressStoreCode, ReadThreads, UserSgprs << 1u, userData, 1);
}

void ReadSurface(std::vector<std::uint32_t>& words, const GuestBlock& surface, const GuestBlock& output) {
    std::array<std::uint32_t, UserSgprs> userData{};
    const auto written = BufferDescriptor(output.Words(), ReadThreads * static_cast<std::uint32_t>(sizeof(std::uint32_t)));
    const auto read = BufferDescriptor(surface.Words(), static_cast<std::uint32_t>(Block));
    std::copy(written.begin(), written.end(), userData.begin());
    std::copy(read.begin(), read.end(), userData.begin() + 4);
    Dispatch(words, ReadSurfaceCode, ReadThreads, UserSgprs << 1u, userData, 1);
}

void Expect(const GuestBlock& block, std::size_t count, std::uint32_t value, const std::string& what) {
    std::vector<std::uint32_t> seen(count);
    AgcDriver::GuestMemory::Read(block.Address(), std::as_writable_bytes(std::span(seen)), sizeof(std::uint32_t));
    for (std::size_t index = 0; index < count; ++index) {
        Require(seen[index] == value, "region ordered flush: " + what + " holds " + Hex(seen[index]) + " at dword " + std::to_string(index) + " instead of the stored " + Hex(value));
    }
}

void Round(const Blocks& blocks, std::uint32_t value, bool lease, const std::string& what) {
    std::vector<std::uint32_t> words;
    StoreRows(words, blocks.surface, value);
    if (lease) StoreThroughAddress(words, blocks.scratch, value);
    ReadSurface(words, blocks.surface, blocks.output);
    Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
    AgcDriver::Submit(&packet, 0);
    AgcDriverWaitIdle_nid_postfix();
    Expect(blocks.output, ReadThreads, value, what + ", the output of the buffer read of the surface");
    Expect(blocks.surface, BlockWords, value, what + ", guest memory of the surface");
    if (lease) Expect(blocks.scratch, ReadThreads, value, what + ", the address-based store");
}

std::uint64_t PlainSkips(const Blocks& blocks, std::uint32_t& value, const std::string& what) {
    std::uint64_t skips = 0;
    for (int round = 0; round < PlainRounds; ++round) {
        Round(blocks, value++, false, what + " " + std::to_string(round));
        skips += StorageTexture::TakeRegionFlushSkips();
    }
    return skips;
}

}

int main(int argc, char** argv) {
    try {
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
        }
        struct ShutdownGuard {
            ~ShutdownGuard() { try { AgcDriverShutdown_nid_postfix(); } catch (...) {} }
        } shutdown;
        const std::string option = argc > 1 ? argv[1] : "";
        const Mode mode = option == "--disabled" ? Mode::Disabled : option == "--unimported" ? Mode::Unimported : Mode::Enabled;
        GuestBlock surface(Untouched);
        const auto described = AgcDriver::Graphics::DescribeSurface(AgcDriver::Graphics::DecodeTextureResource(TextureDescriptor(surface.Words()))).guestBytes;
        Require(described == Block, "region ordered flush: the tiled surface does not cover exactly its guest block");
        GuestBlock output(Sentinel);
        GuestBlock scratch(Sentinel);
        const Blocks blocks{surface, output, scratch};
        std::uint32_t value = FirstStored;
        static_cast<void>(StorageTexture::TakeRegionFlushSkips());
        Round(blocks, value++, true, "the first build");
        Require(StorageTexture::TakeRegionFlushSkips() == 0, "region ordered flush: a first build, which reuses no template, skipped a writer wait");
        const auto before = PlainSkips(blocks, value, "plain round");
        Round(blocks, value++, true, "the round with an address-based store");
        const auto leased = StorageTexture::TakeRegionFlushSkips();
        const auto after = PlainSkips(blocks, value, "plain round after the address-based store");
        Require(leased == 0, "region ordered flush: the reused read skipped its writer wait while an address-based dispatch holding a lease over the surface was pending");
        if (mode == Mode::Enabled) {
            Require(before != 0, "region ordered flush: no reused read of the imported surface left its region flush to GPU order");
            Require(after != 0, "region ordered flush: no reused read left its region flush to GPU order once the address-based dispatch had completed");
        } else {
            Require(before == 0 && after == 0, "region ordered flush: a region flush skipped its writer wait with APS5_NO_REGION_FLUSH_GPU_ORDERED or no host import");
        }
        const auto last = value - 1u;
        Expect(output, ReadThreads, last, "the final output");
        Expect(surface, BlockWords, last, "the final guest memory of the surface");
        AgcDriverShutdown_nid_postfix();
        std::puts("region ordered flush tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
