# Optional exact reviewed secondaryFP24 packed->D32-clamp->packed clear.
# Unknown canonical producer/raster state disables the optional module.
if(DEFINED GOW3_NATIVE_SHADER_DIR AND DEFINED GOW3_NATIVE_GLSLANG)
    set(GOW3_PACKED_SECONDARY_STENCIL_CLEAR_HEADER "${GOW3_NATIVE_SHADER_DIR}/packed_secondary_stencil_clear_spv.h")
    set(secondary_stencil_clear_glslang "${GOW3_NATIVE_GLSLANG}")
else()
    set(GOW3_PACKED_SECONDARY_STENCIL_CLEAR_HEADER "${CMAKE_CURRENT_BINARY_DIR}/packed_secondary_stencil_clear_spv.h")
    set(secondary_stencil_clear_glslang "${GLSLANG_VALIDATOR}")
endif()
get_filename_component(secondary_stencil_clear_root "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(secondary_stencil_clear_source "${CMAKE_CURRENT_LIST_DIR}/packed_secondary_stencil_clear.comp")
set(secondary_stencil_clear_reviewed TRUE)
file(SHA256 "${secondary_stencil_clear_root}/tools/rendering/native/resolve_memory.comp" secondary_stencil_review_0)
if(NOT secondary_stencil_review_0 STREQUAL "052e625aaac362dd844a4a8f96f31cb9d1408606e757a72530ec36a507f2aecf")
    set(secondary_stencil_clear_reviewed FALSE)
endif()
file(SHA256 "${secondary_stencil_clear_root}/tools/rendering/native/restore_depth.frag" secondary_stencil_review_1)
if(NOT secondary_stencil_review_1 STREQUAL "11ead8630030f56152a4501ba4645da748a58d36e46670e80c9da6eadd6ef3fa")
    set(secondary_stencil_clear_reviewed FALSE)
endif()
file(SHA256 "${secondary_stencil_clear_root}/tools/rendering/native/restore_depth_stencil.frag" secondary_stencil_review_2)
if(NOT secondary_stencil_review_2 STREQUAL "35dff8da0c4c0efb05867991247b136997d171e044a356b1358109e84ec42ea2")
    set(secondary_stencil_clear_reviewed FALSE)
endif()
file(SHA256 "${secondary_stencil_clear_root}/src/renderer/native/pass_transfer.cpp" secondary_stencil_review_3)
if(NOT secondary_stencil_review_3 STREQUAL "10708a5737ec0cd0c6871f157646aa69a21872b2df7459b3aa65be9ae7871379")
    set(secondary_stencil_clear_reviewed FALSE)
endif()
file(SHA256 "${secondary_stencil_clear_root}/vendor/plume/plume_render_interface_types.h" secondary_stencil_review_4)
if(NOT secondary_stencil_review_4 STREQUAL "1516e73f7b1972a21f01844520363124cccecbd3fb8df33ce2673bc6b56128bd")
    set(secondary_stencil_clear_reviewed FALSE)
endif()
file(SHA256 "${secondary_stencil_clear_root}/vendor/plume/plume_vulkan.cpp" secondary_stencil_review_5)
if(NOT secondary_stencil_review_5 STREQUAL "4de9d6da613b8d48bb5e4118ca9d980591ce1501b68352819421e8e2c0678524")
    set(secondary_stencil_clear_reviewed FALSE)
endif()
file(SHA256 "${secondary_stencil_clear_source}" secondary_stencil_kernel_hash)
if(NOT secondary_stencil_kernel_hash STREQUAL "d483021b39cc7e743ed19d033629e7e7d40574245355595066966299623b6b24")
    set(secondary_stencil_clear_reviewed FALSE)
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${secondary_stencil_clear_source}"
    "${secondary_stencil_clear_root}/tools/rendering/native/resolve_memory.comp"
    "${secondary_stencil_clear_root}/tools/rendering/native/restore_depth.frag"
    "${secondary_stencil_clear_root}/tools/rendering/native/restore_depth_stencil.frag"
    "${secondary_stencil_clear_root}/src/renderer/native/pass_transfer.cpp"
    "${secondary_stencil_clear_root}/vendor/plume/plume_render_interface_types.h"
    "${secondary_stencil_clear_root}/vendor/plume/plume_vulkan.cpp"
)
if(secondary_stencil_clear_reviewed)
    add_custom_command(OUTPUT "${GOW3_PACKED_SECONDARY_STENCIL_CLEAR_HEADER}"
        COMMAND "${secondary_stencil_clear_glslang}" -V --target-env vulkan1.1
            --vn packed_secondary_stencil_clear_spv -o "${GOW3_PACKED_SECONDARY_STENCIL_CLEAR_HEADER}" "${secondary_stencil_clear_source}"
        DEPENDS "${secondary_stencil_clear_source}" "${CMAKE_CURRENT_LIST_FILE}" VERBATIM)
else()
    file(WRITE "${GOW3_PACKED_SECONDARY_STENCIL_CLEAR_HEADER}"
        "#pragma once\n#include <array>\n#include <cstdint>\ninline constexpr std::array<uint32_t,0> packed_secondary_stencil_clear_spv{};\n")
    message(STATUS "Optional packed secondary stencil clear disabled: unreviewed original codec/raster identity")
endif()
get_property(secondary_stencil_clear_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(secondary_stencil_clear_target IN LISTS secondary_stencil_clear_targets)
    get_target_property(secondary_stencil_clear_sources ${secondary_stencil_clear_target} SOURCES)
    if(secondary_stencil_clear_target STREQUAL "gow3-native-stencil-export-test" OR
       "${secondary_stencil_clear_sources}" MATCHES "scene_executor[.]cpp")
        target_sources(${secondary_stencil_clear_target} PRIVATE "${GOW3_PACKED_SECONDARY_STENCIL_CLEAR_HEADER}")
    endif()
endforeach()
