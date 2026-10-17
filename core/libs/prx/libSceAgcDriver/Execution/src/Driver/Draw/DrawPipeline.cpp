#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

namespace AgcDriver::DriverDetail {

namespace {

constexpr std::array<const char*, static_cast<std::size_t>(DrawPipeline::DrainReason::Count)> DrainNames{"packet", "flush", "labels", "capture", "indirect", "submission"};

std::uint64_t elapsedNs(std::chrono::steady_clock::time_point since) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - since).count());
}

// HeldReads: the read ranges the queue holds, and how often a store asked about one (hits are
// deferred write-backs that waited for a queued index read).
thread_local const std::vector<DrawPipeline::Range>* committingOrderedReads = nullptr;
thread_local const std::vector<DrawPipeline::Range>* committingReads = nullptr;
std::atomic<std::uint64_t> heldReadRanges{0};
std::atomic<std::uint64_t> heldReadQueries{0};
std::atomic<std::uint64_t> heldReadHits{0};

}

DrawPipeline& DrawPipeline::Queue0() {
    static DrawPipeline* pipeline = new DrawPipeline();
    return *pipeline;
}

std::size_t DrawPipeline::Depth() {
    static const std::size_t depth = [] {
        const char* text = std::getenv("APS5_PIPELINED_DRAWS");
        if (std::getenv("APS5_LOCKED_DRAW_PREPARE") != nullptr || std::getenv("APS5_DRAW_DRAIN") != nullptr || std::getenv("APS5_DRAIN_ALL") != nullptr || std::getenv("APS5_NO_WORDWISE_CAPTURE") != nullptr) return std::size_t{0};
        if (text == nullptr) return std::size_t{8};
        const auto value = std::strtoull(text, nullptr, 10);
        return value == 0 ? std::size_t{0} : value == 1 ? std::size_t{8} : static_cast<std::size_t>(std::min<unsigned long long>(value, 256));
    }();
    return depth;
}

std::size_t DrawPipeline::QueuedItems() {
    auto& pipeline = Queue0();
    std::lock_guard lock(pipeline.mutex);
    return pipeline.items.size();
}

bool& DrawPipeline::Active() {
    static thread_local bool active = false;
    return active;
}

std::atomic<std::uint64_t>& DrawPipeline::EpochToken() {
    static std::atomic<std::uint64_t> token{1};
    return token;
}

std::atomic<std::uint64_t>& DrawPipeline::QueueEpochToken(std::uint32_t queue) {
    static std::array<std::atomic<std::uint64_t>, DrawPipeline::QueueCount> tokens{};
    return tokens[std::min<std::size_t>(queue, tokens.size() - 1)];
}

void DrawPipeline::BumpEpoch(std::uint32_t queue) {
    if (!Active()) return;
    EpochToken().fetch_add(1, std::memory_order_relaxed);
    // A bump no queue owns orders every queue's CPU writes (the presenter's drain, a reap off the
    // workers), so every queue's token moves.
    if (queue >= DrawPipeline::QueueCount) {
        for (std::size_t i = 0; i < DrawPipeline::QueueCount; ++i) QueueEpochToken(static_cast<std::uint32_t>(i)).fetch_add(1, std::memory_order_relaxed);
        return;
    }
    QueueEpochToken(queue).fetch_add(1, std::memory_order_relaxed);
}

void DrawPipeline::FollowEpoch(std::uint32_t queue, std::uint64_t sharedToken, std::uint64_t queueToken) {
    static const bool scoped = std::getenv("APS5_NO_TARGET_WALK_SKIP") == nullptr;
    if (!scoped) {
        static thread_local std::uint64_t followed = 0;
        if (followed == sharedToken) return;
        followed = sharedToken;
        GuestMemory::BumpCollectEpoch();
        return;
    }
    // The scope belongs to the item being committed, not to the last token that moved: coming back
    // to a queue whose ordering point has not moved must return to that queue's scope, or its
    // items would reuse whichever queue's scope ran in between.
    static thread_local std::size_t scope = DrawPipeline::QueueCount;
    static thread_local std::uint64_t token = 0;
    const auto slot = std::min<std::size_t>(queue, DrawPipeline::QueueCount - 1);
    if (scope == slot && token == queueToken) return;
    scope = slot;
    token = queueToken;
    GuestMemory::SetCollectScope(static_cast<std::uint32_t>(slot), queueToken);
}

void DrawPipeline::rethrowFailure() {
    if (failure == nullptr) return;
    auto error = std::exchange(failure, nullptr);
    std::rethrow_exception(error);
}

void DrawPipeline::Enqueue(Commit commit, std::vector<Range> writes, std::uint64_t labelAddress, std::vector<std::byte> labelBytes, std::vector<Range> reads, bool bounded, bool orderedReads) {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    std::unique_lock lock(mutex);
    rethrowFailure();
    if (items.size() >= Depth()) {
        if (GuestMemory::GpuMutex().HeldByThisThread()) throw std::runtime_error("draw pipeline full while the enqueuing thread holds the GPU lock");
        const auto start = std::chrono::steady_clock::now();
        ++idleWaiters;
        idle.wait(lock, [&] { return items.size() < Depth(); });
        --idleWaiters;
        fullWaitNs += elapsedNs(start);
        rethrowFailure();
    }
    depthSum += items.size();
    ++enqueued;
    heldReadRanges.fetch_add(reads.size(), std::memory_order_relaxed);
    items.push_back({std::move(commit), std::move(writes), labelAddress, std::move(labelBytes), std::move(reads), bounded, orderedReads});
    outstanding.fetch_add(1, std::memory_order_release);
    if (!thread.joinable()) thread = std::thread([this] { run(); });
    if (committerWaiting) wake.notify_one();
    if (!profile) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - lastReport >= std::chrono::seconds(10)) report(now);
}

void DrawPipeline::report(std::chrono::steady_clock::time_point now) {
    lastReport = now;
    std::string drainText;
    for (std::size_t i = 0; i < drains.size(); ++i) {
        char item[64];
        std::snprintf(item, sizeof(item), " %s %llu (%.1f ms)", DrainNames[i], static_cast<unsigned long long>(drains[i]), static_cast<double>(drainWaitNs[i]) / 1e6);
        drainText += item;
    }
    drainText += "; packet drains by opcode:";
    for (int shown = 0; shown < 5; ++shown) {
        const auto top = std::max_element(drainOpcodes.begin(), drainOpcodes.begin() + 0x100);
        if (*top == 0) break;
        char item[32];
        std::snprintf(item, sizeof(item), " 0x%02x x%llu", static_cast<unsigned>(top - drainOpcodes.begin()), static_cast<unsigned long long>(*top));
        drainText += item;
        *top = 0;
    }
    drainOpcodes = {};
    drainText += "; packet hazards (overlap/disjoint/unknown):";
    for (std::size_t opcode = 0; opcode < packetHazards.size(); ++opcode) {
        const auto& counts = packetHazards[opcode];
        if (counts[0] + counts[1] + counts[2] == 0) continue;
        char item[128];
        std::snprintf(item, sizeof(item), " 0x%02x %llu/%llu/%llu", static_cast<unsigned>(opcode), static_cast<unsigned long long>(counts[0]), static_cast<unsigned long long>(counts[1]), static_cast<unsigned long long>(counts[2]));
        drainText += item;
    }
    packetHazards = {};
    const auto perCommit = commits == 0 ? 0.0 : static_cast<double>(commitNs) / 1e3 / static_cast<double>(commits);
    const auto perDraw = enqueued == 0 ? 0.0 : static_cast<double>(fullWaitNs) / 1e3 / static_cast<double>(enqueued);
    const auto queued = enqueued == 0 ? 0.0 : static_cast<double>(depthSum) / static_cast<double>(enqueued);
    char reads[64];
    std::snprintf(reads, sizeof(reads), " %llu read ranges, %llu stores held", static_cast<unsigned long long>(heldReadRanges.load(std::memory_order_relaxed)), static_cast<unsigned long long>(heldReadHits.exchange(0, std::memory_order_relaxed)));
    drainText += reads;
    // The recorder's commit-wait totals are cumulative; this line reports the interval's share.
    static AgcDriver::Graphics::Recorder::CommitWaitStatistics previousWaits{0, 0.0, 0.0};
    const auto commitWaits = AgcDriver::Graphics::Recorder::CommitUnlockedWaits();
    char commitText[96];
    std::snprintf(commitText, sizeof(commitText), "; commit waits outside the lock %llu / %.1f ms (+ %.1f ms relocking)", static_cast<unsigned long long>(commitWaits.waits - previousWaits.waits), commitWaits.waitedMs - previousWaits.waitedMs, commitWaits.relockMs - previousWaits.relockMs);
    previousWaits = commitWaits;
    static AgcDriver::Graphics::Recorder::HookPrewaitStatistics previousPrewaits{};
    const auto prewaits = AgcDriver::Graphics::Recorder::HookPrewaitCounts();
    char prewaitText[160];
    std::snprintf(prewaitText, sizeof(prewaitText), "; hook pre-waits %llu / %.1f ms unlocked, hook waits still locked inside commits %llu", static_cast<unsigned long long>(prewaits.waits - previousPrewaits.waits), prewaits.waitedMs - previousPrewaits.waitedMs, static_cast<unsigned long long>(prewaits.lockedCommitWaits - previousPrewaits.lockedCommitWaits));
    previousPrewaits = prewaits;
    drainText += prewaitText;
    std::fprintf(stderr, "[draw] pipelined (10 s, depth %zu): %llu commits at %.1f us each on the committer, %llu failed; %llu enqueued behind %.2f on average, waiting %.1f us per draw for room;%s%s; drains:%s\n", Depth(), static_cast<unsigned long long>(commits), perCommit, static_cast<unsigned long long>(commitErrors), static_cast<unsigned long long>(enqueued), queued, perDraw, reads, commitText, drainText.c_str());
    commits = commitNs = commitErrors = enqueued = fullWaitNs = depthSum = 0;
    drains = {};
    drainWaitNs = {};
}

void DrawPipeline::run() {
    for (;;) {
        Commit* commit = nullptr;
        {
            std::unique_lock lock(mutex);
            committerWaiting = true;
            wake.wait(lock, [&] { return !items.empty(); });
            committerWaiting = false;
            commit = stopped ? nullptr : &items.front().commit;
            committingReads = &items.front().reads;
            committingOrderedReads = items.front().orderedReads ? &items.front().reads : nullptr;
        }
        const auto start = std::chrono::steady_clock::now();
        std::exception_ptr fatal;
        bool failed = false;
        bool shutdown = false;
        try {
            if (commit != nullptr) (*commit)();
        } catch (const std::exception&) {
            failed = true;
            fatal = std::current_exception();
        } catch (const ProcessShutdown&) {
            shutdown = true;
            fatal = std::current_exception();
        } catch (...) {
            fatal = std::current_exception();
        }
        committingOrderedReads = nullptr;
        committingReads = nullptr;
        const auto spent = elapsedNs(start);
        if (fatal != nullptr && !stopped) {
            stopped = true;
            if (!shutdown) Driver::Get().ReportFailure(fatal);
        }
        std::lock_guard lock(mutex);
        items.pop_front();
        ++commits;
        commitNs += spent;
        if (failed) ++commitErrors;
        if (fatal != nullptr && failure == nullptr) failure = fatal;
        outstanding.fetch_sub(1, std::memory_order_release);
        if (idleWaiters != 0) idle.notify_all();
    }
}

bool DrawPipeline::DrainPacket(std::span<const std::uint32_t> packet) {
    if (packet.empty()) throw std::runtime_error("draw pipeline packet has no header");
    const auto header = packet[0];
    const auto opcode = (header >> 8u) & 0xffu;
    auto& pipeline = *this;
    if (header == FlipPacketHeader || header == RenderingWaitPacketHeader) {
        pipeline.Drain(DrawPipeline::DrainReason::Packet, header == FlipPacketHeader ? 0xffu : opcode);
        return true;
    }
    std::vector<DrawPipeline::Range> reads, writes;
    bool bounded = false;
    const auto add = [](auto& ranges, std::uint64_t address, std::uint64_t bytes) {
        if (bytes != 0) ranges.emplace_back(address, address + bytes);
    };
    if (opcode == 0x20) {
        bounded = true;
    } else if (opcode == 0x3c || opcode == 0x93) {
        const auto wait = Pm4::DecodeWait(packet);
        add(reads, wait.address, wait.bytes);
        bounded = true;
    } else if (opcode == 0x50 && packet.size() == 7) {
        const auto source = Pm4::DmaSource(packet);
        const auto destination = Pm4::DmaDestination(packet);
        bounded = (source == 0 || source == 2 || source == 3) && (destination == 0 || destination == 3);
        if (source == 0 || source == 3) add(reads, packet[2] | (static_cast<std::uint64_t>(packet[3]) << 32u), Pm4::DmaBytes(packet));
        if (destination == 0 || destination == 3) add(writes, packet[4] | (static_cast<std::uint64_t>(packet[5]) << 32u), Pm4::DmaBytes(packet));
    }
    return pipeline.DrainPacket(opcode, reads, writes, bounded);
}

bool DrawPipeline::PreciseDrains() {
    static const bool enabled = std::getenv("APS5_NO_PRECISE_DRAW_DRAINS") == nullptr;
    return enabled;
}

bool DrawPipeline::DrainPacket(std::uint32_t opcode, std::span<const Range> reads, std::span<const Range> writes, bool bounded) {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    if (!Busy()) return false;
    bool overlap = false;
    bool complete = bounded;
    {
        std::lock_guard lock(mutex);
        const auto intersects = [](const Range& a, const Range& b) { return a.first < b.second && b.first < a.second; };
        const auto valid = [](const Range& range) { return range.first < range.second; };
        complete &= std::all_of(reads.begin(), reads.end(), valid) && std::all_of(writes.begin(), writes.end(), valid);
        for (const auto& item : items) {
            if (!reads.empty() || !writes.empty()) {
                complete &= item.bounded;
                complete &= std::all_of(item.reads.begin(), item.reads.end(), valid) && std::all_of(item.writes.begin(), item.writes.end(), valid);
            }
            for (const auto& range : reads) {
                for (const auto& pending : item.writes) overlap |= intersects(range, pending);
            }
            for (const auto& range : writes) {
                for (const auto& pending : item.writes) overlap |= intersects(range, pending);
                for (const auto& pending : item.reads) overlap |= intersects(range, pending);
            }
        }
        if (profile) ++packetHazards[std::min<std::uint32_t>(opcode, 0x100)][overlap ? 0 : complete ? 1 : 2];
    }
    if (PreciseDrains() && complete && !overlap) return false;
    drain(DrainReason::Packet, opcode);
    return true;
}

void DrawPipeline::Drain(DrainReason reason, std::uint32_t opcode) {
    if (reason == DrainReason::Packet && Busy()) {
        DrainPacket(opcode, {}, {}, false);
        return;
    }
    drain(reason, opcode);
}

void DrawPipeline::drain(DrainReason reason, std::uint32_t opcode) {
    drainRequests[std::min<std::uint32_t>(opcode, 0x100)].fetch_add(1, std::memory_order_relaxed);
    if (!Busy()) return;
    if (GuestMemory::GpuMutex().HeldByThisThread()) throw std::runtime_error("draw pipeline drain under the GPU lock (reason " + std::to_string(static_cast<unsigned>(reason)) + ", opcode " + std::to_string(opcode) + ")");
    const auto start = std::chrono::steady_clock::now();
    std::unique_lock lock(mutex);
    ++idleWaiters;
    idle.wait(lock, [&] { return items.empty(); });
    --idleWaiters;
    const auto index = static_cast<std::size_t>(reason);
    ++drains[index];
    ++drainOpcodes[std::min<std::uint32_t>(opcode, 0x100)];
    drainWaitNs[index] += elapsedNs(start);
    rethrowFailure();
}

std::uint64_t DrawPipeline::DrainRequestsByOpcode(std::uint32_t opcode) {
    return Queue0().drainRequests[std::min<std::uint32_t>(opcode, 0x100)].load(std::memory_order_relaxed);
}

std::span<const DrawPipeline::Range> DrawPipeline::CommitReads() {
    return committingReads != nullptr ? std::span<const Range>(*committingReads) : std::span<const Range>{};
}

bool DrawPipeline::Committing() {
    return committingReads != nullptr;
}

// The index range of a commit that has been decoded but has not run yet is still a guest read at
// the commit's own place in the command stream: the packets that came after it must not store into
// those bytes first. The item is in the queue while its commit runs, so a commit's own flush of a
// deferred write is held back too, and the image keeps its results for the consumer that follows the
// draw. Off with APS5_NO_QUEUED_DRAW_READ_ORDER=1 (the pre-fix behaviour, write-backs overtake
// queued index reads).
bool DrawPipeline::HeldReads(std::uint64_t address, std::size_t bytes) {
    if (bytes == 0 || std::getenv("APS5_NO_QUEUED_DRAW_READ_ORDER") != nullptr) return false;
    const auto end = address + bytes;
    if (committingOrderedReads != nullptr && std::any_of(committingOrderedReads->begin(), committingOrderedReads->end(), [&](const Range& range) { return range.first <= address && end <= range.second; })) return false;
    auto& pipeline = Queue0();
    std::lock_guard lock(pipeline.mutex);
    heldReadQueries.fetch_add(1, std::memory_order_relaxed);
    for (const auto& item : pipeline.items) {
        for (const auto& [begin, limit] : item.reads) {
            if (address >= limit || begin >= end) continue;
            heldReadHits.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }
    return false;
}

std::optional<std::uint64_t> DrawPipeline::PendingLabel(std::uint64_t address, std::size_t bytes) {
    if (bytes == 0 || bytes > 8) return std::nullopt;
    const auto end = address + bytes;
    std::lock_guard lock(mutex);
    for (auto item = items.rbegin(); item != items.rend(); ++item) {
        const bool overlaps = std::any_of(item->writes.begin(), item->writes.end(), [&](const Range& range) { return address < range.second && range.first < end; });
        if (!overlaps) continue;
        if (item->labelBytes.empty() || address < item->labelAddress || end > item->labelAddress + item->labelBytes.size()) return std::nullopt;
        std::uint64_t value = 0;
        std::memcpy(&value, item->labelBytes.data() + (address - item->labelAddress), bytes);
        return value;
    }
    return std::nullopt;
}

bool DrawPipeline::Overlaps(std::uint64_t address, std::size_t bytes) {
    const auto end = address + bytes;
    std::lock_guard lock(mutex);
    for (const auto& item : items) {
        for (const auto& [begin, limit] : item.writes) {
            if (address < limit && begin < end) return true;
        }
    }
    return false;
}

}
