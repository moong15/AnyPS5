#include "prx/libSceAgcDriver/Execution/include/Driver/Dispatch/DispatchCacheDiagnostics.hpp"
#include <cstdio>
#include <stdexcept>

using AgcDriver::DriverDetail::DispatchCacheDiagnostics;

int main() {
    try {
        const auto require = [](bool valid) { if (!valid) throw std::runtime_error("dispatch cache diagnostics mismatch"); };
        DispatchCacheDiagnostics cache(2);
        require(!cache.WasEvicted(1) && !cache.HasIdentity(10));
        cache.Insert(1, 10);
        cache.Insert(2, 10);
        cache.Insert(1, 10);
        cache.Insert(3, 20);
        cache.Evict(1);
        require(cache.WasEvicted(1) && cache.HasIdentity(10) && cache.HasIdentity(20));
        cache.Evict(2);
        require(!cache.HasIdentity(10) && cache.HasIdentity(20));
        cache.Insert(1, 10);
        require(cache.WasEvicted(1) && cache.HasIdentity(10));
        cache.Evict(1);
        require(cache.HistorySize() == 2 && cache.Dropped() == 0 && !cache.HasIdentity(10));
        cache.Evict(3);
        require(!cache.WasEvicted(1) && cache.WasEvicted(2) && cache.WasEvicted(3));
        require(cache.HistorySize() == 2 && cache.Dropped() == 1 && !cache.HasIdentity(20));
        DispatchCacheDiagnostics noHistory(0);
        noHistory.Insert(4, 30);
        noHistory.Evict(4);
        require(!noHistory.WasEvicted(4) && !noHistory.HasIdentity(30) && noHistory.HistorySize() == 0 && noHistory.Dropped() == 1);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
