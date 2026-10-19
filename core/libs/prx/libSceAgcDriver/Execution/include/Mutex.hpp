#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_MUTEX_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_MUTEX_HPP

#include "prx/libc/include/HostMutex.hpp"
#include <atomic>
#include <cstdint>

namespace AgcDriver {

using Mutex = HostMutex;

inline const void* CurrentThreadToken() noexcept {
#if defined(_WIN32) && defined(__x86_64__) && defined(__GNUC__)
    const void* block;
    __asm__("movq %%gs:0x30, %0" : "=r"(block));
    return block;
#else
    thread_local char token;
    return &token;
#endif
}

}

#endif
