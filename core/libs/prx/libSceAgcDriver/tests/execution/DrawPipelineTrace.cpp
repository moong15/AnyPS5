#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libc/include/Shutdown.hpp"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <future>
#include <memory>
#include <vector>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace {

using AgcDriver::DriverDetail::DrawPipeline;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void CheckRetirement() {
    auto& pipeline = DrawPipeline::Queue0();
    std::promise<void> gate;
    const auto opened = gate.get_future().share();
    std::atomic<bool> destroyed{false};
    bool destroyedBeforeNext = false;
    bool ownReadsReleased = false;
    bool queueFollowed = false;
    std::vector<unsigned> order;
    pipeline.Enqueue([opened] { opened.wait(); }, {});
    auto capture = std::shared_ptr<int>(new int(0), [&](int* value) {
        if (std::getenv("APS5_NO_FUSED_RETIRE") == nullptr) static_cast<void>(pipeline.Overlaps(0x1000, 4));
        destroyed.store(true, std::memory_order_release);
        delete value;
    });
    AgcDriver::GuestMemory::TagGpuLockThread(0x20);
    pipeline.Enqueue([capture = std::move(capture), &order, &queueFollowed] {
        order.push_back(0);
        queueFollowed = DrawPipeline::RecordingQueue() == 0x20;
    }, {}, 0, {}, {{0x1000, 0x1040}}, true, true);
    AgcDriver::GuestMemory::TagGpuLockThread(0);
    pipeline.Enqueue([&] {
        order.push_back(1);
        destroyedBeforeNext = destroyed.load(std::memory_order_acquire);
        ownReadsReleased = !DrawPipeline::HeldReads(0x2000, 4);
        pipeline.ReleaseRunningReads();
    }, {}, 0, {}, {{0x2000, 0x2040}}, true, true);
    gate.set_value();
    for (unsigned i = 2; i < 258; ++i) pipeline.Enqueue([&order, i] { order.push_back(i); }, {});
    pipeline.Drain(DrawPipeline::DrainReason::Submission);
    Require(order.size() == 258, "retirement lost a queued callback");
    for (unsigned i = 0; i < order.size(); ++i) Require(order[i] == i, "retirement changed FIFO order");
    Require(destroyedBeforeNext, "retirement kept the previous closure alive into the next callback");
    Require(queueFollowed, "retirement lost the next callback's queue identity");
    Require(ownReadsReleased && DrawPipeline::QueuedReadRanges() == 0, "retirement lost ordered-read accounting");
}

void CheckFailure() {
    auto& pipeline = DrawPipeline::Queue0();
    std::promise<void> gate;
    const auto opened = gate.get_future().share();
    bool laterRan = false;
    pipeline.Enqueue([opened] { opened.wait(); throw ProcessShutdown{}; }, {});
    pipeline.Enqueue([&] { laterRan = true; }, {}, 0, {}, {{0x3000, 0x3040}}, true);
    auto drained = std::async(std::launch::async, [&] {
        try {
            pipeline.Drain(DrawPipeline::DrainReason::Submission);
        } catch (const ProcessShutdown&) {
            return true;
        }
        return false;
    });
    const bool blocked = drained.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout;
    gate.set_value();
    const bool caughtByDrain = drained.get();
    bool caughtByEnqueue = false;
    if (!caughtByDrain) {
        try {
            pipeline.Enqueue([] {}, {});
        } catch (const ProcessShutdown&) {
            caughtByEnqueue = true;
        }
    }
    pipeline.Stop();
    Require(blocked && (caughtByDrain || caughtByEnqueue), "retirement lost the pending failure or drain wait");
    Require(!laterRan && !pipeline.Busy() && DrawPipeline::QueuedReadRanges() == 0, "retirement ran work after failure or left it queued");
    const auto caller = std::this_thread::get_id();
    bool inlineAfterStop = false;
    pipeline.Enqueue([&] { inlineAfterStop = std::this_thread::get_id() == caller; }, {});
    Require(inlineAfterStop, "a closed pipeline stopped committing inline");
}

}

int main(int argc, char** argv) {
    try {
        using namespace AgcDriver;
        using DriverDetail::DrawPipeline;
        if (DrawPipeline::Depth() < 3) return 77;
        const bool disabled = argc > 1 && std::string_view(argv[1]) == "disabled";
        if (PerformanceTrace::Configured() == disabled) throw std::runtime_error("unexpected trace configuration");
        if (!disabled) PerformanceTrace::Present(1, 1, nullptr);
        bool checked = false;
        const char* failure = nullptr;
        DrawPipeline::Queue0().Enqueue([&] {
            auto* trace = PerformanceTrace::Thread();
            if (disabled) {
                if (trace != nullptr) { failure = "unconfigured draw committer allocated a trace buffer"; return; }
                checked = true;
                return;
            }
            if (trace == nullptr) { failure = "draw committer CPU spans are absent from the trace"; return; }
            {
                PerformanceTimer timer("DrawPipeline.TraceRegression");
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            const auto snapshot = trace->Collect();
            if (snapshot.name != "draw committer" || snapshot.dropped) { failure = "draw committer trace identity or capacity is incorrect"; return; }
            const auto found = [](const auto& event) { return event.scope != nullptr && std::string_view(event.scope) == "DrawPipeline.TraceRegression" && event.stage == nullptr && event.length > std::chrono::steady_clock::duration::zero(); };
            if (std::none_of(snapshot.slices.begin(), snapshot.slices.end(), found) || std::none_of(snapshot.totals.begin(), snapshot.totals.end(), found))
                { failure = "committed work was missing from trace slices or totals"; return; }
            checked = true;
        }, {});
        DrawPipeline::Queue0().Drain(DrawPipeline::DrainReason::Submission);
        if (failure != nullptr) throw std::runtime_error(failure);
        if (!checked) throw std::runtime_error("draw committer did not run the trace check");
        CheckRetirement();
        CheckFailure();
        std::cout << "Draw committer tracing and retirement checked\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
