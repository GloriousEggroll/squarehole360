# Builds against Steam Runtime 3 "sniper" (Debian 11, glibc 2.31) using the
# host's Clang and GNU ld with the sniper SDK as sysroot, so the binaries run on
# any distribution with glibc 2.31 or newer, inside or outside the runtime.
#
# Create the sysroot first: tools/steamrt-sysroot.sh (vendor-sdks/sniper-sysroot).
# The prebuilt DirectX shader compiler (libdxcompiler.so) needs glibc 2.34, so
# hosts need glibc 2.34+ unless it runs inside the Steam Runtime container.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(GOW3_SNIPER_SYSROOT "${CMAKE_CURRENT_LIST_DIR}/../../vendor-sdks/sniper-sysroot"
    CACHE PATH "Steam Runtime sniper SDK sysroot")
get_filename_component(GOW3_SNIPER_SYSROOT "${GOW3_SNIPER_SYSROOT}" ABSOLUTE)
if(NOT EXISTS "${GOW3_SNIPER_SYSROOT}/usr/lib/gcc-14/lib/gcc/x86_64-linux-gnu/14")
    message(FATAL_ERROR "No sniper sysroot with GCC 14 at ${GOW3_SNIPER_SYSROOT}; run tools/steamrt-sysroot.sh")
endif()
# Compiler flags reach the shell unquoted; reach the sysroot through a path
# without spaces or parentheses.
if(GOW3_SNIPER_SYSROOT MATCHES "[ ()]")
    set(_gow3_sysroot_link "$ENV{HOME}/.cache/gow3-sniper-sysroot")
    file(MAKE_DIRECTORY "$ENV{HOME}/.cache")
    # Never delete here: CMake's file(REMOVE) follows directory links.
    if(IS_SYMLINK "${_gow3_sysroot_link}")
        file(READ_SYMLINK "${_gow3_sysroot_link}" _gow3_link_target)
        if(NOT _gow3_link_target STREQUAL GOW3_SNIPER_SYSROOT)
            message(FATAL_ERROR "${_gow3_sysroot_link} points to ${_gow3_link_target}; remove it with rm")
        endif()
    elseif(EXISTS "${_gow3_sysroot_link}")
        message(FATAL_ERROR "${_gow3_sysroot_link} exists and is not a link; remove it")
    else()
        file(CREATE_LINK "${GOW3_SNIPER_SYSROOT}" "${_gow3_sysroot_link}" SYMBOLIC)
    endif()
    set(GOW3_SNIPER_SYSROOT "${_gow3_sysroot_link}")
endif()

set(CMAKE_SYSROOT "${GOW3_SNIPER_SYSROOT}")
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_COMPILER_TARGET x86_64-linux-gnu)
set(CMAKE_CXX_COMPILER_TARGET x86_64-linux-gnu)

# GCC 14 headers and runtime from the SDK's separate gcc-14 prefix. Sniper
# ships GCC 10's libstdc++.so, so GCC 14's libstdc++ is linked statically (the
# runtime's documented setup); libgcc_s stays shared so exceptions unwind
# across our shared libraries.
# The gcc-14 prefix has no shared libgcc_s; link the runtime's through a
# directory holding only that library.
set(_gow3_libgcc_dir "$ENV{HOME}/.cache/gow3-sniper-libgcc")
if(NOT EXISTS "${_gow3_libgcc_dir}/libgcc_s.so")
    file(MAKE_DIRECTORY "${_gow3_libgcc_dir}")
    file(CREATE_LINK "${GOW3_SNIPER_SYSROOT}/lib/x86_64-linux-gnu/libgcc_s.so.1"
         "${_gow3_libgcc_dir}/libgcc_s.so" SYMBOLIC)
endif()
set(_gow3_toolchain_flags "--gcc-toolchain=${GOW3_SNIPER_SYSROOT}/usr/lib/gcc-14")
set(CMAKE_C_FLAGS_INIT "${_gow3_toolchain_flags}")
# Debian 11's TBB 2020 headers do not compile as C++23; the host build has no
# TBB either, so parallel algorithms use the serial backend in both.
set(CMAKE_CXX_FLAGS_INIT "${_gow3_toolchain_flags} -D_GLIBCXX_USE_TBB_PAR_BACKEND=0")
# Microsoft's prebuilt libdxcompiler.so references glibc 2.33/2.34 symbols;
# the loader resolves those from the host (or the runtime's newer glibc), so
# only our own objects are held to sniper's glibc 2.31.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-Wl,--allow-shlib-undefined")
foreach(kind EXE SHARED MODULE)
    # GNU ld resolves the sysroot libraries' own dependencies (libffi for
    # libwayland, ...) through -rpath-link.
    set(CMAKE_${kind}_LINKER_FLAGS_INIT "${CMAKE_${kind}_LINKER_FLAGS_INIT} ${_gow3_toolchain_flags} -L${_gow3_libgcc_dir} -static-libstdc++ -Wl,-rpath-link,${GOW3_SNIPER_SYSROOT}/usr/lib/x86_64-linux-gnu:${GOW3_SNIPER_SYSROOT}/lib/x86_64-linux-gnu")
endforeach()

# Wayland protocol code must match sniper's libwayland-client 1.18; its own
# scanner runs on the host.
set(REX_WAYLAND_SCANNER "${GOW3_SNIPER_SYSROOT}/usr/bin/wayland-scanner" CACHE FILEPATH "")

# Libraries, headers and packages only from the sysroot; programs (Python,
# shader tools) from the host.
set(CMAKE_FIND_ROOT_PATH "${GOW3_SNIPER_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${GOW3_SNIPER_SYSROOT}")
set(ENV{PKG_CONFIG_LIBDIR}
    "${GOW3_SNIPER_SYSROOT}/usr/lib/x86_64-linux-gnu/pkgconfig:${GOW3_SNIPER_SYSROOT}/usr/lib/pkgconfig:${GOW3_SNIPER_SYSROOT}/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "")
