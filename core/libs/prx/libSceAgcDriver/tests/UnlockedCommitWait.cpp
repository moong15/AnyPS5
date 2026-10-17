#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

using namespace AgcDriver::Graphics;

namespace {

int failures = 0;

void Expect(bool condition, const std::string& what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++failures;
}

bool Profiled() {
    return std::getenv("APS5_PROFILE_DRAW") != nullptr;
}

struct MockDevice {
    std::uintptr_t next = 1;
    std::map<VkFence, bool> signaled;
    std::vector<VkFence> submitted;
    std::uint64_t timeline = 0;
    std::uint64_t submits = 0;
    std::uint64_t fenceWaits = 0;
    std::uint64_t semaphoreWaits = 0;
    std::uint64_t lockedWaits = 0;
    std::uint64_t semaphores = 0;
};

MockDevice mock;

VKAPI_ATTR VkResult VKAPI_CALL mockAllocateCommandBuffers(VkDevice, const VkCommandBufferAllocateInfo*, VkCommandBuffer* commands) {
    *commands = reinterpret_cast<VkCommandBuffer>(mock.next++);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL mockFreeCommandBuffers(VkDevice, VkCommandPool, std::uint32_t, const VkCommandBuffer*) {}

VKAPI_ATTR VkResult VKAPI_CALL mockCreateFence(VkDevice, const VkFenceCreateInfo*, const VkAllocationCallbacks*, VkFence* fence) {
    *fence = reinterpret_cast<VkFence>(mock.next++);
    mock.signaled[*fence] = false;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL mockDestroyFence(VkDevice, VkFence fence, const VkAllocationCallbacks*) {
    mock.signaled.erase(fence);
}

VKAPI_ATTR VkResult VKAPI_CALL mockGetFenceStatus(VkDevice, VkFence fence) {
    return mock.signaled.at(fence) ? VK_SUCCESS : VK_NOT_READY;
}

// One queue in submission order: a fence signals only once every earlier submission finished, so a
// wait on the newest batch covers the older ones, and the timeline follows the same order.
void CompleteThrough(VkFence fence) {
    for (const auto submitted : mock.submitted) {
        mock.signaled[submitted] = true;
        ++mock.timeline;
        if (submitted == fence) break;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL mockWaitForFences(VkDevice, std::uint32_t count, const VkFence* fences, VkBool32, std::uint64_t) {
    ++mock.fenceWaits;
    if (AgcDriver::GuestMemory::GpuMutex().HeldByThisThread()) ++mock.lockedWaits;
    for (std::uint32_t i = 0; i < count; ++i) CompleteThrough(fences[i]);
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL mockWaitSemaphores(VkDevice, const VkSemaphoreWaitInfoKHR* wait, std::uint64_t) {
    ++mock.semaphoreWaits;
    if (AgcDriver::GuestMemory::GpuMutex().HeldByThisThread()) ++mock.lockedWaits;
    for (std::uint32_t i = 0; i < wait->semaphoreCount; ++i) {
        while (mock.timeline < wait->pValues[i] && mock.timeline < mock.submitted.size()) CompleteThrough(mock.submitted[static_cast<std::size_t>(mock.timeline)]);
    }
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL mockCreateSemaphore(VkDevice, const VkSemaphoreCreateInfo*, const VkAllocationCallbacks*, VkSemaphore* semaphore) {
    *semaphore = reinterpret_cast<VkSemaphore>(mock.next++);
    ++mock.semaphores;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL mockDestroySemaphore(VkDevice, VkSemaphore, const VkAllocationCallbacks*) {}

VKAPI_ATTR VkResult VKAPI_CALL mockResetFences(VkDevice, std::uint32_t count, const VkFence* fences) {
    for (std::uint32_t i = 0; i < count; ++i) mock.signaled[fences[i]] = false;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL mockBeginCommandBuffer(VkCommandBuffer, const VkCommandBufferBeginInfo*) {
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL mockEndCommandBuffer(VkCommandBuffer) {
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL mockQueueSubmit(VkQueue, std::uint32_t, const VkSubmitInfo*, VkFence fence) {
    ++mock.submits;
    mock.submitted.push_back(fence);
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL mockDeviceWaitIdle(VkDevice) {
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL mockCmdUpdateBuffer(VkCommandBuffer, VkBuffer, VkDeviceSize, VkDeviceSize, const void*) {}
VKAPI_ATTR void VKAPI_CALL mockCmdPipelineBarrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags, std::uint32_t, const VkMemoryBarrier*, std::uint32_t, const VkBufferMemoryBarrier*, std::uint32_t, const VkImageMemoryBarrier*) {}
VKAPI_ATTR void VKAPI_CALL mockCmdBeginQuery(VkCommandBuffer, VkQueryPool, std::uint32_t, VkQueryControlFlags) {}
VKAPI_ATTR void VKAPI_CALL mockCmdEndQuery(VkCommandBuffer, VkQueryPool, std::uint32_t) {}
VKAPI_ATTR void VKAPI_CALL mockCmdResetQueryPool(VkCommandBuffer, VkQueryPool, std::uint32_t) {}
VKAPI_ATTR void VKAPI_CALL mockCmdCopyQueryPoolResults(VkCommandBuffer, VkQueryPool, std::uint32_t, VkQueryResultFlags, VkBuffer, VkDeviceSize, VkDeviceSize) {}
VKAPI_ATTR void VKAPI_CALL mockCmdBindPipeline(VkCommandBuffer, VkPipelineBindPoint, VkPipeline) {}
VKAPI_ATTR void VKAPI_CALL mockCmdPushConstants(VkCommandBuffer, VkPipelineLayout, VkShaderStageFlags, std::uint32_t, std::uint32_t, const void*) {}
VKAPI_ATTR void VKAPI_CALL mockCmdDispatch(VkCommandBuffer, std::uint32_t, std::uint32_t, std::uint32_t) {}

PFN_vkVoidFunction VKAPI_CALL mockProc(VkDevice, const char* name) {
    static const std::map<std::string_view, PFN_vkVoidFunction> table{
        {"vkAllocateCommandBuffers", reinterpret_cast<PFN_vkVoidFunction>(mockAllocateCommandBuffers)},
        {"vkFreeCommandBuffers", reinterpret_cast<PFN_vkVoidFunction>(mockFreeCommandBuffers)},
        {"vkCreateFence", reinterpret_cast<PFN_vkVoidFunction>(mockCreateFence)},
        {"vkDestroyFence", reinterpret_cast<PFN_vkVoidFunction>(mockDestroyFence)},
        {"vkGetFenceStatus", reinterpret_cast<PFN_vkVoidFunction>(mockGetFenceStatus)},
        {"vkWaitForFences", reinterpret_cast<PFN_vkVoidFunction>(mockWaitForFences)},
        {"vkWaitSemaphoresKHR", reinterpret_cast<PFN_vkVoidFunction>(mockWaitSemaphores)},
        {"vkCreateSemaphore", reinterpret_cast<PFN_vkVoidFunction>(mockCreateSemaphore)},
        {"vkDestroySemaphore", reinterpret_cast<PFN_vkVoidFunction>(mockDestroySemaphore)},
        {"vkResetFences", reinterpret_cast<PFN_vkVoidFunction>(mockResetFences)},
        {"vkBeginCommandBuffer", reinterpret_cast<PFN_vkVoidFunction>(mockBeginCommandBuffer)},
        {"vkEndCommandBuffer", reinterpret_cast<PFN_vkVoidFunction>(mockEndCommandBuffer)},
        {"vkQueueSubmit", reinterpret_cast<PFN_vkVoidFunction>(mockQueueSubmit)},
        {"vkDeviceWaitIdle", reinterpret_cast<PFN_vkVoidFunction>(mockDeviceWaitIdle)},
        {"vkCmdUpdateBuffer", reinterpret_cast<PFN_vkVoidFunction>(mockCmdUpdateBuffer)},
        {"vkCmdPipelineBarrier", reinterpret_cast<PFN_vkVoidFunction>(mockCmdPipelineBarrier)},
        {"vkCmdBeginQuery", reinterpret_cast<PFN_vkVoidFunction>(mockCmdBeginQuery)},
        {"vkCmdEndQuery", reinterpret_cast<PFN_vkVoidFunction>(mockCmdEndQuery)},
        {"vkCmdResetQueryPool", reinterpret_cast<PFN_vkVoidFunction>(mockCmdResetQueryPool)},
        {"vkCmdCopyQueryPoolResults", reinterpret_cast<PFN_vkVoidFunction>(mockCmdCopyQueryPoolResults)},
        {"vkCmdBindPipeline", reinterpret_cast<PFN_vkVoidFunction>(mockCmdBindPipeline)},
        {"vkCmdPushConstants", reinterpret_cast<PFN_vkVoidFunction>(mockCmdPushConstants)},
        {"vkCmdDispatch", reinterpret_cast<PFN_vkVoidFunction>(mockCmdDispatch)},
    };
    const auto it = table.find(name);
    return it == table.end() ? nullptr : it->second;
}

Context mockContext() {
    Context context{};
    context.device = reinterpret_cast<VkDevice>(mock.next++);
    context.queue = reinterpret_cast<VkQueue>(mock.next++);
    context.deviceProc = mockProc;
    return context;
}

struct Guest {
    alignas(64) std::array<std::byte, 1024> memory{};
    std::uint64_t address(std::size_t offset) const { return reinterpret_cast<std::uint64_t>(memory.data()) + offset; }
    std::uint8_t byte(std::size_t offset) const { return static_cast<std::uint8_t>(memory[offset]); }
};

void Record(Recorder& recorder, Guest& guest, std::size_t offset, std::size_t bytes, std::uint8_t value, std::vector<int>* order, int id) {
    recorder.NotePendingWrite(guest.address(offset), bytes);
    recorder.OnComplete([&guest, offset, value, order, id] {
        guest.memory[offset] = static_cast<std::byte>(value);
        if (order != nullptr) order->push_back(id);
    });
}

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