#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include <cstdio>
#include <exception>
#include <initializer_list>
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

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void IncompleteWindow() {
    const auto lease = MakeLease({{80, 16, true, true, 80, 16, true, true}, {96, 16, true, true, 96, 16, true, true}});
    const auto owners = lease.front().use_count();
    Require(RegisteredRunBounds(lease, 0, 88, 64) == std::pair<std::size_t, std::size_t>{0, 0}, "old path did not select the containing allocation");
    Require(RegisteredRunBounds(lease, 0, 88, 64, true) == std::pair<std::size_t, std::size_t>{0, 1}, "clipped window lost complete allocations");
    Require(!RegisteredRunCovers(lease, 88, 104, 64, {}), "old path covered the incomplete window");
    Require(RegisteredRunCovers(lease, 88, 104, 64, {}, nullptr, true), "clipped window did not cover the view");
    Require(RegisteredImportRejection(lease, 88, 104, 64, {}) == ImportRejection::WindowIncomplete, "old refusal was not window incomplete");
    Require(RegisteredImportRejection(lease, 88, 104, 64, {}, true) == ImportRejection::None, "clipped view retained the refusal");
    Require(!RegisteredRunCovers(lease, 88, 113, 64, {}, nullptr, true), "clipped window covered an unmapped suffix");
    Require(!RegisteredRunCovers(lease, 88, 104, 64, {80}, nullptr, true), "clipping ignored failed aliases");
    Require(RegisteredRunCovers(lease, 88, 96, 64, {80}, nullptr, true), "failed alias lost the containing allocation");
    Require(lease.front().use_count() == owners, "selection retained an allocation owner");
    const auto alone = MakeLease({{80, 16, true, true, 80, 16, true, true}});
    Require(RegisteredRunCovers(alone, 80, 96, 64, {}), "old path rejected a complete containing allocation");
    Require(RegisteredRunCovers(alone, 80, 96, 64, {}, nullptr, true), "clipping rejected a complete containing allocation");
}

void Boundaries() {
    const auto crossing = MakeLease({{112, 32, true, true, 112, 32, true, true}, {144, 16, true, true, 144, 16, true, true}});
    Require(RegisteredRunCovers(crossing, 120, 144, 64, {}, nullptr, true), "allocation crossing a cell was truncated");
    Require(!RegisteredRunCovers(crossing, 120, 152, 64, {}, nullptr, true), "clipping grew beyond the containing allocation across a cell");
    const auto gap = MakeLease({{80, 16, true, true, 80, 16, true, true}, {112, 16, true, true, 112, 16, true, true}});
    Require(!RegisteredRunCovers(gap, 88, 120, 64, {}, nullptr, true), "clipping crossed a hole");
    const auto complete = MakeLease({{64, 32, true, true, 64, 32, true, true}, {96, 32, true, true, 96, 32, true, true}});
    Require(RegisteredRunBounds(complete, 0, 72, 64) == RegisteredRunBounds(complete, 0, 72, 64, true), "clipping changed a complete window");
    for (unsigned flags = 0; flags != 8; ++flags) {
        const bool readable = (flags & 1u) != 0;
        const bool writable = (flags & 2u) != 0;
        const bool gpu = (flags & 4u) != 0;
        const auto mismatch = MakeLease({{80, 16, true, true, 80, 16, true, true}, {96, 16, readable, writable, 96, 16, true, gpu}});
        Require(RegisteredRunCovers(mismatch, 88, 104, 64, {}, nullptr, true) == (readable && writable && gpu), "clipping ignored range flags");
        const auto uniform = MakeLease({{80, 16, readable, writable, 80, 16, true, gpu}, {96, 16, readable, writable, 96, 16, true, gpu}});
        Require(RegisteredRunCovers(uniform, 88, 104, 64, {}, nullptr, true) == (readable && writable && gpu), "clipping admitted a non-writable or non-GPU run");
    }
}

}

int main() {
    try {
        IncompleteWindow();
        Boundaries();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
