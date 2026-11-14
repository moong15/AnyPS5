#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Dispatch/DispatchCache.hpp"
#include "UserDataUse.hpp"
#include <cstdlib>

namespace AgcDriver::DriverDetail {

std::shared_ptr<DispatchVariant> Driver::RelocateDispatch(const DispatchVariant& variant, std::span<const std::uint32_t> from, std::span<const std::uint32_t> to) {
    if (variant.compiled == nullptr || from.size() != to.size() || from.size() > ShaderRecompiler::SrtUserDataUse::TrackedWords || variant.runs.size() != variant.runBases.size()) return nullptr;
    std::uint64_t moved = 0;
    for (std::size_t index = 0; index < from.size(); ++index) {
        if (from[index] == to[index]) continue;
        const auto bit = std::uint64_t{1} << index;
        if ((variant.relocatableWords & bit) == 0 || from[index] == 0 || to[index] == 0 || ((from[index] ^ to[index]) & 3u) != 0) return nullptr;
        if (std::find(variant.runBases.begin(), variant.runBases.end(), static_cast<std::int8_t>(index)) == variant.runBases.end()) return nullptr;
        moved |= bit;
    }
    if (moved == 0) return nullptr;
    auto copy = std::make_shared<DispatchVariant>();
    copy->runs = variant.runs;
    copy->runBases = variant.runBases;
    copy->relocatableWords = variant.relocatableWords;
    copy->userWordsWalked = variant.userWordsWalked;
    copy->words = variant.words;
    copy->compiled = variant.compiled;
    copy->registeredViews = variant.registeredViews;
    copy->shader = variant.shader;
    copy->forgetSerial = variant.forgetSerial;
    for (std::size_t run = 0; run < copy->runs.size(); ++run) {
        const auto index = copy->runBases[run];
        if (index < 0) continue;
        if (static_cast<std::size_t>(index) >= from.size()) return nullptr;
        if ((moved >> index & 1u) == 0) continue;
        if (copy->runs[run].first >= (std::uint64_t{1} << 48u) || copy->runs[run].second > (std::uint64_t{1} << 48u)) return nullptr;
        const auto delta = static_cast<std::int64_t>(to[index] & ~3u) - static_cast<std::int64_t>(from[index] & ~3u);
        const auto begin = static_cast<std::int64_t>(copy->runs[run].first) + delta;
        const auto end = static_cast<std::int64_t>(copy->runs[run].second) + delta;
        if (begin < 0 || end <= begin || end > (std::int64_t{1} << 48u)) return nullptr;
        copy->runs[run] = {static_cast<std::uint64_t>(begin), static_cast<std::uint64_t>(end)};
    }
    std::shared_ptr<ShaderRecompiler::RecompileResult> patched;
    const auto& forwarded = variant.compiled->shaderDataUserWords;
    for (std::size_t slot = 0; slot < forwarded.size(); ++slot) {
        const auto index = forwarded[slot];
        if (index >= from.size() || (moved >> index & 1u) == 0) continue;
        if (patched == nullptr) patched = std::make_shared<ShaderRecompiler::RecompileResult>(*variant.compiled);
        patchForwardedWord(*patched, static_cast<std::uint32_t>(slot), to[index]);
    }
    if (patched != nullptr) copy->compiled = std::move(patched);
    return copy;
}

std::shared_ptr<DispatchEntry> Driver::dispatchRelocationEntry(const DispatchRelocationKey& key) {
    ++entryCounters.relocationLookups;
    const auto aliases = dispatchAliases.find(key.hash);
    if (aliases == dispatchAliases.end()) return nullptr;
    auto relocated = std::make_shared<DispatchEntry>();
    for (const auto ownerKey : aliases->second) {
        const auto found = dispatchCache.find(ownerKey);
        if (found == dispatchCache.end() || found->second->relocation == nullptr || found->second->relocation->state != key.state) continue;
        const auto& owner = found->second;
        for (const auto& variant : owner->variants) {
            auto copy = RelocateDispatch(*variant, owner->relocation->userData, key.userData);
            if (copy == nullptr) {
                ++entryCounters.relocationRefused;
                continue;
            }
            ++entryCounters.relocationCandidates;
            relocated->variants.push_back(std::move(copy));
            relocated->owners.emplace_back(ownerKey, owner);
            if (relocated->variants.size() == MaxDispatchVariants) return relocated;
        }
    }
    return relocated->variants.empty() ? nullptr : relocated;
}

bool Driver::stampValidate() {
    static const bool stamp = std::getenv("APS5_DISPATCH_STAMP_VALIDATE") != nullptr;
    return stamp;
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
        return parsed != 0 ? static_cast<std::uint64_t>(parsed) : std::uint64_t{1} << 30u;
    }();
    return bytes;
}

EntryCounters Driver::DispatchCacheCounters() {
    std::lock_guard lock(dispatchCacheMutex);
    return entryCounters;
}

Driver::DispatchCacheFootprint Driver::DispatchCacheSize() {
    std::lock_guard lock(dispatchCacheMutex);
    return {dispatchCache.size(), dispatchCacheVariantBytes};
}

std::uint64_t Driver::variantBytes(const DispatchVariant& variant) {
    const auto held = [](const auto& values) { return static_cast<std::uint64_t>(values.capacity() * sizeof(values[0])); };
    return sizeof(DispatchVariant) + held(variant.runs) + held(variant.words) + held(variant.shared) + held(variant.dataPositions) + held(variant.dataSlots) + held(variant.runBases) + held(variant.shaderDataPatches) + held(variant.captured) + held(variant.spans) + (variant.registeredViews != nullptr ? held(*variant.registeredViews) : 0u) + (variant.vertexInfo != nullptr ? sizeof(ShaderRecompiler::ShaderVertexStageInfo) : 0) + (variant.memory != nullptr ? variant.memory->Bytes() : 0);
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
    if (it->second->relocation != nullptr) {
        const auto alias = dispatchAliases.find(it->second->relocation->hash);
        if (alias != dispatchAliases.end()) {
            std::erase(alias->second, it->first);
            if (alias->second.empty()) dispatchAliases.erase(alias);
        }
    }
    for (const auto& variant : it->second->variants) accountVariant(*variant, false);
    dispatchOrder.erase(it->second->order);
    dispatchCache.erase(it);
}

}
