#pragma once

// XenosRecomp is MIT licensed; see vendor/xenos-recomp/LICENSE.md. This consumer
// includes its original PCH and changes unsupported-construct assertions into
// recoverable errors only for this library, never for the vendor executable.
#include "pch.h"
#include <stdexcept>

#undef assert
#define assert(condition) ((condition) ? static_cast<void>(0) : \
  throw std::runtime_error("Native shader translation: " #condition))
