#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_HOSTMUTEX_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_HOSTMUTEX_HPP

#include <atomic>
#include <cstdint>

inline void WaitWhile(const std::atomic<std::uint32_t>& word, std::uint32_t value) noexcept {
    word.wait(value, std::memory_order_relaxed);
}

inline void WakeOne(std::atomic<std::uint32_t>& word) noexcept {
    word.notify_one();
}

inline void WakeAll(std::atomic<std::uint32_t>& word) noexcept {
    word.notify_all();
}

class HostMutex {
public:
    constexpr HostMutex() noexcept = default;
    HostMutex(const HostMutex&) = delete;
    HostMutex& operator=(const HostMutex&) = delete;

    void lock() noexcept {
        if (!try_lock()) lockContended();
    }

    bool try_lock() noexcept {
        std::uint32_t expected = Unlocked;
        return state.compare_exchange_strong(expected, Locked, std::memory_order_acquire, std::memory_order_relaxed);
    }

    void unlock() noexcept {
        if (state.exchange(Unlocked, std::memory_order_release) == Contended) WakeOne(state);
    }

private:
    static constexpr std::uint32_t Unlocked = 0;
    static constexpr std::uint32_t Locked = 1;
    static constexpr std::uint32_t Contended = 2;

    [[gnu::noinline]] void lockContended() noexcept {
        while (state.exchange(Contended, std::memory_order_acquire) != Unlocked) WaitWhile(state, Contended);
    }

    std::atomic<std::uint32_t> state{Unlocked};
};

#endif
