#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

// The resident-target proof (Graphics::Draw -> refreshResidentTarget -> StorageTexture::refreshProved)
// walks the target's whole page range to notice a CPU store into it, and the write tracker's collect
// memo skips that walk when the same range was already walked in the same ordering epoch. The
// draw-commit thread runs one item per committed draw for every queue, so the epoch it used to hold
// (one per committed item) never matched anything. The test drives the real GuestMemory side through
// the same switch DrawPipeline::FollowEpoch uses (APS5_NO_TARGET_WALK_SKIP) and checks the two
// halves of the contract: a walk of a queue's range is reused for that queue's later items, and a CPU
// store that lands after the skip is still seen.

namespace {

using namespace AgcDriver::GuestMemory;

constexpr std::size_t Block = 65536;
// The number of scopes the per-epoch range map keeps (GuestMemory.cpp), one per queue.
constexpr std::size_t Scopes = 8;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

bool SkipEnabled() {
    return std::getenv("APS5_NO_TARGET_WALK_SKIP") == nullptr;
}

void* AllocateWatched(std::size_t bytes) {
#ifdef _WIN32
    void* block = GuestArena::GuestArenaAllocate_nid_postfix(bytes, Block);
    GuestArena::GuestArenaCommit_nid_postfix(block, bytes, PAGE_READWRITE, bytes);
#else
    void* raw = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) throw std::runtime_error("cannot map the watched block");
    auto block = reinterpret_cast<void*>((reinterpret_cast<std::uintptr_t>(raw) + Block - 1) & ~static_cast<std::uintptr_t>(Block - 1));
    if (block != raw) munmap(raw, static_cast<std::size_t>(reinterpret_cast<std::uint8_t*>(block) - reinterpret_cast<std::uint8_t*>(raw)));
    GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, bytes);
#endif
    Require(Watched(reinterpret_cast<std::uint64_t>(block), bytes), "the test block is not watched");
    return block;
}

// DrawPipeline::FollowEpoch, reproduced so the test switches the same way the commit thread does.
struct Follow {
    std::size_t scope = 16;
    std::uint64_t token = 0;
    std::uint64_t shared = 0;

    void operator()(std::uint32_t queue, std::uint64_t sharedToken, std::uint64_t queueToken) {
        if (!SkipEnabled()) {
            if (shared == sharedToken) return;
            shared = sharedToken;
            BumpCollectEpoch();
            return;
        }
        if (scope == queue && token == queueToken) return;
        scope = queue;
        token = queueToken;
        SetCollectScope(queue, queueToken);
    }
};

// A walk bumps the tracker generation (walkWrites), a memo hit returns the current one, so an
// unchanged generation is a skipped walk. Counted through the profile counters as well, which is
// what the [draws] readTarget line reports.
struct Walked {
    std::uint64_t generation;
    std::uint64_t collected;
    std::uint64_t skipped;
    std::uint64_t saved;
};

Walked Sample() {
    return {TrackerGeneration(), ThreadCollectedBytes(), ThreadSkippedWalks(), ThreadSkippedBytes()};
}

bool Walked(const Walked& before, const Walked& after) {
    return after.generation != before.generation && after.collected != before.collected;
}

void CheckTargetWalkSkip() {
    constexpr std::size_t bytes = 4 * Block;
    auto* memory = static_cast<std::uint8_t*>(AllocateWatched(bytes));
    struct Cleanup {
        std::uint8_t* memory;
        std::size_t bytes;
        ~Cleanup() {
#ifdef _WIN32
            GuestArena::GuestArenaRelease_nid_postfix(memory, bytes);
#else
            Unwatch(reinterpret_cast<std::uint64_t>(memory), bytes);
            munmap(memory, bytes);
#endif
        }
    } cleanup{memory, bytes};
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    std::memset(memory, 0x11, bytes);
    Follow follow;

    // A queue's first lookup after its ordering point walks the target; the next one does not.
    follow(0, 1, 7);
    auto before = Sample();
    const auto first = CollectWrites(base, bytes);
    Require(first != 0 && Walked(before, Sample()), "the first target lookup of an ordering point did not walk");
    before = Sample();
    Require(CollectWrites(base, bytes) == first && !Walked(before, Sample()), "the next lookup of the same ordering point walked again");
    Require(Sample().skipped == before.skipped + 1 && Sample().saved == before.saved + bytes, "a skipped target walk is not counted");

    // A raw CPU store into the target, then that queue's next ordering point: the store is found.
    memory[2 * Block + 17] ^= 1;
    follow(0, 2, 8);
    before = Sample();
    const auto afterStore = CollectWrites(base, bytes);
    Require(Walked(before, Sample()), "a CPU store after a skipped walk was not collected");
    Require(afterStore > first && !UnchangedSince(base, bytes, first), "a CPU store after a skipped walk reads as unchanged");

    // It is collected once: a further lookup of that ordering point reuses the walk.
    before = Sample();
    Require(CollectWrites(base, bytes) == afterStore && !Walked(before, Sample()), "the ordering point did not reuse its walk");

    // A driver's own store is stamped and never hidden by a memoized observation.
    const auto stored = MarkWritten(base + 0x30, 4);
    Require(CollectWrites(base, 64) >= stored && !UnchangedSince(base, 64, afterStore), "a memoized observation hid a driver write");

    // Another queue's item is a different ordering point for this thread: its lookup walks.
    follow(1, 3, 8);
    before = Sample();
    CollectWrites(base, bytes);
    Require(Walked(before, Sample()), "a walk was reused for another queue's item");

    // Coming back to the queue reuses that queue's own walk (its scope kept its range map), while
    // APS5_NO_TARGET_WALK_SKIP=1 drops it because the thread has one epoch per committed item.
    follow(0, 4, 8);
    before = Sample();
    CollectWrites(base, bytes);
    if (SkipEnabled()) Require(!Walked(before, Sample()), "a queue lost its own range walk when another queue's item ran");
    else Require(Walked(before, Sample()), "APS5_NO_TARGET_WALK_SKIP=1 did not restore the per-item walk");

    if (SkipEnabled()) {
        // More scopes than the map keeps evict the oldest. A lookup then falls back on the
        // per-thread ring or walks, and either way a store made after it is collected at the next
        // ordering point: an eviction can only ever cost a walk.
        follow(0, 5, 9);
        CollectWrites(base, bytes);
        for (std::uint32_t queue = 1; queue <= Scopes; ++queue) {
            follow(queue, 6 + queue, 9);
            CollectWrites(base, bytes);
        }
        follow(0, 7 + Scopes, 9);
        CollectWrites(base, bytes);
        memory[Block + 5] ^= 1;
        follow(1, 100, 10);
        before = Sample();
        CollectWrites(base, bytes);
        Require(Walked(before, Sample()) && !UnchangedSince(base, bytes, afterStore), "a CPU store after an evicted scope reads as unchanged");
    }
}

}

// The dirty summary (GuestMemory.cpp): a collect walks only the blocks of its range that this
// queue's ordering point has not already walked clean, and a range it covers whole is not walked at
// all. It is the same rule the memo gives, one block at a time, and it covers what the memo's range
// map cannot: a range no entry holds exactly, because its first walk was of another range.
void CheckTargetDirtySummary() {
    const bool summary = std::getenv("APS5_NO_TARGET_DIRTY_SUMMARY") == nullptr;
    constexpr std::size_t bytes = 4 * Block;
    auto* memory = static_cast<std::uint8_t*>(AllocateWatched(bytes));
    struct Cleanup {
        std::uint8_t* memory;
        std::size_t bytes;
        ~Cleanup() {
#ifdef _WIN32
            GuestArena::GuestArenaRelease_nid_postfix(memory, bytes);
#else
            Unwatch(reinterpret_cast<std::uint64_t>(memory), bytes);
            munmap(memory, bytes);
#endif
        }
    } cleanup{memory, bytes};
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    std::memset(memory, 0x11, bytes);
    Follow follow;

    // The queue's first ordering point settles the whole target. The tokens are its own: a scope's
    // memo is kept per (queue, token), and the check above uses the low ones.
    follow(0, 1001, 101);
    Require(CollectWrites(base, bytes) != 0, "the target was not collected");

    // The next one: its walks find nothing, so each block stands clean under that ordering point.
    follow(0, 1002, 102);
    Require(CollectWrites(base, 2 * Block) != 0, "the target's first half was not collected");
    const auto generation = TrackerGeneration();
    const auto beforeWalked = ThreadSummaryWalkedBytes();

    // A range no memo entry holds exactly, whose first two blocks stand clean: only the third block
    // is walked. The same collect with the summary off walks all three.
    auto before = Sample();
    Require(CollectWrites(base, 3 * Block) != 0, "the target's three blocks were not collected");
    Require(Walked(before, Sample()), "the range the summary had to leave did not walk");
    if (summary) {
        Require(ThreadSummaryWalkedBytes() == beforeWalked + Block, "the summary walked a block it had cleared");
    }
    Require(UnchangedSince(base, bytes, generation), "the partly summarised target reads as written");

    // The last block walked on its own, then the whole target: no memo entry covers the whole range,
    // but every block of it stands clean, so the summary skips the walk and a raw CPU store waits for
    // the next ordering point, exactly as it does for a memoized walk.
    Require(CollectWrites(base + 3 * Block, Block) != 0, "the target's last block was not collected");
    static_cast<volatile std::uint8_t*>(memory)[3 * Block + 11] = 0x22;
    before = Sample();
    const auto beforeSkips = ThreadSummarySkips();
    Require(CollectWrites(base, bytes) != 0, "the target was not collected");
    if (summary) {
        Require(!Walked(before, Sample()), "a range the summary covers was walked after a CPU store");
        Require(ThreadSummarySkips() == beforeSkips + 1, "a range the summary covers whole is not counted as a summary skip");
        Require(UnchangedSince(base, bytes, generation), "a store made after a summary skip was seen before the next ordering point");
    } else {
        Require(Walked(before, Sample()), "APS5_NO_TARGET_DIRTY_SUMMARY=1 did not restore the walk of the whole range");
        Require(!UnchangedSince(base, bytes, generation), "APS5_NO_TARGET_DIRTY_SUMMARY=1 did not restore the walk that finds the store");
    }

    // The queue's next ordering point walks the target again and finds the store.
    follow(0, 1003, 103);
    before = Sample();
    const auto after = CollectWrites(base, bytes);
    Require(after != 0 && Walked(before, Sample()), "the next ordering point did not walk the target");
    Require(!UnchangedSince(base, bytes, generation) && WrittenSince(base + 3 * Block, 4, generation), "a CPU store after a summary skip was missed");
}

int main() {
    try {
        if (!WriteWatched()) {
            std::cout << "no write watching: skipped\n";
            return 77;
        }
        CheckTargetWalkSkip();
        CheckTargetDirtySummary();
    } catch (const std::exception& error) {
        std::cerr << "target walk skip test failed: " << error.what() << "\n";
        return 1;
    }
    std::cout << "target walk skip test passed (" << (SkipEnabled() ? "scopes" : "APS5_NO_TARGET_WALK_SKIP=1") << ")\n";
    return 0;
}