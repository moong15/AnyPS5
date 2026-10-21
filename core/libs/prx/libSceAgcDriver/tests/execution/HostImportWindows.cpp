#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <mutex>
#include <string>

namespace AgcDriver {
struct VulkanDeviceTestAccess {
    static Graphics::Context Context(const VulkanDevice& device) { return device.graphicsContext(); }
};
}

namespace {

namespace Graphics = AgcDriver::Graphics;
using Graphics::Require;

constexpr std::uint64_t MiB = 1ull << 20u;
constexpr std::uint64_t WindowBytes = 64 * MiB;
constexpr std::uint64_t BlockBytes = 2 * WindowBytes;
constexpr std::uint64_t OtherBytes = 64 * 1024;
constexpr std::uint64_t GuestAlignment = 64 * 1024;
constexpr std::uint64_t ProbeBytes = 4096;

std::string Hex(std::uint64_t value) {
    char text[32];
    std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(value));
    return text;
}

class GuestBlock {
public:
    explicit GuestBlock(std::uint64_t size) : bytes(size) {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
        block = static_cast<std::uint8_t*>(std::aligned_alloc(GuestAlignment, bytes));
#endif
        Require(block != nullptr, "host import windows: cannot allocate a guest block of " + Hex(bytes));
        std::memset(block, 0, bytes);
        GuestAllocations::Mutation().Add(block, bytes, true, true, true);
    }

    ~GuestBlock() {
        GuestAllocations::Mutation().Remove(block);
#ifdef _WIN32
        VirtualFree(block, 0, MEM_RELEASE);
#else
        std::free(block);
#endif
    }

    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;

    std::uint8_t* Data() const { return block; }
    std::uint64_t Address() const { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(block)); }

private:
    std::uint8_t* block = nullptr;
    std::uint64_t bytes = 0;
};

enum class Outcome : std::uint8_t { NoDevice, NoImport, Passed };

Graphics::Context Probe(const AgcDriver::VulkanDevice& device) {
    Graphics::Context probe{};
    probe.device = device.Device();
    probe.hostImportAlignment = 1;
    return probe;
}

bool Fill(AgcDriver::VulkanDevice& device, std::uint64_t address) {
    const std::array<std::uint32_t, 4> pattern{0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u};
    std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
    const bool filled = device.FillBuffer(address, ProbeBytes, pattern);
    device.WaitIdle();
    return filled;
}

double MappedMiB() {
    const auto close = [](std::FILE* file) { std::fclose(file); };
    std::unique_ptr<std::FILE, decltype(close)> output(std::tmpfile(), close);
    Require(output != nullptr, "mapped ledger: tmpfile failed");
    AgcDriver::ReportVulkanMemory_nid_no_patch(output.get());
    std::rewind(output.get());
    char line[8192]{};
    Require(std::fgets(line, sizeof(line), output.get()) != nullptr, "mapped ledger: report missing");
    const auto* field = std::strstr(line, "; mapped dl ");
    std::array<double, 5> bytes{};
    Require(field != nullptr && std::sscanf(field, "; mapped dl %lf (%*u) dlhv %lf (%*u) hv %lf (%*u) none %lf (%*u) import %lf", &bytes[0], &bytes[1], &bytes[2], &bytes[3], &bytes[4]) == 5, "mapped ledger: report malformed");
    double total = 0;
    for (const auto value : bytes) total += value;
    return total;
}

void CheckMappedLedger(const AgcDriver::VulkanDevice& device) {
    if (!ShaderRecompiler::HostMemoryLedgerEnabled()) return;
    const auto context = AgcDriver::VulkanDeviceTestAccess::Context(device);
    std::uint32_t type = 0;
    while (type < context.memory.memoryTypeCount && !(context.memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) ++type;
    Require(type < context.memory.memoryTypeCount, "mapped ledger: no host-visible memory type");
    VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    info.allocationSize = MiB;
    info.memoryTypeIndex = type;
    struct Memory {
        Graphics::Context context;
        VkDeviceMemory handle = VK_NULL_HANDLE;
        ~Memory() { if (handle != VK_NULL_HANDLE) context.Function<PFN_vkFreeMemory>("vkFreeMemory")(context.device, handle, nullptr); }
    } memory{context};
    Graphics::Check(context.Function<PFN_vkAllocateMemory>("vkAllocateMemory")(context.device, &info, nullptr, &memory.handle), "mapped ledger allocation");
    const auto before = MappedMiB();
    void* data = nullptr;
    Graphics::Check(context.Function<PFN_vkMapMemory>("vkMapMemory")(context.device, memory.handle, 4096, VK_WHOLE_SIZE, 0, &data), "mapped ledger map");
    const auto during = MappedMiB();
    const double expected = (info.allocationSize - 4096) / static_cast<double>(MiB);
    Require(during - before > expected - 0.0011 && during - before < expected + 0.0011, "mapped ledger: whole size minus offset not recorded");
    context.Function<PFN_vkUnmapMemory>("vkUnmapMemory")(context.device, memory.handle);
    const auto after = MappedMiB();
    Require(after == before, "mapped ledger: bytes remained after unmap");
}

Outcome ClearingDropsImports() {
    GuestBlock guest(BlockBytes);
    auto device = OpenVulkanTestDevice();
    if (!device) return Outcome::NoDevice;
    CheckMappedLedger(*device);
    if (!Fill(*device, guest.Address() + WindowBytes)) return Outcome::NoImport;
    const auto probe = Probe(*device);
    Require(Graphics::HostImportCovers(probe, guest.Address() + WindowBytes, ProbeBytes), "host import windows: the second window of " + Hex(guest.Address()) + " is not imported");
    Graphics::ClearHostImports(device->Device());
    Require(!Graphics::HostImportCovers(probe, guest.Address() + WindowBytes, ProbeBytes), "host import windows: ClearHostImports kept the window of " + Hex(guest.Address()));
    return Outcome::Passed;
}

Outcome WindowsFollowTheirRange() {
    GuestBlock guest(BlockBytes);
    auto device = OpenVulkanTestDevice();
    if (!device) return Outcome::NoDevice;
    const auto probe = Probe(*device);
    const auto address = guest.Address() + WindowBytes + WindowBytes / 2;
    if (!Fill(*device, address)) return Outcome::NoImport;
    Require(Graphics::HostImportCovers(probe, guest.Address() + WindowBytes, ProbeBytes), "host import windows: the second window of " + Hex(guest.Address()) + " is not imported");
    Require(!Graphics::HostImportCovers(probe, guest.Address(), ProbeBytes), "host import windows: the first window of " + Hex(guest.Address()) + " is imported along with the second");
    const auto first = Graphics::HostImportSerial(probe, address, ProbeBytes, false);
    Require(first != 0, "host import windows: the window at " + Hex(address) + " has no serial");

    GuestBlock other(OtherBytes);
    Require(Fill(*device, address), "host import windows: the window at " + Hex(address) + " cannot be filled after an unrelated registration");
    Require(Graphics::HostImportSerial(probe, address, ProbeBytes, false) == first, "host import windows: the window at " + Hex(address) + " was dropped by an unrelated registration, although its range still owns it");

    GuestAllocations::Mutation().Remove(guest.Data());
    GuestAllocations::Mutation().Add(guest.Data(), BlockBytes, true, true, true);
    Require(Fill(*device, address), "host import windows: the window at " + Hex(address) + " cannot be filled after its range was mapped again");
    const auto second = Graphics::HostImportSerial(probe, address, ProbeBytes, false);
    Require(second != 0 && second != first, "host import windows: the window of the range mapped again kept the serial " + Hex(first));
    return Outcome::Passed;
}

Outcome ResidentMappingsImportAfterCollection() {
#ifdef _WIN32
    constexpr std::size_t bytes = MiB;
    auto* data = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(bytes, 0x200000));
    Require(data != nullptr, "cannot allocate resident import address");
    const auto section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_RESERVE, 0, bytes, nullptr);
    Require(section != nullptr, "cannot reserve resident import backing");
    GuestArena::GuestArenaMapDirect_nid_postfix(data, bytes, section, 0, PAGE_READWRITE);
    std::memset(data, 0x37, bytes);
    GuestAllocations::Mutation().Add(data, bytes, true, true, true);
    const auto address = reinterpret_cast<std::uint64_t>(data);
    Require(AgcDriver::GuestMemory::CollectWritesUncached(address, bytes) != 0, "resident import memory is not watched");
    {
        auto device = OpenVulkanTestDevice();
        Require(device != nullptr, "resident import test lost its Vulkan device");
        const auto context = Probe(*device);
        for (unsigned int pass = 0; pass < 2; ++pass) {
            Require(AgcDriver::GuestMemory::CollectWritesUncached(address, bytes) != 0, "resident import collection failed");
            Require(Fill(*device, address), "resident memory could not be filled after collecting its writes");
            std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
            Require(Graphics::HostImportCovers(context, address, ProbeBytes), "resident import did not cover its data");
            Graphics::ClearHostImports(device->Device());
        }
        Require(Fill(*device, address), "resident memory could not be filled after collecting its writes");
        std::uint32_t first = 0;
        std::memcpy(&first, data, sizeof(first));
        Require(first == 0x11111111u && data[ProbeBytes] == 0x37 && data[bytes - 1] == 0x37, "resident import lost the GPU fill or neighboring bytes");
        Graphics::ClearHostImports(device->Device());
    }
    GuestAllocations::Mutation().Remove(data);
    GuestArena::GuestArenaReset_nid_postfix(data, bytes);
    GuestArena::GuestArenaRelease_nid_postfix(data, bytes);
    GuestArena::GuestArenaForgetSection_nid_postfix(section);
    CloseHandle(section);
#endif
    return Outcome::Passed;
}

}

int main() {
    try {
        const auto counting = ClearingDropsImports();
        if (counting == Outcome::NoDevice) return VulkanTestSkipped;
        if (counting == Outcome::NoImport) {
            std::printf("skipped, the device does not import guest memory\n");
            return VulkanTestSkipped;
        }
        const auto windows = WindowsFollowTheirRange();
        Require(windows == Outcome::Passed, "host import windows: the device stopped importing guest memory between two scenarios");
        Require(ResidentMappingsImportAfterCollection() == Outcome::Passed, "resident import test failed");
        std::puts("host import windows tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
