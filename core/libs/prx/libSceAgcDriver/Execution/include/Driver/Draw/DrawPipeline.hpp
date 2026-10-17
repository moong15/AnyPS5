#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_DRAWPIPELINE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_DRAWPIPELINE_HPP

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace AgcDriver::DriverDetail {

class DrawPipeline {
public:
    using Commit = std::function<void()>;
    using Range = std::pair<std::uint64_t, std::uint64_t>;
    enum class DrainReason : std::uint8_t { Packet, Flush, Labels, Capture, Indirect, Submission, Count };
    // One token per queue a worker can own: advanced by that queue's own ordering points, and by
    // every queue's when a bump is made off a queue worker (nothing then orders one queue's CPU
    // writes without another's). EpochToken is the device-wide token, advanced by every bump.
    static constexpr std::size_t QueueCount = 16;

    static DrawPipeline& Queue0();
    static std::size_t Depth();
    static std::size_t QueuedItems();
    static bool& Active();
    static std::atomic<std::uint64_t>& EpochToken();
    static std::atomic<std::uint64_t>& QueueEpochToken(std::uint32_t queue);
    static void BumpEpoch(std::uint32_t queue);
    // Puts the committing thread under the ordering point the item's queue reached. One epoch for
    // the whole thread under APS5_NO_TARGET_WALK_SKIP=1 (the shared token alone, as before);
    // otherwise one scope per queue, so a walk made for a queue's item is reused for that queue's
    // later items and never for another queue's.
    static void FollowEpoch(std::uint32_t queue, std::uint64_t sharedToken, std::uint64_t queueToken);

    // `reads` are guest ranges the commit reads at its own place in the command stream (the draw's
    // index range, known from the packets): a write the guest ordered after the item must not be
    // recorded into them before the item runs, or the commit reads what came later. See HeldReads.
    void Enqueue(Commit commit, std::vector<Range> writes, std::uint64_t labelAddress = 0, std::vector<std::byte> labelBytes = {}, std::vector<Range> reads = {}, bool bounded = false, bool orderedReads = false);
    void Drain(DrainReason reason, std::uint32_t opcode = 0x100);
    static bool PreciseDrains();
    bool DrainPacket(std::span<const std::uint32_t> packet);
    bool DrainPacket(std::uint32_t opcode, std::span<const Range> reads, std::span<const Range> writes, bool bounded);
    static std::uint64_t DrainRequestsByOpcode(std::uint32_t opcode);
    bool Busy() const { return outstanding.load(std::memory_order_acquire) != 0; }
    std::size_t Outstanding() const { return outstanding.load(std::memory_order_acquire); }
    bool Overlaps(std::uint64_t address, std::size_t bytes);
    std::optional<std::uint64_t> PendingLabel(std::uint64_t address, std::size_t bytes);
    // Whether a queued commit, itself included, still has to read the range from guest memory. The
    // commit's own read is the point: its commit is the first to run after the packets that came
    // before it, so a write recorded while it waits is ordered after the read it belongs to.
    static bool HeldReads(std::uint64_t address, std::size_t bytes);
    static std::span<const Range> CommitReads();
    static bool Committing();

private:
    struct Item {
        Commit commit;
        std::vector<Range> writes;
        std::uint64_t labelAddress = 0;
        std::vector<std::byte> labelBytes;
        std::vector<Range> reads;
        bool bounded = false;
        bool orderedReads = false;
    };
    DrawPipeline() = default;
    void drain(DrainReason reason, std::uint32_t opcode);
    void run();
    void rethrowFailure();
    void report(std::chrono::steady_clock::time_point now);

    std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable idle;
    std::deque<Item> items;
    std::exception_ptr failure;
    std::atomic<std::size_t> outstanding{0};
    std::uint32_t idleWaiters = 0;
    bool committerWaiting = false;
    bool stopped = false;
    std::thread thread;
    std::uint64_t commits = 0;
    std::uint64_t commitNs = 0;
    std::uint64_t commitErrors = 0;
    std::uint64_t enqueued = 0;
    std::uint64_t fullWaitNs = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(DrainReason::Count)> drains{};
    std::array<std::uint64_t, static_cast<std::size_t>(DrainReason::Count)> drainWaitNs{};
    std::array<std::uint64_t, 257> drainOpcodes{};
    std::array<std::atomic<std::uint64_t>, 257> drainRequests{};
    std::array<std::array<std::uint64_t, 3>, 257> packetHazards{};
    std::uint64_t depthSum = 0;
    std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();
};

}

#endif
