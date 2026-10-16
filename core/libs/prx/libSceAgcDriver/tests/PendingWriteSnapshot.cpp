#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <map>
#include <memory>
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

struct MockDevice {
    std::uintptr_t next = 1;
    std::map<VkFence, bool> signaled;
    std::uint64_t submits = 0;
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

VKAPI_ATTR VkResult VKAPI_CALL mockWaitForFences(VkDevice, std::uint32_t count, const VkFence* fences, VkBool32, std::uint64_t) {
    for (std::uint32_t i = 0; i < count; ++i) mock.signaled.at(fences[i]) = true;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL mockResetFences(VkDevice, std::uint32_t count, const VkFence* fences) {
    for (std::uint32_t i = 0; i < count; ++i) mock.signaled.at(fences[i]) = false;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL mockBeginCommandBuffer(VkCommandBuffer, const VkCommandBufferBeginInfo*) {
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL mockEndCommandBuffer(VkCommandBuffer) {
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL mockQueueSubmit(VkQueue, std::uint32_t, const VkSubmitInfo*, VkFence) {
    ++mock.submits;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL mockDeviceWaitIdle(VkDevice) {
    for (auto& [fence, signaled] : mock.signaled) signaled = true;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL mockCmdUpdateBuffer(VkCommandBuffer, VkBuffer, VkDeviceSize, VkDeviceSize, const void*) {}
VKAPI_ATTR void VKAPI_CALL mockCmdPipelineBarrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags, std::uint32_t, const VkMemoryBarrier*, std::uint32_t, const VkBufferMemoryBarrier*, std::uint32_t, const VkImageMemoryBarrier*) {}
VKAPI_ATTR void VKAPI_CALL mockCmdBeginQuery(VkCommandBuffer, VkQueryPool, std::uint32_t, VkQueryControlFlags) {}
VKAPI_ATTR void VKAPI_CALL mockCmdEndQuery(VkCommandBuffer, VkQueryPool, std::uint32_t) {}
VKAPI_ATTR void VKAPI_CALL mockCmdResetQueryPool(VkCommandBuffer, VkQueryPool, std::uint32_t, std::uint32_t) {}
VKAPI_ATTR void VKAPI_CALL mockCmdCopyQueryPoolResults(VkCommandBuffer, VkQueryPool, std::uint32_t, std::uint32_t, VkBuffer, VkDeviceSize, VkDeviceSize, VkQueryResultFlags) {}
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

void DisjointRangesAreAnsweredByTheSnapshot() {
    mock = MockDevice{};
    const auto filter = Recorder::PendingWriteSnapshotFilter();
    const auto tests = Recorder::PendingWriteTests();
    const auto answers = Recorder::PendingWriteSnapshotAnswers();
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    alignas(64) std::array<std::byte, 4096> memory{};
    const auto base = reinterpret_cast<std::uint64_t>(memory.data());
    {
        Recorder recorder(mockContext());
        recorder.Activate();
        for (std::uint64_t offset = 0; offset < 2048; offset += 64) recorder.NotePendingWrite(base + offset, 64);
        Expect(Recorder::SnapshotWriteOverlaps(base, 2048), "the noted range left the published pending-write snapshot");
        Expect(!recorder.PendingWriteOverlaps(base + 2048, 64), "a range past the noted one is reported as a pending write");
        Expect(recorder.PendingWriteOverlaps(base, 64), "the first noted range is not reported as a pending write");
        Expect(recorder.PendingWriteOverlaps(base + 2047, 2), "the last byte of the noted range is not reported as a pending write");
        Expect(!recorder.PendingWriteOverlaps(base + 4096, 64), "a range past the memory is reported as a pending write");
        recorder.Sync();
    }
    const auto grew = Recorder::PendingWriteSnapshotAnswers() > answers;
    Expect(grew == filter, filter ? "no pending-write test was answered by the published snapshot" : "a pending-write test was answered by the snapshot with the filter off");
    Expect(Recorder::PendingWriteTests() > tests, "the pending-write tests were not counted");
}

void SubmittedAndSyncedRangesStayFound() {
    mock = MockDevice{};
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    alignas(64) std::array<std::byte, 8192> memory{};
    const auto base = reinterpret_cast<std::uint64_t>(memory.data());
    {
        Recorder recorder(mockContext());
        recorder.Activate();
        recorder.NotePendingWrite(base, 4096);
        recorder.Submit();
        Expect(recorder.InFlightBatches() == 1, "the noted range left no batch in flight");
        recorder.NotePendingWrite(base + 4096, 4096);
        Expect(recorder.PendingWriteOverlaps(base, 16), "an in-flight batch's range is not reported as a pending write");
        Expect(recorder.PendingWriteOverlaps(base + 4096, 16), "the open batch's range is not reported as a pending write");
        Expect(!recorder.PendingWriteOverlaps(base + 8192, 16), "a range outside both batches is reported as a pending write");
        recorder.Sync();
        Expect(!recorder.PendingWriteOverlaps(base, 8192), "a synced recorder still reports a pending write");
    }
}

}

int main() {
    try {
        DisjointRangesAreAnsweredByTheSnapshot();
        SubmittedAndSyncedRangesStayFound();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    if (failures != 0) {
        std::fprintf(stderr, "%d pending write snapshot checks failed\n", failures);
        return 1;
    }
    std::printf("pending write snapshot tests passed\n");
    return 0;
}
