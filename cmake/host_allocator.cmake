# Optional host allocation policy; game/renderer code uses standard C/C++ APIs.
# AUTO uses the measured ELF allocator on Linux when already installed. Other
# platforms retain their system allocator until their own linkage is validated.
set(GOW3_HOST_ALLOCATOR "AUTO" CACHE STRING "Host allocator: AUTO, SYSTEM, or JEMALLOC")
set_property(CACHE GOW3_HOST_ALLOCATOR PROPERTY STRINGS AUTO SYSTEM JEMALLOC)
if(NOT GOW3_HOST_ALLOCATOR MATCHES "^(AUTO|SYSTEM|JEMALLOC)$")
    message(FATAL_ERROR "GOW3_HOST_ALLOCATOR must be AUTO, SYSTEM, or JEMALLOC")
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
    elseif(GOW3_HOST_ALLOCATOR STREQUAL "JEMALLOC")
        message(FATAL_ERROR "JEMALLOC linkage is currently validated only for Linux; use AUTO or SYSTEM on this platform")
    endif()
endif()
message(STATUS "Gears host allocator: ${GOW3_SELECTED_HOST_ALLOCATOR} (${GOW3_HOST_ALLOCATOR})")
