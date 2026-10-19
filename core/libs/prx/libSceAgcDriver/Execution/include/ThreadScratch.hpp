#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_THREADSCRATCH_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_THREADSCRATCH_HPP

#include "prx/libc/include/HostThreadSlot.hpp"
#include "ThreadOwned.hpp"

namespace AgcDriver {

template<typename TValue, typename TTag>
TValue& ThreadScratch() {
    auto* object = HostThreadSlot<TValue*, TTag>::Get();
    if (object == nullptr) {
        ShaderRecompiler::ThreadOwned(object);
        HostThreadSlot<TValue*, TTag>::Set(object);
    }
    return *object;
}

}

#endif
