#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {
constexpr std::size_t Bytes = 4 * 65536;

void Require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}

LONG CALLBACK HandleWrite(EXCEPTION_POINTERS* exception) {
    const auto* record = exception->ExceptionRecord;
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2 && record->ExceptionInformation[0] == 1 && GuestArena::GuestArenaHandleWrite_nid_postfix(record->ExceptionInformation[1])) return EXCEPTION_CONTINUE_EXECUTION;
    return EXCEPTION_CONTINUE_SEARCH;
}

struct Mapping {
    HANDLE section = nullptr;
    void* memory = nullptr;
    void* alias = nullptr;
    void* handler = nullptr;

    Mapping() {
        handler = AddVectoredExceptionHandler(1, HandleWrite);
        Require(handler != nullptr, "cannot install write fault handler");
        section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, Bytes, nullptr);
        Require(section != nullptr, "cannot create shared backing");
        memory = GuestArena::GuestArenaAllocate_nid_postfix(Bytes, 65536);
        alias = GuestArena::GuestArenaAllocate_nid_postfix(Bytes, 65536);
        GuestArena::GuestArenaMap_nid_postfix(memory, Bytes, section, 0, PAGE_READWRITE);
        GuestArena::GuestArenaMap_nid_postfix(alias, Bytes, section, 0, PAGE_READWRITE);
    }
    ~Mapping() {
        if (alias != nullptr) GuestArena::GuestArenaRelease_nid_postfix(alias, Bytes);
        if (memory != nullptr) GuestArena::GuestArenaRelease_nid_postfix(memory, Bytes);
        if (section != nullptr) {
            GuestArena::GuestArenaForgetSection_nid_postfix(section);
            CloseHandle(section);
        }
        if (handler != nullptr) RemoveVectoredExceptionHandler(handler);
    }
};

std::array<std::uint64_t, 2> Stats() {
    std::array<std::uint64_t, 2> stats{};
    GuestArena::GuestArenaCollectStats_nid_no_patch(&stats[0], &stats[1]);
    return stats;
}

void Check() {
    using namespace AgcDriver::GuestMemory;
    Mapping mapping;
    const auto address = reinterpret_cast<std::uintptr_t>(mapping.memory);
    const auto initial = CollectWritesUncached(address, Bytes);
    Require(initial != 0, "shared mapping could not be collected");
    Mapping unrelated;
    const bool enabled = std::getenv("APS5_NO_COLLECT_FAST_PATH") == nullptr;
    const auto before = Stats();
    std::array<void*, Bytes / 4096> pages{};
    std::size_t count = pages.size();
    Require(GuestArena::GuestArenaCollectWrites_nid_postfix(address, Bytes, pages.data(), &count, true) && count == 0, "clean repeat reported a write");
    const auto after = Stats();
    Require(enabled ? after[0] == before[0] + 1 && after[1] == before[1] : after[0] == before[0] && after[1] >= before[1] + Bytes / 16384 + 1, "clean collection did not use the selected traversal path");
    Require(UnchangedSince(address, Bytes, initial), "clean repeat invalidated the snapshot");
    auto* alias = static_cast<volatile unsigned char*>(mapping.alias);
    alias[65536 + 17] = 37;
    const auto written = CollectWritesUncached(address, Bytes);
    Require(written != 0 && !UnchangedSince(address + 65536, 65536, initial), "raw CPU alias write after the clean repeat was missed");
    Require(UnchangedSince(address, 65536, initial), "alias write dirtied an unrelated block");
    Require(CollectWritesUncached(address, Bytes) != 0 && UnchangedSince(address, Bytes, written), "settled write was reported twice");
    GuestArena::GuestArenaPinWritable_nid_postfix(mapping.alias, Bytes);
    alias[2 * 65536 + 19] = 41;
    const auto pinned = CollectWritesUncached(address, Bytes);
    Require(pinned != 0 && !UnchangedSince(address + 2 * 65536, 65536, written), "pinned alias reused a clean proof");
    GuestArena::GuestArenaUnpinWritable_nid_postfix(mapping.alias, Bytes);
    Require(CollectWritesUncached(address, Bytes) != 0, "unpin collection failed");
    const auto hostBefore = TrackerGeneration();
    {
        GuestArena::HostWrite host(mapping.alias, Bytes, false);
        Require(host.Open(), "host write window failed");
        alias[3 * 65536 + 23] = 43;
        Require(CollectWritesUncached(address, Bytes) != 0 && !UnchangedSince(address + 3 * 65536, 65536, hostBefore), "host write window reused a clean proof");
    }
}
}
#endif

int main() {
#ifdef _WIN32
    try {
        Check();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
#else
    return 77;
#endif
}
