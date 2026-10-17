#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <map>
#include <mutex>
#include <span>
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

void CompletionWriter(bool signaled, bool submitted = true) {
    mock = MockDevice{};
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    auto context = mockContext();
    alignas(64) std::array<std::uint32_t, 16> memory{};
    const auto address = reinterpret_cast<std::uint64_t>(memory.data());
    const std::array<std::uint32_t, 1> value{0x12345678};
    bool laterCompleted = false;
    Recorder recorder(context);
    recorder.Activate();
    recorder.Commands();
    recorder.AfterCompletions(address, std::as_bytes(std::span(value)), 1, 0, false);
    recorder.NotePendingWrite(address + 32, 4);
    if (submitted) recorder.Submit();
    Expect(!CompletePendingCpuWrites(context, address + 32, 4), "a non-overlapping completion writer requested a sync");
    Expect(mock.fenceWaits == 0 && memory[0] == 0, "a non-overlapping completion writer waited or landed early");
    if (submitted) mock.signaled.at(mock.submitted.front()) = signaled;
    if (signaled) {
        recorder.NotePendingWrite(address, 4);
        recorder.OnComplete([&] { memory[0] = 0xabcdef01; laterCompleted = true; });
        recorder.Submit();
    }
    const auto waits = mock.fenceWaits;
    Expect(CompletePendingCpuWrites(context, address, 4), "the overlapping completion writer did not report progress");
    static const bool legacy = std::getenv("APS5_NO_PRECISE_CPU_WRITE_SYNC") != nullptr;
    Expect(memory[0] == (signaled && legacy ? 0xabcdef01u : value[0]), "the prepared resource cannot see the ordered writer bytes");
    if (signaled && !legacy) {
        Expect(mock.fenceWaits == waits, "an already completed writer waited for a fence");
        Expect(!laterCompleted && recorder.InFlightBatches() == 1, "an already completed writer drained later GPU work");
        Expect(recorder.PendingWriteOverlaps(address, 4), "later GPU writes lost their CPU-read hazard");
        std::array<std::uint32_t, 1> readback{};
        AgcDriver::GuestMemory::Read(address, std::as_writable_bytes(std::span(readback)));
        Expect(readback[0] == 0xabcdef01, "a CPU read saw bytes before the newer GPU writer completed");
        Expect(laterCompleted && mock.fenceWaits == waits + 1, "a CPU read did not wait for later overlapping GPU writes");
    } else {
        Expect(mock.fenceWaits > waits, "an overlapping pending writer did not sync");
        if (signaled) Expect(laterCompleted, "the legacy path did not drain the newer overlapping batch");
    }
    recorder.Sync();
}

}

int main() {
    try {
        CompletionWriter(true);
        CompletionWriter(false);
        CompletionWriter(false, false);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
