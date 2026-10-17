#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include <cstdio>
#include <exception>
#include <initializer_list>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>

using namespace AgcDriver::Graphics;

namespace {

GuestAllocations::Lease MakeLease(std::initializer_list<GuestAllocations::Range> ranges) {
    auto owners = std::make_shared<GuestAllocations::Lease::Ranges>();
    for (const auto& range : ranges) owners->push_back(std::make_shared<const GuestAllocations::Range>(range));
    return GuestAllocations::Lease(std::move(owners));
}

void Expect(const GuestAllocations::Lease& lease, std::uint64_t begin, std::uint64_t end, std::uint64_t window, const std::set<std::uint64_t>& unaliased, bool expected, const char* what) {
    bool avoided = true;
    const auto before = lease.empty() ? 0 : lease.front().use_count();
    if (RegisteredRunCovers(lease, begin, end, window, unaliased, &avoided) != expected) throw std::runtime_error(what);
    if (avoided != (RegisteredRangeIndex(lease, begin) != lease.size())) throw std::runtime_error("owner-list saving classification differs from range ownership");
    if (!lease.empty() && lease.front().use_count() != before) throw std::runtime_error("coverage retained a range owner");
}

void BoundariesAndPolicies() {
    const auto lease = MakeLease({{64, 16, true, true, 64, 64}, {80, 16, true, true, 64, 64}, {96, 16, true, true, 64, 64}, {112, 16, true, true, 64, 64}});
    Expect({}, 64, 65, 0, {}, false, "empty lease covered bytes");
    Expect(lease, 63, 65, 0, {}, false, "unregistered prefix covered bytes");
    Expect(lease, 128, 129, 0, {}, false, "exclusive end covered bytes");
    Expect(lease, 79, 80, 0, {}, true, "last byte was excluded");
    Expect(lease, 80, 128, 0, {}, true, "adjacent equal ranges were not joined");
    Expect(lease, 80, 129, 0, {}, false, "coverage extended past the run");
    Expect(lease, 80, 128, 64, {}, true, "complete window was not joined");
    Expect(lease, 80, 97, 32, {}, false, "access crossed the selected window");
    Expect(lease, 80, 96, 32, {}, true, "window's last byte was excluded");
    Expect(lease, 80, 97, 48, {}, false, "partial window was joined");
    Expect(lease, 80, 97, 0, {64}, false, "failed alias span was joined");
    Expect(lease, 80, 96, 0, {64}, true, "failed alias span lost its containing range");
    Expect(lease, 80, 128, 0, {80}, true, "unrelated failed alias changed the run");
    Expect(lease, 80, 112, 0, {}, true, "updated alias policy was not observed");
    const auto large = MakeLease({{64, 128, true, true, 64, 128}});
    Expect(large, 80, 192, 32, {}, true, "large containing allocation was clipped to a window");
    Expect(large, 80, 193, 32, {}, false, "large allocation covered bytes beyond its end");
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    const auto high = MakeLease({{maximum - 63, 63, true, true, maximum - 63, 63}});
    Expect(high, maximum - 1, maximum, 64, {}, true, "high address last byte was lost");
}

void Discontinuities() {
    const auto gap = MakeLease({{64, 16, true, true, 64, 16}, {96, 16, true, true, 96, 16}});
    Expect(gap, 72, 100, 0, {}, false, "hole was covered");
    Expect(gap, 80, 81, 0, {}, false, "hole had an owner");
    for (unsigned flags = 0; flags != 8; ++flags) {
        const bool readable = (flags & 1u) != 0;
        const bool writable = (flags & 2u) != 0;
        const bool gpu = (flags & 4u) != 0;
        const auto lease = MakeLease({{64, 16, true, true, 64, 32, true, true}, {80, 16, readable, writable, 64, 32, true, gpu}});
        Expect(lease, 72, 96, 0, {}, readable && writable && gpu, "protection or GPU policy boundary was ignored");
        Expect(lease, 80, 96, 0, {}, readable, "containing range readability was ignored");
    }
}

void RejectionGates() {
    const auto expect = [](const GuestAllocations::Lease& lease, std::uint64_t begin, std::uint64_t end, std::uint64_t window, const std::set<std::uint64_t>& unaliased, ImportRejection reason) {
        if (RegisteredImportRejection(lease, begin, end, window, unaliased) != reason) throw std::runtime_error("incorrect import coverage rejection gate");
    };
    const auto dense = MakeLease({{64, 16, true, true, 64, 64}, {80, 16, true, true, 64, 64}, {96, 16, true, true, 64, 64}, {112, 16, true, true, 64, 64}});
    expect({}, 64, 65, 0, {}, ImportRejection::NoRegistryCoverage);
    expect(dense, 63, 65, 0, {}, ImportRejection::NoRegistryCoverage);
    expect(dense, 128, 129, 0, {}, ImportRejection::NoRegistryCoverage);
    expect(dense, 80, 128, 64, {}, ImportRejection::None);
    expect(dense, 80, 97, 32, {}, ImportRejection::WindowIncomplete);
    expect(dense, 80, 97, 48, {}, ImportRejection::WindowIncomplete);
    expect(dense, 80, 97, 0, {64}, ImportRejection::HostAliasProtectionFailure);
    expect(dense, 80, 96, 0, {64}, ImportRejection::None);
    expect(dense, 80, 129, 64, {}, ImportRejection::CoverageHole);
    const auto gap = MakeLease({{64, 16, true, true, 64, 16}, {96, 16, true, true, 96, 16}});
    expect(gap, 72, 100, 32, {}, ImportRejection::CoverageHole);
    for (unsigned flags = 0; flags != 8; ++flags) {
        const bool readable = (flags & 1u) != 0;
        const bool writable = (flags & 2u) != 0;
        const bool gpu = (flags & 4u) != 0;
        const auto lease = MakeLease({{64, 16, true, true, 64, 32, true, true}, {80, 16, readable, writable, 64, 32, true, gpu}});
        expect(lease, 72, 96, 0, {}, readable && writable && gpu ? ImportRejection::None : ImportRejection::FlagsMismatch);
        expect(lease, 80, 96, 0, {}, readable ? ImportRejection::None : ImportRejection::FlagsMismatch);
    }
}

void LongRunAndReplacement() {
    auto owners = std::make_shared<GuestAllocations::Lease::Ranges>();
    for (std::uint64_t i = 0; i != 4096; ++i) owners->push_back(std::make_shared<const GuestAllocations::Range>(GuestAllocations::Range{65536 + i * 16, 16, true, true, 65536, 65536}));
    const GuestAllocations::Lease lease(owners);
    for (std::uint64_t i = 0; i != owners->size(); ++i) {
        Expect(lease, 65536 + i * 16, 65536 + i * 16 + 16, 0, {}, true, "single-range shortcut failed in a long run");
    }
    Expect(lease, 65536 + 16, 131072, 0, {}, true, "long run was truncated");
    Expect(lease, 65536 + 16, 131072, 0, {65536}, false, "new alias failure was not observed");
    const auto replacement = MakeLease({{65536, 16, true, true, 65536, 16}, {65568, 16, true, true, 65568, 16}});
    Expect(replacement, 65536, 65568, 0, {}, false, "replacement lease reused stale coverage");
    Expect(lease, 65536, 65568, 0, {}, true, "replacement changed the original lease");
}

}

int main() {
    try {
        BoundariesAndPolicies();
        Discontinuities();
        RejectionGates();
        LongRunAndReplacement();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
