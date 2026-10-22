#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DccMetadata.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include "Recompiler.hpp"
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
#include <cstdlib>
#include <string_view>
#include <chrono>
#include <cstring>
#include <thread>
#include <cstdio>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::size_t BlockBytes = 1u << 20u;
constexpr std::uint32_t Width = 256;
constexpr std::uint32_t Height = 128;
constexpr std::size_t ImageBytes = Width * Height * 4u;
constexpr std::uint32_t Format32UInt = 20;
constexpr std::uint32_t Type2D = 9;
constexpr std::uint32_t Initial = 0x3f800000u;
constexpr std::uint32_t Stored = 0x11111111u;
constexpr std::uint32_t Overwritten = 0x22222222u;
constexpr std::size_t OverwrittenDword = 0x100u / 4u;
constexpr std::size_t FarDword = 100u * Width;
constexpr std::size_t KeysOffset = 0x40000u;
constexpr std::size_t KeysBytes = 4096u;

alignas(256) constexpr std::array<std::uint32_t, 11> StoreImageCode{0x7e000280u, 0x7e020280u, 0x7e0402ffu, Stored, 0xf0200108u, 0x00010200u, 0x7e0202ffu, 0x00000064u, 0xf0200108u, 0x00010200u, 0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 7> StoreGlobalCode{0x7e000208u, 0x7e020209u, 0x7e0402ffu, Overwritten, 0xdc708100u, 0x007d0200u, 0xbf810000u};

class GuestBlock {
public:
    explicit GuestBlock(bool watched) : watched(watched) {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(BlockBytes, 65536));
        if (block != nullptr) GuestArena::GuestArenaCommit_nid_postfix(block, BlockBytes, PAGE_READWRITE, BlockBytes);
#else
        constexpr std::uintptr_t alignment = 65536;
        void* mapped = mmap(nullptr, BlockBytes + alignment, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapped != MAP_FAILED) {
            const auto begin = reinterpret_cast<std::uintptr_t>(mapped);
            const auto aligned = (begin + alignment - 1) & ~(alignment - 1);
            if (aligned != begin) munmap(mapped, aligned - begin);
            if (aligned + BlockBytes != begin + BlockBytes + alignment) munmap(reinterpret_cast<void*>(aligned + BlockBytes), begin + alignment - aligned);
            block = reinterpret_cast<std::uint8_t*>(aligned);
            if (watched) GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, BlockBytes);
        }
#endif
        Require(block != nullptr, "storage flush ordering: cannot allocate the guest block");
        Require(!watched || AgcDriver::GuestMemory::Watched(Address(), BlockBytes), "storage flush ordering: the guest block is not write-watched");
        std::fill(Words(), Words() + BlockBytes / 4u, Initial);
        GuestAllocations::Mutation().Add(block, BlockBytes, true, true, true);
        AgcDriver::GuestMemory::CollectWritesUncached(Address(), BlockBytes);
    }

    ~GuestBlock() {
        GuestAllocations::Mutation().Remove(block);
#ifdef _WIN32
        GuestArena::GuestArenaReset_nid_postfix(block, BlockBytes);
        GuestArena::GuestArenaRelease_nid_postfix(block, BlockBytes);
#else
        munmap(block, BlockBytes);
        if (watched) GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(block, BlockBytes);
#endif
    }

    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;

    std::uint32_t* Words() const { return reinterpret_cast<std::uint32_t*>(block); }
    std::uint64_t Address() const { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(block)); }

private:
    bool watched;
    std::uint8_t* block = nullptr;
};

ShaderRecompiler::RecompileResult Compile(std::span<const std::uint32_t> code, std::span<const std::uint32_t> userData, const ShaderRecompiler::SpirvTarget& target) {
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{64u, 1u, 1u}, 0u, {false, false, false}, false, 1u};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {64u, 0, userData, compute, std::nullopt, std::nullopt, memory},
        target,
        {0, 0, 0, 128}
    };
    request.useCache = false;
    return ShaderRecompiler::Recompile(request);
}

std::vector<std::uint32_t> ImageUserData(std::uint64_t base) {
    std::vector<std::uint32_t> userData(12, 0u);
    userData[4] = static_cast<std::uint32_t>(base >> 8u);
    userData[5] = static_cast<std::uint32_t>((base >> 40u) & 0xffu) | (Format32UInt << 20u) | (((Width - 1u) & 3u) << 30u);
    userData[6] = ((Width - 1u) >> 2u) | ((Height - 1u) << 14u);
    userData[7] = 0xfacu | (Type2D << 28u);
    return userData;
}

std::vector<std::uint32_t> GlobalUserData(std::uint64_t base) {
    std::vector<std::uint32_t> userData(10, 0u);
    userData[8] = static_cast<std::uint32_t>(base);
    userData[9] = static_cast<std::uint32_t>(base >> 32u);
    return userData;
}

bool AddressBasedStorePending(std::uint64_t image, bool open) {
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    const auto* recorder = AgcDriver::Graphics::Recorder::Active();
    const auto read = recorder != nullptr ? recorder->DescribePendingRead(image + ImageBytes, 4u) : std::nullopt;
    return read.has_value() && read->kind == AgcDriver::Graphics::Recorder::ReadKind::AddressBased && read->open == open;
}

void Run(AgcDriver::VulkanDevice& device, const GuestBlock& guest, std::size_t offset, bool submit, const char* name) {
    const auto* words = guest.Words() + offset / 4u;
    const auto image = guest.Address() + offset;
    const auto target = device.Target();
    const auto storeImage = Compile(StoreImageCode, ImageUserData(image), target);
    const auto storeGlobal = Compile(StoreGlobalCode, GlobalUserData(image), target);
    device.Dispatch(storeImage, 1u, 1u, 1u, {}, reinterpret_cast<std::uintptr_t>(StoreImageCode.data()));
    device.Dispatch(storeGlobal, 1u, 1u, 1u, {}, reinterpret_cast<std::uintptr_t>(StoreGlobalCode.data()));
    if (submit) {
        std::uint64_t serial = 0;
        {
            std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
            serial = device.SubmitAndEpoch();
        }
        device.WaitRecorded(serial);
    }
    const std::string what = std::string("storage flush ordering, ") + name + ": ";
    Require(AddressBasedStorePending(image, !submit), what + "the address-based store was not pending in the expected batch at the flush");
    Require(AgcDriver::Graphics::StorageTexture::FlushPending(image, ImageBytes, nullptr, "test"), what + "the image store left no results pending");
    device.WaitIdle();
    Require(words[FarDword] == Stored, what + "the pending image's texel away from the newer write was not stored");
    Require(words[OverwrittenDword] == Overwritten, what + "the pending image's store put its older texels over a newer address-based store");
}

class WriterGate {
public:
    explicit WriterGate(AgcDriver::Graphics::Recorder& recorder) : context(recorder.DeviceContext()), recorder(recorder) {
        const VkEventCreateInfo info{VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
        AgcDriver::Graphics::Check(context.Function<PFN_vkCreateEvent>("vkCreateEvent")(context.device, &info, nullptr, &event), "create writer gate");
    }
    ~WriterGate() {
        Release();
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        recorder.Sync();
        context.Function<PFN_vkDestroyEvent>("vkDestroyEvent")(context.device, event, nullptr);
    }
    void Record(AgcDriver::Graphics::Recorder& recorder) {
        context.Function<PFN_vkCmdWaitEvents>("vkCmdWaitEvents")(recorder.Commands(), 1, &event, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, nullptr, 0, nullptr, 0, nullptr);
    }
    void Release() {
        AgcDriver::Graphics::Check(context.Function<PFN_vkSetEvent>("vkSetEvent")(context.device, event), "release writer gate");
    }
private:
    AgcDriver::Graphics::Context context;
    AgcDriver::Graphics::Recorder& recorder;
    VkEvent event = VK_NULL_HANDLE;
};

struct InStreamResult {
    std::uint32_t seen = 0;
    std::vector<std::byte> image;
    std::vector<std::byte> keys;
};

bool DccMode(std::string_view mode) {
    return mode == "dcc" || mode == "dcc_clear" || mode == "dcc_mixed" || mode == "dcc_queued";
}

int FlushInStream(AgcDriver::VulkanDevice& device, const GuestBlock& guest, std::string_view mode, bool reference, InStreamResult& result) {
    using namespace AgcDriver::Graphics;
    AgcDriver::GuestMemory::TagGpuLockThread(0);
    const auto image = guest.Address();
    if (mode == "unwatched" && AgcDriver::GuestMemory::Watched(image, ImageBytes)) return VulkanTestSkipped;
    const bool dcc = DccMode(mode);
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        if (HostImportFor(device.graphicsContext(), image, ImageBytes) == nullptr) return VulkanTestSkipped;
        if (dcc && HostImportFor(device.graphicsContext(), image + KeysOffset, KeysBytes) == nullptr) return VulkanTestSkipped;
    }
    const bool tiled = mode == "store_tiled" || dcc;
    auto imageData = ImageUserData(image);
    if (tiled) imageData[7] |= 0x1bu << 20u;
    if (dcc) {
        const auto keys = image + KeysOffset;
        auto* keyBytes = reinterpret_cast<std::uint8_t*>(guest.Words() + KeysOffset / 4u);
        std::memset(keyBytes, mode == "dcc_clear" ? 0x00 : 0xff, KeysBytes);
        if (mode == "dcc_mixed") {
            for (std::size_t i = 1; i < KeysBytes; i += 2u) keyBytes[i] = 0x00;
        }
        imageData[10] = (1u << 20u) | (1u << 21u) | (static_cast<std::uint32_t>((keys >> 8u) & 0xffu) << 24u);
        imageData[11] = static_cast<std::uint32_t>(keys >> 16u);
    }
    const auto shader = Compile(StoreImageCode, imageData, device.Target());
    device.Dispatch(shader, 1u, 1u, 1u, {}, reinterpret_cast<std::uintptr_t>(StoreImageCode.data()));
    std::unique_lock gpu(AgcDriver::GuestMemory::GpuMutex());
    auto* recorder = Recorder::Active();
    Require(recorder != nullptr, "in-stream flush: no recorder");
    const auto context = recorder->DeviceContext();
    Require(HostImportCovers(context, image, ImageBytes), "in-stream flush: the image lost its import");
    const auto texture = StorageTexture::FindPending(image, ImageBytes);
    Require(texture != nullptr, "in-stream flush: no pending image");
    Require(!dcc || texture->Descriptor().dccAddress == image + KeysOffset, "in-stream flush: the pending image has no DCC keys");
    const auto keyCount = dcc ? DccKeyCount(texture->Descriptor(), texture->GuestBytes()) : 0u;
    Require(keyCount <= KeysBytes, "in-stream flush: the image's DCC keys exceed the filled key range");
    const auto address = tiled ? image : image + FarDword * 4u;
    const auto* import = HostImportFor(context, address, 4u);
    Require(import != nullptr, "in-stream flush: no import");
    if (mode == "cpu") guest.Words()[FarDword] = Overwritten;
    if (mode == "cross") AgcDriver::GuestMemory::TagGpuLockThread(0x20);
    const std::array<std::uint32_t, 1> value{Overwritten};
    recorder->RecordStore(import->buffer, HostImportOffset(*import, address), std::as_bytes(std::span(value)), address);
    recorder->FlushStoresOverlapping(address, 4u);
    const auto kind = mode == "shader" ? Recorder::WriteKind::ShaderBuffer : mode == "copy" ? Recorder::WriteKind::Copy : Recorder::WriteKind::Store;
    recorder->NotePendingWrite(address, 4u, kind);
    AgcDriver::GuestMemory::TagGpuLockThread(0);
    if (mode == "bda") recorder->NotePendingRead(image, ImageBytes, Recorder::ReadKind::AddressBased);
    if (mode == "dcc_queued") {
        std::memset(guest.Words() + KeysOffset / 4u, 0x00, KeysBytes);
        if (reference) MarkDccUncompressed(context, image + KeysOffset, ImageBytes, keyCount);
        else RecordDccUncompressed(context, image + KeysOffset, ImageBytes, keyCount);
    }
    const auto before = recorder->Submissions();
    const auto site = mode == "flip" || reference ? StorageTexture::FlushSite::Other : StorageTexture::FlushSite::RevalidateRead;
    const bool dccInStream = std::getenv("APS5_NO_INSTREAM_DCC") == nullptr;
    const bool eligible = mode == "store" || mode == "store_tiled" || mode == "shader" || (dcc && dccInStream);
    const bool skipped = !reference && eligible && std::getenv("APS5_NO_INSTREAM_FLUSH") == nullptr;
    Require(StorageTexture::FlushPending(image, ImageBytes, nullptr, mode == "flip" ? "flip" : "imported buffer region", PublishScope::Whole, nullptr, site), "in-stream flush: no results flushed");
    Require((recorder->Submissions() == before) == skipped, "in-stream flush: unexpected CPU synchronization");
    if (!reference && std::getenv("APS5_PROFILE_DRAW") != nullptr) {
        const std::string why = mode == "cpu" ? "cpuWritten" : mode == "unwatched" ? "unwatched" : dcc && !dccInStream ? "dcc" : "none";
        const auto report = Recorder::FlushSyncReport();
        Require(report.find("/why=" + why + "=") != std::string::npos, "in-stream flush: the flush Sync candidates do not name why=" + why + ":" + report);
        if (skipped) Require(report.find("/stream1/why=none=") != std::string::npos, "in-stream flush: the flush Sync candidates do not count an in-stream flush:" + report);
        if (dcc && !dccInStream) Require(report.find("/stream0/why=dcc=") != std::string::npos, "in-stream flush: the DCC switch did not retain CPU synchronization:" + report);
    }
    auto readback = std::make_shared<Buffer>(context, 4u + (dcc ? KeysBytes : 0u), VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    recorder->Keep(readback);
    const auto commands = recorder->Commands();
    CopyBuffer(context, commands, import->buffer, HostImportOffset(*import, address), readback->Handle(), 0, 4u);
    if (dcc) {
        const auto* keyImport = HostImportFor(context, image + KeysOffset, KeysBytes);
        Require(keyImport != nullptr, "in-stream flush: the keys lost their import");
        CopyBuffer(context, commands, keyImport->buffer, HostImportOffset(*keyImport, image + KeysOffset), readback->Handle(), 4u, KeysBytes);
    }
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
    recorder->Sync();
    readback->Invalidate();
    std::memcpy(&result.seen, readback->Bytes().data(), sizeof(result.seen));
    Require(result.seen == (mode == "cpu" || mode == "unwatched" ? Overwritten : Stored), "in-stream flush: GPU consumer saw stale bytes");
    const auto* bytes = reinterpret_cast<const std::byte*>(guest.Words());
    result.image.assign(bytes, bytes + ImageBytes);
    if (dcc) {
        result.keys.assign(bytes + KeysOffset, bytes + KeysOffset + KeysBytes);
        const auto gpuKeys = readback->Bytes().subspan(4u, KeysBytes);
        Require(std::equal(result.keys.begin(), result.keys.end(), gpuKeys.begin()), "in-stream flush: GPU consumer saw stale DCC keys");
    }
    return 0;
}

int RunInStream(AgcDriver::VulkanDevice& device, std::string_view mode) {
    if (!AgcDriver::GuestMemory::WriteWatched()) return VulkanTestSkipped;
    GuestBlock guest(mode != "unwatched");
    InStreamResult result;
    if (const auto status = FlushInStream(device, guest, mode, false, result); status != 0 || !DccMode(mode)) return status;
    GuestBlock referenceGuest(true);
    InStreamResult expected;
    if (const auto status = FlushInStream(device, referenceGuest, mode, true, expected); status != 0) return status;
    Require(result.seen == expected.seen, "in-stream flush: the GPU consumer saw other bytes than after a CPU synchronization");
    Require(result.image == expected.image, "in-stream flush: the stored image bytes differ from a CPU-synchronized store");
    Require(result.keys == expected.keys, "in-stream flush: the stored DCC keys differ from a CPU-synchronized store");
    return 0;
}

int RunSmallStoreRetry(AgcDriver::VulkanDevice& device, bool submitted) {
    using namespace AgcDriver::Graphics;
    if (!AgcDriver::GuestMemory::WriteWatched()) return VulkanTestSkipped;
    GuestBlock guest(true);
    constexpr std::size_t offset = 4096;
    constexpr std::uint32_t height = 64;
    constexpr std::size_t bytes = Width * height * 4u;
    constexpr std::size_t farWord = (height - 1u) * Width;
    const auto image = guest.Address() + offset;
    alignas(256) auto code = StoreImageCode;
    code[7] = height - 1u;
    auto userData = ImageUserData(image);
    userData[6] = ((Width - 1u) >> 2u) | ((height - 1u) << 14u);
    const auto shader = Compile(code, userData, device.Target());
    device.Dispatch(shader, 1u, 1u, 1u, {}, reinterpret_cast<std::uintptr_t>(code.data()));
    std::unique_lock gpu(AgcDriver::GuestMemory::GpuMutex());
    auto* recorder = Recorder::Active();
    Require(recorder != nullptr, "small store retry: no recorder");
    const auto context = recorder->DeviceContext();
    if (!HostImportCovers(context, image, bytes)) return VulkanTestSkipped;
    const auto texture = StorageTexture::FindPending(image, bytes);
    Require(texture != nullptr && texture->GuestBytes() <= StorageTexture::SmallStoreBytes, "small store retry: no small pending image");
    WriterGate gate(*recorder);
    gate.Record(*recorder);
    const auto address = image + OverwrittenDword * 4u;
    const auto* import = HostImportFor(context, address, 4u);
    Require(import != nullptr, "small store retry: no writer import");
    const std::array<std::uint32_t, 1> value{Overwritten};
    recorder->RecordStore(import->buffer, HostImportOffset(*import, address), std::as_bytes(std::span(value)), address);
    recorder->NotePendingWrite(address, 4u);
    recorder->OnComplete([address] { AgcDriver::GuestMemory::MarkWritten(address, 4u); });
    const auto writer = recorder->OpenSerial();
    if (submitted) recorder->Submit();
    const bool defer = StorageTexture::DeferSmallStores();
    if (!defer) gate.Release();
    const auto stored = StorageTexture::StoreSmallPending("flip");
    auto eop = std::make_shared<std::array<bool, 2>>();
    Require(recorder->AfterRecordedWork([eop, image] {
        (*eop)[0] = true;
        (*eop)[1] = reinterpret_cast<const volatile std::uint32_t*>(image)[farWord] == Stored;
    }), "small store retry: EOP was not deferred");
    if (defer) {
        Require(!(*eop)[0], "small store retry: EOP overtook the deferred store");
        Require(stored == 0, "small store retry: flip stored an unfinished writer");
        Require(StorageTexture::FindPending(image, bytes) == texture, "small store retry: deferral lost the image");
        Require(recorder->PendingWriterSerial(image, bytes) == writer, "small store retry: open writer was not submitted or pending writer was lost");
        Require(recorder->OpenSerial() > writer, "small store retry: flip did not submit the open writer");
        Require(StorageTexture::SmallStoresDeferred(), "small store retry: retry was not requested");
        gate.Release();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (StorageTexture::FindPending(image, bytes) != nullptr && std::chrono::steady_clock::now() < deadline) {
            static_cast<void>(StorageTexture::StoreSmallPending("memory wait"));
            gpu.unlock();
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            gpu.lock();
        }
        Require(StorageTexture::FindPending(image, bytes) == nullptr, "small store retry: retry did not store the image");
    } else {
        Require(stored != 0, "small store retry: legacy flip did not store the image");
    }
    gpu.unlock();
    const volatile std::uint32_t* words = guest.Words() + offset / 4u;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (words[farWord] != Stored && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::microseconds(200));
    const bool seen = words[farWord] == Stored;
    device.WaitIdle();
    Require(seen || !defer, "small store retry: retry batch was not submitted for plain CPU loads");
    Require((*eop)[0] && (*eop)[1], "small store retry: EOP did not observe the published small result");
    Require(words[farWord] == Stored, "small store retry: later image texel was not stored");
    Require(words[OverwrittenDword] == Overwritten, "small store retry: older image overwrote a newer dword");
    return 0;
}

}

int main(int argc, char** argv) {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        if (argc == 3 && std::strcmp(argv[1], "--instream") == 0) return RunInStream(*device, argv[2]);
        if (argc == 3 && std::strcmp(argv[1], "--small-store-retry") == 0) return RunSmallStoreRetry(*device, std::strcmp(argv[2], "submitted") == 0);
        const bool watched = AgcDriver::GuestMemory::WriteWatched();
        GuestBlock guest(watched);
        if (!watched) std::puts("guest memory has no write watch: the writer-in-flight case is skipped");
        else if (!device->CanWaitUnlocked()) std::puts("the recorder cannot wait for the GPU without running completions: the writer-in-flight case is skipped");
        else Run(*device, guest, 0, true, "writer in flight");
        Run(*device, guest, BlockBytes / 2u, false, "writer in the open batch");
        std::puts("storage flush ordering tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
