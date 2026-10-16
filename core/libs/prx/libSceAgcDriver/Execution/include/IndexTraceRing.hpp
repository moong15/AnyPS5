#pragma once

#include "prx/libc/include/FailureReport.hpp"
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string_view>

namespace AgcDriver {

// APS5_TRACE_INDEX_BASE=1 prints every index trace line as it happens (slow: hundreds of thousands
// of lines a minute). APS5_TRACE_INDEX_BASE=ring keeps the last IndexTraceRing::Lines lines in
// memory and prints them only from the failure report, so the title runs at normal speed.
enum class IndexTraceMode { Off, Print, Ring };

namespace IndexTraceDetail {

struct Ring {
    static constexpr std::size_t Lines = 65536;
    static constexpr std::size_t LineBytes = 320;
    std::atomic<std::uint64_t> next{0};
    std::unique_ptr<std::array<char, LineBytes>[]> lines;
};

inline void report() noexcept;

inline Ring& ring() {
    static Ring* value = [] {
        auto* created = new Ring;
        created->lines.reset(new std::array<char, Ring::LineBytes>[Ring::Lines]());
        LibcRegisterFailureReport_nid_no_patch(report);
        return created;
    }();
    return *value;
}

inline void report() noexcept {
    auto& traced = ring();
    const auto end = traced.next.load(std::memory_order_acquire);
    const auto begin = end > Ring::Lines ? end - Ring::Lines : 0;
    std::fprintf(stderr, "[index-ring] last %llu of %llu index trace lines, oldest first\n", static_cast<unsigned long long>(end - begin), static_cast<unsigned long long>(end));
    for (auto i = begin; i < end; ++i) std::fputs(traced.lines[i % Ring::Lines].data(), stderr);
    std::fputs("[index-ring] end\n", stderr);
    std::fflush(stderr);
}

} // namespace IndexTraceDetail

inline IndexTraceMode indexTraceMode() {
    static const IndexTraceMode mode = [] {
        const char* value = std::getenv("APS5_TRACE_INDEX_BASE");
        if (value == nullptr) return IndexTraceMode::Off;
        if (std::string_view(value) != "ring") return IndexTraceMode::Print;
        IndexTraceDetail::ring();
        return IndexTraceMode::Ring;
    }();
    return mode;
}

[[gnu::format(printf, 1, 2)]] inline void indexTrace(const char* format, ...) {
    const auto mode = indexTraceMode();
    if (mode == IndexTraceMode::Off) return;
    va_list arguments;
    va_start(arguments, format);
    if (mode == IndexTraceMode::Print) {
        std::vfprintf(stderr, format, arguments);
    } else {
        auto& traced = IndexTraceDetail::ring();
        auto& line = traced.lines[traced.next.fetch_add(1, std::memory_order_acq_rel) % IndexTraceDetail::Ring::Lines];
        const int written = std::vsnprintf(line.data(), line.size(), format, arguments);
        // A truncated line keeps its newline so the dump stays one record per line.
        if (written >= static_cast<int>(line.size())) std::memcpy(line.data() + line.size() - 2, "\n", 2);
    }
    va_end(arguments);
}

} // namespace AgcDriver
