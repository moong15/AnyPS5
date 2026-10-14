#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_DRIVER_DRAW_DRAWREGISTERKEY_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_DRIVER_DRAW_DRAWREGISTERKEY_HPP

#include "prx/libSceAgcDriver/Execution/include/QueueState.hpp"

namespace AgcDriver::DriverDetail {

bool DrawTargetRelocationEnabled();
bool DrawTargetRegister(std::uint32_t offset);
std::uint64_t DrawTargetStateKey(const QueueState& queue);

std::uint64_t RegisterStateKey(const QueueState& queue, bool allUserWords = false, bool incremental = true, std::uint64_t* shapeKey = nullptr, bool relocateTargets = false);

}

#endif
