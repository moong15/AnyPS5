#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <memory>
#ifdef _WIN32
#include <windows.h>
#endif

int main() {
    static const bool noHeapWalk = [] {
        const auto* value = std::getenv("APS5_HOST_MEMORY_LEDGER_NO_HEAPWALK");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
#ifdef _WIN32
    const auto destroyHeap = [](void* heap) { HeapDestroy(heap); };
    std::unique_ptr<void, decltype(destroyHeap)> heap(HeapCreate(0, 0, 0), destroyHeap);
    if (!heap) return 10;
    constexpr std::size_t heapBytes = 16u * 1048576u;
    constexpr std::size_t regionBytes = 64u * 1048576u;
    const auto releaseRegion = [](void* region) { VirtualFree(region, 0, MEM_RELEASE); };
    std::unique_ptr<void, decltype(releaseRegion)> region(VirtualAlloc(nullptr, regionBytes, MEM_RESERVE, PAGE_READWRITE), releaseRegion);
    if (!region) return 11;
#endif
    const auto close = [](std::FILE* file) { std::fclose(file); };
    std::unique_ptr<std::FILE, decltype(close)> output(std::tmpfile(), close);
    if (!output) return 1;
    AgcDriverReportHostMemory_nid_no_patch(output.get());
    std::rewind(output.get());
    char line[8192]{};
    if (!std::fgets(line, sizeof(line), output.get())) return 2;
    if (std::strncmp(line, "[hostmem] private", 17) != 0) return 3;
    double privateMiB = 0;
    if (std::sscanf(line, "[hostmem] private %lf MiB:", &privateMiB) != 1) return 4;
#ifdef _WIN32
    if (privateMiB <= 0) return 5;
#endif
    if (std::strchr(line, '\n') == nullptr || std::fgetc(output.get()) != EOF) return 6;
    const char* fields[]{"; vq private ", " (guest ", ", host ", ") mapped ", " image ", "; vk dev ", "; pipelines ", " cache ", "; spirv ", "; heap ", " mapped ", ") vectors ", "; cache loaded ", " saved ", "; vk import host ", " dev ", " unique ", " guest ", "; vk alloc host "};
    for (const auto* field : fields) if (std::strstr(line, field) == nullptr) return 7;
    const char* newFields[]{"; heaps ", " process ", " crt ", " other-heaps ", " largest ", "; heapwalk ", "; heap host ", " heap-other ", " nonheap host ", "; nonheap groups ", "; nh <64K ", "; nh 64K-1M ", "; nh 1-16M ", "; nh 16-64M ", "; nh 64-256M ", "; nh >=256M ", "; nh top ", "; vk class dl ", " dlhv ", " hv ", " none ", "; mapped dl ", " import ", "; types [", "; budget ["};
    for (const auto* field : newFields) if (std::strstr(line, field) == nullptr) return 12;
    if (std::strstr(line, noHeapWalk ? "; heapwalk off" : "; heapwalk on") == nullptr) return 23;
    double walkMiB = 0;
    if (std::sscanf(std::strstr(line, "; vq private "), "; vq private %lf", &walkMiB) != 1) return 8;
#ifdef _WIN32
    if (walkMiB <= 0) return 9;
    std::size_t heapCount = 0;
    double heapBefore = 0;
    if (std::sscanf(std::strstr(line, " other-heaps "), " other-heaps %zu %lf", &heapCount, &heapBefore) != 2) return 13;
    std::size_t bucketBefore = 0;
    double bytesBefore = 0;
    if (std::sscanf(std::strstr(line, "; nh 64-256M "), "; nh 64-256M %zu %lf", &bucketBefore, &bytesBefore) != 2) return 14;
    double nonheapBefore = 0;
    if (std::sscanf(std::strstr(line, " nonheap host "), " nonheap host %lf", &nonheapBefore) != 1) return 24;
    const auto freeAllocation = [&](void* allocation) { HeapFree(heap.get(), 0, allocation); };
    std::unique_ptr<void, decltype(freeAllocation)> allocation(HeapAlloc(heap.get(), 0, heapBytes), freeAllocation);
    if (!allocation) return 15;
    std::memset(allocation.get(), 0x5a, heapBytes);
    if (!VirtualAlloc(region.get(), regionBytes, MEM_COMMIT, PAGE_READWRITE)) return 16;
    std::memset(region.get(), 0xa5, regionBytes);
    std::unique_ptr<std::FILE, decltype(close)> after(std::tmpfile(), close);
    if (!after) return 17;
    AgcDriverReportHostMemory_nid_no_patch(after.get());
    std::rewind(after.get());
    if (!std::fgets(line, sizeof(line), after.get())) return 18;
    const auto* heaps = std::strstr(line, " other-heaps ");
    const auto* bucket = std::strstr(line, "; nh 64-256M ");
    double heapAfter = 0;
    std::size_t bucketAfter = 0;
    double bytesAfter = 0;
    if (!heaps || std::sscanf(heaps, " other-heaps %zu %lf", &heapCount, &heapAfter) != 2) return 19;
    if (!bucket || std::sscanf(bucket, "; nh 64-256M %zu %lf", &bucketAfter, &bytesAfter) != 2) return 20;
    double nonheapAfter = 0;
    const auto* nonheap = std::strstr(line, " nonheap host ");
    if (!nonheap || std::sscanf(nonheap, " nonheap host %lf", &nonheapAfter) != 1) return 25;
    if (noHeapWalk && (std::strstr(line, "; heapwalk off") == nullptr || nonheapAfter - nonheapBefore < regionBytes / 1048576.0 - 1)) return 26;
    if (heapAfter - heapBefore < heapBytes / 1048576.0) return 21;
    if (bucketAfter <= bucketBefore || bytesAfter - bytesBefore < regionBytes / 1048576.0) return 22;
#endif
    return 0;
}
