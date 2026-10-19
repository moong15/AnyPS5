#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using AgcDriver::DriverDetail::DrawPipeline;

constexpr std::uint64_t Base = 0x7f0000000000ull;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void WaitFor(const std::atomic<bool>& flag, const char* message) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!flag.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error(message);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void Drain() {
    DrawPipeline::Queue0().Drain(DrawPipeline::DrainReason::Submission);
}

struct Unblock {
    std::vector<std::atomic<bool>*> flags;
    ~Unblock() {
        for (auto* flag : flags) flag->store(true, std::memory_order_release);
        try {
            Drain();
        } catch (...) {
        }
    }
};

void RunningAndQueuedReads() {
    auto& pipeline = DrawPipeline::Queue0();
    std::atomic<bool> running{false};
    std::atomic<bool> release{false};
    std::atomic<bool> released{false};
    std::atomic<bool> finish{false};
    const Unblock unblock{{&release, &finish}};
    pipeline.Enqueue([&] {
        running.store(true, std::memory_order_release);
        WaitFor(release, "the running commit was never asked to release its reads");
        DrawPipeline::Queue0().ReleaseRunningReads();
        released.store(true, std::memory_order_release);
        WaitFor(finish, "the running commit was never allowed to finish");
    }, {}, 0, {}, {{Base, Base + 64}, {Base + 0x100, Base + 0x140}}, true);
    pipeline.Enqueue([] {}, {}, 0, {}, {{Base + 0x1000, Base + 0x1010}}, true);
    pipeline.Enqueue([] {}, {{Base + 0x2000, Base + 0x2040}}, 0, {}, {}, true);
    WaitFor(running, "the committer never started the first commit");
    Require(DrawPipeline::QueuedReadRanges() == 3, "the queued read ranges are miscounted");
    Require(DrawPipeline::HeldReads(Base, 64), "the running commit's read was not held");
    Require(DrawPipeline::HeldReads(Base + 63, 1), "the last byte of the running commit's read was not held");
    Require(!DrawPipeline::HeldReads(Base + 64, 64), "the bytes after the running commit's read were held");
    Require(DrawPipeline::HeldReads(Base + 0x120, 4), "the running commit's second read was not held");
    Require(DrawPipeline::HeldReads(Base + 0x100c, 8), "a queued commit's read was not held");
    Require(!DrawPipeline::HeldReads(Base + 0x2000, 0x40), "a queued write was held as a read");
    Require(!DrawPipeline::HeldReads(Base, 0), "an empty range was held");
    release.store(true, std::memory_order_release);
    WaitFor(released, "the running commit never released its reads");
    Require(DrawPipeline::QueuedReadRanges() == 1, "released reads are still counted");
    Require(!DrawPipeline::HeldReads(Base, 64), "a released read was still held");
    Require(DrawPipeline::HeldReads(Base + 0x1000, 16), "releasing the running reads dropped a queued commit's read");
    finish.store(true, std::memory_order_release);
    Drain();
    Require(DrawPipeline::QueuedReadRanges() == 0, "retired commits left their reads counted");
    Require(!DrawPipeline::HeldReads(Base + 0x1000, 16), "a retired commit's read was still held");
}

void OrderedReadsOnTheCommitter() {
    auto& pipeline = DrawPipeline::Queue0();
    std::atomic<bool> running{false};
    std::atomic<bool> check{false};
    bool inside = true;
    bool straddling = false;
    bool later = false;
    bool elsewhere = true;
    const Unblock unblock{{&check}};
    pipeline.Enqueue([&] {
        running.store(true, std::memory_order_release);
        WaitFor(check, "the ordered commit was never asked to check its reads");
        inside = DrawPipeline::HeldReads(Base + 0x3008, 8);
        straddling = DrawPipeline::HeldReads(Base + 0x3038, 0x10);
        later = DrawPipeline::HeldReads(Base + 0x4000, 4);
        elsewhere = DrawPipeline::HeldReads(Base + 0x5000, 4);
    }, {}, 0, {}, {{Base + 0x3000, Base + 0x3040}}, true, true);
    pipeline.Enqueue([] {}, {}, 0, {}, {{Base + 0x4000, Base + 0x4004}}, true);
    WaitFor(running, "the committer never started the ordered commit");
    check.store(true, std::memory_order_release);
    Drain();
    Require(!inside, "a commit's own ordered read held its own store");
    Require(straddling, "a store reaching past the commit's own ordered read was not held");
    Require(later, "a later commit's read was not held while an ordered commit ran");
    Require(!elsewhere, "a range no commit reads was held");
    Require(DrawPipeline::QueuedReadRanges() == 0, "ordered commits left their reads counted");
}

void ManyRetiredCommits() {
    auto& pipeline = DrawPipeline::Queue0();
    for (std::uint64_t round = 0; round < 512; ++round) {
        const auto address = Base + 0x10000 + round * 0x100;
        std::vector<DrawPipeline::Range> reads{{address, address + 0x10}};
        if (round % 3 == 0) reads.push_back({address + 0x40, address + 0x80});
        pipeline.Enqueue([] {}, {}, 0, {}, std::move(reads), true, round % 2 == 0);
    }
    Drain();
    Require(DrawPipeline::QueuedReadRanges() == 0, "the read count drifted over many commits");
    Require(!DrawPipeline::HeldReads(Base + 0x10000, 512 * 0x100), "a retired commit's read was still held after many commits");
}

}

int main() {
    try {
        if (DrawPipeline::Depth() < 3) return 77;
        Require(DrawPipeline::QueuedReadRanges() == 0, "a fresh pipeline counts queued reads");
        Require(!DrawPipeline::HeldReads(Base, 64), "an empty pipeline held a read");
        RunningAndQueuedReads();
        OrderedReadsOnTheCommitter();
        ManyRetiredCommits();
        std::cout << "Held reads checked\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
