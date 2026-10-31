# Optional host allocation policy; game/renderer code uses standard C/C++ APIs.
# AUTO uses the measured ELF allocator on Linux when already installed and
# mimalloc on Windows. macOS retains its system allocator until its own linkage
# is validated.
set(GOW3_HOST_ALLOCATOR "AUTO" CACHE STRING "Host allocator: AUTO, SYSTEM, JEMALLOC, or MIMALLOC")
set_property(CACHE GOW3_HOST_ALLOCATOR PROPERTY STRINGS AUTO SYSTEM JEMALLOC MIMALLOC)
if(NOT GOW3_HOST_ALLOCATOR MATCHES "^(AUTO|SYSTEM|JEMALLOC|MIMALLOC)$")
    message(FATAL_ERROR "GOW3_HOST_ALLOCATOR must be AUTO, SYSTEM, JEMALLOC, or MIMALLOC")
endif()

set(GOW3_SELECTED_HOST_ALLOCATOR "SYSTEM")
if(NOT GOW3_HOST_ALLOCATOR STREQUAL "SYSTEM")
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # Also permit the installed runtime package without a development symlink.
        find_library(GOW3_JEMALLOC_LIBRARY NAMES jemalloc libjemalloc.so.2)
        if(GOW3_JEMALLOC_LIBRARY)
            target_link_libraries(gow3 PRIVATE "${GOW3_JEMALLOC_LIBRARY}")
            set(GOW3_SELECTED_HOST_ALLOCATOR "JEMALLOC")
        elseif(GOW3_HOST_ALLOCATOR STREQUAL "JEMALLOC")
            message(FATAL_ERROR "JEMALLOC requested but no jemalloc library was found")
        endif()
    elseif(WIN32 AND GOW3_HOST_ALLOCATOR MATCHES "^(AUTO|MIMALLOC)$")
        # The Windows heap (and Wine's under Proton) took ~13% of CPU in heap
        # calls and their locks. One shared mimalloc.dll serves operator
        # new/delete in both gow3.exe and rexruntime.dll, which free each
        # other's allocations; malloc itself is left alone (no redirect DLL).
        include(FetchContent)
        FetchContent_Declare(mimalloc
            URL https://github.com/microsoft/mimalloc/archive/refs/tags/v3.5.3.tar.gz
            URL_HASH SHA256=3b4a15153a59905995f7070296ed604bb5ccc00cabb8b93446931aff77224d47)
        set(MI_OVERRIDE OFF CACHE BOOL "" FORCE)
        set(MI_WIN_REDIRECT OFF CACHE BOOL "" FORCE)
        set(MI_BUILD_SHARED ON CACHE BOOL "" FORCE)
        set(MI_BUILD_STATIC OFF CACHE BOOL "" FORCE)
        set(MI_BUILD_OBJECT OFF CACHE BOOL "" FORCE)
        set(MI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(mimalloc)
        # Every module that links rexruntime (the game, tools and tests) compiles
        # the override too, so no module frees mimalloc memory with the CRT heap.
        add_library(gow3-mimalloc-new-delete INTERFACE)
        target_sources(gow3-mimalloc-new-delete INTERFACE
            "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/mimalloc_new_delete.cpp")
        target_link_libraries(gow3-mimalloc-new-delete INTERFACE mimalloc)
        target_compile_definitions(gow3-mimalloc-new-delete INTERFACE GOW3_HOST_MIMALLOC=1)
        target_link_libraries(rexruntime PUBLIC gow3-mimalloc-new-delete)
        # DXC's working memory too; see the source for why it is linked here.
        target_sources(gow3 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/native/dxc_mimalloc_win.cpp")
        target_link_libraries(gow3 PRIVATE Microsoft::DirectXShaderCompiler)
        if(TARGET gow3-shader-precompile)
            target_sources(gow3-shader-precompile PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/native/dxc_mimalloc_win.cpp")
            target_link_libraries(gow3-shader-precompile PRIVATE Microsoft::DirectXShaderCompiler mimalloc)
        endif()
        if(TARGET gow3-native-compiler-timing)
            # Same benchmark with the game's DXC allocator, for comparison.
            add_executable(gow3-native-compiler-timing-mimalloc
                tools/rendering/native/native_compiler_timing_bench.cpp
                "${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/native/dxc_mimalloc_win.cpp")
            target_include_directories(gow3-native-compiler-timing-mimalloc PRIVATE src)
            target_link_libraries(gow3-native-compiler-timing-mimalloc PRIVATE gow3-native-renderer
                Microsoft::DirectXShaderCompiler mimalloc)
        endif()
        set(GOW3_SELECTED_HOST_ALLOCATOR "MIMALLOC")
    elseif(GOW3_HOST_ALLOCATOR STREQUAL "JEMALLOC")
        message(FATAL_ERROR "JEMALLOC linkage is currently validated only for Linux; use AUTO or SYSTEM on this platform")
    elseif(GOW3_HOST_ALLOCATOR STREQUAL "MIMALLOC")
        message(FATAL_ERROR "MIMALLOC linkage is currently validated only for Windows; use AUTO or SYSTEM on this platform")
    endif()
endif()
message(STATUS "Gears host allocator: ${GOW3_SELECTED_HOST_ALLOCATOR} (${GOW3_HOST_ALLOCATOR})")
