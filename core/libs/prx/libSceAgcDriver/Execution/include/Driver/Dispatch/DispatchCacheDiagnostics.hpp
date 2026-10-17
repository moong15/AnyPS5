#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DISPATCH_DISPATCHCACHEDIAGNOSTICS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DISPATCH_DISPATCHCACHEDIAGNOSTICS_HPP

#include <cstddef>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <unordered_set>

namespace AgcDriver::DriverDetail {

class DispatchCacheDiagnostics {
public:
    explicit DispatchCacheDiagnostics(std::size_t historyLimit = 65536) : historyLimit(historyLimit) {}

    void Insert(std::uint64_t key, std::uint64_t identity) {
        if (residents.emplace(key, identity).second) ++identities[identity];
    }

    void Evict(std::uint64_t key) {
        const auto found = residents.find(key);
        if (found != residents.end()) {
            const auto group = identities.find(found->second);
            if (--group->second == 0) identities.erase(group);
            residents.erase(found);
        }
        if (evicted.insert(key).second) history.push_back(key);
        while (history.size() > historyLimit) {
            evicted.erase(history.front());
            history.pop_front();
            ++dropped;
        }
    }

    bool WasEvicted(std::uint64_t key) const { return evicted.contains(key); }
    bool HasIdentity(std::uint64_t identity) const { return identities.contains(identity); }
    std::uint64_t Dropped() const { return dropped; }
    std::size_t HistorySize() const { return history.size(); }

private:
    std::size_t historyLimit;
    std::unordered_map<std::uint64_t, std::uint64_t> residents;
    std::unordered_map<std::uint64_t, std::size_t> identities;
    std::unordered_set<std::uint64_t> evicted;
    std::deque<std::uint64_t> history;
    std::uint64_t dropped = 0;
};

}

#endif
