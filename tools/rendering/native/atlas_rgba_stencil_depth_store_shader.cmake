# Same descriptor/push ABI as memory_store. Only the stencil texture type and
# original RestoreDepth alpha->S8 expression differ; all other bytes restore.
if(DEFINED GOW3_NATIVE_SHADER_DIR AND DEFINED GOW3_NATIVE_GLSLANG)
    set(GOW3_ATLAS_RGBA_STENCIL_HEADER "${GOW3_NATIVE_SHADER_DIR}/memory_store_rgba_stencil_spv.h")
    set(atlas_rgba_glslang "${GOW3_NATIVE_GLSLANG}")
else()
    set(GOW3_ATLAS_RGBA_STENCIL_HEADER "${CMAKE_CURRENT_BINARY_DIR}/memory_store_rgba_stencil_spv.h")
    set(atlas_rgba_glslang "${GLSLANG_VALIDATOR}")
endif()
set(atlas_rgba_original "${CMAKE_CURRENT_LIST_DIR}/resolve_memory.comp")
set(atlas_rgba_source "${CMAKE_CURRENT_LIST_DIR}/memory_store_rgba_stencil.comp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${atlas_rgba_original}" "${atlas_rgba_source}")
file(SHA256 "${atlas_rgba_original}" atlas_rgba_original_hash)
file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/restore_depth.frag" atlas_rgba_legacy_hash)
file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/restore_depth_stencil.frag" atlas_rgba_export_hash)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/restore_depth.frag" "${CMAKE_CURRENT_LIST_DIR}/restore_depth_stencil.frag")
file(READ "${atlas_rgba_source}" atlas_rgba_recovered)
string(REPLACE "layout(set=0,binding=2) uniform texture2D stencil_image;"
    "layout(set=0,binding=2) uniform utexture2D stencil_image;" atlas_rgba_recovered "${atlas_rgba_recovered}")
string(REPLACE "uint stencil=(pc.format&0x80000000u)!=0?uint(roundEven(clamp(texelFetch(stencil_image,ivec2(p)+pc.stencil_offset,0).a,0.0,1.0)*255.0)):0;"
    "uint stencil=(pc.format&0x80000000u)!=0?texelFetch(stencil_image,ivec2(p)+pc.stencil_offset,0).x:0;"
    atlas_rgba_recovered "${atlas_rgba_recovered}")
string(SHA256 atlas_rgba_recovered_hash "${atlas_rgba_recovered}")
set(atlas_rgba_reviewed "052e625aaac362dd844a4a8f96f31cb9d1408606e757a72530ec36a507f2aecf")
if(atlas_rgba_original_hash STREQUAL atlas_rgba_reviewed AND atlas_rgba_recovered_hash STREQUAL atlas_rgba_reviewed AND
   atlas_rgba_legacy_hash STREQUAL "11ead8630030f56152a4501ba4645da748a58d36e46670e80c9da6eadd6ef3fa" AND
   atlas_rgba_export_hash STREQUAL "35dff8da0c4c0efb05867991247b136997d171e044a356b1358109e84ec42ea2")
    add_custom_command(OUTPUT "${GOW3_ATLAS_RGBA_STENCIL_HEADER}"
        COMMAND "${atlas_rgba_glslang}" -V --target-env vulkan1.1 -DSTORE=1
            --vn memory_store_rgba_stencil_spv -o "${GOW3_ATLAS_RGBA_STENCIL_HEADER}" "${atlas_rgba_source}"
        DEPENDS "${atlas_rgba_source}" "${atlas_rgba_original}" "${CMAKE_CURRENT_LIST_FILE}" VERBATIM)
else()
    # Unknown future producer/source declines this optional pipeline at startup;
    # it must not quietly use a stale codec or prevent canonical rendering.
    file(WRITE "${GOW3_ATLAS_RGBA_STENCIL_HEADER}"
        "#pragma once\n#include <array>\n#include <cstdint>\ninline constexpr std::array<uint32_t,0> memory_store_rgba_stencil_spv{};\n")
    message(STATUS "Optional atlas RGBA stencil store disabled: unreviewed source identity")
endif()
get_property(atlas_rgba_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(atlas_rgba_target IN LISTS atlas_rgba_targets)
    get_target_property(atlas_rgba_sources ${atlas_rgba_target} SOURCES)
    if(atlas_rgba_target STREQUAL "gow3-native-stencil-export-test" OR
       "${atlas_rgba_sources}" MATCHES "scene_executor[.]cpp")
        target_sources(${atlas_rgba_target} PRIVATE "${GOW3_ATLAS_RGBA_STENCIL_HEADER}")
    endif()
endforeach()
