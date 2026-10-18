#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Queues/WorkerAffinity.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include <algorithm>
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include <system_error>
#include "prx/libkernel/DirectMemory/DirectMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/TracyZone.hpp"
#include <cstdlib>
#include <cstdio>
#include <array>
#include <map>
#include <mutex>
#include <chrono>
#include <iterator>
#include "prx/libSceAgcDriver/Graphics/include/BufferPool.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#include <malloc.h>
#endif

namespace {

std::uint64_t ProcessPrivateBytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) return counters.PrivateUsage;
#endif
    return 0;
}

struct ProcessRegions {
    struct HeapBytes {
        std::uint64_t allocated = 0;
        std::uint64_t committed = 0;
    };
    struct Reservation {
        std::uint64_t committed = 0;
        std::uint64_t reserved = 0;
        unsigned long protect = 0;
    };
    std::uint64_t privateBytes = 0;
    std::uint64_t guestBytes = 0;
    std::uint64_t mappedBytes = 0;
    std::uint64_t imageBytes = 0;
    std::size_t heaps = 0;
    std::size_t otherHeaps = 0;
    std::size_t summaryFailed = 0;
    bool crtSeparate = false;
    HeapBytes processHeap;
    HeapBytes crtHeap;
    HeapBytes otherHeap;
    HeapBytes largestHeap;
    std::uint64_t heapBytes = 0;
    double walkMs = 0;
    double walkAge = 0;
    std::size_t walkEntries = 0;
    std::size_t walkFailed = 0;
    std::size_t nonheapGroups = 0;
    std::uint64_t nonheapBytes = 0;
    std::array<std::size_t, 6> bucketCounts{};
    std::array<std::uint64_t, 6> bucketBytes{};
    std::array<Reservation, 5> largestRegions{};
};

#ifdef _WIN32
using HostRange = std::pair<std::uintptr_t, std::uintptr_t>;

void MergeHostRanges(std::vector<HostRange>& ranges) {
    std::sort(ranges.begin(), ranges.end());
    std::size_t size = 0;
    for (const auto range : ranges) {
        if (size == 0 || range.first > ranges[size - 1].second) ranges[size++] = range;
        else ranges[size - 1].second = std::max(ranges[size - 1].second, range.second);
    }
    ranges.resize(size);
}

struct HeapRanges {
    std::vector<HostRange> ranges;
    std::chrono::steady_clock::time_point sampled{};
    double ms = 0;
    std::size_t entries = 0;
    std::size_t failed = 0;
};

void RefreshHeapRanges(HeapRanges& cache, const std::vector<HANDLE>& heaps) {
    const auto start = std::chrono::steady_clock::now();
    if (cache.sampled != std::chrono::steady_clock::time_point{} && start - cache.sampled < std::chrono::seconds(60)) return;
    cache.ranges.clear();
    cache.entries = 0;
    cache.failed = 0;
    struct Entry {
        HostRange range;
        bool region = false;
    };
    std::vector<Entry> scratch(4096);
    std::vector<HostRange> busy;
    for (const auto heap : heaps) {
        for (;;) {
            if (!HeapLock(heap)) {
                ++cache.failed;
                break;
            }
            PROCESS_HEAP_ENTRY entry{};
            std::size_t used = 0;
            std::size_t visited = 0;
            bool full = false;
            HostRange currentRegion{};
            while (HeapWalk(heap, &entry)) {
                ++visited;
                if (!(entry.wFlags & (PROCESS_HEAP_REGION | PROCESS_HEAP_ENTRY_BUSY))) continue;
                const auto begin = reinterpret_cast<std::uintptr_t>(entry.lpData);
                const bool region = (entry.wFlags & PROCESS_HEAP_REGION) != 0;
                const auto bytes = region ? static_cast<std::uint64_t>(entry.Region.dwCommittedSize) + entry.Region.dwUnCommittedSize : entry.cbData;
                if (region) currentRegion = {begin, begin + bytes};
                else if (begin >= currentRegion.first && begin + bytes <= currentRegion.second) continue;
                if (used == scratch.size()) {
                    full = true;
                    break;
                }
                scratch[used++] = {{begin, begin + bytes}, region};
            }
            const auto error = GetLastError();
            HeapUnlock(heap);
            if (full) {
                scratch.resize(scratch.size() * 2);
                continue;
            }
            cache.entries += visited;
            if (error != ERROR_NO_MORE_ITEMS) ++cache.failed;
            for (std::size_t i = 0; i < used; ++i) {
                if (scratch[i].region) cache.ranges.push_back(scratch[i].range);
                else busy.push_back(scratch[i].range);
            }
            break;
        }
    }
    MergeHostRanges(cache.ranges);
    std::vector<HostRange> large;
    for (const auto range : busy) {
        const auto next = std::upper_bound(cache.ranges.begin(), cache.ranges.end(), HostRange{range.first, UINTPTR_MAX});
        if (next != cache.ranges.begin() && std::prev(next)->second >= range.second) continue;
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<const void*>(range.first), &info, sizeof(info)) == 0 || info.Type != MEM_PRIVATE) continue;
        const auto base = reinterpret_cast<std::uintptr_t>(info.AllocationBase);
        auto end = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
        while (VirtualQuery(reinterpret_cast<const void*>(end), &info, sizeof(info)) != 0 && reinterpret_cast<std::uintptr_t>(info.AllocationBase) == base) {
            const auto nextEnd = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
            if (nextEnd <= end) break;
            end = nextEnd;
        }
        large.emplace_back(base, end);
    }
    cache.ranges.insert(cache.ranges.end(), large.begin(), large.end());
    MergeHostRanges(cache.ranges);
    cache.sampled = start;
    cache.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}
#endif

ProcessRegions ProcessCommittedRegions() {
    ProcessRegions result;
#ifdef _WIN32
    static HeapRanges heapRanges;
    std::vector<HANDLE> heaps;
    for (;;) {
        const auto size = GetProcessHeaps(0, nullptr);
        if (size == 0) throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "host heap enumeration");
        heaps.resize(size);
        const auto actual = GetProcessHeaps(size, heaps.data());
        if (actual == 0) throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "host heap enumeration");
        if (actual <= size) {
            heaps.resize(actual);
            break;
        }
    }
    result.heaps = heaps.size();
    const auto processHeap = GetProcessHeap();
    const auto crtHeap = reinterpret_cast<HANDLE>(_get_heap_handle());
    result.crtSeparate = crtHeap != processHeap;
    static const auto summary = reinterpret_cast<decltype(&HeapSummary)>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "HeapSummary"));
    for (const auto heap : heaps) {
        const bool other = heap != processHeap && heap != crtHeap;
        if (other) ++result.otherHeaps;
        HEAP_SUMMARY bytes{};
        bytes.cb = sizeof(bytes);
        if (!summary || !summary(heap, 0, &bytes)) {
            ++result.summaryFailed;
            continue;
        }
        const ProcessRegions::HeapBytes value{bytes.cbAllocated, bytes.cbCommitted};
        if (heap == processHeap) result.processHeap = value;
        if (heap == crtHeap && result.crtSeparate) result.crtHeap = value;
        if (other) {
            result.otherHeap.allocated += value.allocated;
            result.otherHeap.committed += value.committed;
            if (value.committed > result.largestHeap.committed) result.largestHeap = value;
        }
    }
    RefreshHeapRanges(heapRanges, heaps);
    result.walkMs = heapRanges.ms;
    result.walkAge = std::chrono::duration<double>(std::chrono::steady_clock::now() - heapRanges.sampled).count();
    result.walkEntries = heapRanges.entries;
    result.walkFailed = heapRanges.failed;
    const auto count = KernelGuestMemoryRanges_nid_no_patch(nullptr, 0);
    std::vector<std::uintptr_t> ranges(count * 2);
    KernelGuestMemoryRanges_nid_no_patch(ranges.data(), count);
    std::vector<HostRange> excluded = heapRanges.ranges;
    for (std::size_t i = 0; i < count; ++i) excluded.emplace_back(ranges[i * 2], ranges[i * 2 + 1]);
    MergeHostRanges(excluded);
    std::map<std::uintptr_t, ProcessRegions::Reservation> reservations;
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const auto maximum = reinterpret_cast<std::uintptr_t>(system.lpMaximumApplicationAddress);
    for (std::uintptr_t cursor = 0; cursor <= maximum;) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &info, sizeof(info)) == 0) throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "host memory region walk");
        const auto begin = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        const auto end = begin + info.RegionSize;
        if (end <= cursor) break;
        if (info.State != MEM_FREE) reservations[reinterpret_cast<std::uintptr_t>(info.AllocationBase)].reserved += info.RegionSize;
        if (info.State == MEM_COMMIT) {
            auto& reservation = reservations[reinterpret_cast<std::uintptr_t>(info.AllocationBase)];
            if (reservation.protect == 0) reservation.protect = info.Protect;
            if (info.Type == MEM_PRIVATE) {
                result.privateBytes += info.RegionSize;
                for (std::size_t i = 0; i < count; ++i) {
                    const auto first = std::max(begin, ranges[i * 2]);
                    const auto last = std::min(end, ranges[i * 2 + 1]);
                    if (first < last) result.guestBytes += last - first;
                }
                std::uint64_t excludedBytes = 0;
                auto range = std::lower_bound(excluded.begin(), excluded.end(), begin, [](const HostRange& value, std::uintptr_t address) { return value.second <= address; });
                for (; range != excluded.end() && range->first < end; ++range) excludedBytes += std::min(end, range->second) - std::max(begin, range->first);
                const auto nonheap = info.RegionSize - excludedBytes;
                result.nonheapBytes += nonheap;
                if (nonheap != 0) {
                    reservation.committed += nonheap;
                }
            } else if (info.Type == MEM_MAPPED) result.mappedBytes += info.RegionSize;
            else if (info.Type == MEM_IMAGE) result.imageBytes += info.RegionSize;
        }
        cursor = end;
    }
    result.heapBytes = result.privateBytes - result.guestBytes - result.nonheapBytes;
    constexpr std::array<std::uint64_t, 5> limits{65536, 1048576, 16777216, 67108864, 268435456};
    for (const auto& [base, reservation] : reservations) {
        if (reservation.committed == 0) continue;
        ++result.nonheapGroups;
        const auto bucket = std::upper_bound(limits.begin(), limits.end(), reservation.committed) - limits.begin();
        ++result.bucketCounts[bucket];
        result.bucketBytes[bucket] += reservation.committed;
        auto value = reservation;
        for (auto& largest : result.largestRegions) if (value.committed > largest.committed) std::swap(value, largest);
    }
#endif
    return result;
}

std::pair<std::uint64_t, std::uint64_t> ImportedHostBytes() {
    auto ranges = AgcDriver::HostMemoryImportRanges_nid_no_patch();
    std::sort(ranges.begin(), ranges.end());
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> merged;
    for (const auto& range : ranges) {
        if (merged.empty() || range.first > merged.back().second) merged.push_back(range);
        else merged.back().second = std::max(merged.back().second, range.second);
    }
    const auto count = KernelGuestMemoryRanges_nid_no_patch(nullptr, 0);
    std::vector<std::uintptr_t> guest(count * 2);
    KernelGuestMemoryRanges_nid_no_patch(guest.data(), count);
    std::uint64_t uniqueBytes = 0;
    std::uint64_t guestBytes = 0;
    for (const auto& [begin, end] : merged) {
        uniqueBytes += end - begin;
        for (std::size_t i = 0; i < count; ++i) {
            const auto first = std::max(begin, guest[i * 2]);
            const auto last = std::min(end, guest[i * 2 + 1]);
            if (first < last) guestBytes += last - first;
        }
    }
    return {uniqueBytes, guestBytes};
}

void ReportHostMemory(std::FILE* output, std::span<const std::shared_ptr<AgcDriver::Graphics::BufferPool>> pools = {}) {
    using namespace AgcDriver;
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    const auto privateBytes = ProcessPrivateBytes();
    const auto regions = ProcessCommittedRegions();
    const auto host = Graphics::HostBufferCounters();
    std::uint64_t poolBytes = 0;
    for (const auto& pool : pools) if (pool) poolBytes += pool->Retained();
    const auto mirrors = Graphics::MirrorCounters();
    const auto storage = Graphics::StorageHostCounters();
    const auto sampled = Graphics::TextureCacheUsage(true);
    auto& cache = Graphics::SharedResourceCache();
    const auto resources = cache.HostBytes();
    const auto entries = cache.Size();
    const auto capacity = cache.Capacity();
    const auto recorder = Graphics::RecorderHostCounters();
    const auto shader = ShaderRecompiler::ResidentStats();
    const auto trace = PerformanceTrace::BufferBytes();
    const auto imported = ImportedHostBytes();
    const auto cacheSaved = Graphics::pipelineCacheBlobBytes.load(std::memory_order_relaxed);
    const double attributed = static_cast<double>(regions.guestBytes) + mirrors.shadowBytes + storage.originalBytes + sampled.sampledHostBytes + resources.first + resources.second + shader.sourceBytes + shader.artifactBytes - shader.spirvMappedBytes + shader.memoBytes + trace;
    constexpr double mib = 1048576.0;
    std::fprintf(output, "[hostmem] private %.0f MiB: hostbuf %.0f (%llu; mirrors %.0f+%.0f, saved %.0f, drawsnap %.0f+%.0f, unwritten %.0f, kept %.0f) pool %.0f; cpu: shadow %.0f, original %.0f (%llu images), sampled %.0f (%zu), rescache %.0f+%.0f (%zu/%zu), shader %zu sources %.0f %zu plans %.0f variants %zu %.0f memo %.0f, trace %.0f; other %.0f; fallback %.0f cumulative; imports pinned %.0f; vq private %.3f (guest %.0f, host %.0f) mapped %.0f image %.0f; vk dev %.0f (%llu) host %.0f (%llu); pipelines %llu cache %.0f; spirv %.0f (%zu; heap %.0f/%.0f mapped %.0f) vectors %.0f; cache loaded %.0f saved %.0f; vk import host %.0f (%llu) dev %.0f (%llu) unique %.0f guest %.0f; vk alloc host %.0f (%llu) dev %.0f (%llu)",
        privateBytes / mib, host.bytes / mib, static_cast<unsigned long long>(host.count), mirrors.heapBytes / mib, mirrors.nonHeapBytes / mib, storage.savedBytes / mib,
        recorder.drawSnapshotBytes[0] / mib, recorder.drawSnapshotBytes[1] / mib, recorder.unwrittenBytes / mib, recorder.keptBytes / mib, poolBytes / mib,
        mirrors.shadowBytes / mib, storage.originalBytes / mib, static_cast<unsigned long long>(storage.images), sampled.sampledHostBytes / mib, sampled.sampledEntries,
        resources.first / mib, resources.second / mib, entries, capacity, shader.sources, shader.sourceBytes / mib, shader.plans, shader.planBytes / mib, shader.variants, shader.artifactBytes / mib, shader.memoBytes / mib,
        trace / mib, (static_cast<double>(privateBytes) - attributed) / mib, Graphics::hostPlacedBytes.load(std::memory_order_relaxed) / mib, 0.0,
        regions.privateBytes / mib, regions.guestBytes / mib, (regions.privateBytes - regions.guestBytes) / mib, regions.mappedBytes / mib, regions.imageBytes / mib,
        Graphics::vulkanHeapBytes[0].load(std::memory_order_relaxed) / mib, static_cast<unsigned long long>(Graphics::vulkanHeapAllocations[0].load(std::memory_order_relaxed)),
        Graphics::vulkanHeapBytes[1].load(std::memory_order_relaxed) / mib, static_cast<unsigned long long>(Graphics::vulkanHeapAllocations[1].load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(Graphics::vulkanPipelines.load(std::memory_order_relaxed)), cacheSaved / mib,
        shader.spirvBytes / mib, shader.spirvPayloads, (shader.spirvBytes - shader.spirvMappedBytes) / mib, shader.spirvHeapBytes / mib, shader.spirvMappedBytes / mib, shader.artifactVectorBytes / mib,
        Graphics::pipelineCacheLoadedBytes.load(std::memory_order_relaxed) / mib, cacheSaved / mib,
        Graphics::vulkanImportedHeapBytes[1].load(std::memory_order_relaxed) / mib, static_cast<unsigned long long>(Graphics::vulkanImportedHeapAllocations[1].load(std::memory_order_relaxed)),
        Graphics::vulkanImportedHeapBytes[0].load(std::memory_order_relaxed) / mib, static_cast<unsigned long long>(Graphics::vulkanImportedHeapAllocations[0].load(std::memory_order_relaxed)),
        imported.first / mib, imported.second / mib,
        Graphics::vulkanAllocatedHeapBytes[1].load(std::memory_order_relaxed) / mib, static_cast<unsigned long long>(Graphics::vulkanAllocatedHeapAllocations[1].load(std::memory_order_relaxed)),
        Graphics::vulkanAllocatedHeapBytes[0].load(std::memory_order_relaxed) / mib, static_cast<unsigned long long>(Graphics::vulkanAllocatedHeapAllocations[0].load(std::memory_order_relaxed)));
    double heapOther = 0;
#ifdef _WIN32
    const auto cpuHeapBytes = attributed - static_cast<double>(regions.guestBytes);
    heapOther = static_cast<double>(regions.processHeap.allocated) + regions.crtHeap.allocated - cpuHeapBytes;
#endif
    std::fprintf(output, "; heaps %zu (failed %zu) process %.3f/%.3f crt %s %.3f/%.3f other-heaps %zu %.3f/%.3f largest %.3f/%.3f; heapwalk %.3f ms (%zu entries, %.3f s old, %zu failed); heap host %.3f heap-other %.3f nonheap host %.3f; nonheap groups %zu %.3f",
        regions.heaps, regions.summaryFailed, regions.processHeap.allocated / mib, regions.processHeap.committed / mib,
        regions.crtSeparate ? "separate" : "process", regions.crtHeap.allocated / mib, regions.crtHeap.committed / mib,
        regions.otherHeaps, regions.otherHeap.allocated / mib, regions.otherHeap.committed / mib, regions.largestHeap.allocated / mib, regions.largestHeap.committed / mib,
        regions.walkMs, regions.walkEntries, regions.walkAge, regions.walkFailed, regions.heapBytes / mib,
        heapOther / mib, regions.nonheapBytes / mib, regions.nonheapGroups, regions.nonheapBytes / mib);
    constexpr std::array<const char*, 6> labels{"<64K", "64K-1M", "1-16M", "16-64M", "64-256M", ">=256M"};
    for (std::size_t i = 0; i < labels.size(); ++i) std::fprintf(output, "; nh %s %zu %.3f", labels[i], regions.bucketCounts[i], regions.bucketBytes[i] / mib);
    std::fprintf(output, "; nh top");
    for (const auto& region : regions.largestRegions) std::fprintf(output, " %.3f/%.3f/0x%lx", region.committed / mib, region.reserved / mib, region.protect);
    std::fprintf(output, "\n");
}

}

extern "C" void AgcDriverReportHostMemory_nid_no_patch(std::FILE* output) {
    if (ShaderRecompiler::HostMemoryLedgerEnabled()) ReportHostMemory(output);
}

namespace AgcDriver::DriverDetail {

void Driver::reportHostMemory() {
    if (!ShaderRecompiler::HostMemoryLedgerEnabled()) return;
    static std::atomic<std::int64_t> nextReport{0};
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    auto next = nextReport.load(std::memory_order_relaxed);
    if (now < next || !nextReport.compare_exchange_strong(next, now + 10000, std::memory_order_relaxed)) return;
    std::vector<std::shared_ptr<Graphics::BufferPool>> pools;
    {
        GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Present);
        std::lock_guard lock(GuestMemory::GpuMutex());
        if (const auto current = device.Load()) pools.push_back(current->HostBufferPool());
        for (const auto& previous : replacedDevices) pools.push_back(previous->HostBufferPool());
    }
    ReportHostMemory(stderr, pools);
}

std::uint32_t Driver::ReserveEopFlip(const FlipInfo& info) {
    std::lock_guard lock(mutex);
    rethrowFailure();
    checkStopping();
    require(nextEopToken <= EopFlipTokenLimit, "EOP flip token space exhausted");
    const auto output = outputs.find(info.handle);
    require(output != outputs.end(), "EOP flip references an unregistered video output");
    auto request = output->second->TryReserve(info);
    if (!request) return 0;
    const auto token = nextEopToken;
    eopFlips.emplace(token, EopFlip{info.handle, std::move(request)});
    ++nextEopToken;
    return token;
}

void Driver::RegisterVideoOutput(std::uint32_t handle, const std::shared_ptr<IVideoOutput>& output) {
    require(output != nullptr, "null video output");
    std::lock_guard lock(mutex);
    rethrowFailure();
    checkStopping();
    require(outputs.emplace(handle, output).second, "video output already registered");
}

void Driver::UnregisterVideoOutput(std::uint32_t handle, const std::shared_ptr<IVideoOutput>& output) {
    std::lock_guard lock(mutex);
    const auto it = outputs.find(handle);
    require(it != outputs.end() && it->second == output, "video output registration mismatch");
    std::erase_if(eopFlips, [handle](const auto& entry) { return entry.second.handle == handle; });
    outputs.erase(it);
}

void Driver::AttachWindow(const PresentationWindow& window) {
    CheckFailure();
    std::unique_lock replacing(deviceReplacement);
    GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Present);
    std::lock_guard lock(GuestMemory::GpuMutex());
    if (device) {
        require(device->Window() == nullptr, "a window is already attached to the device");
        device->PrepareForReplacement();
        replacedDevices.push_back(device);
    }
    device = std::make_shared<VulkanDevice>(&window);
}

void Driver::Present(const PresentationWindow& window, const DisplayBuffer* buffer, bool opaque, void (*gpuReady)(void*), void* context) {
    reportHostMemory();
    if (PerformanceTrace::Configured()) PerformanceTrace::Present(window.width, window.height, device.Load().get());
    PerformanceContext timingContext(window.timing.get());
    PerformanceTimer timing("Driver.Present");
    static thread_local bool pinned = false;
    if (!pinned) {
        pinned = true;
        PinWorkerThread("presenter");
        GuestMemory::MarkPresenterThread();
    }
    CheckFailure();
    require(gpuReady != nullptr && context != nullptr, "missing GPU completion callback");
    require(window.getDrawableSize != nullptr, "missing window drawable size query");

    static const bool syncFlip = std::getenv("APS5_SYNC_FLIP") != nullptr;
    static const std::size_t inFlight = VulkanDevice::FlipInFlight();
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    std::shared_ptr<VulkanDevice> presenting;
    timing.Mark("validate");
    try {
        bool submitted = false;
        bool presentable = false;
        bool trailing = false;
        double waitedMs = 0;
        std::uint32_t drawableWidth = 0;
        std::uint32_t drawableHeight = 0;
        GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Present);
        WithDevicePreparation_nid_no_patch([&] {
            PerformanceTimer query("Driver.DrawableSize");
            window.getDrawableSize(window.context, &drawableWidth, &drawableHeight);
            if (NarrowDeviceLock_nid_no_patch()) timing.Mark("drawable_query");
        }, [&] {
            timing.Mark("gpu_mutex_wait");
            require(device != nullptr && device->Window() == window.context, "presentation window is not attached to the device");
            presenting = device;
            timing.Mark("device_setup");
        }, [&] {
            presenting->Resize(drawableWidth, drawableHeight);
            presenting->SelectDynamicRange(buffer);
            timing.Mark("resize");
            presentable = presenting->Presentable();
            if (buffer != nullptr) require(buffer->width == window.width && buffer->height == window.height, "display buffer extent differs from output");
        });

        if (presentable && !syncFlip) {
            if (inFlight != 0) {

                waitedMs = presenting->RetirePresents(presenting->PresentWaitsForSlots(buffer) ? 0 : inFlight);
                timing.Mark("inflight_wait");
            }
            presentable = presenting->AcquireImage();
            timing.Mark("acquire_image");
        }
        if (presentable) {
            GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Present);
            std::lock_guard lock(GuestMemory::GpuMutex());
            timing.Mark("gpu_mutex_wait");
            if (buffer != nullptr) {
                if (syncFlip) {
                    presenting->WaitIdle();
                    timing.Mark("device_idle_wait");
                }
                submitted = presenting->PresentDisplayBuffer(*buffer);
                timing.Mark("present_display_buffer");
            } else {
                submitted = presenting->PresentClear(window.width, window.height, opaque);
                timing.Mark("present_clear");
            }
            if (submitted && syncFlip) {
                waitedMs = presenting->FinishPresent();
                timing.Mark("render_fence_wait");
            }
            if (submitted && (syncFlip || inFlight != 0)) {

                presenting->QueuePresent();
                timing.Mark("queue_present");
                trailing = !syncFlip;
                submitted = false;
            }
        }
        if (trailing) {
            waitedMs += presenting->FinishPresent();
            timing.Mark("render_fence_wait");
        }
        if (submitted) {
            waitedMs = presenting->FinishPresent();
            timing.Mark("render_fence_wait");

            GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Present);
            std::lock_guard lock(GuestMemory::GpuMutex());
            presenting->QueuePresent();
            timing.Mark("queue_present");
        }
        gpuReady(context);
        timing.Mark("release_and_callback");
        TracyZone::MarkFrame();
        if (profile) reportPresents(waitedMs, inFlight);
        CheckFailure();
    } catch (const ProcessShutdown&) {
        throw;
    } catch (...) {
        ReportFailure(std::current_exception());
        throw;
    }
}

void Driver::reportPresents(double waitedMs, std::size_t inFlight) {
    static std::vector<double> waits;
    static VulkanDevice::PresentStatistics previous{};
    static std::uint64_t previousFlips = 0, windowSerial = 0, previousUnsignaled = 0;
    static auto lastReport = std::chrono::steady_clock::now();
    waits.push_back(waitedMs);
    if (waits.size() == 1) windowSerial = flipSerial.load();
    const auto now = std::chrono::steady_clock::now();
    if (now - lastReport < std::chrono::seconds(10)) return;
    lastReport = now;
    std::sort(waits.begin(), waits.end());
    const auto percentile = [&](double fraction) { return waits.empty() ? 0.0 : waits[std::min(waits.size() - 1, static_cast<std::size_t>(fraction * static_cast<double>(waits.size())))]; };
    const auto counts = VulkanDevice::PresentCounts();
    const auto presents = counts.presents - previous.presents;
    const double perPresent = presents != 0 ? 1.0 / static_cast<double>(presents) : 0.0;
    const auto dropped = counts.acquireDrops - previous.acquireDrops;
    const double perFrame = presents + dropped != 0 ? 1.0 / static_cast<double>(presents + dropped) : 0.0;
    const auto flips = flipsCounted.load() - previousFlips;
    const double perFlip = flips != 0 ? 1.0 / static_cast<double>(flips) : 0.0;
    const auto serial = flipSerial.load();
    AgcDriver::ProfilePrint_nid_no_patch( "[present] %llu presents over 10 s (%llu dropped: no free swapchain image; %zu may trail on the GPU); %s p50 %.2f ms, p90 %.2f ms, max %.2f ms over %zu; per frame, presented or dropped: GPU busy %.1f ms, idle gaps %.1f ms (%llu batches without a completion record); per flip: %.1f batches recorded (%.1f unsignaled at the flip); per present: %.1f batches ahead of the blit, %.1f after it; after the flip packet: last submit %.1f ms, blit submit %.1f ms; in-place reads overwritten before execution: %llu of %llu checked%s\n", static_cast<unsigned long long>(presents), static_cast<unsigned long long>(dropped), inFlight, inFlight != 0 ? "inflight_wait" : "render_fence_wait", percentile(0.5), percentile(0.9), waits.empty() ? 0.0 : waits.back(), waits.size(), (counts.gpuBusyMs - previous.gpuBusyMs) * perFrame, (counts.gpuGapMs - previous.gpuGapMs) * perFrame, static_cast<unsigned long long>(counts.gpuUnread - previous.gpuUnread), static_cast<double>(serial - windowSerial) * perFlip, static_cast<double>(flipBatchesUnsignaled.load() - previousUnsignaled) * perFlip, static_cast<double>(counts.batchesAheadOfBlit - previous.batchesAheadOfBlit) * perPresent, static_cast<double>(counts.batchesAfterFlip - previous.batchesAfterFlip) * perPresent, (counts.lastSubmitAfterFlipMs - previous.lastSubmitAfterFlipMs) * perPresent, (counts.blitSubmitAfterFlipMs - previous.blitSubmitAfterFlipMs) * perPresent, static_cast<unsigned long long>(counts.readsOverwritten - previous.readsOverwritten), static_cast<unsigned long long>(counts.readsChecked - previous.readsChecked), Graphics::Recorder::SmallDmaReport().c_str());
    previous = counts;
    previousFlips = flipsCounted.load();
    previousUnsignaled = flipBatchesUnsignaled.load();
    waits.clear();
}

void Driver::ReleaseWindow(void* window) {
    std::lock_guard lock(GuestMemory::GpuMutex());
    if (device && device->Window() == window) device.Reset();
}

}
