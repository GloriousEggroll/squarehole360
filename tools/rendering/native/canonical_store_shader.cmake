# The candidate copies the canonical encoder/decoder rather than changing it.
# Refuse it if that reference changes without a corresponding parity review.
if(DEFINED GOW3_NATIVE_SHADER_DIR AND DEFINED GOW3_NATIVE_GLSLANG)
    set(GOW3_CANONICAL_STORE_HEADER "${GOW3_NATIVE_SHADER_DIR}/memory_store_canonical_spv.h")
    set(canonical_store_glslang "${GOW3_NATIVE_GLSLANG}")
else()
    set(GOW3_CANONICAL_STORE_HEADER "${CMAKE_CURRENT_BINARY_DIR}/memory_store_canonical_spv.h")
    set(canonical_store_glslang "${GLSLANG_VALIDATOR}")
endif()
set(canonical_store_reference "${CMAKE_CURRENT_LIST_DIR}/resolve_memory.comp")
file(SHA256 "${canonical_store_reference}" canonical_store_hash)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${canonical_store_reference}")
if(canonical_store_hash STREQUAL "052e625aaac362dd844a4a8f96f31cb9d1408606e757a72530ec36a507f2aecf")
    add_custom_command(OUTPUT "${GOW3_CANONICAL_STORE_HEADER}"
        COMMAND "${canonical_store_glslang}" -V --target-env vulkan1.1 --vn memory_store_canonical_spv
            -o "${GOW3_CANONICAL_STORE_HEADER}" "${CMAKE_CURRENT_LIST_DIR}/memory_store_canonical.comp"
        DEPENDS "${CMAKE_CURRENT_LIST_DIR}/memory_store_canonical.comp" "${CMAKE_CURRENT_LIST_FILE}"
            "${canonical_store_reference}" VERBATIM)
else()
    file(WRITE "${GOW3_CANONICAL_STORE_HEADER}"
        "#pragma once\n#include <array>\n#include <cstdint>\ninline constexpr std::array<uint32_t,0> memory_store_canonical_spv{};\n")
    message(STATUS "Optional canonical resolve store disabled: unreviewed reference shader")
endif()
get_property(canonical_store_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(canonical_store_target IN LISTS canonical_store_targets)
    get_target_property(canonical_store_sources ${canonical_store_target} SOURCES)
    if("${canonical_store_sources}" MATCHES "scene_executor[.]cpp")
        target_sources(${canonical_store_target} PRIVATE "${GOW3_CANONICAL_STORE_HEADER}")
    endif()
endforeach()
