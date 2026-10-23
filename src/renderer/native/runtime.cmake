# Native rendering consumer integration; no changes to Plume's source tree.
if(TARGET gow3-native-renderer)
    return()
endif()
get_filename_component(GOW3_NATIVE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(PLUME_BUILD_EXAMPLES OFF CACHE BOOL "")
add_subdirectory("${GOW3_NATIVE_ROOT}/vendor/plume" "${CMAKE_CURRENT_BINARY_DIR}/native-plume")
# Header versions must agree with the host allocator/function tables. The
# private shims prevent Plume from embedding second implementations in its TU.
target_include_directories(plume BEFORE PUBLIC
    "${REXSDK_DIR}/thirdparty/vulkan-memory-allocator/include"
    "${REXSDK_DIR}/thirdparty/volk"
    "${REXSDK_DIR}/thirdparty/vulkan-headers/include")
target_include_directories(plume BEFORE PRIVATE "${CMAKE_CURRENT_LIST_DIR}/host_vulkan_headers")
target_link_libraries(plume PUBLIC volk::volk PRIVATE rex::runtime ${CMAKE_DL_LIBS})
if(UNIX AND NOT APPLE)
    find_package(X11 REQUIRED)
    # Plume's unused standalone swapchain code still references this loader
    # entry. The live game continues to use the host's Wayland presenter.
    target_compile_definitions(volk PRIVATE VK_USE_PLATFORM_XLIB_KHR)
    target_link_libraries(plume PUBLIC X11::X11)
elseif(WIN32)
    target_compile_definitions(volk PRIVATE VK_USE_PLATFORM_WIN32_KHR)
endif()
include("${CMAKE_CURRENT_LIST_DIR}/shader_compiler.cmake")
target_sources(gow3-native-shader-compiler PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/retained_opaque_phase_compiler.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/retained_opaque_phase_module.cpp")
include("${CMAKE_CURRENT_LIST_DIR}/native_early_depth_shader.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/native_early_depth_catalog.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/stencil_material_cutoff.cmake")
target_link_libraries(gow3-native-shader-compiler PUBLIC gow3-native-early-depth-catalog)
include("${CMAKE_CURRENT_LIST_DIR}/content_hash.cmake")

find_program(GOW3_NATIVE_GLSLANG glslangValidator REQUIRED)
set(GOW3_NATIVE_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/native-renderer")
set(GOW3_NATIVE_SHADER_SOURCE_DIR "${GOW3_NATIVE_ROOT}/tools/rendering/native")
foreach(mode restore store)
    add_custom_command(OUTPUT "${GOW3_NATIVE_SHADER_DIR}/retained_opaque_${mode}_spv.h"
        COMMAND "${GOW3_NATIVE_GLSLANG}" -V --target-env vulkan1.2
            --vn retained_opaque_${mode}_spv "-I${GOW3_NATIVE_SHADER_SOURCE_DIR}"
            -o "${GOW3_NATIVE_SHADER_DIR}/retained_opaque_${mode}_spv.h"
            "${GOW3_NATIVE_SHADER_SOURCE_DIR}/retained_opaque_${mode}.comp"
        DEPENDS "${GOW3_NATIVE_SHADER_SOURCE_DIR}/retained_opaque_${mode}.comp"
            "${GOW3_NATIVE_SHADER_SOURCE_DIR}/readonly_depth_attachment.glsl" VERBATIM)
endforeach()
# Distinct V4 phase conversion namespace; legacy retained modules stay intact.
foreach(mode restore store)
    add_custom_command(OUTPUT "${GOW3_NATIVE_SHADER_DIR}/retained_opaque_phase_${mode}_spv.h"
        COMMAND "${GOW3_NATIVE_GLSLANG}" -V --target-env vulkan1.2
            --vn retained_opaque_phase_${mode}_spv "-I${GOW3_NATIVE_SHADER_SOURCE_DIR}"
            -o "${GOW3_NATIVE_SHADER_DIR}/retained_opaque_phase_${mode}_spv.h"
            "${GOW3_NATIVE_SHADER_SOURCE_DIR}/retained_opaque_phase_${mode}.comp"
        DEPENDS "${GOW3_NATIVE_SHADER_SOURCE_DIR}/retained_opaque_phase_${mode}.comp"
            "${GOW3_NATIVE_SHADER_SOURCE_DIR}/retained_opaque_phase_depth.glsl" VERBATIM)
endforeach()
file(MAKE_DIRECTORY "${GOW3_NATIVE_SHADER_DIR}")
foreach(shader fullscreen resolve restore_depth restore_depth_stencil)
    if(shader STREQUAL "fullscreen")
        set(extension vert)
    else()
        set(extension frag)
    endif()
    add_custom_command(OUTPUT "${GOW3_NATIVE_SHADER_DIR}/${shader}_spv.h"
        COMMAND "${GOW3_NATIVE_GLSLANG}" -V --target-env vulkan1.1 --vn "${shader}_spv"
            -o "${GOW3_NATIVE_SHADER_DIR}/${shader}_spv.h" "${GOW3_NATIVE_SHADER_SOURCE_DIR}/${shader}.${extension}"
        DEPENDS "${GOW3_NATIVE_SHADER_SOURCE_DIR}/${shader}.${extension}" VERBATIM)
endforeach()
foreach(mode store load)
    if(mode STREQUAL "store")
        set(store_value 1)
    else()
        set(store_value 0)
    endif()
    add_custom_command(OUTPUT "${GOW3_NATIVE_SHADER_DIR}/memory_${mode}_spv.h"
        COMMAND "${GOW3_NATIVE_GLSLANG}" -V --target-env vulkan1.1 -DSTORE=${store_value}
            --vn "memory_${mode}_spv" -o "${GOW3_NATIVE_SHADER_DIR}/memory_${mode}_spv.h"
            "${GOW3_NATIVE_SHADER_SOURCE_DIR}/resolve_memory.comp"
        DEPENDS "${GOW3_NATIVE_SHADER_SOURCE_DIR}/resolve_memory.comp" VERBATIM)
endforeach()
add_custom_command(OUTPUT "${GOW3_NATIVE_SHADER_DIR}/memory_restore_frag_spv.h"
    COMMAND "${GOW3_NATIVE_GLSLANG}" -V --target-env vulkan1.1 -S frag -DSTORE=0 -DFRAGMENT=1
        --vn memory_restore_frag_spv -o "${GOW3_NATIVE_SHADER_DIR}/memory_restore_frag_spv.h"
        "${GOW3_NATIVE_SHADER_SOURCE_DIR}/resolve_memory.comp"
    DEPENDS "${GOW3_NATIVE_SHADER_SOURCE_DIR}/resolve_memory.comp" VERBATIM)
include("${GOW3_NATIVE_SHADER_SOURCE_DIR}/atlas_rgba_stencil_depth_store_shader.cmake")
include("${GOW3_NATIVE_SHADER_SOURCE_DIR}/fused_packed_atlas_resolve_shader.cmake")
include("${GOW3_NATIVE_SHADER_SOURCE_DIR}/canonical_store_shader.cmake")
include("${GOW3_NATIVE_SHADER_SOURCE_DIR}/packed_primary_stencil_clear_shader.cmake")
include("${GOW3_NATIVE_SHADER_SOURCE_DIR}/packed_secondary_stencil_clear_shader.cmake")
foreach(channels 1 2)
    if(channels EQUAL 1)
        set(compact_name r32)
    else()
        set(compact_name rg32)
    endif()
    add_custom_command(OUTPUT "${GOW3_NATIVE_SHADER_DIR}/memory_load_${compact_name}_spv.h"
        COMMAND "${GOW3_NATIVE_GLSLANG}" -V --target-env vulkan1.1 -DSTORE=0 -DLOAD_COMPONENTS=${channels}
            --vn memory_load_${compact_name}_spv -o "${GOW3_NATIVE_SHADER_DIR}/memory_load_${compact_name}_spv.h"
            "${GOW3_NATIVE_SHADER_SOURCE_DIR}/resolve_memory.comp"
        DEPENDS "${GOW3_NATIVE_SHADER_SOURCE_DIR}/resolve_memory.comp" VERBATIM)
endforeach()
foreach(atlas_clear_mode packed host)
    if(atlas_clear_mode STREQUAL "host")
        set(atlas_clear_name atlas_depth_clear_host)
        set(atlas_host_source 1)
    else()
        set(atlas_clear_name atlas_depth_clear)
        set(atlas_host_source 0)
    endif()
    add_custom_command(OUTPUT "${GOW3_NATIVE_SHADER_DIR}/${atlas_clear_name}_spv.h"
        COMMAND "${GOW3_NATIVE_GLSLANG}" -V --target-env vulkan1.1 -DHOST_SOURCE=${atlas_host_source}
            --vn ${atlas_clear_name}_spv -o "${GOW3_NATIVE_SHADER_DIR}/${atlas_clear_name}_spv.h"
            "${GOW3_NATIVE_SHADER_SOURCE_DIR}/atlas_depth_clear.comp"
        DEPENDS "${GOW3_NATIVE_SHADER_SOURCE_DIR}/atlas_depth_clear.comp" VERBATIM)
endforeach()
add_custom_command(OUTPUT "${GOW3_NATIVE_SHADER_DIR}/raw_float_texture_spv.h"
    COMMAND "${GOW3_NATIVE_GLSLANG}" -V --target-env vulkan1.1 --vn raw_float_texture_spv
        -o "${GOW3_NATIVE_SHADER_DIR}/raw_float_texture_spv.h" "${GOW3_NATIVE_SHADER_SOURCE_DIR}/raw_float_texture.comp"
    DEPENDS "${GOW3_NATIVE_SHADER_SOURCE_DIR}/raw_float_texture.comp" VERBATIM)
add_custom_command(OUTPUT "${GOW3_NATIVE_SHADER_DIR}/state_transfer_spv.h"
    COMMAND "${GOW3_NATIVE_GLSLANG}" -V --target-env vulkan1.1 --vn state_transfer_spv
        -o "${GOW3_NATIVE_SHADER_DIR}/state_transfer_spv.h" "${GOW3_NATIVE_SHADER_SOURCE_DIR}/state_transfer.comp"
    DEPENDS "${GOW3_NATIVE_SHADER_SOURCE_DIR}/state_transfer.comp" VERBATIM)
add_custom_command(OUTPUT "${GOW3_NATIVE_SHADER_DIR}/early_depth_lower_bound_comp_spv.h"
    COMMAND "${GOW3_NATIVE_GLSLANG}" -V --target-env vulkan1.2
        --vn early_depth_lower_bound_comp_spv
        -o "${GOW3_NATIVE_SHADER_DIR}/early_depth_lower_bound_comp_spv.h"
        "${GOW3_NATIVE_SHADER_SOURCE_DIR}/early_depth_lower_bound.comp"
    DEPENDS "${GOW3_NATIVE_SHADER_SOURCE_DIR}/early_depth_lower_bound.comp" VERBATIM)
foreach(stage comp frag)
    add_custom_command(OUTPUT "${GOW3_NATIVE_SHADER_DIR}/readonly_depth_attachment_${stage}_spv.h"
        COMMAND "${GOW3_NATIVE_GLSLANG}" -V --target-env vulkan1.2
            --vn readonly_depth_attachment_${stage}_spv "-I${GOW3_NATIVE_SHADER_SOURCE_DIR}"
            -o "${GOW3_NATIVE_SHADER_DIR}/readonly_depth_attachment_${stage}_spv.h"
            "${GOW3_NATIVE_SHADER_SOURCE_DIR}/readonly_depth_attachment.${stage}"
        DEPENDS "${GOW3_NATIVE_SHADER_SOURCE_DIR}/readonly_depth_attachment.${stage}"
            "${GOW3_NATIVE_SHADER_SOURCE_DIR}/readonly_depth_attachment.glsl" VERBATIM)
endforeach()
foreach(mode restore store)
    add_custom_command(OUTPUT "${GOW3_NATIVE_SHADER_DIR}/retained_fp24_depth_${mode}_spv.h"
        COMMAND "${GOW3_NATIVE_GLSLANG}" -V --target-env vulkan1.2 --vn retained_fp24_depth_${mode}_spv
            -o "${GOW3_NATIVE_SHADER_DIR}/retained_fp24_depth_${mode}_spv.h"
            "${GOW3_NATIVE_SHADER_SOURCE_DIR}/retained_fp24_depth_${mode}.comp"
        DEPENDS "${GOW3_NATIVE_SHADER_SOURCE_DIR}/retained_fp24_depth_${mode}.comp" VERBATIM)
endforeach()
add_library(gow3-native-renderer STATIC
    "${CMAKE_CURRENT_LIST_DIR}/transfer_batch.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/gpu_work_queue.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/draw_timestamps.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/pass_transfer.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/fused_packed_atlas_resolve.cpp"
    "${GOW3_FUSED_PACKED_ATLAS_RESOLVE_HEADER}"
    "${CMAKE_CURRENT_LIST_DIR}/texture_data.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/texture_upload.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/draw_state.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/resolve_memory.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/state_transfer.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/memory_export_copy.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/live_device.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/fsi_device.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/live_plan.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/live_draw_json.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/lazy_device_state.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/runtime_bridge.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/query_tracker.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/query_comparison.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/scene_executor.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/native_monotonic_depth_module.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/readonly_depth_conversion.cpp"
    "${GOW3_NATIVE_SHADER_DIR}/early_depth_lower_bound_comp_spv.h"
    "${CMAKE_CURRENT_LIST_DIR}/retained_opaque_writer.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/retained_fp24_depth_writer.cpp"
    "${GOW3_NATIVE_SHADER_DIR}/retained_fp24_depth_restore_spv.h"
    "${GOW3_NATIVE_SHADER_DIR}/retained_fp24_depth_store_spv.h"
    "${GOW3_NATIVE_SHADER_DIR}/retained_opaque_restore_spv.h"
    "${GOW3_NATIVE_SHADER_DIR}/retained_opaque_store_spv.h"
    "${GOW3_NATIVE_SHADER_DIR}/retained_opaque_phase_restore_spv.h"
    "${GOW3_NATIVE_SHADER_DIR}/retained_opaque_phase_store_spv.h"
    "${GOW3_NATIVE_SHADER_DIR}/readonly_depth_attachment_comp_spv.h"
    "${GOW3_NATIVE_SHADER_DIR}/readonly_depth_attachment_frag_spv.h"
    "${CMAKE_CURRENT_LIST_DIR}/fsi_shader_variant.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/builtin_atomic_depth.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/builtin_readonly_query.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/pipeline_prewarm.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/fsi_state_recipe_cache.cpp"
    "${GOW3_NATIVE_SHADER_DIR}/fullscreen_spv.h"
    "${GOW3_NATIVE_SHADER_DIR}/resolve_spv.h"
    "${GOW3_NATIVE_SHADER_DIR}/restore_depth_spv.h"
    "${GOW3_NATIVE_SHADER_DIR}/restore_depth_stencil_spv.h"
    "${GOW3_NATIVE_SHADER_DIR}/memory_load_spv.h"
    "${GOW3_NATIVE_SHADER_DIR}/memory_load_r32_spv.h"
    "${GOW3_NATIVE_SHADER_DIR}/memory_load_rg32_spv.h"
    "${GOW3_NATIVE_SHADER_DIR}/memory_store_spv.h"
    "${GOW3_NATIVE_SHADER_DIR}/memory_restore_frag_spv.h"
    "${GOW3_CANONICAL_STORE_HEADER}"
    "${GOW3_ATLAS_RGBA_STENCIL_HEADER}"
    "${GOW3_PACKED_STENCIL_CLEAR_HEADER}"
    "${GOW3_PACKED_SECONDARY_STENCIL_CLEAR_HEADER}"
    "${GOW3_NATIVE_SHADER_DIR}/atlas_depth_clear_spv.h"
    "${GOW3_NATIVE_SHADER_DIR}/atlas_depth_clear_host_spv.h"
    "${GOW3_NATIVE_SHADER_DIR}/raw_float_texture_spv.h"
    "${GOW3_NATIVE_SHADER_DIR}/state_transfer_spv.h")
target_compile_features(gow3-native-renderer PUBLIC cxx_std_23)
target_include_directories(gow3-native-renderer PUBLIC "${GOW3_NATIVE_ROOT}/src"
    "${REXSDK_DIR}/thirdparty/inja/third_party/include")
target_include_directories(gow3-native-renderer PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" "${GOW3_NATIVE_SHADER_DIR}"
    "${REXSDK_DIR}/thirdparty/picosha2" "${REXSDK_DIR}/thirdparty/glslang")
# Counter census remains a replay-only target; production links only the
# authenticated source/compiler helper and its uninstrumented module.
target_link_libraries(gow3-native-renderer PUBLIC plume gow3-native-shader-compiler PRIVATE rex::runtime xxHash::xxhash gow3-native-content-hash gow3-native-stencil-material-cutoff)
if(UNIX AND NOT APPLE)
    # Both header consumers must see the complete platform declaration set on
    # their first inclusion of vulkan.h, regardless of include order.
    target_compile_definitions(gow3-native-renderer PRIVATE
        VK_USE_PLATFORM_XLIB_KHR= VK_USE_PLATFORM_XCB_KHR= VK_USE_PLATFORM_WAYLAND_KHR=)
endif()
