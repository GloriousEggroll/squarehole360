# Optional exact reviewed primaryFP24 packed->D32-clamp->packed clear.
# Unknown canonical producer/raster state disables the optional module.
if(DEFINED GOW3_NATIVE_SHADER_DIR AND DEFINED GOW3_NATIVE_GLSLANG)
    set(GOW3_PACKED_STENCIL_CLEAR_HEADER "${GOW3_NATIVE_SHADER_DIR}/packed_primary_stencil_clear_spv.h")
    set(stencil_clear_glslang "${GOW3_NATIVE_GLSLANG}")
else()
    set(GOW3_PACKED_STENCIL_CLEAR_HEADER "${CMAKE_CURRENT_BINARY_DIR}/packed_primary_stencil_clear_spv.h")
    set(stencil_clear_glslang "${GLSLANG_VALIDATOR}")
endif()
get_filename_component(stencil_clear_root "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(stencil_clear_source "${CMAKE_CURRENT_LIST_DIR}/packed_primary_stencil_clear.comp")
set(stencil_clear_reviewed TRUE)
file(SHA256 "${stencil_clear_root}/tools/rendering/native/resolve_memory.comp" stencil_review_0)
if(NOT stencil_review_0 STREQUAL "052e625aaac362dd844a4a8f96f31cb9d1408606e757a72530ec36a507f2aecf")
    set(stencil_clear_reviewed FALSE)
endif()
file(SHA256 "${stencil_clear_root}/tools/rendering/native/restore_depth.frag" stencil_review_1)
if(NOT stencil_review_1 STREQUAL "11ead8630030f56152a4501ba4645da748a58d36e46670e80c9da6eadd6ef3fa")
    set(stencil_clear_reviewed FALSE)
endif()
file(SHA256 "${stencil_clear_root}/tools/rendering/native/restore_depth_stencil.frag" stencil_review_2)
if(NOT stencil_review_2 STREQUAL "35dff8da0c4c0efb05867991247b136997d171e044a356b1358109e84ec42ea2")
    set(stencil_clear_reviewed FALSE)
endif()
file(SHA256 "${stencil_clear_root}/src/renderer/native/pass_transfer.cpp" stencil_review_3)
if(NOT stencil_review_3 STREQUAL "10708a5737ec0cd0c6871f157646aa69a21872b2df7459b3aa65be9ae7871379")
    set(stencil_clear_reviewed FALSE)
endif()
file(SHA256 "${stencil_clear_root}/vendor/plume/plume_render_interface_types.h" stencil_review_4)
if(NOT stencil_review_4 STREQUAL "1516e73f7b1972a21f01844520363124cccecbd3fb8df33ce2673bc6b56128bd")
    set(stencil_clear_reviewed FALSE)
endif()
file(SHA256 "${stencil_clear_root}/vendor/plume/plume_vulkan.cpp" stencil_review_5)
if(NOT stencil_review_5 STREQUAL "4de9d6da613b8d48bb5e4118ca9d980591ce1501b68352819421e8e2c0678524")
    set(stencil_clear_reviewed FALSE)
endif()
file(SHA256 "${stencil_clear_source}" stencil_kernel_hash)
if(NOT stencil_kernel_hash STREQUAL "f4a237941a592c142535eb7124a93a9c88c93dc30f6852efdecb2cbe3d038c5c")
    set(stencil_clear_reviewed FALSE)
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${stencil_clear_source}"
    "${stencil_clear_root}/tools/rendering/native/resolve_memory.comp"
    "${stencil_clear_root}/tools/rendering/native/restore_depth.frag"
    "${stencil_clear_root}/tools/rendering/native/restore_depth_stencil.frag"
    "${stencil_clear_root}/src/renderer/native/pass_transfer.cpp"
    "${stencil_clear_root}/vendor/plume/plume_render_interface_types.h"
    "${stencil_clear_root}/vendor/plume/plume_vulkan.cpp"
)
if(stencil_clear_reviewed)
    add_custom_command(OUTPUT "${GOW3_PACKED_STENCIL_CLEAR_HEADER}"
        COMMAND "${stencil_clear_glslang}" -V --target-env vulkan1.1
            --vn packed_primary_stencil_clear_spv -o "${GOW3_PACKED_STENCIL_CLEAR_HEADER}" "${stencil_clear_source}"
        DEPENDS "${stencil_clear_source}" "${CMAKE_CURRENT_LIST_FILE}" VERBATIM)
else()
    file(WRITE "${GOW3_PACKED_STENCIL_CLEAR_HEADER}"
        "#pragma once\n#include <array>\n#include <cstdint>\ninline constexpr std::array<uint32_t,0> packed_primary_stencil_clear_spv{};\n")
    message(STATUS "Optional packed primary stencil clear disabled: unreviewed original codec/raster identity")
endif()
get_property(stencil_clear_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(stencil_clear_target IN LISTS stencil_clear_targets)
    get_target_property(stencil_clear_sources ${stencil_clear_target} SOURCES)
    if(stencil_clear_target STREQUAL "gow3-native-stencil-export-test" OR
       "${stencil_clear_sources}" MATCHES "scene_executor[.]cpp")
        target_sources(${stencil_clear_target} PRIVATE "${GOW3_PACKED_STENCIL_CLEAR_HEADER}")
    endif()
endforeach()
