#include "UnlockedCommitWaitMock.hpp"

namespace {

// A commit's own sync under the outermost hold: two submitted batches, the draw's write-backs in
// their completion actions. What a profile run must show is the wait leaving the GpuMutex, with the
// completions still running under it.
void CommitSyncLeavesTheMutex() {
    mock = MockDevice{};
    const bool unlocked = Recorder::UnlockedCommitWaits();
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    Expect(AgcDriver::GuestMemory::GpuMutex().DepthOnThisThread() == 1, "the commit's hold is not the outermost acquisition on its thread");
    Guest guest;
    std::vector<int> order;
    {
        Recorder recorder(mockContext(), true);
        Expect(recorder.HasTimeline(), "the mock device made no timeline semaphore for the recorder");
        recorder.Activate();
        Record(recorder, guest, 0, 256, 0xa1, &order, 1);
        recorder.Submit();
        Record(recorder, guest, 512, 256, 0xb2, &order, 2);
        recorder.Submit();
        Expect(recorder.InFlightBatches() == 2 && mock.submits == 2, "the two recorded batches are not both in flight");
        recorder.Sync(Recorder::UnlockedCommitWaits());
        Expect(mock.semaphoreWaits == (unlocked ? 1u : 0u), "the commit's sync made " + std::to_string(mock.semaphoreWaits) + " timeline waits, not " + (unlocked ? "1" : "0")
            + (unlocked ? "" : " (APS5_NO_UNLOCKED_COMMIT_WAITS keeps the wait under the mutex)"));
        Expect(mock.lockedWaits == (unlocked ? 0u : 2u), "a GPU wait of the draw commit ran while the thread held the GpuMutex");
        Expect(order == std::vector<int>{1, 2}, "the write-backs did not run in submission order");
        Expect(guest.byte(0) == 0xa1 && guest.byte(512) == 0xb2, "the write-backs did not land in submission order");
        Expect(recorder.InFlightBatches() == 0, "the commit's sync left a batch in flight");
        Expect(!recorder.PendingWriteOverlaps(guest.address(0), 1024), "a synced range still reports a pending write");
        recorder.Sync();
    }
    if (!Profiled()) return;
    const auto stats = Recorder::CommitUnlockedWaits();
    Expect(stats.waits == (unlocked ? 1u : 0u), "the profile counted " + std::to_string(stats.waits) + " commit waits moved outside the lock, not " + (unlocked ? "1" : "0"));
}

// A hook nested inside the commit's hold (its own acquisition) must not give the hold up: only the
// outermost acquisition may.
void NestedHookKeepsTheMutex() {
    mock = MockDevice{};
    std::lock_guard outer(AgcDriver::GuestMemory::GpuMutex());
    Guest guest;
    std::vector<int> order;
    {
        Recorder recorder(mockContext(), true);
        recorder.Activate();
        Record(recorder, guest, 0, 256, 0xa1, &order, 1);
        recorder.Submit();
        Record(recorder, guest, 512, 256, 0xb2, &order, 2);
        recorder.Submit();
        {
            std::lock_guard inner(AgcDriver::GuestMemory::GpuMutex());
            recorder.Sync(Recorder::UnlockedCommitWaits());
        }
        Expect(mock.semaphoreWaits == 0, "a nested acquisition waited with the GpuMutex released, giving up the caller's hold");
        Expect(mock.fenceWaits == 2, "the locked fallback waited for " + std::to_string(mock.fenceWaits) + " fences, not one per batch");
        Expect(mock.lockedWaits == 2, "the locked fallback did not wait with the GpuMutex held");
        Expect(order == std::vector<int>{1, 2}, "the write-backs did not run in submission order");
        Expect(guest.byte(0) == 0xa1 && guest.byte(512) == 0xb2, "the write-backs did not land in submission order");
        Expect(recorder.InFlightBatches() == 0, "the locked sync left a batch in flight");
    }
}

// The same for a targeted sync: the draw commit's read of a range a batch will write.
void CommitReadLeavesTheMutex() {
    mock = MockDevice{};
    const bool unlocked = Recorder::UnlockedCommitWaits();
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    Guest guest;
    std::vector<int> order;
    {
        Recorder recorder(mockContext(), true);
        recorder.Activate();
        Record(recorder, guest, 0, 256, 0xa1, &order, 1);
        recorder.Submit();
        Record(recorder, guest, 512, 256, 0xb2, &order, 2);
        recorder.Submit();
        recorder.SyncThrough(guest.address(512), 256, Recorder::UnlockedCommitWaits());
        Expect(mock.semaphoreWaits == (unlocked ? 1u : 0u), "the commit's read made " + std::to_string(mock.semaphoreWaits) + " timeline waits, not " + (unlocked ? "1" : "0"));
        Expect(mock.lockedWaits == (unlocked ? 0u : 1u), "a GPU wait of the draw commit ran while the thread held the GpuMutex");
        Expect(mock.fenceWaits == (unlocked ? 0u : 1u), "the commit's read waited on " + std::to_string(mock.fenceWaits) + " batch fences, not " + (unlocked ? "none" : "the target's"));
        Expect(order == std::vector<int>{1, 2}, "the write-backs did not run in submission order");
        Expect(guest.byte(0) == 0xa1 && guest.byte(512) == 0xb2, "the write-backs did not land in submission order");
        Expect(recorder.InFlightBatches() == 0, "the commit's read left a batch in flight");
        recorder.Sync();
    }
}

// A device without timeline semaphores cannot wait unlocked: the locked path must still be made,
// with the same result.
void WithoutTimelineTheWaitStaysLocked() {
    mock = MockDevice{};
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    Guest guest;
    std::vector<int> order;
    {
        Recorder recorder(mockContext(), false);
        Expect(!recorder.HasTimeline() && mock.semaphores == 0, "a recorder without timeline semaphores created one");
        recorder.Activate();
        Record(recorder, guest, 0, 256, 0xa1, &order, 1);
        recorder.Submit();
        Record(recorder, guest, 512, 256, 0xb2, &order, 2);
        recorder.Submit();
        recorder.Sync(Recorder::UnlockedCommitWaits());
        Expect(mock.semaphoreWaits == 0 && mock.fenceWaits == 2, "a device without a timeline did not fall back to the locked wait");
        Expect(mock.lockedWaits == 2, "the locked fallback did not wait with the GpuMutex held");
        Expect(order == std::vector<int>{1, 2}, "the write-backs did not run in submission order");
        Expect(guest.byte(0) == 0xa1 && guest.byte(512) == 0xb2, "the write-backs did not land in submission order");
        Expect(recorder.InFlightBatches() == 0, "the locked sync left a batch in flight");
    }
}

}

int main() {
    try {
        CommitSyncLeavesTheMutex();
        NestedHookKeepsTheMutex();
        CommitReadLeavesTheMutex();
        WithoutTimelineTheWaitStaysLocked();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    if (failures != 0) {
        std::fprintf(stderr, "%d unlocked commit wait checks failed\n", failures);
        return 1;
    }
    std::printf("unlocked commit wait tests passed\n");
    return 0;
}