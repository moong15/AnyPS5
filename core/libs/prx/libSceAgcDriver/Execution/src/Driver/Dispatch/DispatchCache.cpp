#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Dispatch/DispatchCache.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <cstdlib>
#include <cstring>

namespace AgcDriver::DriverDetail {

bool Driver::stampValidate() {
    static const bool stamp = std::getenv("APS5_DISPATCH_STAMP_VALIDATE") != nullptr;
    return stamp;
}

bool Driver::dispatchRelocationEnabled() {
    static const bool enabled = std::getenv("APS5_NO_DISPATCH_RELOCATION") == nullptr && std::getenv("APS5_NO_DISPATCH_KEY_HYGIENE") == nullptr;
    return enabled && !stampValidate();
}

EntryCounters Driver::DispatchCacheCounters() {
    std::lock_guard cacheLock(dispatchCacheMutex);
    return entryCounters;
}

std::uint64_t Driver::dispatchAliasKey(std::uint64_t identity, std::span<const std::uint32_t> userData, const DrawRelocation& relocation) {
    if (relocation.words.empty()) return 0;
    auto key = identity;
    bool readable = true;
    for (std::uint32_t index = 0; index < userData.size(); ++index) {
        const auto found = std::find_if(relocation.words.begin(), relocation.words.end(), [&](const RelocatableUserWord& word) { return word.reg == index; });
        key ^= found != relocation.words.end() ? relocatedContentKey(*found, userData[index], readable) : userData[index];
        key *= 0x100000001b3ull;
    }
    return readable ? key : 0;
}

void Driver::learnDispatchRelocation(std::uint64_t identity, std::span<const std::uint32_t> userData, const std::shared_ptr<DispatchVariant>& variant) {
    if (!dispatchRelocationEnabled() || variant == nullptr) return;
    std::lock_guard cacheLock(dispatchCacheMutex);
    constexpr std::size_t MaxAliases = 16384;
    if (dispatchRelocations.size() >= MaxAliases) {
        dispatchRelocations.clear();
        dispatchRelocationTrials.clear();
        dispatchAliases.clear();
    }
    const auto trial = dispatchRelocationTrials.find(identity);
    if (trial != dispatchRelocationTrials.end() && trial->second.idle != 0) return;
    auto shape = std::make_shared<DrawRelocation>();
    const auto known = dispatchRelocations.find(identity);
    if (known != dispatchRelocations.end()) *shape = *known->second;
    for (std::uint32_t index = 0; index < userData.size() && index < 64; ++index) {
        const auto held = std::find_if(shape->words.begin(), shape->words.end(), [&](const RelocatableUserWord& word) { return word.reg == index; });
        if ((variant->relocatableWords >> index & 1u) == 0) {
            if (held != shape->words.end()) shape->words.erase(held);
            if (std::find(shape->banned.begin(), shape->banned.end(), index) == shape->banned.end()) shape->banned.push_back(index);
            continue;
        }
        if (held != shape->words.end() || std::find(shape->banned.begin(), shape->banned.end(), index) != shape->banned.end()) continue;
        RelocatableUserWord word{index, {}};
        const auto base = userData[index] & ~std::uint64_t {3};
        for (std::size_t run = 0; run < variant->relocationRuns.size() && run < variant->runBases.size(); ++run) {
            if (variant->runBases[run] != static_cast<std::int8_t>(index)) continue;
            word.reads.emplace_back(static_cast<std::int64_t>(variant->relocationRuns[run].first - base), static_cast<std::uint32_t>((variant->relocationRuns[run].second - variant->relocationRuns[run].first) / sizeof(std::uint32_t)));
        }
        if (!word.reads.empty()) shape->words.push_back(std::move(word));
    }
    std::sort(shape->words.begin(), shape->words.end(), [](const RelocatableUserWord& a, const RelocatableUserWord& b) { return a.reg < b.reg; });
    dispatchRelocations[identity] = shape;
    if (dispatchRelocationTrials[identity].idle != 0) return;
    const auto alias = dispatchAliasKey(identity, userData, *shape);
    if (alias == 0) return;
    if (dispatchAliases.size() >= MaxAliases) dispatchAliases.clear();
    dispatchAliases[alias] = variant;
}

bool Driver::relocateDispatch(std::uint64_t identity, std::span<const std::uint32_t> userData, std::uint64_t address, const Submission& submission, std::span<const ShaderRecompiler::MemoryRegion> memory, std::shared_ptr<DispatchVariant>& variant, std::vector<ShaderRecompiler::MemoryRegion>& captured) {
    std::shared_ptr<const DrawRelocation> shape;
    {
        std::lock_guard cacheLock(dispatchCacheMutex);
        const auto known = dispatchRelocations.find(identity);
        if (known == dispatchRelocations.end() || known->second->words.empty()) return false;
        auto& trial = dispatchRelocationTrials[identity];
        if (trial.idle != 0) {
            --trial.idle;
            return false;
        }
        shape = known->second;
    }
    const auto alias = dispatchAliasKey(identity, userData, *shape);
    std::unique_lock cacheLock(dispatchCacheMutex);
    auto& counters = entryCounters;
    ++counters.aliasLookups;
    const auto finish = [&](bool hit) {
        auto& trial = dispatchRelocationTrials[identity];
        ++trial.lookups;
        if (hit) ++trial.hits;
        if (trial.lookups >= DrawRelocationTrial::Window) {
            if (trial.hits * DrawRelocationTrial::MinimumHitShare < trial.lookups) trial.idle = DrawRelocationTrial::IdleDraws;
            trial.lookups = trial.hits = 0;
        }
        return hit;
    };
    if (alias == 0) {
        ++counters.aliasUnreadable;
        return finish(false);
    }
    const auto found = dispatchAliases.find(alias);
    const auto donor = found != dispatchAliases.end() ? found->second.lock() : nullptr;
    if (donor == nullptr || donor->compiled == nullptr) return finish(false);
    if (donor->userData.size() != userData.size() || donor->runBases.size() != donor->relocationRuns.size()) {
        ++counters.aliasWords;
        return finish(false);
    }
    auto copy = std::make_shared<DispatchVariant>();
    copy->runs = donor->relocationRuns;
    copy->runBases = donor->runBases;
    copy->relocatableWords = donor->relocatableWords;
    copy->words = donor->words;
    copy->compiled = donor->compiled;
    copy->shader = donor->shader;
    copy->forgetSerial = donor->forgetSerial;
    copy->userData.assign(userData.begin(), userData.end());
    bool moved = false;
    for (std::uint32_t index = 0; index < userData.size(); ++index) {
        const auto from = donor->userData[index], to = userData[index];
        if (from == to) continue;
        if (index >= 64 || (donor->relocatableWords >> index & 1u) == 0 || from == 0 || to == 0 || ((from ^ to) & 3u) != 0) {
            ++counters.aliasWords;
            return finish(false);
        }
        moved = true;
        const auto delta = static_cast<std::int64_t>(to) - from;
        for (std::size_t run = 0; run < copy->runs.size(); ++run) {
            if (copy->runBases[run] != static_cast<std::int8_t>(index)) continue;
            auto& [begin, end] = copy->runs[run];
            if ((delta < 0 && begin < static_cast<std::uint64_t>(-delta)) || (delta > 0 && end > std::numeric_limits<std::uint64_t>::max() - static_cast<std::uint64_t>(delta))) {
                ++counters.aliasRange;
                return finish(false);
            }
            begin += delta;
            end += delta;
        }
        const auto& forwarded = donor->compiled->shaderDataUserWords;
        for (std::uint32_t slot = 0; slot < forwarded.size(); ++slot) {
            if (forwarded[slot] != index) continue;
            bool patchable = (static_cast<std::size_t>(slot) + 1) * sizeof(std::uint32_t) <= donor->compiled->pushConstants.size();
            for (const auto& binding : donor->compiled->bindings) {
                if (binding.role == ShaderRecompiler::DescriptorRole::ShaderData && slot < binding.guestDescriptor.size()) patchable = true;
            }
            if (!patchable) {
                ++counters.aliasPatches;
                return finish(false);
            }
            copy->shaderDataPatches.emplace_back(slot, to);
        }
    }
    if (!moved) return finish(false);
    applyShaderDataPatches(*copy);
    if (!copy->patchedPushConstants.empty()) {
        auto patched = std::make_shared<ShaderRecompiler::RecompileResult>(*copy->compiled);
        patched->pushConstants = std::move(copy->patchedPushConstants);
        copy->compiled = std::move(patched);
    }
    const auto data = std::find_if(copy->compiled->bindings.begin(), copy->compiled->bindings.end(), [](const auto& binding) { return binding.role == ShaderRecompiler::DescriptorRole::ShaderData; });
    for (std::size_t slot = 0; slot < copy->compiled->shaderDataUserWords.size(); ++slot) {
        const auto word = copy->compiled->shaderDataUserWords[slot];
        std::uint32_t patched = 0;
        bool valid = word < userData.size();
        if (data != copy->compiled->bindings.end()) {
            valid = valid && slot < data->guestDescriptor.size();
            if (valid) patched = data->guestDescriptor[slot];
        } else {
            valid = valid && (slot + 1u) * sizeof(patched) <= copy->compiled->pushConstants.size();
            if (valid) std::memcpy(&patched, copy->compiled->pushConstants.data() + slot * sizeof(patched), sizeof(patched));
        }
        if (!valid || patched != userData[word]) {
            ++counters.aliasPatches;
            return finish(false);
        }
    }
    cacheLock.unlock();
    std::vector<ShaderRecompiler::MemoryRegion> regions;
    appendEntryRegions(*copy, regions);
    std::uint64_t imagesFlushed = 0, runsSynced = 0;
    std::optional<SampledReadScope> sampling;
    const GuestMemory::ReadSiteScope site(GuestMemory::ReadSite::DispatchCache);
    auto outcome = validateVariant(address, submission.queue, *copy, regions, imagesFlushed, runsSynced, sampling);
    if (gateRetry() && (outcome == EntryOutcome::PublishMoved || outcome == EntryOutcome::PendingMoved)) outcome = validateVariant(address, submission.queue, *copy, regions, imagesFlushed, runsSynced, sampling);
    cacheLock.lock();
    if (outcome != EntryOutcome::Equal) {
        ++counters.aliasValidation;
        return finish(false);
    }
    captured.assign(memory.begin(), memory.end());
    captured.insert(captured.end(), regions.begin(), regions.end());
    variant = std::move(copy);
    ++counters.aliasHits;
    ++dispatchCacheHits;
    return finish(true);
}

bool Driver::dataHits() {
    static const bool hits = std::getenv("APS5_NO_DATA_HITS") == nullptr && VulkanDevice::TemplateDataRefresh();
    return hits;
}

bool Driver::verifyDataHits() {
    static const bool verify = std::getenv("APS5_VERIFY_DATA_HITS") != nullptr;
    return verify;
}

std::size_t Driver::dispatchVariants() {
    static const std::size_t variants = [] {
        if (stampValidate()) return std::size_t{1};
        const char* text = std::getenv("APS5_DISPATCH_VARIANTS");
        const auto parsed = text != nullptr ? std::strtoull(text, nullptr, 10) : 4ull;
        return static_cast<std::size_t>(std::clamp<unsigned long long>(parsed, 1, MaxDispatchVariants));
    }();
    return variants;
}

bool Driver::insertCompare() {
    static const bool compare = std::getenv("APS5_INSERT_COMPARE") != nullptr;
    return compare;
}

std::size_t Driver::dispatchCacheEntries() {
    static const std::size_t entries = [] {
        const char* text = std::getenv("APS5_DISPATCH_CACHE_ENTRIES");
        const auto parsed = text != nullptr ? std::strtoull(text, nullptr, 10) : 0ull;
        if (parsed != 0) return static_cast<std::size_t>(parsed);
        if (stampValidate()) return std::size_t{4096};
        return dispatchVariants() > 2 ? std::size_t{8192} : std::size_t{16384};
    }();
    return entries;
}

std::uint64_t Driver::DispatchCacheBytes() {
    static const std::uint64_t bytes = [] {
        const char* text = std::getenv("APS5_DISPATCH_CACHE_BYTES");
        const auto parsed = text != nullptr ? std::strtoull(text, nullptr, 10) : 0ull;
        return parsed != 0 ? static_cast<std::uint64_t>(parsed) : std::uint64_t{256} << 20u;
    }();
    return bytes;
}

Driver::DispatchCacheFootprint Driver::DispatchCacheSize() {
    std::lock_guard lock(dispatchCacheMutex);
    return {dispatchCache.size(), dispatchCacheVariantBytes};
}

std::uint64_t Driver::variantBytes(const DispatchVariant& variant) {
    return sizeof(DispatchVariant) + variant.words.size() * sizeof(std::uint32_t) + (variant.runs.size() + variant.relocationRuns.size()) * sizeof(std::pair<std::uint64_t, std::uint64_t>) + variant.runBases.size() * sizeof(std::int8_t) + variant.userData.size() * sizeof(std::uint32_t) + variant.captured.size() * sizeof(ShaderRecompiler::MemoryRegion) + (variant.vertexInfo != nullptr ? sizeof(ShaderRecompiler::ShaderVertexStageInfo) : 0);
}

void Driver::accountVariant(const DispatchVariant& variant, bool added) {
    if (added) {
        ++dispatchCacheVariants;
        dispatchCacheVariantBytes += variantBytes(variant);
    } else {
        --dispatchCacheVariants;
        dispatchCacheVariantBytes -= variantBytes(variant);
    }
}

void Driver::eraseDispatchEntry(std::unordered_map<std::uint64_t, std::shared_ptr<DispatchEntry>>::iterator it) {
    for (const auto& variant : it->second->variants) accountVariant(*variant, false);
    dispatchOrder.erase(it->second->order);
    dispatchCache.erase(it);
}

}
