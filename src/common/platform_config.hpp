#pragma once

#if defined(PLATFORM_QEMU)
#  include "platform/qemu.hpp"
#elif defined(PLATFORM_ARM64)
#  include "platform/arm64.hpp"
#else
#  include "platform/atom.hpp"
#endif
