#pragma once

// Plume normally embeds Volk. The game already links the host's Volk target;
// suppress its implementation macro in this consumer-only include directory.
#undef VOLK_IMPLEMENTATION
#include "../../../../vendor/rexglue-sdk/thirdparty/volk/volk.h"
