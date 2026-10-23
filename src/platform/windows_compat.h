#pragma once
// Force-included into the project's own Windows translation units (CMake /FI).
// <windows.h> defines `interface` (COM) and min/max as macros. Include it and
// every COM header the SDK's public headers use up front, with NOMINMAX, then
// drop `interface` so identifiers named `interface` keep working. The later
// includes from SDK headers are then no-ops.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <dxgi1_5.h>
#include <dxgidebug.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#undef interface
#endif
