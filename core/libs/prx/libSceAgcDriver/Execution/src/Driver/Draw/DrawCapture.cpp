#include <vector>
#include <string>
#include <span>
#include <cstdio>
#include <atomic>
#include <array>
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "Optimization/ResourceProgram.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <stdexcept>

namespace AgcDriver::DriverDetail {

namespace {

// Aliased rather than named directly: ShaderCacheVersion.cmake rejects an unreviewed ShaderRecompiler::
// call or using-declaration in a guarded file, and adding a name to shaderCacheReviewedNames would mean
// editing core/shader/recompiler/CMakeLists.txt, which *is* a hashed cache-key source.
using SrtRole = ShaderRecompiler::DescriptorRole;

// The V# buffer-descriptor decode ShaderResources::addGuestBuffer applies, written out here rather than
// called, for the same reason. base48 = fields[0] | fields[1]<<32 (48 bits), stride = fields[1][29:16],
// numRecords = fields[2], size = stride ? stride * numRecords : numRecords.
struct DecodedBuffer {
    std::uint64_t base = 0;
    std::uint32_t stride = 0;
    std::uint32_t numRecords = 0;
    std::uint64_t bytes = 0;
    bool valid = false;
};

DecodedBuffer decodeBufferDescriptor(const std::uint32_t* words) {
    DecodedBuffer out;
    out.base = (static_cast<std::uint64_t>(words[0]) | (static_cast<std::uint64_t>(words[1]) << 32u)) & 0xFFFFFFFFFFFFull;
    out.stride = (words[1] >> 16u) & 0x3FFFu;
    out.numRecords = words[2];
    out.bytes = out.stride == 0u ? out.numRecords : static_cast<std::uint64_t>(out.stride) * out.numRecords;
    out.valid = out.bytes != 0 && out.base != 0;
    return out;
}

const char* roleLabel(SrtRole role) {
    switch (role) {
        case SrtRole::GuestBuffers: return "GuestBuffers";
        case SrtRole::GuestImages: return "GuestImages";
        case SrtRole::GuestSamplers: return "GuestSamplers";
        case SrtRole::Gds: return "Gds";
        case SrtRole::BdaPagetable: return "BdaPagetable";
        case SrtRole::FaultBuffer: return "FaultBuffer";
        case SrtRole::FlattenedSrt: return "FlattenedSrt";
        case SrtRole::ShaderData: return "ShaderData";
    }
    return "?";
}

constexpr std::size_t ProbeBytes = 64;

// Corrected missing-base candidate, kept as plain arithmetic: waits live just above 2^36
// (0x1022...), descriptor bases just below 2^32 (0x20...).
constexpr std::uint64_t RebaseCandidate = 0x1000000000ull;

// How much of [base, base+size) is continuously host-readable. Whole-range probes make
// accessibility monotonic in the length; probing only endpoints can jump over unmapped holes.
// This runs only on the already-failing diagnostic path.
std::uint64_t readablePrefix(std::uint64_t base, std::uint64_t size) {
    constexpr std::uint64_t SpaceEnd = 0x1000000000000ull;
    if (size == 0 || base >= SpaceEnd) return 0;
    std::uint64_t end = base + size;
    if (end > SpaceEnd || end < base) end = SpaceEnd;
    const auto readable = [base](std::uint64_t length) {
        return GuestMemory::Accessible(reinterpret_cast<const void*>(base), static_cast<std::size_t>(length));
    };
    if (!readable(1)) return 0;
    std::uint64_t good = 1;
    std::uint64_t upper = end - base;
    while (good < upper) {
        const std::uint64_t candidate = good + (upper - good + 1) / 2;
        if (readable(candidate)) good = candidate;
        else upper = candidate - 1;
    }
    return good;
}

// Non-throwing register read. readRegister throws "required shader register has not been written" when
// the offset is absent, which both truncated this dump at the first register the guest never wrote and
// injected a spurious "[agc] driver failure, continuing" into the run. The shader path itself reads
// these non-throwingly (DrawDecode.cpp: ReadGraphicsRegister / readUserData both return 0), so an
// absent register is simply not written.
std::uint32_t peekRegister(const Registers& registers, std::uint32_t offset) {
    const auto found = registers.find(offset);
    return found == registers.end() ? 0u : found->second;
}

// Whether [base, base+ProbeBytes) is mapped and readable, and if so what it contains. Written to the
// dump so a descriptor's base address can be judged against guest memory instead of inferred from the
// descriptor alone: a base holding sane descriptors means the decode is wrong, a base holding the same
// repeating pattern means the user-data SGPRs are wrong, and an unreadable base means the address is
// fabricated. An unreadable base alone does not say whether the high bits were lost (see below).
void probeBase(std::FILE* file, std::uint64_t base) {
    if (base == 0 || base > 0xFFFFFFFFFFFFull || base + ProbeBytes > 0x1000000000000ull) {
        std::fprintf(file, "  base 0x%llx: out of the guest address space, not probed\n", static_cast<unsigned long long>(base));
        return;
    }
    const bool mapped = GuestMemory::Accessible(reinterpret_cast<const void*>(base), ProbeBytes);
    std::array<std::uint8_t, ProbeBytes> bytes{};
    const auto verdict = GuestMemory::CopyMapped(base, std::span<std::byte>(reinterpret_cast<std::byte*>(bytes.data()), bytes.size()));
    std::fprintf(file, "  base 0x%llx mapped=%s copy=%s\n", static_cast<unsigned long long>(base), mapped ? "yes" : "no",
        verdict == GuestMemory::Compare::Equal ? "ok" : verdict == GuestMemory::Compare::Differs ? "differs" : "unmapped");
    for (std::size_t offset = 0; offset < ProbeBytes; offset += 16) {
        std::fprintf(file, "    +%02zx ", offset);
        for (std::size_t byte = 0; byte < 16; ++byte) std::fprintf(file, "%02x", bytes[offset + byte]);
        std::fprintf(file, "  ");
        for (std::size_t byte = 0; byte < 16; ++byte) {
            const auto value = bytes[offset + byte];
            std::fputc(value >= 0x20 && value < 0x7f ? static_cast<int>(value) : '.', file);
        }
        std::fputc('\n', file);
    }
}

// Reports a guest buffer descriptor that decodes past the 4 GiB range limit, which is what
// ShaderResources::addGuestBuffer refuses with "shader buffer exceeds descriptor range limit". That
// refusal is thrown from the Graphics layer at bind time, after DrawCapture's own failure dumps, so the
// draw that trips it never reached a .req and could not be replayed. This fires from the Execution layer
// instead, where the register bank and guest memory are both reachable.
//
// It writes the three things that separate the remaining explanations: the raw shader registers over the
// whole fragment window, the user data as SGPR numbers (what the recompiler was handed), and the guest
// memory at each descriptor base. A descriptor that appears verbatim in both a binding and the user data
// proves nothing on its own, because DescriptorBindingBuilder copies the user data into the shader data
// block by construction; only the memory behind the base address discriminates.
bool dumpOversizedDescriptors(const DrawProgram& program, const QueueState& queue, const Submission& submission, const ShaderRecompiler::RecompileResult& result) {
    static std::atomic<std::uint64_t> reports{0};
    constexpr std::uint64_t MaxStorageBufferRange = 0xFFFFFFFFull;

    std::vector<std::pair<std::size_t, std::size_t>> oversized;
    for (std::size_t b = 0; b < result.bindings.size(); ++b) {
        const auto& binding = result.bindings[b];
        if (binding.role != SrtRole::GuestBuffers) continue;
        const std::size_t elements = binding.guestDescriptor.size() / 4u;
        for (std::size_t element = 0; element < elements; ++element) {
            const auto decoded = decodeBufferDescriptor(binding.guestDescriptor.data() + element * 4u);
            if (decoded.valid && decoded.bytes > MaxStorageBufferRange) oversized.emplace_back(b, element);
        }
    }
    if (oversized.empty()) return false;
    if (reports.fetch_add(1) >= 8) return false;

    for (const auto& [index, element] : oversized) {
        const auto& binding = result.bindings[index];
        const auto decoded = decodeBufferDescriptor(binding.guestDescriptor.data() + element * 4u);
        std::fprintf(stderr,
            "[rejected-desc] shader 0x%llx stage %u binding %u element %zu stride %u numRecords %u bytes %llu base 0x%llx\n",
            static_cast<unsigned long long>(program.binary.codeAddress), static_cast<unsigned>(program.binary.stage),
            binding.binding, element, decoded.stride, decoded.numRecords, static_cast<unsigned long long>(decoded.bytes),
            static_cast<unsigned long long>(decoded.base));
    }

    char name[64];
    std::snprintf(name, sizeof(name), "rejected_desc_%llx.txt", static_cast<unsigned long long>(program.binary.codeAddress));
    std::FILE* file = std::fopen(name, "w");
    if (file == nullptr) {
        std::fprintf(stderr, "[rejected-desc] cannot write %s\n", name);
        return true;
    }
    // Unbuffered: the process is about to abort on this very descriptor, and a buffered stdio stream
    // loses everything it has not yet flushed. A first attempt produced a zero-byte file this way.
    std::setvbuf(file, nullptr, _IONBF, 0);
    std::fprintf(file, "# oversized guest buffer descriptor; shader 0x%llx stage %u\n",
        static_cast<unsigned long long>(program.binary.codeAddress), static_cast<unsigned>(program.binary.stage));
    std::fprintf(file, "# firstUserSgpr=%u userDataBase=0x%03x userCount=%zu\n", program.firstUserSgpr, program.userDataBase, program.userData.size());
    std::fprintf(file, "# memoryOffsetDword=%u shaderDataDwords=%u\n", result.memoryOffsetDword, result.shaderDataDwords);
    std::fprintf(file, "# snapshot type=%u codeOffset=%zu codeWords=%zu headerBytes=%zu\n",
        static_cast<unsigned>(program.snapshot->type), program.codeOffset, program.binary.code.size(), program.binary.header.size());
    std::fprintf(file, "# submission=%llu queue=0x%x commandWords=%zu\n",
        static_cast<unsigned long long>(submission.serial), submission.queue, submission.commands.size());
    // Registered snapshots retain code from registration. Compare the entire submitted
    // program at the failure, rather than inferring freshness from the 64-byte band probe.
    const auto codeBytes = std::as_bytes(std::span(program.binary.code));
    GuestMemory::FlushGpuWrites(program.binary.codeAddress, codeBytes.size());
    const auto codeComparison = GuestMemory::CompareMapped(program.binary.codeAddress, codeBytes);
    std::fprintf(file, "# live code comparison bytes=%zu result=%s\n", codeBytes.size(),
        codeComparison == GuestMemory::Compare::Equal ? "equal" :
        codeComparison == GuestMemory::Compare::Differs ? "differs" : "unmapped");
    if (codeComparison == GuestMemory::Compare::Differs) {
        std::vector<std::uint32_t> current(program.binary.code.size());
        if (GuestMemory::CopyMapped(program.binary.codeAddress, std::as_writable_bytes(std::span(current))) == GuestMemory::Compare::Equal) {
            for (std::size_t word = 0; word < current.size(); ++word) {
                if (current[word] == program.binary.code[word]) continue;
                std::fprintf(file, "# first code mismatch word=%zu snapshot=%08x live=%08x\n",
                    word, program.binary.code[word], current[word]);
                break;
            }
        }
    }

    // User data first: it is the data the descriptor was actually decoded from, so it must be on disk
    // before anything that could fault.
    std::fprintf(file, "\n# user data as SGPR numbers: what the recompiler was handed\n");
    for (std::size_t i = 0; i < program.userData.size(); ++i) {
        std::fprintf(file, "sgpr 0x%03x = %08x\n", program.userDataBase + static_cast<std::uint32_t>(i), program.userData[i]);
    }

    std::fprintf(file, "\n# raw shader registers 0x008..0x03f (the fragment window 0x00b..0x024 is inside)\n");
    for (std::uint32_t offset = 0x008u; offset <= 0x03fu; ++offset) {
        std::fprintf(file, "reg 0x%03x = %08x\n", offset, peekRegister(queue.shader, offset));
    }
    std::fprintf(file, "\n# last fragment register command origins (APS5_TRACE_SHADER_ORIGINS)\n");
    for (std::uint32_t offset = 0x008u; offset < queue.fragmentWriteOrigins.size(); ++offset) {
        const auto& origin = queue.fragmentWriteOrigins[offset];
        if (origin.opcode == 0) continue;
        std::fprintf(file, "origin 0x%03x submission=%llu cursor=%llu opcode=0x%x written=%08x current=%08x source=0x%llx",
            offset, static_cast<unsigned long long>(origin.submission), static_cast<unsigned long long>(origin.cursor),
            origin.opcode, origin.value, peekRegister(queue.shader, offset), static_cast<unsigned long long>(origin.sourceAddress));
        if (origin.sourceAddress != 0) {
            std::uint32_t live = 0;
            const auto status = GuestMemory::CopyMapped(origin.sourceAddress, std::as_writable_bytes(std::span(&live, 1)));
            std::fprintf(file, " host-source=%s/%08x", status == GuestMemory::Compare::Equal ? "mapped" : "unmapped", live);
        }
        std::fputc('\n', file);
    }

    std::fprintf(file, "\n# every binding descriptor\n");
    for (std::size_t b = 0; b < result.bindings.size(); ++b) {
        const auto& binding = result.bindings[b];
        std::fprintf(file, "binding[%zu] set=%u number=%u count=%u role=%s dwords=%zu\n", b, binding.descriptorSet, binding.binding,
            binding.count, roleLabel(binding.role), binding.guestDescriptor.size());
        for (std::size_t d = 0; d < binding.guestDescriptor.size(); ++d) {
            const bool oversizedDword = (d % 4u) == 0u && binding.role == SrtRole::GuestBuffers &&
                [&] { const auto candidate = decodeBufferDescriptor(binding.guestDescriptor.data() + d); return candidate.valid && candidate.bytes > MaxStorageBufferRange; }();
            std::fprintf(file, "  [%zu]%s %08x\n", d, oversizedDword ? " <-- OVERSIZED" : "        ", binding.guestDescriptor[d]);
        }
    }

    std::fprintf(file, "\n# guest memory at each descriptor base\n");
    for (const auto& [index, element] : oversized) {
        const auto& binding = result.bindings[index];
        const auto decoded = decodeBufferDescriptor(binding.guestDescriptor.data() + element * 4u);
        std::fprintf(file, "binding[%zu] element %zu: stride=%u numRecords=%u bytes=%llu\n", index, element, decoded.stride,
            decoded.numRecords, static_cast<unsigned long long>(decoded.bytes));
        probeBase(file, decoded.base);
        // Corrected missing-base candidate: waits live just above 2^36 (0x1022...), descriptor bases
        // just below 2^32 (0x20...), and 0x2036f800 + 0x1000000000 = 0x102036f800 lands in the wait
        // band. (The earlier note added 0x10000000000, 2^40, which overshoots to 0x1002036f800.)
        // Mapped with sane bytes here means the descriptor's high address bits were lost.
        std::fprintf(file, "  rebased +0x1000000000 (2^36 band test):\n");
        probeBase(file, decoded.base + RebaseCandidate);
        std::fprintf(file, "  readable prefix of [base, base+bytes): 0x%llx\n",
            static_cast<unsigned long long>(readablePrefix(decoded.base, decoded.bytes)));
    }
    // Band control: the shader's own code address must be readable, and it lives in the same
    // 0x20... band as the descriptor bases. If it ever probes unmapped, the run's low band is
    // gone (or the probe itself is broken), and no "unmapped base" verdict in this dump means
    // anything.
    std::fprintf(file, "band control (shader code, same 0x20... band):\n");
    probeBase(file, program.binary.codeAddress);
    for (std::size_t b = 0; b < result.bindings.size(); ++b) {
        const auto& binding = result.bindings[b];
        if (binding.role == SrtRole::GuestBuffers) continue;
        for (std::size_t d = 0; d + 4u <= binding.guestDescriptor.size(); d += 4u) {
            const auto decoded = decodeBufferDescriptor(binding.guestDescriptor.data() + d);
            if (!decoded.valid || decoded.bytes > MaxStorageBufferRange) continue;
            std::fprintf(file, "binding[%zu] dword %zu (base %llu bytes %llu)\n", b, d, static_cast<unsigned long long>(decoded.base),
                static_cast<unsigned long long>(decoded.bytes));
            probeBase(file, decoded.base);
        }
    }
    std::fprintf(file, "\n# user-data window read as four dword descriptors: if every group decodes to the same\n# stride and record count, the SGPRs themselves are wrong rather than one descriptor being mis-slotted\n");
    for (std::size_t i = 0; i + 4u <= program.userData.size(); i += 4u) {
        const auto decoded = decodeBufferDescriptor(program.userData.data() + i);
        std::fprintf(file, "sgpr 0x%03x group: stride=%u numRecords=%u bytes=%llu base=0x%llx\n", program.userDataBase + static_cast<std::uint32_t>(i),
            decoded.stride, decoded.numRecords, static_cast<unsigned long long>(decoded.bytes), static_cast<unsigned long long>(decoded.base));
    }
    std::fprintf(file, "\n# preceding graphics submission history (bounded to 8 submissions)\n");
    for (const auto& snapshot : queue.fragmentCommandHistory) {
        if (!snapshot || snapshot->submission == submission.serial) continue;
        std::fprintf(file, "history submission=%llu totalWords=%zu capturedWords=%zu\n",
            static_cast<unsigned long long>(snapshot->submission), snapshot->totalWords, snapshot->commands.size());
        for (std::size_t i = 0; i < snapshot->commands.size(); ++i)
            std::fprintf(file, "  history-command[%zu]=%08x\n", i, snapshot->commands[i]);
        for (const auto& [cursor, pairs] : snapshot->registerLists) {
            for (std::size_t i = 0; i + 1 < pairs.size(); i += 2)
                std::fprintf(file, "  history-reg cursor=%zu offset=%08x value=%08x\n", cursor, pairs[i], pairs[i + 1]);
        }
    }
    std::fprintf(file, "\n# failing submission command snapshot (up to 65536 dwords)\n");
    constexpr std::size_t CommandDumpLimit = 65536;
    for (std::size_t cursor = 0; cursor < submission.commands.size() && cursor < CommandDumpLimit;) {
        const auto header = submission.commands[cursor];
        const auto words = Pm4::PacketWords(header);
        if (words > submission.commands.size() - cursor) break;
        std::fprintf(file, "packet cursor=%zu name=%s words=%zu\n", cursor, Pm4::Name(header).c_str(), words);
        for (std::size_t i = 0; i < words && cursor + i < CommandDumpLimit; ++i)
            std::fprintf(file, "  command[%zu]=%08x\n", cursor + i, submission.commands[cursor + i]);
        if (const auto found = submission.registerLists.find(cursor); found != submission.registerLists.end()) {
            for (std::size_t i = 0; i + 1 < found->second.size(); i += 2)
                std::fprintf(file, "  captured-reg[%08x]=%08x\n", found->second[i], found->second[i + 1]);
        }
        cursor += words;
    }
    if (submission.commands.size() > CommandDumpLimit) std::fprintf(file, "# command snapshot truncated\n");
    std::fclose(file);
    std::fprintf(stderr, "[rejected-desc] wrote %s\n", name);
    return true;
}

}


std::shared_ptr<const ShaderRecompiler::RecompileResult> Driver::materializeDrawStage(std::size_t i, std::uint32_t pushOffset, const QueueState& queue, const Submission& submission, const std::vector<DrawProgram>& programs, const Graphics::State& graphics, const ShaderRecompiler::ShaderPixelStageInfo& pixel, const std::vector<std::optional<ShaderRecompiler::ShaderVertexStageInfo>>& vertexInfos, std::vector<ShaderRecompiler::MemoryRegion>& memory, const std::vector<ShaderRecompiler::LinkedProgram>& linked, const Pm4::DrawParameters& drawParameters, const std::shared_ptr<VulkanDevice>& localDevice, ShaderMemory& shaderMemory, std::vector<StageCapture>& stageCaptures, std::vector<bool>& recompiled, bool drawHit, const std::vector<std::shared_ptr<DispatchVariant>>& matched, const std::vector<std::vector<ShaderRecompiler::MemoryRegion>>& matchedRegions, bool profile, std::uint64_t dumpTarget, std::uint64_t dumpSlot1, std::uint64_t& captures, DrawPhaseTiming& phaseTiming, std::array<double, DrawDriverPhaseCount>& phaseMs, std::string& rejected) {
    PerformanceTimer timing("Shader.DrawStage");
    using Stage = ShaderRecompiler::ShaderStage;
    phaseTiming.Phase(DrawRowVectors);
    const auto& program = programs[i];
    const auto waveSize = program.binary.stage == Stage::Fragment ? graphics.stages.fragmentWaveSize : graphics.stages.vertexWaveSize;
    ShaderRecompiler::RecompileRequest request{
        program.binary,
        {waveSize, program.firstUserSgpr, program.userData, std::nullopt, program.binary.stage == Stage::Fragment ? std::optional(pixel) : std::nullopt, vertexInfos[i], memory, RegisteredFloatMode(*program.snapshot)},
        localDevice->Target(),
        {0, 0, pushOffset, (graphics.stages.mesh ? ShaderRecompiler::MeshDrawPushOffsetBytes : graphics.stages.tessellation && pushOffset < Graphics::PipelinePushSlotBytes ? ShaderRecompiler::TessellationLevelPushOffsetBytes : Graphics::PipelinePushSlotBytes) - pushOffset % Graphics::PipelinePushSlotBytes},
        ShaderRecompiler::GraphicsCompileContext{program.firstUserSgpr, linked, graphics.stages.mesh, graphics.stages.tessellation, {drawParameters.indexAddress, drawParameters.indexCount, drawParameters.indexSize, drawParameters.instanceCount}}
    };
    const auto waitedBefore = traceCapSync() || profile ? Graphics::Recorder::ThreadWaitedMs() : 0.0;
    const auto invocation = InvocationFor(*program.snapshot, program.codeOffset, request);
    timing.Mark("prepared_invocation");
    auto& stageCapture = stageCaptures[i];
    stageCapture.forgetSerial = GuestMemory::ForgetSerial();
    stageCapture.pushOffset = pushOffset;
    const auto named = [&](const std::exception& error) {
        std::string dump;
        try {
            dump = dumpRequest(program.binary.codeAddress, request);
        } catch (...) {}
        std::ostringstream where;
        where << "shader 0x" << std::hex << program.binary.codeAddress << " stage " << std::dec << static_cast<unsigned>(program.binary.stage);
        if (!dump.empty()) where << " (" << dump << ")";
        where << ": ";
        return std::runtime_error(where.str() + error.what());
    };
    const auto capture = [&] {
        const SampledReadScope sampling(evidenceReads);
        try {
            return shaderMemory.Capture(invocation);
        } catch (const std::exception& error) {
            memory = shaderMemory.Regions();
            request.context.memory = memory;
            throw named(error);
        }
    }();

    stageCapture.regions = shaderMemory.TakeRecentRegions();
    stageCapture.userDataUse = capture->userDataUse;
    stageCapture.prepared = invocation.Handle();
    stageCapture.bufferBaseWords = capture->bufferBaseWords;
    recompiled[i] = true;
    memory = shaderMemory.Regions();

    for (std::size_t j = 0; j < programs.size(); ++j) {
        if (matched[j] != nullptr && !recompiled[j] && (drawHit || j < i)) memory.insert(memory.end(), matchedRegions[j].begin(), matchedRegions[j].end());
    }
    request.context.memory = memory;
    static const bool dumpShaders = std::getenv("APS5_DUMP_SHADERS") != nullptr;
    if (dumpShaders) static_cast<void>(dumpRequest(program.binary.codeAddress, request));
    if (traceCapSync()) traceCapture("draw-capture", program.binary.codeAddress, submission.queue, memory, Graphics::Recorder::ThreadWaitedMs() - waitedBefore);
    if (profile) {
        ++captures;
        phaseTiming.Phase(DrawRowCapture);

        const auto waited = std::min(Graphics::Recorder::ThreadWaitedMs() - waitedBefore, phaseMs[DrawRowCapture]);
        phaseMs[DrawRowCapture] -= waited;
        phaseMs[DrawRowCaptureHookWaits] += waited;
    }
    if (dumpTarget != 0) {

        const auto slot0 = (static_cast<std::uint64_t>(readRegister(queue.context, 0x390)) << 40u) | (static_cast<std::uint64_t>(readRegister(queue.context, 0x318)) << 8u);
        if ((graphics.hasColorTarget && graphics.color.address == dumpTarget) || slot0 == dumpTarget) static_cast<void>(dumpRequest(program.binary.codeAddress, request));
    }
    if (dumpSlot1 != 0) {
        const auto value = [&](std::uint32_t offset) -> std::uint64_t { const auto it = queue.context.find(offset); return it == queue.context.end() ? 0u : it->second; };
        const auto slot1 = (value(0x391) << 40u) | (value(0x327) << 8u);
        if (slot1 == dumpSlot1) {
            static_cast<void>(dumpRequest(program.binary.codeAddress, request));
            if (std::FILE* file = std::fopen("draw_slot1.regs", "w")) {
                for (const auto& [offset, value] : queue.context) std::fprintf(file, "context %x %08x\n", offset, value);
                for (const auto& [offset, value] : queue.userConfig) std::fprintf(file, "uconfig %x %08x\n", offset, value);
                for (const auto& [offset, value] : queue.shader) std::fprintf(file, "shader %x %08x\n", offset, value);
                std::fclose(file);
            }
        }
    }

    phaseTiming.Phase(DrawRowCapture);

    timing.Mark("capture_resources");
    try {
        stageCapture.compiled = invocation.Materialize(*capture);
    } catch (const std::exception& error) {
        throw named(error);
    }
    stageCapture.capture = capture;
    timing.Mark("materialize");
    if (dumpOversizedDescriptors(program, queue, submission, *stageCapture.compiled)) static_cast<void>(dumpRequest(program.binary.codeAddress, request));
    phaseTiming.Phase(DrawRowRecompile);
    return stageCapture.compiled;
}

void Driver::recordStageDataPositions(DispatchVariant& variant, const ShaderRecompiler::ResourceCapture& capture, const std::vector<Graphics::DecodeRead>& decodeReads) {
    const auto& compiled = *variant.compiled;
    for (std::size_t b = 0; b < compiled.bindings.size(); ++b) {
        if (compiled.bindings[b].role != ShaderRecompiler::DescriptorRole::FlattenedSrt) continue;
        variant.flatBinding = static_cast<std::uint32_t>(b);
        static_cast<void>(DataWordPositions(variant.runs, capture.readTrace.leaves, capture.readTrace.otherReads, variant.words, compiled.bindings[b].guestDescriptor, variant.dataPositions, variant.dataSlots));
        break;
    }
    if (variant.dataPositions.empty() || decodeReads.empty()) return;
    std::vector<bool> decoded(variant.words.size(), false);
    std::size_t offset = 0;
    for (const auto& [begin, end] : variant.runs) {
        for (const auto& read : decodeReads) {
            const auto from = std::max(begin, read.address);
            const auto to = std::min(end, read.address + read.bytes.size());
            if (from >= to) continue;
            for (auto word = (from - begin) / sizeof(std::uint32_t); word < (to - begin + sizeof(std::uint32_t) - 1) / sizeof(std::uint32_t); ++word) decoded[offset + static_cast<std::size_t>(word)] = true;
        }
        offset += static_cast<std::size_t>((end - begin) / sizeof(std::uint32_t));
    }
    std::size_t kept = 0;
    for (std::size_t k = 0; k < variant.dataPositions.size(); ++k) {
        if (decoded[variant.dataPositions[k]]) continue;
        variant.dataPositions[kept] = variant.dataPositions[k];
        variant.dataSlots[kept] = variant.dataSlots[k];
        ++kept;
    }
    variant.dataPositions.resize(kept);
    variant.dataSlots.resize(kept);
}

void Driver::cacheDrawStages(bool useDrawEntries, bool drawHit, const Pm4::DrawParameters& drawParameters, const std::optional<Graphics::IndirectDrawPath>& indirectCpu, const std::vector<DrawProgram>& programs, const std::vector<StageCapture>& stageCaptures, const std::vector<std::optional<ShaderRecompiler::ShaderVertexStageInfo>>& vertexInfos, const std::vector<std::vector<Graphics::DecodeRead>>& decodeReads, bool verifyHit, const std::vector<std::shared_ptr<DispatchVariant>>& matched, std::vector<std::shared_ptr<DispatchVariant>>& fresh, std::uint64_t drawKey, bool registerKey, const std::shared_ptr<const DrawDecode>& decode, DrawPhaseTiming& phaseTiming, const std::vector<bool>& reused, const std::shared_ptr<DrawEntry>& entry, std::uint64_t shapeKey) {
    if (useDrawEntries && !drawHit && !(drawParameters.indirect && indirectCpu) && (verifyHit || admitDrawKey(drawKey, registerKey && drawDataHits() ? shapeKey : 0))) {
        phaseTiming.Phase(DrawRowVectors);
        std::uint64_t unstable = 0, mismatches = 0, differingSameRuns = 0, differingWords = 0, differingRunsChanged = 0;
        for (std::size_t i = 0; i < programs.size(); ++i) {
            const auto& stageCapture = stageCaptures[i];
            if (reused[i]) {
                fresh[i] = matched[i];
                continue;
            }
            if (stageCapture.compiled == nullptr || !CacheableResult(*stageCapture.compiled)) continue;
            auto variant = std::make_shared<DispatchVariant>();
            variant->compiled = stageCapture.compiled;
            if (programs[i].binary.stage == ShaderRecompiler::ShaderStage::Mesh) {
                std::copy_n(programs[i].userData.begin() + ShaderRecompiler::MeshIndexBufferUserWord, variant->meshIndexDescriptor.size(), variant->meshIndexDescriptor.begin());
            }
            variant->shader = programs[i].snapshot;
            variant->forgetSerial = stageCapture.forgetSerial;
            variant->pushOffset = stageCapture.pushOffset;
            variant->prepared = stageCapture.prepared;
            if (vertexInfos[i]) variant->vertexInfo = std::make_shared<const ShaderRecompiler::ShaderVertexStageInfo>(*vertexInfos[i]);

            std::vector<ShaderRecompiler::MemoryRegion> regions(stageCapture.regions.begin(), stageCapture.regions.end());
            for (const auto& read : decodeReads[i]) regions.push_back({read.address, std::as_bytes(std::span(read.bytes))});
            std::stable_sort(regions.begin(), regions.end(), [](const ShaderRecompiler::MemoryRegion& a, const ShaderRecompiler::MemoryRegion& b) { return a.guestAddress < b.guestAddress; });
            tagVariantRuns(*variant, regions, stageCapture.userDataUse, decodeReads[i].empty() && (!vertexInfos[i] || vertexInfos[i]->resourcesNum == 0), programs[i].hiddenUserWords);
            if (drawStageDataHits() && stageCapture.capture != nullptr && !stageCapture.capture->readTrace.leaves.empty()) recordStageDataPositions(*variant, *stageCapture.capture, decodeReads[i]);
            tagBufferWords(*variant, stageCapture.bufferBaseWords);
            if (traceDrawCache() && entry != nullptr && matched[i] == nullptr && i < entry->stages.size()) {
                const auto& variants = entry->stages[i];
                const auto front = std::find_if(variants.begin(), variants.end(), [&](const std::shared_ptr<DispatchVariant>& kept) { return kept->pushOffset == variant->pushOffset; });
                if (front != variants.end() && (*front)->runs == variant->runs && (*front)->words.size() == variant->words.size()) {
                    ++differingSameRuns;
                    for (std::size_t w = 0; w < variant->words.size(); ++w) differingWords += (*front)->words[w] != variant->words[w] ? 1u : 0u;
                } else if (front != variants.end()) {
                    ++differingRunsChanged;
                }
            }
            const auto sameContent = [](const DispatchVariant& a, const DispatchVariant& b) {
                const auto flatten = [](const DispatchVariant& source) {
                    std::vector<std::pair<std::uint64_t, std::uint32_t>> flat;
                    std::size_t word = 0;
                    for (const auto& [begin, end] : source.runs) {
                        for (auto address = begin; address < end && word < source.words.size(); address += sizeof(std::uint32_t)) flat.emplace_back(address, source.words[word++]);
                    }
                    std::sort(flat.begin(), flat.end());
                    return flat;
                };
                return (a.runs == b.runs && a.words == b.words) || flatten(a) == flatten(b);
            };
            const auto sameBindings = [&](const ShaderRecompiler::RecompileResult& a, const ShaderRecompiler::RecompileResult& b) {
                if (a.bindings.size() != b.bindings.size()) return false;
                for (std::size_t binding = 0; binding < a.bindings.size(); ++binding) {
                    if (a.bindings[binding].role == ShaderRecompiler::DescriptorRole::ShaderData) continue;
                    if (a.bindings[binding].guestDescriptor != b.bindings[binding].guestDescriptor) return false;
                }
                return true;
            };
            const auto sameForwardedWords = [](const ShaderRecompiler::RecompileResult& a, const std::vector<std::byte>& push, const ShaderRecompiler::RecompileResult& b, std::uint32_t hidden) {
                if (push.size() != b.pushConstants.size() || a.shaderDataUserWords != b.shaderDataUserWords) return false;
                for (std::size_t slot = 0; slot < a.shaderDataUserWords.size() && (slot + 1) * sizeof(std::uint32_t) <= push.size(); ++slot) {
                    if (a.shaderDataUserWords[slot] >= hidden && std::memcmp(push.data() + slot * sizeof(std::uint32_t), b.pushConstants.data() + slot * sizeof(std::uint32_t), sizeof(std::uint32_t)) != 0) return false;
                }
                return true;
            };
            if (verifyHit && matched[i] != nullptr && (!sameContent(*matched[i], *variant) || (programs[i].hiddenUserWords == 0 && !sameBindings(*matched[i]->compiled, *variant->compiled)) || !sameForwardedWords(*matched[i]->compiled, matched[i]->patchedPushConstants.empty() ? matched[i]->compiled->pushConstants : matched[i]->patchedPushConstants, *variant->compiled, programs[i].hiddenUserWords))) {
                ++mismatches;
                static std::atomic<std::uint64_t> reports{0};
                if (reports.fetch_add(1) < 20) std::fprintf(stderr, "[draw-cache] verify: stage %zu (program 0x%llx) of a hit captured differently: %zu runs / %zu words matched, %zu / %zu fresh\n", i, static_cast<unsigned long long>(programs[i].binary.codeAddress), matched[i]->runs.size(), matched[i]->words.size(), variant->runs.size(), variant->words.size());
            }
            if (insertCompare()) {
                const GuestMemory::ReadSiteScope site(GuestMemory::ReadSite::DrawCache);
                if (!captureStable(stageCapture.regions)) {
                    ++unstable;
                    continue;
                }
            }
            fresh[i] = std::move(variant);
        }
        insertDrawEntry(drawKey, fresh, registerKey ? decode : nullptr, registerKey && drawDataHits() ? shapeKey : 0);
        std::uint64_t dataInserts = 0, dataPositions = 0;
        for (std::size_t i = 0; i < programs.size(); ++i) {
            if (fresh[i] == nullptr || reused[i] || fresh[i]->dataPositions.empty()) continue;
            ++dataInserts;
            dataPositions += fresh[i]->dataPositions.size();
        }
        if (unstable != 0 || mismatches != 0 || differingSameRuns != 0 || differingRunsChanged != 0 || dataInserts != 0) {
            std::lock_guard cacheLock(drawCacheMutex);
            drawEntryCounters.stageDataInserts += dataInserts;
            drawEntryCounters.stageDataPositions += dataPositions;
            drawEntryCounters.unstable += unstable;
            drawEntryCounters.verifyMismatches += mismatches;
            drawEntryCounters.differingSameRuns += differingSameRuns;
            drawEntryCounters.differingWords += differingWords;
            drawEntryCounters.differingRunsChanged += differingRunsChanged;
        }
        phaseTiming.Phase(DrawRowKeyLookupValidate);
    }
}

}
