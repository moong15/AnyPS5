#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DEVICEACCESS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DEVICEACCESS_HPP

#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <utility>
#include "prx/libc/include/general/AtomicSharedPtr.hpp"

namespace AgcDriver::DriverDetail {

class DevicePointer {
public:
    std::shared_ptr<VulkanDevice> Load() const;
    operator std::shared_ptr<VulkanDevice>() const;
    DevicePointer& operator=(std::shared_ptr<VulkanDevice> value);
    void Reset();
    VulkanDevice* operator->() const;
    explicit operator bool() const;
    bool operator==(std::nullptr_t) const;

private:
    AtomicSharedPtr<VulkanDevice> pointer;
};

bool NarrowDeviceLock_nid_no_patch();

template <typename TPrepare, typename TValidate, typename TWork>
void WithDevicePreparation_nid_no_patch(TPrepare&& prepare, TValidate&& validate, TWork&& work) {
    const bool narrow = NarrowDeviceLock_nid_no_patch();
    std::unique_lock lock(GuestMemory::GpuMutex(), std::defer_lock);
    if (narrow) std::forward<TPrepare>(prepare)();
    lock.lock();
    std::forward<TValidate>(validate)();
    if (!narrow) std::forward<TPrepare>(prepare)();
    std::forward<TWork>(work)();
}

class DeviceUseGate {
public:
    void lock_shared();
    void unlock_shared();
    void lock();
    void unlock();

private:
    std::mutex mutex;
    std::condition_variable changed;
    std::size_t users = 0;
    bool replacing = false;
};

}

#endif
