#include "prx/libSceAgcDriver/Execution/include/Mutex.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/DeviceAccess.hpp"

#include <cstdlib>

namespace AgcDriver::DriverDetail {

std::shared_ptr<VulkanDevice> DevicePointer::Load() const { return pointer.load(std::memory_order_acquire); }

DevicePointer::operator std::shared_ptr<VulkanDevice>() const { return Load(); }

DevicePointer& DevicePointer::operator=(std::shared_ptr<VulkanDevice> value) {
    pointer.store(std::move(value), std::memory_order_release);
    return *this;
}

void DevicePointer::Reset() { *this = nullptr; }

VulkanDevice* DevicePointer::operator->() const { return Load().get(); }

DevicePointer::operator bool() const { return Load() != nullptr; }

bool DevicePointer::operator==(std::nullptr_t) const { return Load() == nullptr; }

bool NarrowDeviceLock_nid_no_patch() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_NO_NARROW_DEVICE_LOCK");
        return value == nullptr || value[0] == '\0' || value[0] == '0';
    }();
    return enabled;
}

void DeviceUseGate::lock_shared() {
    auto value = state.load(std::memory_order_relaxed);
    for (;;) {
        if ((value & Replacing) != 0) {
            WaitWhile(state, value);
            value = state.load(std::memory_order_relaxed);
        } else if (state.compare_exchange_weak(value, value + 1, std::memory_order_acquire, std::memory_order_relaxed)) {
            return;
        }
    }
}

void DeviceUseGate::unlock_shared() {
    if (state.fetch_sub(1, std::memory_order_release) == (Replacing | 1)) WakeAll(state);
}

void DeviceUseGate::lock() {
    auto value = state.load(std::memory_order_relaxed);
    for (;;) {
        if ((value & Replacing) != 0) {
            WaitWhile(state, value);
            value = state.load(std::memory_order_relaxed);
        } else if (state.compare_exchange_weak(value, value | Replacing, std::memory_order_acquire, std::memory_order_relaxed)) {
            break;
        }
    }
    for (value = state.load(std::memory_order_acquire); value != Replacing; value = state.load(std::memory_order_acquire)) WaitWhile(state, value);
}

void DeviceUseGate::unlock() {
    state.fetch_and(~Replacing, std::memory_order_release);
    WakeAll(state);
}

}
