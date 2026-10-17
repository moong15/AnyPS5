#include "UnlockedCommitWaitMock.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"

using AgcDriver::DriverDetail::DrawPipeline;

namespace {

void CommitReadPrewait() {
    mock = MockDevice{};
    Guest guest;
    std::vector<int> order;
    std::unique_ptr<Recorder> recorder;
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        recorder = std::make_unique<Recorder>(mockContext(), true);
        recorder->Activate();
        for (int i = 1; i <= 3; ++i) {
            const auto offset = static_cast<std::size_t>((i - 1) * 256);
            recorder->NotePendingWrite(guest.address(offset), 128);
            recorder->OnComplete([&, i, offset] {
                Expect(AgcDriver::GuestMemory::GpuMutex().HeldByThisThread(), "a write-back ran without the GPU mutex");
                guest.memory[offset] = static_cast<std::byte>(i);
                order.push_back(i);
            });
            recorder->Submit();
        }
    }
    const bool enabled = std::getenv("APS5_NO_HOOK_PREWAIT") == nullptr;
    const auto before = Recorder::HookPrewaitCounts();
    auto& pipeline = DrawPipeline::Queue0();
    pipeline.Enqueue([&] {
        Expect(DrawPipeline::Committing(), "the committer did not expose its read context");
        Recorder::PrewaitHookReads(DrawPipeline::CommitReads());
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        std::array<std::byte, 1> value{};
        AgcDriver::GuestMemory::Read(guest.address(256), value);
        Expect(value[0] == std::byte{2}, "the commit read preceded its producer's write-back");
        Expect(order == std::vector<int>{1, 2}, "the targeted read changed completion order or completed an unrelated later batch");
        Expect(recorder->InFlightBatches() == 1, "the targeted read did not leave the unrelated later batch in flight");
    }, {}, 0, {}, {{guest.address(256), guest.address(384)}});
    pipeline.Drain(DrawPipeline::DrainReason::Submission);
    Expect(mock.lockedWaits == (enabled ? 0u : 1u), "the hook wait did not follow the pre-wait switch");
    Expect(mock.semaphoreWaits == (enabled ? 1u : 0u), "the pre-wait did not use the unlocked timeline path");
    if (Profiled()) {
        const auto after = Recorder::HookPrewaitCounts();
        Expect(after.waits - before.waits == (enabled ? 1u : 0u), "the pre-wait profile count is wrong");
        Expect(after.lockedCommitWaits - before.lockedCommitWaits == (enabled ? 0u : 1u), "the locked hook profile count is wrong");
    }
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        recorder->Sync();
        Expect(order == std::vector<int>{1, 2, 3}, "the final write-back order changed");
        recorder.reset();
    }
}

void EmptyAndDisjointSnapshots() {
    mock = MockDevice{};
    Guest guest;
    std::unique_ptr<Recorder> recorder;
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        recorder = std::make_unique<Recorder>(mockContext(), true);
        recorder->Activate();
    }
    const std::vector<DrawPipeline::Range> reads(256, {guest.address(0), guest.address(128)});
    const auto serial = AgcDriver::GuestMemory::GpuHoldSerial();
    for (int i = 0; i < 1000; ++i) Recorder::PrewaitHookReads(reads);
    Expect(AgcDriver::GuestMemory::GpuHoldSerial() == serial, "empty snapshots acquired the GPU mutex");
    Expect(mock.fenceWaits == 0 && mock.semaphoreWaits == 0, "empty snapshots waited for the GPU");
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        Record(*recorder, guest, 512, 128, 3, nullptr, 0);
        recorder->Submit();
    }
    const auto pendingSerial = AgcDriver::GuestMemory::GpuHoldSerial();
    Recorder::PrewaitHookReads(reads);
    Expect(AgcDriver::GuestMemory::GpuHoldSerial() == pendingSerial, "a disjoint pending write acquired the GPU mutex");
    Expect(mock.fenceWaits == 0 && mock.semaphoreWaits == 0, "a disjoint pending write caused a GPU wait");
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        recorder->Sync();
        recorder.reset();
    }
}

void OpenBatchAndExtraRanges() {
    mock = MockDevice{};
    Guest guest;
    std::unique_ptr<Recorder> recorder;
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        recorder = std::make_unique<Recorder>(mockContext(), true);
        recorder->Activate();
        Record(*recorder, guest, 256, 128, 9, nullptr, 0);
    }
    const std::vector<DrawPipeline::Range> ranges(256, {guest.address(256), guest.address(384)});
    const auto before = Recorder::HookPrewaitCounts();
    Recorder::PrewaitHookReads({}, ranges);
    const auto after = Recorder::HookPrewaitCounts();
    const bool enabled = std::getenv("APS5_NO_HOOK_PREWAIT") == nullptr;
    if (Profiled()) Expect(after.locks - before.locks == (enabled ? 1u : 0u), "duplicate extra ranges took more than one pre-wait acquisition");
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        std::array<std::byte, 1> value{};
        AgcDriver::GuestMemory::Read(guest.address(256), value);
        Expect(value[0] == std::byte{9}, "the open batch's write-back did not precede its reader");
        Expect(mock.submits == 1, "the open writer was not submitted exactly once");
        Expect(mock.lockedWaits == (enabled ? 0u : 1u), "the open writer's wait did not follow the switch");
        recorder.reset();
    }
}

void NewlyPendingWriteUsesTheHook() {
    mock = MockDevice{};
    Guest guest;
    auto& pipeline = DrawPipeline::Queue0();
    pipeline.Enqueue([&] {
        Recorder::PrewaitHookReads(DrawPipeline::CommitReads());
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        Recorder recorder(mockContext(), true);
        recorder.Activate();
        Record(recorder, guest, 0, 128, 7, nullptr, 0);
        recorder.Submit();
        std::array<std::byte, 1> value{};
        AgcDriver::GuestMemory::Read(guest.address(0), value);
        Expect(value[0] == std::byte{7}, "the hook missed a write recorded after the pre-wait");
    }, {}, 0, {}, {{guest.address(0), guest.address(128)}});
    pipeline.Drain(DrawPipeline::DrainReason::Submission);
    Expect(mock.lockedWaits == 1, "the nested safety-net hook released the draw's GPU mutex");
}

}

int main() {
    try {
        CommitReadPrewait();
        EmptyAndDisjointSnapshots();
        OpenBatchAndExtraRanges();
        NewlyPendingWriteUsesTheHook();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    if (failures != 0) return 1;
    std::puts("hook pre-wait tests passed");
    return 0;
}
