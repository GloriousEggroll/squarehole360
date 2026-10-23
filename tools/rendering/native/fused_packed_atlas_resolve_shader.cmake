if(DEFINED GOW3_NATIVE_SHADER_DIR AND DEFINED GOW3_NATIVE_GLSLANG)
    set(GOW3_FUSED_PACKED_ATLAS_RESOLVE_HEADER "${GOW3_NATIVE_SHADER_DIR}/fused_packed_atlas_resolve_spv.h")
    set(fused_atlas_glslang "${GOW3_NATIVE_GLSLANG}")
else()
    set(GOW3_FUSED_PACKED_ATLAS_RESOLVE_HEADER "${CMAKE_CURRENT_BINARY_DIR}/fused_packed_atlas_resolve_spv.h")
    set(fused_atlas_glslang "${GLSLANG_VALIDATOR}")
endif()
set(fused_atlas_reference_0 "${CMAKE_CURRENT_LIST_DIR}/fullscreen.vert")
file(SHA256 "${fused_atlas_reference_0}" fused_atlas_hash_0)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${fused_atlas_reference_0}")
set(fused_atlas_reference_1 "${CMAKE_CURRENT_LIST_DIR}/resolve.frag")
file(SHA256 "${fused_atlas_reference_1}" fused_atlas_hash_1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${fused_atlas_reference_1}")
set(fused_atlas_reference_2 "${CMAKE_CURRENT_LIST_DIR}/resolve_memory.comp")
file(SHA256 "${fused_atlas_reference_2}" fused_atlas_hash_2)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${fused_atlas_reference_2}")
set(fused_atlas_reference_3 "${CMAKE_CURRENT_LIST_DIR}/memory_store_rgba_stencil.comp")
file(SHA256 "${fused_atlas_reference_3}" fused_atlas_hash_3)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${fused_atlas_reference_3}")
get_filename_component(fused_atlas_root "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(fused_atlas_reference_4 "${fused_atlas_root}/src/renderer/native/pass_transfer.cpp")
file(SHA256 "${fused_atlas_reference_4}" fused_atlas_hash_4)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${fused_atlas_reference_4}")
if(fused_atlas_hash_0 STREQUAL "edbcee00a1f95ad7e79325e5e55b9aebe35a7fb624ce8e1c5675d066e75c1c1c"
   AND fused_atlas_hash_1 STREQUAL "33dfab40a9997d90e6cc60799fdf8a137b584cc59a5f0bc169b8e473964054f8"
   AND fused_atlas_hash_2 STREQUAL "052e625aaac362dd844a4a8f96f31cb9d1408606e757a72530ec36a507f2aecf"
   AND fused_atlas_hash_3 STREQUAL "6503d0071557172541fdb32e2f9ee829fac6c7ff7250bd950c621b9987ba0baa"
   AND fused_atlas_hash_4 STREQUAL "10708a5737ec0cd0c6871f157646aa69a21872b2df7459b3aa65be9ae7871379")
    add_custom_command(OUTPUT "${GOW3_FUSED_PACKED_ATLAS_RESOLVE_HEADER}"
        COMMAND "${fused_atlas_glslang}" -V --target-env vulkan1.1 --vn fused_packed_atlas_resolve_spv
            -o "${GOW3_FUSED_PACKED_ATLAS_RESOLVE_HEADER}" "${CMAKE_CURRENT_LIST_DIR}/fused_packed_atlas_resolve.frag"
        DEPENDS "${CMAKE_CURRENT_LIST_DIR}/fused_packed_atlas_resolve.frag" "${CMAKE_CURRENT_LIST_FILE}"
            "${fused_atlas_reference_0}" "${fused_atlas_reference_1}" "${fused_atlas_reference_2}"
            "${fused_atlas_reference_3}" "${fused_atlas_reference_4}" VERBATIM)
else()
    file(WRITE "${GOW3_FUSED_PACKED_ATLAS_RESOLVE_HEADER}"
        "#pragma once\n#include <array>\n#include <cstdint>\ninline constexpr std::array<uint32_t,0> fused_packed_atlas_resolve_spv{};\n")
    message(STATUS "Optional fused packed atlas resolve disabled: unreviewed canonical source identity")
endif()
get_property(fused_atlas_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(fused_atlas_target IN LISTS fused_atlas_targets)
    get_target_property(fused_atlas_sources ${fused_atlas_target} SOURCES)
    if(fused_atlas_target STREQUAL "gow3-native-stencil-export-test" OR
       "${fused_atlas_sources}" MATCHES "scene_executor[.]cpp")
        target_sources(${fused_atlas_target} PRIVATE "${GOW3_FUSED_PACKED_ATLAS_RESOLVE_HEADER}"
            "${fused_atlas_root}/src/renderer/native/fused_packed_atlas_resolve.cpp")
    endif()
endforeach()
