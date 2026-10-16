#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "ThreadOwned.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/SynchronizationStatistics.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/DeferredLabels.hpp"
#include "prx/libSceAgcDriver/Execution/include/CaptureTrace.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libSceAgcDriver/Execution/include/IndexTraceRing.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include "prx/libSceAgcDriver/Eq/include/Event.hpp"
#include <cstdlib>
#include <functional>
#include <shared_mutex>

namespace AgcDriver::DriverDetail {

namespace {

bool traceIndexBase() {
    static const bool enabled = std::getenv("APS5_TRACE_INDEX_BASE") != nullptr;
    return enabled;
}

void traceIndexPacket(const Submission& submission, std::size_t cursor, std::span<const std::uint32_t> packet, const QueueState& queue) {
    if (!traceIndexBase()) return;
    const auto opcode = (packet[0] >> 8u) & 0xffu;
    bool relevant = Pm4::DrawOpcode(opcode) || opcode == 0x11 || opcode == 0x13 || opcode == 0x26 || opcode == 0x2a || opcode == 0x91 || (opcode == 0x10 && ((packet[0] >> 2u) & 0x3fu) == 9);
    if (opcode == 0x79 || opcode == 0x7a) {
        const auto base = packet[1] & ~0x70000000u;
        relevant = base <= 0x243 && 0x243 - base < packet.size() - 2;
    } else if (opcode == 0x64) {
        const auto& pairs = submission.registerLists.at(cursor);
        for (std::size_t i = 0; i < pairs.size(); i += 2) relevant |= (pairs[i] & ~0x70000000u) == 0x243;
    }
    if (!relevant) return;
    std::string words;
    for (const auto word : packet) {
        char text[16];
        std::snprintf(text, sizeof(text), " %08x", word);
        words += text;
    }
    const auto source = submission.indexCommandSources.find(cursor);
    AgcDriver::indexTrace("[index-base] packet serial=%llu queue=0x%x cursor=%zu source=0x%llx op=%s base=0x%llx limitIndices=%u type=%u primitive=%u raw:%s\n", static_cast<unsigned long long>(submission.serial), submission.queue, cursor, static_cast<unsigned long long>(source != submission.indexCommandSources.end() ? source->second : 0), Pm4::Name(packet[0]).c_str(), static_cast<unsigned long long>(queue.indexBase), queue.indexBufferSize, queue.indexType, queue.userConfig.contains(0x242) ? queue.userConfig.at(0x242) : 0u, words.c_str());
}

bool orderedFills() {
    static const bool ordered = std::getenv("APS5_PIPELINE_DRAIN_FILLS") == nullptr;
    return ordered;
}

bool keepsDrawPipeline(std::span<const std::uint32_t> packet, std::uint32_t header, std::uint32_t opcode) {
    if (header == FlipPacketHeader || header == RenderingWaitPacketHeader) return false;
    if (Pm4::DrawOpcode(opcode)) return true;
    switch (opcode) {
        case 0x11: case 0x12: case 0x13: case 0x26: case 0x2a: case 0x2f: case 0x42: case 0x58: case 0x59: case 0x69: case 0x76: case 0x79: case 0x7a: case 0x81: return true;
        case 0x46: return packet.size() >= 2 && (packet[1] & 0x3fu) != 0x39u;
        case 0x15: return orderedFills();
        case 0x63: case 0x64: case 0x9f:
            return packet.size() >= 5 && !DrawPipeline::Queue0().Overlaps(packet[1] | (static_cast<std::uint64_t>(packet[2]) << 32u), static_cast<std::size_t>(packet[4]) * 8);
        case 0x10: {
            const auto operation = (header >> 2u) & 0x3fu;
            return operation == 0 || operation == 0x09 || operation == 0x0b || operation == 0x0c || operation == 0x1a;
        }
        default: return false;
    }
}

}

void checkDeferredShadersOnShRegWrite(std::uint32_t opcode, std::span<const std::uint32_t> packet, std::size_t cursor, const Submission& submission, const std::shared_ptr<VulkanDevice>& device, const QueueState& queue) {
    if (submission.shaders == nullptr || device == nullptr) return;
    if (opcode == 0x76) {
        const auto regOffset = packet[1] & ~0x70000000u;
        const auto regCount = static_cast<std::uint32_t>(packet.size() - 2);
        if (regOffset <= 0x20du && regOffset + regCount > 0x20cu) TriggerDeferredShaderPreparation(*submission.shaders, device, queue, 0x20cu, {0});
        if (regOffset <= 0x009u && regOffset + regCount > 0x008u) TriggerDeferredShaderPreparation(*submission.shaders, device, queue, 0x008u, {1});
        if (regOffset <= 0x0c9u && regOffset + regCount > 0x0c8u) TriggerDeferredShaderPreparation(*submission.shaders, device, queue, 0x0c8u, {2, 4});
        if (regOffset <= 0x089u && regOffset + regCount > 0x088u) TriggerDeferredShaderPreparation(*submission.shaders, device, queue, 0x088u, {6});
        if (regOffset <= 0x109u && regOffset + regCount > 0x108u) TriggerDeferredShaderPreparation(*submission.shaders, device, queue, 0x108u, {7});
    } else if (opcode == 0x63) {
        const auto& pairs = submission.registerLists.at(cursor);
        bool checkCompute = false, checkFragment = false, checkVertex = false, checkMesh = false, checkTess = false;
        for (std::size_t i = 0; i < pairs.size(); i += 2) {
            const auto reg = pairs[i] & ~0x70000000u;
            if (reg == 0x20cu || reg == 0x20du) checkCompute = true;
            else if (reg == 0x008u || reg == 0x009u) checkFragment = true;
            else if (reg == 0x0c8u || reg == 0x0c9u) checkVertex = true;
            else if (reg == 0x088u || reg == 0x089u) checkMesh = true;
            else if (reg == 0x108u || reg == 0x109u) checkTess = true;
        }
        if (checkCompute) TriggerDeferredShaderPreparation(*submission.shaders, device, queue, 0x20cu, {0});
        if (checkFragment) TriggerDeferredShaderPreparation(*submission.shaders, device, queue, 0x008u, {1});
        if (checkVertex) TriggerDeferredShaderPreparation(*submission.shaders, device, queue, 0x0c8u, {2, 4});
        if (checkMesh) TriggerDeferredShaderPreparation(*submission.shaders, device, queue, 0x088u, {6});
        if (checkTess) TriggerDeferredShaderPreparation(*submission.shaders, device, queue, 0x108u, {7});
    }
}

template <typename TWork>
void Driver::timed(double WorkerProfile::*bucket, TWork&& work) {
    static thread_local WorkerProfile profile;
    const auto begin = std::chrono::steady_clock::now();
    work();
    const auto end = std::chrono::steady_clock::now();
    profile.*bucket += std::chrono::duration<double, std::milli>(end - begin).count();
    static const bool report = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    if (report && end - profile.reported > std::chrono::seconds(10)) {
        profile.reported = end;
        AgcDriver::ProfilePrint_nid_no_patch( "[gpu] worker at %.0f s: dispatch %.1f s, draw %.1f s, wait %.1f s\n", std::chrono::duration<double>(end - profile.start).count(), profile.dispatchMs / 1000, profile.drawMs / 1000, profile.waitMs / 1000);
    }
}

void Driver::execute(const Submission& submission) {
    auto* submissionTiming = includeTimingSubmission(submission, true);
    struct FlipHold {
        Driver& driver;
        bool held;
        void Release() {
            if (!held) return;
            held = false;
            driver.releaseFlipHold();
        }
        ~FlipHold() { Release(); }
    } flipHold{*this, submission.holdsFlip};
    const bool pipelined = submission.queue == 0 && DrawPipeline::Depth() != 0;
    static const bool orderedWaits = std::getenv("APS5_PIPELINE_DRAIN_LABELS") == nullptr;
    DrawPipeline::Active() = pipelined;
    struct PipelineDrain {
        bool pipelined;
        ~PipelineDrain() {
            if (!pipelined || std::uncaught_exceptions() == 0) return;
            try {
                DrawPipeline::Queue0().Drain(DrawPipeline::DrainReason::Submission);
            } catch (...) {
            }
        }
    } pipelineDrain{pipelined};
    static const bool orderedCompletion = std::getenv("APS5_PIPELINE_DRAIN_SUBMISSIONS") == nullptr && orderedWaits;
    if (pipelined && (submission.suspend || !orderedCompletion)) DrawPipeline::Queue0().Drain(DrawPipeline::DrainReason::Submission);
    if (submission.suspend) {
        PerformanceContext timingContext(submissionTiming);
        PerformanceTimer timing("Driver.Suspend");

        static const bool suspendDrain = std::getenv("APS5_SUSPEND_DRAIN") != nullptr;
        static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
        ++suspendPoints;
        auto& costs = submissionCosts(submission.queue);

        if (suspendDrain || !deferredLabels().labels.empty() || Graphics::Recorder::PendingLabelSince().has_value() || Graphics::Recorder::RecordedWorkSinceSubmit() != 0) {
            const auto start = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Flush);
            std::lock_guard gpuLock(GuestMemory::GpuMutex());
            const auto localDevice = device.Load();
            recordDeferredLabels(localDevice.get(), submission.queue);
            if (localDevice != nullptr) {
                if (suspendDrain) localDevice->WaitIdle();
                else localDevice->SubmitRecorded(submission.queue == 0);
            }
            ++costs.suspends;
            if (profile) costs.suspendNs += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
        } else {
            ++costs.suspendsSkipped;
        }
        if (traceIndexBase()) AgcDriver::indexTrace("[index-base] suspend serial=%llu queue=0x%x resetGraphics=1\n", static_cast<unsigned long long>(submission.serial), submission.queue);
        resetGraphics = true;
        return;
    }
    QueueState* state = nullptr;
    {
        std::lock_guard lock(mutex);
        if (submission.queue == 0 && resetGraphics) {
            queues.erase(0);
            resetGraphics = false;
        }
        state = &queues[submission.queue];
    }
    auto& queue = *state;
    if (traceIndexBase()) AgcDriver::indexTrace("[index-base] begin serial=%llu queue=0x%x base=0x%llx limitIndices=%u type=%u\n", static_cast<unsigned long long>(submission.serial), submission.queue, static_cast<unsigned long long>(queue.indexBase), queue.indexBufferSize, queue.indexType);
    static const bool traceShaderOrigins = std::getenv("APS5_TRACE_SHADER_ORIGINS") != nullptr;
    if (traceShaderOrigins && submission.queue == 0) {
        constexpr std::size_t HistoryWordLimit = 65536;
        auto snapshot = std::make_shared<QueueState::ShaderCommandSnapshot>();
        snapshot->submission = submission.serial;
        snapshot->totalWords = submission.commands.size();
        const auto copiedWords = std::min(HistoryWordLimit, submission.commands.size());
        snapshot->commands.assign(submission.commands.begin(), submission.commands.begin() + copiedWords);
        std::size_t remainingPairs = HistoryWordLimit;
        for (const auto& [cursor, pairs] : submission.registerLists) {
            if (cursor >= copiedWords || remainingPairs == 0) break;
            const auto count = std::min(remainingPairs, pairs.size());
            snapshot->registerLists.emplace(cursor, std::vector<std::uint32_t>(pairs.begin(), pairs.begin() + count));
            remainingPairs -= count;
        }
        for (std::size_t i = 1; i < queue.fragmentCommandHistory.size(); ++i)
            queue.fragmentCommandHistory[i - 1] = std::move(queue.fragmentCommandHistory[i]);
        queue.fragmentCommandHistory.back() = std::move(snapshot);
    }
    static const bool traceGpu = std::getenv("APS5_TRACE_GPU") != nullptr;
    if (traceGpu) std::fprintf(stderr, "[gpu] %.1f execute serial=%llu queue=0x%x dwords=%zu\n", TraceMs(), static_cast<unsigned long long>(submission.serial), submission.queue, submission.commands.size());

    static const long dumpQueue = [] { const char* text = std::getenv("APS5_DUMP_QUEUE"); return text ? std::strtol(text, nullptr, 16) : -1L; }();
    if (static_cast<long>(submission.queue) == dumpQueue) {

        static int dumped = 0;
        if (dumped++ < 40) {
            std::string text = "[queue] submission " + std::to_string(submission.serial) + ":\n";
            for (std::size_t cursor = 0; cursor < submission.commands.size();) {
                const auto header = submission.commands[cursor];
                const auto count = Pm4::PacketWords(header);
                char line[200];
                int length = std::snprintf(line, sizeof(line), "[queue]   %s", Pm4::Name(header).c_str());
                for (std::size_t i = 1; i < count && i < 10 && length < 180; ++i) length += std::snprintf(line + length, sizeof(line) - length, " %08x", submission.commands[cursor + i]);
                text += line;
                text += "\n";
                cursor += count;
            }
            std::fputs(text.c_str(), stderr);
        }
    }
    PacketHistory recent{submission.commands};

    static const bool profilePackets = std::getenv("APS5_PROFILE_DRAW") != nullptr;

    thread_local PacketProfile* packetProfileSlot = nullptr;
    auto& packetProfile = ShaderRecompiler::ThreadOwned(packetProfileSlot);
    ++packetProfile.submissions;

    static const bool epochEach = std::getenv("APS5_SUBMISSION_EPOCH_EACH") != nullptr;
    if (epochEach || submission.enqueuedAt == std::chrono::steady_clock::time_point{} || submission.enqueuedAt > lastEpochBump()) {
        if (!epochEach) noteReleasedTails(submission);
        bumpEpoch(&EpochBumps::submissions);
    }
    clearResolvedAhead();
    gateOpened() = true;
    struct ClearAhead {
        ~ClearAhead() { clearResolvedAhead(); }
    } clearAhead;
    for (std::size_t cursor = 0; cursor < submission.commands.size();) {
        if (packetEpoch()) bumpEpoch(&EpochBumps::packets);
        CheckFailure();
        const auto header = submission.commands[cursor];
        if (Pm4::FillerPacket(header)) { ++cursor; continue; }
        const auto count = Pm4::PacketWords(header);
        const auto packet = std::span(submission.commands).subspan(cursor, count);
        const auto opcode = (header >> 8u) & 0xffu;
        auto nextCursor = cursor + count;
        if (APS5_ENABLE_TIMING_LOG && submission.queue == 0 && pendingFrameTiming == nullptr) includeTimingSubmission(submission, false);
        PerformanceContext timingContext(submission.queue == 0 ? pendingFrameTiming.get() : FrameTiming::Async());
        PerformanceTimer timing("Driver.Packet");
        const bool predicated = Pm4::Predicated(header) && queue.predication.operation != 0;
        if (pipelined && !predicated && header != FlipPacketHeader && header != RenderingWaitPacketHeader && (enqueueLabelPacket(packet, opcode, submission.queue) || enqueueDmaPacket(packet, opcode, submission.queue, queue))) {
            traceLabel(packet, submission.queue);
            recent.Record(cursor);
            cursor = nextCursor;
            continue;
        }
        if (pipelined && !predicated && (opcode == 0x3c || opcode == 0x93) && header != RenderingWaitPacketHeader && orderedWaits && DrawPipeline::Queue0().Busy()) {
            const std::uint64_t awaited = packet[2] | (static_cast<std::uint64_t>(packet[3]) << 32u);
            if (const auto value = DrawPipeline::Queue0().PendingLabel(awaited, Pm4::WaitAwaitedBytes(packet)); value.has_value() && Pm4::WaitComparesValue(packet, *value)) {
                ++waitOutcomes().fromRecorderSameQueue;
                gateOpened() = true;
                recent.Record(cursor);
                cursor = nextCursor;
                continue;
            }
        }
        if (pipelined && DrawPipeline::Queue0().Busy() && !keepsDrawPipeline(packet, header, opcode)) DrawPipeline::Queue0().Drain(DrawPipeline::DrainReason::Packet, header == FlipPacketHeader ? 0xffu : opcode);
        if (pipelined && predicated && DrawPipeline::Queue0().Busy() && DrawPipeline::Queue0().Overlaps(queue.predication.address, queue.predication.operation == 3 ? 8 : 4)) DrawPipeline::Queue0().Drain(DrawPipeline::DrainReason::Packet, opcode);
        std::shared_lock deviceUse(deviceReplacement, std::defer_lock);
        if (opcode != 0x3c && opcode != 0x93 && header != RenderingWaitPacketHeader && header != FlipPacketHeader) deviceUse.lock();

        timing.Mark("device_lock");
        GuestMemory::SetCurrentPacket(header == FlipPacketHeader ? 0xffffu : opcode, submission.queue);
        CaptureTrace::Log("packet submission=%llu queue=%x offset=%zu header=%08x words=%zu", static_cast<unsigned long long>(submission.serial), submission.queue, cursor, header, packet.size());
        if (predicated) {
            landPendingWaits(submission.queue);
            recordQueuedLabelsBeforeRead(submission.queue);
            if (!Pm4::PredicationPasses(queue)) {
                cursor = opcode == 0x3f ? submission.conditionalEnds.at(cursor) : nextCursor;
                continue;
            }
        }
        if (opcode == 0x3f) {
            cursor = nextCursor;
            continue;
        }

        const auto flushStart = profilePackets ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        flushBetweenPackets(submission.queue, header, opcode == 0x49 || opcode == 0x37);
        timing.Mark("flush_between_packets");
        PacketTimer packetTimer{profilePackets, header == FlipPacketHeader ? 0xffffu : opcode, submission.queue, packetProfile, std::chrono::steady_clock::now()};

        if (profilePackets) {
            packetStartedAt() = packetTimer.start;
            pendingDispatchPhases() = {};
            pendingDrawPhases() = {};
        }
        const auto finishDispatchPacket = [&](bool indirect) {
            if (!profilePackets) return;
            const auto now = std::chrono::steady_clock::now();
            auto& pending = pendingDispatchPhases();
            if (pending.phases) {
                pending.ms[PhaseEpilogue] = std::chrono::duration<double, std::milli>(now - pending.tailAt).count();
                addDriverPhases(submission.queue != 0 ? OtherQueues : indirect ? Queue0Indirect : Queue0Direct, pending.ms, pending.hit, pending.validated);
            }
            if (indirect) return;
            auto& row = packetProfile.dispatchOutcomes[static_cast<std::size_t>(pending.outcome)];
            ++row.first;
            row.second += std::chrono::duration<double, std::milli>(now - packetTimer.start).count();
        };
        const auto finishDrawPacket = [&](bool drawn) {
            if (!profilePackets) return;
            const auto now = std::chrono::steady_clock::now();
            auto& pending = pendingDrawPhases();
            std::array<double, DrawDriverPhaseCount> ms{};
            if (drawn && pending.phases) {
                ms = pending.ms;
                ms[DrawRowEpilogue] = std::chrono::duration<double, std::milli>(now - pending.tailAt).count();
            } else {
                ms[DrawRowSkipped] = std::chrono::duration<double, std::milli>(now - packetTimer.start).count();
            }
            addDrawPhases(ms, drawn && pending.phases, pending.captures);
        };
        if (profilePackets) packetProfile.flushMs += std::chrono::duration<double, std::milli>(packetTimer.start - flushStart).count();

        bool wroteOnGpu = false, endOfPipeInterrupt = false, interruptDeferred = false, drawPacket = false, sampleDump = false;
        const bool drains = preparePacketMemory(submission, queue, packet, header, opcode, wroteOnGpu, endOfPipeInterrupt, interruptDeferred, drawPacket, sampleDump);
        traceLabel(packet, submission.queue);
        timing.Mark("prepare_memory");

        const bool waitPacket = opcode == 0x3c || opcode == 0x93 || header == RenderingWaitPacketHeader;
        struct Progress {
            Driver& driver;
            bool counted;
            ~Progress() {
                if (!counted) return;
                --driver.packetsInFlight;
                ++driver.packetsDone;
            }
        } progress{*this, !waitPacket};
        if (!waitPacket) ++packetsInFlight;
        recent.Record(cursor);
        if (header == RenderingWaitPacketHeader) {
            timed(&WorkerProfile::waitMs, [&] { submission.renderingWaits.at(cursor)->Wait(); });
        } else if (submission.flips.contains(cursor)) {
            if (opcode == 0x49) {
                if (!wroteOnGpu) {
                    Pm4::Execute(packet, queue);
                    if (const auto label = Pm4::DecodeLabelWrite(packet)) noteLabelStore(label->address, label->Bytes(), ++eventSerial, submission.queue);
                }
                if (endOfPipeInterrupt && !interruptDeferred) AgcDriverDeliverEopInterrupt(submission.queue);
            }
            if (deviceUse.owns_lock()) deviceUse.unlock();
            CheckFailure();
            std::uint64_t batchesAtFlip = 0, unsignaledAtFlip = 0;
            bool flipInterruptDeferred = false;
            if (!drains) {

                GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Flush);
                std::lock_guard gpuLock(GuestMemory::GpuMutex());
                const auto localDevice = device.Load();

                if (!recordLabelsForPacket(localDevice.get(), submission.queue) && localDevice != nullptr) localDevice->SubmitRecorded(submission.queue == 0);
                if (localDevice != nullptr) {
                    localDevice->FlipBatches(batchesAtFlip, unsignaledAtFlip);
                    const auto queueId = submission.queue;
                    flipInterruptDeferred = localDevice->AfterRecordedWork([queueId] { AgcDriverDeliverEopInterrupt(queueId); }, false);
                }
            }
            if (!flipInterruptDeferred) AgcDriverDeliverEopInterrupt(submission.queue);
            ++flipsCounted;
            if (batchesAtFlip != 0) flipSerial = batchesAtFlip;
            flipBatchesUnsignaled += unsignaledAtFlip;

            timing.Mark("flip_flush");
            timing.FinishPacket(submission.serial, cursor, 0xffffu, count);
            auto frame = APS5_ENABLE_TIMING_LOG ? std::move(pendingFrameTiming) : std::make_shared<FrameTiming>(++frameSerial);
            require(frame != nullptr, "flip has no frame timing");
            const auto now = FrameTiming::Clock::now();
            if (!APS5_ENABLE_TIMING_LOG) frame->IncludeSubmission(submission.serial, now, now, now, true);
            frame->SetFlip(submission.serial, cursor, APS5_ENABLE_TIMING_LOG ? submission.receivedAt : now, now);
            if (APS5_ENABLE_TIMING_LOG) frame->CollectBackground();
            frame->NoteFlipBatches(batchesAtFlip, unsignaledAtFlip);
            CaptureTrace::Log("flip frame=%llu submission=%llu offset=%zu batch=%llu unsignaled=%llu", static_cast<unsigned long long>(frameSerial), static_cast<unsigned long long>(submission.serial), cursor, static_cast<unsigned long long>(batchesAtFlip), static_cast<unsigned long long>(unsignaledAtFlip));
            flipHold.Release();
            submission.flips.at(cursor)->GpuReady(frame);
        } else if (opcode == 0x15 || opcode == 0x16) {
            landPendingWaits(submission.queue);
            currentPacketOffset() = cursor;
            if (gateOpened()) {
                gateOpened() = false;
                if (!pipelineDispatch() || submission.queue != 0) {
                    if (pipelined && DrawPipeline::Queue0().Busy()) DrawPipeline::Queue0().Drain(DrawPipeline::DrainReason::Packet, opcode);
                    timed(&WorkerProfile::dispatchMs, [&] { resolveGroupAhead(submission, queue, cursor); });
                }
            }
            if (opcode == 0x15) timed(&WorkerProfile::dispatchMs, [&] { dispatch(queue, packet, submission); });
            else timed(&WorkerProfile::dispatchMs, [&] { dispatchIndirect(queue, packet, submission); });
            Pm4::NoteDispatchRegisters(packet, queue);
            Graphics::Recorder::CountRecordedWork();
            finishDispatchPacket(opcode == 0x16);
        } else if (opcode == 0x3c || opcode == 0x93) {
            static const bool traceGpu = std::getenv("APS5_TRACE_GPU") != nullptr;
            const auto waitStart = std::chrono::steady_clock::now();
            timed(&WorkerProfile::waitMs, [&] { waitMemory(packet, submission.queue, recent, submission.received, submission.heldAtSubmit.contains(cursor)); });
            gateOpened() = true;
            if (traceGpu && std::chrono::steady_clock::now() - waitStart > std::chrono::milliseconds(200)) {

                for (std::size_t next = cursor + count, shown = 0; next < submission.commands.size() && shown < 48; ++shown) {
                    const auto nextHeader = submission.commands[next];
                    const auto nextCount = Pm4::PacketWords(nextHeader);
                    const auto nextPacket = std::span(submission.commands).subspan(next, nextCount);
                    std::fprintf(stderr, "[gpu]   then %s", Pm4::Name(nextHeader).c_str());
                    for (std::size_t i = 1; i < nextPacket.size() && i < 7; ++i) std::fprintf(stderr, " %08x", nextPacket[i]);
                    std::fprintf(stderr, "\n");
                    next += nextCount;
                }
            }
        } else if (drawPacket) {
            landPendingWaits(submission.queue);
            traceIndexPacket(submission, cursor, packet, queue);
            bool drawn = false;
            timed(&WorkerProfile::drawMs, [&] {
                static const bool traceDraws = std::getenv("APS5_TRACE_DRAWS") != nullptr;
                static const bool profileDraws = std::getenv("APS5_PROFILE_DRAW") != nullptr;
                const auto color = (static_cast<std::uint64_t>(readRegister(queue.context, 0x390)) << 40u) | (static_cast<std::uint64_t>(readRegister(queue.context, 0x318)) << 8u);
                const auto started = profileDraws ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                const auto countSkip = [&](Graphics::DrawSkip kind) {
                    if (profileDraws) Graphics::CountDrawSkip(kind, std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started).count());
                };

                try {
                    std::string rejected;
                    const auto verdict = draw(queue, packet, submission, rejected);
                    drawn = verdict == DrawVerdict::Drawn;
                    CaptureTrace::Log("draw submission=%llu queue=%x offset=%zu target=%llx mask=%x verdict=%d reason=%.256s", static_cast<unsigned long long>(submission.serial), submission.queue, cursor, static_cast<unsigned long long>(color), readRegister(queue.context, 0x8e), static_cast<int>(verdict), rejected.c_str());
                    if (verdict == DrawVerdict::Rejected) {
                        countSkip(Graphics::DrawSkip::Prechecked);
                        throw std::runtime_error(rejected);
                    } else if (verdict == DrawVerdict::Nothing) {
                        countSkip(Graphics::DrawSkip::Nothing);
                    } else if (traceDraws) {
                        std::fprintf(stderr, "[draw] target 0x%llx mask 0x%x ok\n",static_cast<unsigned long long>(color), readRegister(queue.context, 0x8e));
                    }
                } catch (const std::exception& error) {
                    CaptureTrace::Log("draw-error submission=%llu offset=%zu reason=%.256s", static_cast<unsigned long long>(submission.serial), cursor, error.what());
                    if (traceIndexBase()) AgcDriver::indexTrace("[index-base] failure serial=%llu queue=0x%x cursor=%zu error=%s\n", static_cast<unsigned long long>(submission.serial), submission.queue, cursor, error.what());
                    countSkip(Graphics::DrawSkip::Thrown);
                    throw;
                }
            });
            finishDrawPacket(drawn);
        } else if (opcode == 0x22) {
            landPendingWaits(submission.queue);
            recordQueuedLabelsBeforeRead(submission.queue);
            const auto condition = Pm4::ReadCondition(packet);
            if (condition == 0) nextCursor = submission.conditionalEnds.at(cursor);
            if (traceGpu) std::fprintf(stderr, "[gpu] %.1f queue 0x%x COND_EXEC at DWORD %zu reads 0x%x at 0x%llx: %s %zu dwords\n", TraceMs(), submission.queue, cursor, condition, static_cast<unsigned long long>(packet[1] | (static_cast<std::uint64_t>(packet[2]) << 32u)), condition == 0 ? "skips" : "executes", submission.conditionalEnds.at(cursor) - cursor - count);
        } else if (opcode == 0x91) {
            landPendingWaits(submission.queue);
            recordQueuedLabelsBeforeRead(submission.queue);
            Pm4::Execute(packet, queue);
            traceIndexPacket(submission, cursor, packet, queue);
        } else if (sampleDump && !wroteOnGpu) {
            dumpSampleCounters(Pm4::SampleDumpAddress(packet));
        } else if (opcode != 0x42 && opcode != 0x46 && opcode != 0x58) {
            if (!wroteOnGpu) {
                if (traceShaderOrigins && (opcode == 0x76 || opcode == 0x63) && !Pm4::IsTagMarker(packet)) {
                    const auto remember = [&](std::uint32_t offset, std::uint32_t value, std::uint64_t source) {
                        offset &= ~0x70000000u;
                        if (offset < queue.fragmentWriteOrigins.size())
                            queue.fragmentWriteOrigins[offset] = {submission.serial, cursor, source, opcode, value};
                    };
                    if (opcode == 0x76) {
                        const auto base = packet[1] & ~0x70000000u;
                        for (std::size_t i = 2; i < packet.size(); ++i)
                            remember(base + static_cast<std::uint32_t>(i - 2), packet[i], 0);
                    } else {
                        const auto& pairs = submission.registerLists.at(cursor);
                        const auto source = packet[1] | (static_cast<std::uint64_t>(packet[2]) << 32u);
                        const bool interleaved = packet[3] == 0x80000000u;
                        for (std::size_t i = 0; i < pairs.size(); i += 2)
                            remember(pairs[i], pairs[i + 1], source + (interleaved ? i + 1 : i / 2) * sizeof(std::uint32_t));
                    }
                }
                if (Pm4::IndirectRegisterOpcode(opcode)) Pm4::ExecuteIndirectRegisters(packet, submission.registerLists.at(cursor), queue);
                else Pm4::Execute(packet, queue);
                traceIndexPacket(submission, cursor, packet, queue);
                if (opcode == 0x49 || opcode == 0x37) {
                    if (const auto label = Pm4::DecodeLabelWrite(packet)) noteLabelStore(label->address, label->Bytes(), ++eventSerial, submission.queue);
                }
                if (opcode == 0x76 || opcode == 0x63) {
                    checkDeferredShadersOnShRegWrite(opcode, packet, cursor, submission, device.Load(), queue);
                }
            }
            if (endOfPipeInterrupt && !interruptDeferred) AgcDriverDeliverEopInterrupt(submission.queue);
        }
        if (drawPacket || (sampleDump && wroteOnGpu)) Graphics::Recorder::CountRecordedWork();
        timing.Mark(header == RenderingWaitPacketHeader ? "rendering_wait" : opcode == 0x3c || opcode == 0x93 ? "memory_wait" : opcode == 0x15 || opcode == 0x16 ? "dispatch" : drawPacket ? "draw" : "execute");
        if (submission.queue == 0) timing.FinishPacket(submission.serial, cursor, opcode, count);
        else timing.Finish();
        cursor = nextCursor;
    }

    PerformanceContext endContext(submission.queue == 0 ? frameTiming() : FrameTiming::Async());
    PerformanceTimer endTiming("Driver.EndSubmission");
    static const bool submitAtEnd = std::getenv("APS5_SUBMIT_AT_END") != nullptr;
    if (pipelined && orderedCompletion && deferredLabels().labels.empty()) {
        if (submission.rewindTail == nullptr) {
            deferCompletion(submission);
            return;
        }
        if ((std::atomic_ref<std::uint32_t>(*const_cast<std::uint32_t*>(submission.rewindTail - 1)).load(std::memory_order_acquire) & 0x80000000u) != 0) {
            DrawPipeline::Queue0().Enqueue([this] { submitOpenWork(); }, {});
            executeRewindTail(submission);
            return;
        }
    }
    if (pipelined) DrawPipeline::Queue0().Drain(DrawPipeline::DrainReason::Submission);
    if (!deferredLabels().labels.empty() || Graphics::Recorder::PendingLabelSince().has_value() || Graphics::Recorder::RecordedWorkSinceSubmit() != 0) {
        auto& costs = submissionCosts(submission.queue);
        if (!submitAtEnd && submission.rewindTail == nullptr && submission.queue == 0 && workerQueued() != nullptr && workerQueued()->load(std::memory_order_acquire) != 0) {
            ++costs.endSkipped;
            return;
        }
        ++costs.endSubmits;
        GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::End);
        std::lock_guard gpuLock(GuestMemory::GpuMutex());
        const auto localDevice = device.Load();

        recordDeferredLabels(localDevice.get(), submission.queue);
        if (localDevice != nullptr) localDevice->SubmitRecorded(submission.queue == 0);
    }
    endTiming.Finish();
    if (submission.rewindTail != nullptr) executeRewindTail(submission);
}

}
