#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace AgcDriver::DriverDetail {

namespace {

constexpr std::size_t MaxAliases = 16384;
constexpr std::size_t MaxContentWords = 64;

std::uint64_t AlignedWord(std::uint32_t word) {
    return word & ~std::uint32_t {3};
}

}

bool Driver::drawRelocationEnabled() {
    static const bool enabled = std::getenv("APS5_NO_DRAW_RELOCATION") == nullptr;
    return enabled;
}

std::uint64_t Driver::relocatedContentKey(const RelocatableUserWord& word, std::uint32_t value, bool& readable) {
    std::uint64_t hash = 0x9e3779b97f4a7c15ull;
    const auto mix = [&](std::uint64_t mixed) {
        hash ^= mixed;
        hash *= 0x100000001b3ull;
    };
    mix(value & 3u);
    if (value == 0) {
        readable = false;
        return hash;
    }
    std::array<std::uint32_t, MaxContentWords> content;
    for (const auto& [offset, words] : word.reads) {
        mix(static_cast<std::uint64_t>(offset));
        mix(words);
        for (std::size_t done = 0; done < words; done += MaxContentWords) {
            const auto count = std::min<std::size_t>(words - done, MaxContentWords);
            const auto address = AlignedWord(value) + static_cast<std::uint64_t>(offset) + done * sizeof(std::uint32_t);
            if (GuestMemory::CopyMapped(address, std::as_writable_bytes(std::span(content).first(count))) != GuestMemory::Compare::Equal) {
                readable = false;
                return hash;
            }
            for (std::size_t i = 0; i < count; ++i) mix(std::find(word.dataOffsets.begin(), word.dataOffsets.end(), offset + static_cast<std::int64_t>(done + i) * 4) != word.dataOffsets.end() ? 0u : content[i]);
        }
    }
    return hash;
}

void Driver::tagVariantRuns(DispatchVariant& variant, std::span<const ShaderRecompiler::MemoryRegion> regions, const ShaderRecompiler::SrtUserDataUse& use, bool eligible, std::uint32_t hiddenWords) {
    std::uint64_t relocatable = eligible && drawRelocationEnabled() ? use.asBase & ~use.asValue : 0;
    if (hiddenWords != 0) relocatable &= hiddenWords >= 64 ? 0 : ~((std::uint64_t {1} << hiddenWords) - 1);
    std::vector<std::pair<std::uint64_t, std::int32_t>> reads;
    const auto covered = [&](std::uint64_t address) {
        return std::any_of(regions.begin(), regions.end(), [&](const ShaderRecompiler::MemoryRegion& region) { return address >= region.guestAddress && address - region.guestAddress + sizeof(std::uint32_t) <= region.bytes.size(); });
    };
    if (relocatable != 0) {
        reads = use.reads;
        std::sort(reads.begin(), reads.end());
        reads.erase(std::unique(reads.begin(), reads.end()), reads.end());
        const auto drop = [&](std::int32_t origin) {
            if (origin >= 0) relocatable &= ~(std::uint64_t {1} << origin);
        };
        for (std::size_t i = 0; i < reads.size(); ++i) {
            if (i + 1 < reads.size() && reads[i].first == reads[i + 1].first) {
                drop(reads[i].second);
                drop(reads[i + 1].second);
            }
            if (reads[i].second >= 0 && ((reads[i].first & 3u) != 0 || !covered(reads[i].first))) drop(reads[i].second);
        }
    }
    const auto origin = [&](std::uint64_t address) -> std::int8_t {
        const auto found = std::lower_bound(reads.begin(), reads.end(), std::make_pair(address, std::numeric_limits<std::int32_t>::min()));
        if (found == reads.end() || found->first != address || found->second < 0) return -1;
        return (relocatable >> found->second & 1u) != 0 ? static_cast<std::int8_t>(found->second) : std::int8_t {-1};
    };
    variant.relocatableWords = relocatable;
    for (const auto& region : regions) {
        const auto count = region.bytes.size() / sizeof(std::uint32_t);
        const auto offset = variant.words.size();
        variant.words.resize(offset + count);
        std::memcpy(variant.words.data() + offset, region.bytes.data(), count * sizeof(std::uint32_t));
        if (relocatable == 0) {
            variant.runs.emplace_back(region.guestAddress, region.guestAddress + region.bytes.size());
            continue;
        }
        std::size_t start = 0;
        auto current = count != 0 ? origin(region.guestAddress) : std::int8_t {-1};
        for (std::size_t word = 1; word <= count; ++word) {
            const auto next = word < count ? origin(region.guestAddress + word * sizeof(std::uint32_t)) : std::int8_t {-2};
            if (next == current) continue;
            variant.runs.emplace_back(region.guestAddress + start * sizeof(std::uint32_t), region.guestAddress + word * sizeof(std::uint32_t));
            variant.runBases.push_back(current);
            start = word;
            current = next;
        }
    }
}

namespace {

template <typename TMap>
std::size_t dropEveryOther(TMap& map) {
    std::size_t dropped = 0;
    bool drop = false;
    for (auto it = map.begin(); it != map.end();) {
        drop = !drop;
        if (!drop) {
            ++it;
            continue;
        }
        it = map.erase(it);
        ++dropped;
    }
    return dropped;
}

}

void Driver::learnPendingRelocation() {
    const auto pending = drawRelocationPending;
    drawRelocationPending = {};
    if (pending.key == 0) return;
    const auto inserted = drawCache.find(pending.key);
    if (inserted == drawCache.end() || inserted->second->decode == nullptr) return;
    const auto& entry = *inserted->second;
    const auto& programs = entry.decode->programs;
    if (entry.stages.size() != programs.size()) return;
    const auto known = drawRelocations.find(pending.identity);
    auto shape = std::make_shared<DrawRelocation>();
    if (known != drawRelocations.end()) *shape = *known->second;
    bool changed = known == drawRelocations.end();
    for (std::size_t i = 0; i < programs.size(); ++i) {
        if (entry.stages[i].empty()) continue;
        const auto& variant = *entry.stages[i].front();
        const auto& program = programs[i];
        for (std::uint32_t index = program.hiddenUserWords; index < program.userData.size() && index < 64; ++index) {
            const auto reg = program.userDataBase + index - program.hiddenUserWords;
            const auto held = std::find_if(shape->words.begin(), shape->words.end(), [&](const RelocatableUserWord& word) { return word.reg == reg; });
            if ((variant.relocatableWords >> index & 1u) == 0) {
                if (held == shape->words.end()) continue;
                shape->words.erase(held);
                shape->banned.push_back(reg);
                changed = true;
                continue;
            }
            if (held != shape->words.end() || std::find(shape->banned.begin(), shape->banned.end(), reg) != shape->banned.end()) continue;
            RelocatableUserWord word{reg, {}};
            const auto base = AlignedWord(program.userData[index]);
            std::uint32_t position = 0;
            for (std::size_t run = 0; run < variant.runs.size(); ++run) {
                const auto count = static_cast<std::uint32_t>((variant.runs[run].second - variant.runs[run].first) / sizeof(std::uint32_t));
                if (run < variant.runBases.size() && variant.runBases[run] == static_cast<std::int8_t>(index)) {
                    word.reads.emplace_back(static_cast<std::int64_t>(variant.runs[run].first - base), count);
                    if (!variant.bufferWordBuffers.empty()) {
                        for (const auto data : variant.dataPositions) {
                            if (data >= position && data < position + count) word.dataOffsets.push_back(static_cast<std::int64_t>(variant.runs[run].first - base) + static_cast<std::int64_t>(data - position) * 4);
                        }
                    }
                }
                position += count;
            }
            shape->words.push_back(std::move(word));
            changed = true;
        }
    }
    if (changed) {
        std::sort(shape->words.begin(), shape->words.end(), [](const RelocatableUserWord& a, const RelocatableUserWord& b) { return a.reg < b.reg; });
        drawRelocations[pending.identity] = std::move(shape);
    }
    if (pending.aliases.structure == 0) return;
    if (drawStageDonors.size() >= MaxAliases) drawRelocationCounters.evicted += dropEveryOther(drawStageDonors);
    if (drawDecodes.size() >= MaxAliases) drawRelocationCounters.evicted += dropEveryOther(drawDecodes);
    DrawDecodeDonor donor{entry.decode, std::vector<std::uint64_t>(programs.size(), 0)};
    for (std::size_t i = 0; i < programs.size(); ++i) {
        if (entry.stages[i].empty() || entry.stages[i].front() == nullptr || entry.stages[i].front()->prepared == nullptr) continue;
        const auto& variant = entry.stages[i].front();
        const auto& program = programs[i];
        std::uint64_t key = 0xcbf29ce484222325ull;
        const auto mix = [&](std::uint64_t value) {
            key ^= value;
            key *= 0x100000001b3ull;
        };
        mix(reinterpret_cast<std::uintptr_t>(variant->prepared.get()));
        mix(variant->pushOffset);
        mix(program.userData.size());
        for (std::uint32_t index = 0; index < program.userData.size(); ++index) {
            const bool relocatable = pending.relocation != nullptr && index >= program.hiddenUserWords && std::any_of(pending.relocation->words.begin(), pending.relocation->words.end(), [&](const RelocatableUserWord& word) { return word.reg == program.userDataBase + index - program.hiddenUserWords; });
            if (!relocatable && index >= program.hiddenUserWords) mix(program.userData[index]);
        }
        donor.stageKeys[i] = key == 0 ? 1 : key;
        drawStageDonors[pending.aliases.Key(donor.stageKeys[i], program.userDataBase)] = {variant, program.userData};
    }
    const auto held = drawDecodes.find(pending.aliases.structure);
    if (held != drawDecodes.end() && held->second.stageKeys.size() == donor.stageKeys.size()) {
        for (std::size_t i = 0; i < donor.stageKeys.size(); ++i) {
            if (donor.stageKeys[i] == 0) donor.stageKeys[i] = held->second.stageKeys[i];
        }
    }
    drawDecodes[pending.aliases.structure] = std::move(donor);
}

void Driver::relocateDrawEntry(const QueueState& queue, const DrawAliasKeys& aliases, std::shared_ptr<DrawEntry>& entry, std::shared_ptr<const DrawDecode>& decode, std::vector<DrawUserPatch>& patches) {
    auto& counters = drawRelocationCounters;
    ++counters.lookups;
    const auto known = drawDecodes.find(aliases.structure);
    if (known == drawDecodes.end() || known->second.decode == nullptr) return;
    const auto shared = known->second.decode;
    const auto& stageKeys = known->second.stageKeys;
    const auto& programs = shared->programs;
    auto relocated = std::make_shared<DrawEntry>();
    relocated->decode = shared;
    relocated->stages.resize(programs.size());
    std::vector<DrawUserPatch> current;
    bool complete = true;
    drawStageDonorKeys.assign(programs.size(), 0);
    for (std::size_t i = 0; i < programs.size(); ++i) {
        const auto& program = programs[i];
        if (i < shared->roles.size() && shared->roles[i] == ShaderRecompiler::ProgramRole::GeometryBack) continue;
        const auto found = i < stageKeys.size() && stageKeys[i] != 0 ? drawStageDonors.find(aliases.Key(stageKeys[i], program.userDataBase)) : drawStageDonors.end();
        if (found != drawStageDonors.end()) drawStageDonorKeys[i] = found->first;
        const auto* donor = found != drawStageDonors.end() && found->second.variant != nullptr && found->second.variant->compiled != nullptr && found->second.userData.size() == program.userData.size() ? &found->second : nullptr;
        struct Move {
            std::uint32_t index, from, to;
        };
        std::vector<Move> moves;
        bool allowed = donor != nullptr;
        for (std::uint32_t index = program.hiddenUserWords; index < program.userData.size(); ++index) {
            const auto now = readUserData(queue.shader, program.userDataBase + index - program.hiddenUserWords);
            if (now != program.userData[index]) current.push_back({i, index, now});
            if (donor == nullptr || now == donor->userData[index]) continue;
            const auto from = donor->userData[index];
            if (index >= 64 || (donor->variant->relocatableWords >> index & 1u) == 0 || from == 0 || now == 0 || ((from ^ now) & 3u) != 0) allowed = false;
            moves.push_back({index, from, now});
        }
        if (!allowed) {
            if (donor != nullptr) ++counters.refused;
            complete = false;
            continue;
        }
        if (moves.empty()) {
            relocated->stages[i].push_back(donor->variant);
            continue;
        }
        const auto& variant = donor->variant;
        auto copy = cloneVariant(*variant);
        for (const auto& move : moves) {
            const auto delta = AlignedWord(move.to) - AlignedWord(move.from);
            for (std::size_t run = 0; run < copy->runs.size() && run < copy->runBases.size(); ++run) {
                if (copy->runBases[run] != static_cast<std::int8_t>(move.index)) continue;
                copy->runs[run].first += delta;
                copy->runs[run].second += delta;
            }
            const auto& forwarded = variant->compiled->shaderDataUserWords;
            for (std::uint32_t slot = 0; slot < forwarded.size(); ++slot) {
                if (forwarded[slot] == move.index) copy->shaderDataPatches.emplace_back(slot, move.to);
            }
        }
        relocated->stages[i].push_back(std::move(copy));
    }
    if (complete) ++counters.donors;
    patches = std::move(current);
    entry = std::move(relocated);
    decode = shared;
}

std::shared_ptr<DispatchVariant> Driver::cloneVariant(const DispatchVariant& variant) {
    auto copy = std::make_shared<DispatchVariant>();
    copy->runs = variant.runs;
    copy->runBases = variant.runBases;
    copy->relocatableWords = variant.relocatableWords;
    copy->words = variant.words;
    copy->forgetSerial = variant.forgetSerial;
    copy->compiled = variant.compiled;
    copy->shader = variant.shader;
    copy->dataPositions = variant.dataPositions;
    copy->dataSlots = variant.dataSlots;
    copy->flatBinding = variant.flatBinding;
    copy->pushOffset = variant.pushOffset;
    copy->prepared = variant.prepared;
    copy->bufferWordBuffers = variant.bufferWordBuffers;
    copy->patchedPushConstants = variant.patchedPushConstants;
    copy->shaderDataPatches = variant.shaderDataPatches;
    copy->vertexInfo = variant.vertexInfo;
    return copy;
}

void Driver::tagBufferWords(DispatchVariant& variant, std::span<const ShaderRecompiler::BufferBaseWord> bufferBaseWords) {
    if (!drawRelocationEnabled() || bufferBaseWords.empty() || variant.compiled == nullptr || !variant.dataPositions.empty()) return;
    const auto& compiled = *variant.compiled;
    using Patches = std::vector<std::pair<std::uint32_t, std::uint32_t>>;
    std::vector<std::pair<std::uint32_t, Patches>> tagged;
    for (const auto& base : bufferBaseWords) {
        std::uint32_t position = 0;
        bool located = false;
        for (const auto& [begin, end] : variant.runs) {
            if (base.address >= begin && base.address < end && (base.address - begin) % sizeof(std::uint32_t) == 0) {
                position += static_cast<std::uint32_t>((base.address - begin) / sizeof(std::uint32_t));
                located = true;
                break;
            }
            position += static_cast<std::uint32_t>((end - begin) / sizeof(std::uint32_t));
        }
        if (!located || position >= variant.words.size() || variant.words[position] == 0) continue;
        const auto value = variant.words[position];
        Patches patches;
        std::size_t slots = 0, others = 0;
        for (std::size_t binding = 0; binding < compiled.bindings.size(); ++binding) {
            const auto& words = compiled.bindings[binding].guestDescriptor;
            for (std::size_t word = 0; word < words.size(); ++word) {
                if (words[word] != value) continue;
                const bool slot = std::any_of(compiled.bufferBaseSlots.begin(), compiled.bufferBaseSlots.end(), [&](const std::array<std::uint32_t, 3>& entry) { return entry[0] == base.buffer && entry[1] == binding && entry[2] == word; });
                const bool flat = compiled.bindings[binding].role == ShaderRecompiler::DescriptorRole::FlattenedSrt && base.flatOffset >= 0 && static_cast<std::int64_t>(word) == base.flatOffset;
                if (slot) ++slots;
                if (!slot && !flat) ++others;
                if (slot || flat) patches.emplace_back(static_cast<std::uint32_t>(binding), static_cast<std::uint32_t>(word));
            }
        }
        for (std::size_t at = 0; at + sizeof(std::uint32_t) <= compiled.pushConstants.size(); at += sizeof(std::uint32_t)) {
            std::uint32_t word = 0;
            std::memcpy(&word, compiled.pushConstants.data() + at, sizeof(word));
            if (word == value) ++others;
        }
        for (const auto& constant : compiled.specialization) {
            if (constant.value == value) ++others;
        }
        const auto expected = static_cast<std::size_t>(std::count_if(compiled.bufferBaseSlots.begin(), compiled.bufferBaseSlots.end(), [&](const std::array<std::uint32_t, 3>& entry) { return entry[0] == base.buffer; }));
        if (slots == 0 || slots != expected || others != 0) continue;
        if (std::any_of(tagged.begin(), tagged.end(), [&](const auto& entry) { return entry.first == position; })) continue;
        tagged.emplace_back(position, std::move(patches));
    }
    std::sort(tagged.begin(), tagged.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto& [position, patches] : tagged) {
        variant.dataPositions.push_back(position);
        variant.bufferWordBuffers.push_back(std::move(patches));
    }
}

std::shared_ptr<DispatchVariant> Driver::patchBufferWords(const DispatchVariant& variant, std::span<const std::pair<std::uint32_t, std::uint32_t>> live) {
    if (live.empty() || variant.compiled == nullptr || variant.dataPositions.size() != variant.bufferWordBuffers.size()) return nullptr;
    auto copy = cloneVariant(variant);
    auto rebased = std::make_shared<ShaderRecompiler::RecompileResult>(*variant.compiled);
    for (const auto& [position, fresh] : live) {
        const auto found = std::lower_bound(variant.dataPositions.begin(), variant.dataPositions.end(), position);
        if (found == variant.dataPositions.end() || *found != position || position >= copy->words.size()) return nullptr;
        const auto stored = copy->words[position];
        if (stored == 0 || fresh == 0 || ((stored ^ fresh) & 3u) != 0) return nullptr;
        for (const auto& [binding, word] : variant.bufferWordBuffers[static_cast<std::size_t>(found - variant.dataPositions.begin())]) {
            if (binding >= rebased->bindings.size() || word >= rebased->bindings[binding].guestDescriptor.size() || rebased->bindings[binding].guestDescriptor[word] != stored) return nullptr;
            rebased->bindings[binding].guestDescriptor[word] = fresh;
        }
        copy->words[position] = fresh;
    }
    copy->compiled = std::move(rebased);
    copy->dataPatched = true;
    return copy;
}

void Driver::applyShaderDataPatches(DispatchVariant& variant) {
    if (variant.shaderDataPatches.empty()) return;
    const auto patch = [&](std::vector<std::byte>& push) {
        for (const auto& [slot, value] : variant.shaderDataPatches) {
            const auto at = static_cast<std::size_t>(slot) * sizeof(std::uint32_t);
            if (at + sizeof(std::uint32_t) <= push.size()) std::memcpy(push.data() + at, &value, sizeof(value));
        }
    };
    const bool stored = std::any_of(variant.compiled->bindings.begin(), variant.compiled->bindings.end(), [](const ShaderRecompiler::DescriptorBinding& binding) { return binding.role == ShaderRecompiler::DescriptorRole::ShaderData; });
    if (!stored) {
        if (variant.patchedPushConstants.empty()) variant.patchedPushConstants = variant.compiled->pushConstants;
        patch(variant.patchedPushConstants);
        variant.shaderDataPatches.clear();
        return;
    }
    auto rebased = std::make_shared<ShaderRecompiler::RecompileResult>(*variant.compiled);
    patch(rebased->pushConstants);
    for (const auto& [slot, value] : variant.shaderDataPatches) {
        for (auto& binding : rebased->bindings) {
            if (binding.role == ShaderRecompiler::DescriptorRole::ShaderData && slot < binding.guestDescriptor.size()) binding.guestDescriptor[slot] = value;
        }
    }
    variant.compiled = std::move(rebased);
    variant.shaderDataPatches.clear();
}

void Driver::reportDrawRelocation() {
    auto& counters = drawRelocationCounters;
    AgcDriver::ProfilePrint_nid_no_patch("[draw-relocate] %llu alias lookups: %llu donors with moved user words, %llu refusals, %llu hits through a relocated entry; %zu shader sets learned, %zu stage donors held (%llu dropped at the limit)\n", static_cast<unsigned long long>(counters.lookups), static_cast<unsigned long long>(counters.donors), static_cast<unsigned long long>(counters.refused), static_cast<unsigned long long>(counters.hits), drawRelocations.size(), drawStageDonors.size(), static_cast<unsigned long long>(counters.evicted));
    counters = {};
}

}
