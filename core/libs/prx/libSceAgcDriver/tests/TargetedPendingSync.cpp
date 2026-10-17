#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

bool Targeted() {
    return std::getenv("APS5_NO_TARGETED_PENDING_SYNC") == nullptr;
}

struct MockDevice {
    std::uintptr_t next = 1;
    std::map<VkFence, bool> signaled;
    std::vector<VkFence> submitted;
    std::uint64_t submits = 0;
    std::uint64_t fenceWaits = 0;
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
    // One queue in submission order: a fence signals only once every earlier submission finished,
    // so a wait on the newest batch covers the older ones.
    ++mock.fenceWaits;
    for (std::uint32_t i = 0; i < count; ++i) {
        for (const auto fence : mock.submitted) {
            mock.signaled[fence] = true;
            if (fence == fences[i]) break;
        }
    }
    return VK_SUCCESS;
}

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

// One batch: notes `bytes` at `offset` and, when it finishes, stores `value` there (the write-back a
// copied buffer's completion makes).
void Record(Recorder& recorder, Guest& guest, std::size_t offset, std::size_t bytes, std::uint8_t value, std::vector<int>* order, int id) {
    recorder.NotePendingWrite(guest.address(offset), bytes);
    recorder.OnComplete([&guest, offset, value, order, id] {
        guest.memory[offset] = static_cast<std::byte>(value);
        if (order != nullptr) order->push_back(id);
    });
}

void OlderBatchIsTheOnlyOneWaitedFor() {
    mock = MockDevice{};
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    Guest guest;
    std::vector<int> order;
    {
        Recorder recorder(mockContext());
        Record(recorder, guest, 0, 256, 0xa1, &order, 1);
        recorder.Submit();
        Record(recorder, guest, 512, 256, 0xb2, &order, 2);
        recorder.Submit();
        Expect(recorder.InFlightBatches() == 2 && mock.submits == 2, "the two recorded batches are not both in flight");
        recorder.SyncThrough(guest.address(0), 256);
        Expect(mock.fenceWaits == 1, "a sync targeting the older batch waited for " + std::to_string(mock.fenceWaits) + " fences, not its one");
        Expect(recorder.InFlightBatches() == 1, "a sync targeting the older batch retired " + std::to_string(2 - recorder.InFlightBatches()) + " batches, not one");
        Expect(!mock.signaled.at(mock.submitted[1]), "a sync for the older batch returned only after the newer one completed");
        Expect(guest.byte(0) == 0xa1 && guest.byte(512) == 0x00, "the accessed range did not hold the finished batch's bytes and the untouched one stayed unwritten");
        Expect(order == std::vector<int>{1}, "the write-back of the batch the access waited for did not run, or a later one did");
        Expect(!recorder.PendingWriteOverlaps(guest.address(0), 16) && recorder.PendingWriteOverlaps(guest.address(512), 16),
            "the waited batch's range stayed pending or the untouched batch's left the pending writes");
        recorder.Sync();
    }
}

void OpenBatchTargetWaitsForOneFence() {
    mock = MockDevice{};
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    Guest guest;
    std::vector<int> order;
    {
        Recorder recorder(mockContext());
        Record(recorder, guest, 0, 256, 0xa1, &order, 1);
        recorder.Submit();
        Record(recorder, guest, 512, 256, 0xb2, &order, 2);
        Expect(recorder.Recording(), "the second batch did not stay open");
        recorder.SyncThrough(guest.address(512), 256);
        Expect(mock.submits == 2, "the open batch holding the write was not submitted");
        Expect(mock.fenceWaits == (Targeted() ? 1u : 2u), "a sync for the open batch waited for " + std::to_string(mock.fenceWaits) + " fences");
        Expect(recorder.InFlightBatches() == 0, "a sync for the open batch left a batch in flight");
        Expect(order == (std::vector<int>{1, 2}), "the batches ahead of the target did not run their completions in submission order");
        Expect(guest.byte(0) == 0xa1 && guest.byte(512) == 0xb2, "the write-backs did not land in submission order");
        recorder.Sync();
    }
}

void OlderBatchesRunAfterTheTargetsFence() {
    mock = MockDevice{};
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    Guest guest;
    std::vector<int> order;
    {
        Recorder recorder(mockContext());
        for (int batch = 1; batch <= 3; ++batch) {
            Record(recorder, guest, static_cast<std::size_t>(batch - 1) * 256, 256, static_cast<std::uint8_t>(0x10 + batch), &order, batch);
            recorder.Submit();
        }
        recorder.SyncThrough(guest.address(512), 256);
        Expect(mock.fenceWaits == (Targeted() ? 1u : 3u), "a sync for the newest of three batches waited for " + std::to_string(mock.fenceWaits) + " fences");
        Expect(recorder.InFlightBatches() == 0, "a sync for the newest of three batches left a batch in flight");
        Expect(order == (std::vector<int>{1, 2, 3}), "the batches ahead of the target did not run their completions in submission order");
        Expect(guest.byte(0) == 0x11 && guest.byte(256) == 0x12 && guest.byte(512) == 0x13, "the write-backs did not land in submission order");
        Expect(!recorder.PendingWriteOverlaps(guest.address(0), 1024), "a synced range still reports a pending write");
        recorder.Sync();
    }
}

void NothingOverlappingWaitsForNothing() {
    mock = MockDevice{};
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    Guest guest;
    {
        Recorder recorder(mockContext());
        Record(recorder, guest, 0, 256, 0xa1, nullptr, 1);
        recorder.Submit();
        recorder.SyncThrough(guest.address(512), 256);
        Expect(mock.fenceWaits == 0 && recorder.InFlightBatches() == 1, "an access overlapping no pending write waited for " + std::to_string(mock.fenceWaits) + " batches");
        recorder.Sync();
    }
}

}

int main() {
    try {
        OlderBatchIsTheOnlyOneWaitedFor();
        OpenBatchTargetWaitsForOneFence();
        OlderBatchesRunAfterTheTargetsFence();
        NothingOverlappingWaitsForNothing();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    if (failures != 0) {
        std::fprintf(stderr, "%d targeted pending sync checks failed\n", failures);
        return 1;
    }
    std::printf("targeted pending sync tests passed\n");
    return 0;
}