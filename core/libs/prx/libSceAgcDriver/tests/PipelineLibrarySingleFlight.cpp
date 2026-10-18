#include "prx/libSceAgcDriver/Graphics/include/PipelineLibrary.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

using namespace AgcDriver::Graphics;

namespace {

using Blob = std::vector<std::byte>;

std::mutex mockMutex;
std::condition_variable mockCondition;
unsigned partCreates = 0;
unsigned linkCreates = 0;
bool blockParts = false;
bool releaseParts = false;
VkResult partResult = VK_SUCCESS;
std::uintptr_t nextHandle = 1;
unsigned livePipelines = 0;
unsigned liveLayouts = 0;
std::vector<std::vector<VkPipeline>> linkLibraries;

VKAPI_ATTR void VKAPI_CALL mockDestroyPipeline(VkDevice, VkPipeline pipeline, const VkAllocationCallbacks*) {
    if (pipeline == VK_NULL_HANDLE) return;
    std::lock_guard lock(mockMutex);
    --livePipelines;
}

VKAPI_ATTR void VKAPI_CALL mockDestroyPipelineLayout(VkDevice, VkPipelineLayout layout, const VkAllocationCallbacks*) {
    if (layout == VK_NULL_HANDLE) return;
    std::lock_guard lock(mockMutex);
    --liveLayouts;
}

VKAPI_ATTR void VKAPI_CALL mockDestroyShaderModule(VkDevice, VkShaderModule, const VkAllocationCallbacks*) {}

VKAPI_ATTR VkResult VKAPI_CALL mockCreatePipelineLayout(VkDevice, const VkPipelineLayoutCreateInfo*, const VkAllocationCallbacks*, VkPipelineLayout* layout) {
    std::lock_guard lock(mockMutex);
    ++liveLayouts;
    *layout = reinterpret_cast<VkPipelineLayout>(nextHandle++);
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL mockCreateGraphicsPipelines(VkDevice, VkPipelineCache, std::uint32_t count, const VkGraphicsPipelineCreateInfo* infos, const VkAllocationCallbacks*, VkPipeline* pipelines) {
    const auto* next = static_cast<const VkBaseInStructure*>(infos[0].pNext);
    VkResult result = VK_SUCCESS;
    {
        std::unique_lock lock(mockMutex);
        if (next != nullptr && next->sType == VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT) {
            ++partCreates;
            mockCondition.notify_all();
            if (blockParts) mockCondition.wait(lock, [] { return releaseParts; });
            result = partResult;
        } else {
            const auto* library = static_cast<const VkPipelineLibraryCreateInfoKHR*>(static_cast<const void*>(next));
            ++linkCreates;
            std::vector<VkPipeline> linked;
            for (std::uint32_t index = 0; index < library->libraryCount; ++index) linked.push_back(library->pLibraries[index]);
            linkLibraries.push_back(std::move(linked));
        }
    }
    if (result != VK_SUCCESS) return result;
    for (std::uint32_t index = 0; index < count; ++index) {
        std::lock_guard lock(mockMutex);
        ++livePipelines;
        pipelines[index] = reinterpret_cast<VkPipeline>(nextHandle++);
    }
    return VK_SUCCESS;
}

PFN_vkVoidFunction VKAPI_CALL mockProc(VkDevice, const char* name) {
    if (std::strcmp(name, "vkCreatePipelineLayout") == 0) return reinterpret_cast<PFN_vkVoidFunction>(mockCreatePipelineLayout);
    if (std::strcmp(name, "vkCreateGraphicsPipelines") == 0) return reinterpret_cast<PFN_vkVoidFunction>(mockCreateGraphicsPipelines);
    if (std::strcmp(name, "vkDestroyPipeline") == 0) return reinterpret_cast<PFN_vkVoidFunction>(mockDestroyPipeline);
    if (std::strcmp(name, "vkDestroyPipelineLayout") == 0) return reinterpret_cast<PFN_vkVoidFunction>(mockDestroyPipelineLayout);
    if (std::strcmp(name, "vkDestroyShaderModule") == 0) return reinterpret_cast<PFN_vkVoidFunction>(mockDestroyShaderModule);
    return nullptr;
}

void resetMock() {
    std::lock_guard lock(mockMutex);
    partCreates = 0;
    linkCreates = 0;
    blockParts = false;
    releaseParts = false;
    partResult = VK_SUCCESS;
    linkLibraries.clear();
}

unsigned partCreateCount() {
    std::lock_guard lock(mockMutex);
    return partCreates;
}

unsigned linkCreateCount() {
    std::lock_guard lock(mockMutex);
    return linkCreates;
}

unsigned livePipelineCount() {
    std::lock_guard lock(mockMutex);
    return livePipelines;
}

unsigned liveLayoutCount() {
    std::lock_guard lock(mockMutex);
    return liveLayouts;
}

std::vector<std::vector<VkPipeline>> linkedLibraries() {
    std::lock_guard lock(mockMutex);
    return linkLibraries;
}

void setBlockParts(bool block) {
    std::lock_guard lock(mockMutex);
    blockParts = block;
}

void setPartResult(VkResult result) {
    std::lock_guard lock(mockMutex);
    partResult = result;
}

bool waitForPartCreates(unsigned count, std::chrono::milliseconds timeout) {
    std::unique_lock lock(mockMutex);
    return mockCondition.wait_for(lock, timeout, [&] { return partCreates >= count; });
}

void releaseParts() {
    {
        std::lock_guard lock(mockMutex);
        releaseParts = true;
    }
    mockCondition.notify_all();
}

Blob blob(std::uint32_t seed, std::size_t size) {
    Blob value(size);
    for (std::size_t index = 0; index < size; ++index) value[index] = static_cast<std::byte>((seed * 31u + static_cast<std::uint32_t>(index)) & 0xffu);
    return value;
}

struct Request {
    Context context{};
    std::vector<VkPipelineShaderStageCreateInfo> stages;
    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_ATTACHMENT_STATE_CREATE_INFO};
    VkPipelineColorBlendStateCreateInfo colorBlend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    VkPipelineRenderingCreateInfoKHR rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    PipelineLibraryKeys keys;
};

void makeRequest(Request& request, VkDevice device, std::uint32_t seed) {
    request.context.device = device;
    request.context.deviceProc = mockProc;
    request.context.pipelineLinkTimeOptimization = false;
    request.assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    request.raster.lineWidth = 1.0f;
    request.samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    request.blend.colorWriteMask = 0xfu;
    request.colorBlend.attachmentCount = 1;
    request.colorBlend.pAttachments = &request.blend;
    request.stages = {
        VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, reinterpret_cast<VkShaderModule>(static_cast<std::uintptr_t>(0x100)), "main", nullptr},
        VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, reinterpret_cast<VkShaderModule>(static_cast<std::uintptr_t>(0x200)), "main", nullptr},
    };
    request.info.stageCount = static_cast<std::uint32_t>(request.stages.size());
    request.info.pStages = request.stages.data();
    request.info.pVertexInputState = &request.vertexInput;
    request.info.pInputAssemblyState = &request.assembly;
    request.info.pViewportState = &request.viewport;
    request.info.pRasterizationState = &request.raster;
    request.info.pMultisampleState = &request.samples;
    request.info.pColorBlendState = &request.colorBlend;
    request.info.pDynamicState = PipelineLibraryDynamicStates().data();
    request.rendering.colorAttachmentCount = 1;
    request.rendering.pColorAttachmentFormats = &request.format;
    request.keys.vertexInput = blob(seed, 16);
    request.keys.preRasterization = blob(seed + 1u, 32);
    request.keys.fragmentShader = blob(seed + 2u, 48);
    request.keys.fragmentOutput = blob(seed + 3u, 8);
    request.keys.renderPass = blob(seed + 4u, 4);
    request.keys.layout = blob(seed + 5u, 4);
    request.keys.preRasterizationLayout = blob(seed + 6u, 4);
    request.keys.fragmentLayout = blob(seed + 7u, 4);
}

struct Outcome {
    std::atomic<bool> started{false};
    std::atomic<bool> finished{false};
    std::atomic<bool> threw{false};
    VkPipeline pipeline = VK_NULL_HANDLE;
};

void run(Outcome& outcome, const Request& request) {
    outcome.started.store(true);
    try {
        outcome.pipeline = LinkPipelineFromLibraries(request.context, request.info, request.rendering, request.layout, request.keys);
    } catch (...) {
        outcome.threw.store(true);
    }
    outcome.finished.store(true);
}

void waitStarted(Outcome& outcome) {
    while (!outcome.started.load()) std::this_thread::yield();
}

void destroyLinked(const Request& request, VkPipeline pipeline) {
    request.context.Function<PFN_vkDestroyPipeline>("vkDestroyPipeline")(request.context.device, pipeline, nullptr);
}

void sameKeyBuildsOnce() {
    resetMock();
    const auto device = reinterpret_cast<VkDevice>(static_cast<std::uintptr_t>(0x1010));
    Request request;
    makeRequest(request, device, 1);
    setBlockParts(true);
    Outcome first;
    Outcome second;
    std::thread build([&] { run(first, request); });
    Require(waitForPartCreates(1, std::chrono::seconds(5)), "the first request never reached a pipeline creation");
    std::thread share([&] { run(second, request); });
    waitStarted(second);
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    const auto during = partCreateCount();
    releaseParts();
    build.join();
    share.join();
    Require(!first.threw.load() && !second.threw.load(), "a concurrent library build threw");
    Require(first.pipeline != VK_NULL_HANDLE && second.pipeline != VK_NULL_HANDLE, "a concurrent library build returned no pipeline");
    Require(during == 1, "a second request for a part another thread was building started its own creation");
    Require(partCreateCount() == 4, "two requests for the same pipeline did not build one library per part");
    Require(linkCreateCount() == 2, "the two requests did not each link their parts");
    const auto libraries = linkedLibraries();
    Require(libraries.size() == 2 && libraries[0] == libraries[1] && libraries[0].size() == 4, "the two requests linked different library parts");
    const auto counters = PipelineLibraryCountersOf(device);
    Require(counters.built == std::array<std::uint64_t, 4>{1, 1, 1, 1}, "a part was built more than once for two requests");
    Require(counters.shared == std::array<std::uint64_t, 4>{1, 1, 1, 1}, "the second request did not take every part from the first");
    Require(counters.waited == 4, "the second request did not wait for all four parts");
    Require(counters.discarded == 0 && counters.failed == 0, "a concurrent request reported a discarded or failed build");
    destroyLinked(request, first.pipeline);
    destroyLinked(request, second.pipeline);
    ClearPipelineLibraries(device);
    Require(livePipelineCount() == 0 && liveLayoutCount() == 0, "clearing the libraries of a concurrent build leaked Vulkan objects");
}

void differentKeysStayConcurrent() {
    resetMock();
    const auto device = reinterpret_cast<VkDevice>(static_cast<std::uintptr_t>(0x2020));
    Request first;
    Request second;
    makeRequest(first, device, 10);
    makeRequest(second, device, 20);
    setBlockParts(true);
    Outcome build;
    Outcome other;
    std::thread buildThread([&] { run(build, first); });
    Require(waitForPartCreates(1, std::chrono::seconds(5)), "the first request never reached a pipeline creation");
    std::thread otherThread([&] { run(other, second); });
    waitStarted(other);
    const bool overlapped = waitForPartCreates(2, std::chrono::seconds(5));
    releaseParts();
    buildThread.join();
    otherThread.join();
    Require(!build.threw.load() && !other.threw.load(), "a library build for another key threw");
    Require(overlapped, "a build of another key waited behind an in-flight part");
    Require(partCreateCount() == 8, "two requests for different keys did not build eight parts");
    const auto counters = PipelineLibraryCountersOf(device);
    Require(counters.built == std::array<std::uint64_t, 4>{2, 2, 2, 2}, "a request for another key reused or dropped a part");
    Require(counters.shared == std::array<std::uint64_t, 4>{0, 0, 0, 0} && counters.discarded == 0, "a request for another key took a part built for this one");
    destroyLinked(first, build.pipeline);
    destroyLinked(first, other.pipeline);
    ClearPipelineLibraries(device);
    Require(livePipelineCount() == 0 && liveLayoutCount() == 0, "clearing the libraries of two keys leaked Vulkan objects");
}

void failureReleasesWaiters() {
    resetMock();
    const auto device = reinterpret_cast<VkDevice>(static_cast<std::uintptr_t>(0x3030));
    Request request;
    makeRequest(request, device, 30);
    setPartResult(VK_ERROR_INITIALIZATION_FAILED);
    setBlockParts(true);
    Outcome build;
    Outcome waiting;
    std::thread buildThread([&] { run(build, request); });
    Require(waitForPartCreates(1, std::chrono::seconds(5)), "the failing request never reached a pipeline creation");
    std::thread waitThread([&] { run(waiting, request); });
    waitStarted(waiting);
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    releaseParts();
    buildThread.join();
    waitThread.join();
    Require(build.threw.load(), "a failed library creation reported success");
    Require(waiting.finished.load(), "a waiter was left behind by a failed library creation");
    Require(waiting.threw.load(), "a waiter took a failed library creation's place as a success");
    const auto counters = PipelineLibraryCountersOf(device);
    Require(counters.failed == 1, "a failed library creation was not counted once");
    Require(counters.built == std::array<std::uint64_t, 4>{0, 0, 0, 0} && counters.discarded == 0, "a failed library creation published a part");
    ClearPipelineLibraries(device);
    Require(livePipelineCount() == 0 && liveLayoutCount() == 0, "clearing the libraries after a failed build leaked Vulkan objects");
}

}

int main() {
    sameKeyBuildsOnce();
    differentKeysStayConcurrent();
    failureReleasesWaiters();
    std::cout << "Pipeline library single-flight tests passed\n";
    return 0;
}
