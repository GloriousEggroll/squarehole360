cmake_minimum_required(VERSION 3.25)
project(gow3_native_renderer_validation LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 23)
get_filename_component(GOW3_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
add_executable(gow3-native-cpu-image-slot-lookup-test cpu_image_slot_lookup_test.cpp)
target_include_directories(gow3-native-cpu-image-slot-lookup-test PRIVATE "${GOW3_ROOT}/src")
# CPU-only exact image-storage identity and decode/footprint regression corpus.
add_executable(gow3-native-cpu-texture-storage-sampling-key-test
    cpu_texture_storage_sampling_key_test.cpp
    "${GOW3_ROOT}/src/renderer/native/texture_data.cpp")
target_include_directories(gow3-native-cpu-texture-storage-sampling-key-test PRIVATE "${GOW3_ROOT}/src")
target_compile_features(gow3-native-cpu-texture-storage-sampling-key-test PRIVATE cxx_std_23)
add_executable(gow3-native-cpu-upload-receipts-test cpu_upload_receipts_test.cpp)
target_include_directories(gow3-native-cpu-upload-receipts-test PRIVATE "${GOW3_ROOT}/src")
add_executable(gow3-native-retained-resource-pool-policy-test retained_resource_pool_policy_test.cpp)
target_include_directories(gow3-native-retained-resource-pool-policy-test PRIVATE "${GOW3_ROOT}/src")
add_executable(gow3-native-draw-timestamps-test draw_timestamps_test.cpp)
target_include_directories(gow3-native-draw-timestamps-test PRIVATE "${GOW3_ROOT}/src")
add_executable(gow3-native-query-prefix-completion-test query_prefix_completion_test.cpp)
target_include_directories(gow3-native-query-prefix-completion-test PRIVATE
    "${GOW3_ROOT}/src" "${GOW3_ROOT}/vendor/rexglue-sdk/include")
add_executable(gow3-native-raw-float-texture-test raw_float_texture_test.cpp
    "${GOW3_ROOT}/src/renderer/native/texture_data.cpp")
target_include_directories(gow3-native-raw-float-texture-test PRIVATE "${GOW3_ROOT}/src")
# The enclosing standalone test project already requires C++23; std::byteswap
# also makes this target's language requirement explicit for other consumers.
target_compile_features(gow3-native-raw-float-texture-test PRIVATE cxx_std_23)

add_executable(gow3-native-retired-buffer-pool-test retired_buffer_pool_test.cpp)
target_include_directories(gow3-native-retired-buffer-pool-test PRIVATE "${GOW3_ROOT}/src")
add_executable(gow3-native-cpu-write-policy-test cpu_write_policy_test.cpp)
target_include_directories(gow3-native-cpu-write-policy-test PRIVATE
    "${GOW3_ROOT}/src" "${GOW3_ROOT}/vendor/rexglue-sdk/include")
add_library(gow3-native-live-draw-json STATIC "${GOW3_ROOT}/src/renderer/native/live_draw_json.cpp")
target_sources(gow3-native-live-draw-json PRIVATE "${GOW3_ROOT}/src/renderer/native/lazy_device_state.cpp")
target_link_libraries(gow3-native-live-draw-json PRIVATE gow3-native-content-hash)
if(NOT TARGET xxHash::xxhash)
    set(XXHASH_BUILD_XXHSUM OFF CACHE BOOL "")
    add_subdirectory("${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/xxHash/cmake_unofficial" xxhash)
endif()
target_include_directories(gow3-native-live-draw-json PUBLIC "${GOW3_ROOT}/src"
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/inja/third_party/include")
set(PLUME_BUILD_EXAMPLES OFF CACHE BOOL "")
add_subdirectory("${GOW3_ROOT}/vendor/plume" plume)
find_program(GLSLANG_VALIDATOR glslangValidator REQUIRED)
if(UNIX AND NOT APPLE)
    find_package(X11 REQUIRED)
endif()
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/fullscreen_spv.h"
    COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.1
        --vn fullscreen_spv -o "${CMAKE_CURRENT_BINARY_DIR}/fullscreen_spv.h"
        "${CMAKE_CURRENT_LIST_DIR}/fullscreen.vert"
    DEPENDS fullscreen.vert VERBATIM)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/raw_float_texture_spv.h"
    COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.1 --vn raw_float_texture_spv
        -o "${CMAKE_CURRENT_BINARY_DIR}/raw_float_texture_spv.h" "${CMAKE_CURRENT_LIST_DIR}/raw_float_texture.comp"
    DEPENDS raw_float_texture.comp VERBATIM)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/resolve_spv.h"
    COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.1
        --vn resolve_spv -o "${CMAKE_CURRENT_BINARY_DIR}/resolve_spv.h"
        "${CMAKE_CURRENT_LIST_DIR}/resolve.frag"
    DEPENDS resolve.frag VERBATIM)
foreach(mode store load)
    if(mode STREQUAL "store")
        set(store_value 1)
    else()
        set(store_value 0)
    endif()
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/memory_${mode}_spv.h"
        COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.1 -DSTORE=${store_value}
            --vn memory_${mode}_spv -o "${CMAKE_CURRENT_BINARY_DIR}/memory_${mode}_spv.h"
            "${CMAKE_CURRENT_LIST_DIR}/resolve_memory.comp"
        DEPENDS resolve_memory.comp VERBATIM)
endforeach()
# Same rule as src/renderer/native/runtime.cmake (direct color restore).
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/memory_restore_frag_spv.h"
    COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.1 -S frag -DSTORE=0 -DFRAGMENT=1
        --vn memory_restore_frag_spv -o "${CMAKE_CURRENT_BINARY_DIR}/memory_restore_frag_spv.h"
        "${CMAKE_CURRENT_LIST_DIR}/resolve_memory.comp"
    DEPENDS resolve_memory.comp VERBATIM)
foreach(channels 1 2)
    if(channels EQUAL 1)
        set(compact_name r32)
    else()
        set(compact_name rg32)
    endif()
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/memory_load_${compact_name}_spv.h"
        COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.1 -DSTORE=0 -DLOAD_COMPONENTS=${channels}
            --vn memory_load_${compact_name}_spv -o "${CMAKE_CURRENT_BINARY_DIR}/memory_load_${compact_name}_spv.h"
            "${CMAKE_CURRENT_LIST_DIR}/resolve_memory.comp"
        DEPENDS resolve_memory.comp VERBATIM)
endforeach()
add_executable(gow3-native-pass-test pass_test.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/fullscreen_spv.h" "${CMAKE_CURRENT_BINARY_DIR}/resolve_spv.h"
    "${GOW3_ROOT}/src/renderer/native/pass_transfer.cpp")
add_executable(gow3-native-scene-replay scene_replay.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/raw_float_texture_spv.h"
    "${GOW3_ROOT}/src/renderer/native/pipeline_prewarm.cpp"
    "${GOW3_ROOT}/src/renderer/native/query_tracker.cpp"
    "${GOW3_ROOT}/src/renderer/native/scene_executor.cpp"
    "${GOW3_ROOT}/src/renderer/native/native_monotonic_depth_module.cpp"
    "${GOW3_ROOT}/src/renderer/native/readonly_depth_conversion.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/readonly_depth_attachment_comp_spv.h"
    "${CMAKE_CURRENT_BINARY_DIR}/readonly_depth_attachment_frag_spv.h"
    "${GOW3_ROOT}/src/renderer/native/draw_timestamps.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/restore_depth_stencil_spv.h"
    "${GOW3_ROOT}/src/renderer/native/fsi_shader_variant.cpp"
    "${GOW3_ROOT}/src/renderer/native/builtin_atomic_depth.cpp"
    "${GOW3_ROOT}/src/renderer/native/builtin_readonly_query.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/fullscreen_spv.h" "${CMAKE_CURRENT_BINARY_DIR}/resolve_spv.h"
    "${GOW3_ROOT}/src/renderer/native/pass_transfer.cpp"
    "${GOW3_ROOT}/src/renderer/native/texture_data.cpp"
    "${GOW3_ROOT}/src/renderer/native/texture_upload.cpp"
    "${GOW3_ROOT}/src/renderer/native/draw_state.cpp")
foreach(target gow3-native-scene-replay gow3-native-pass-test)
    target_sources(${target} PRIVATE "${GOW3_ROOT}/src/renderer/native/transfer_batch.cpp"
        "${GOW3_ROOT}/src/renderer/native/gpu_work_queue.cpp")
    target_sources(${target} PRIVATE "${GOW3_ROOT}/src/renderer/native/resolve_memory.cpp"
        "${CMAKE_CURRENT_BINARY_DIR}/memory_store_spv.h" "${CMAKE_CURRENT_BINARY_DIR}/memory_load_spv.h")
    target_sources(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/restore_depth_spv.h")
endforeach()
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/restore_depth_spv.h"
    COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.1 --vn restore_depth_spv
        -o "${CMAKE_CURRENT_BINARY_DIR}/restore_depth_spv.h" "${CMAKE_CURRENT_LIST_DIR}/restore_depth.frag"
    DEPENDS restore_depth.frag VERBATIM)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/restore_depth_stencil_spv.h"
    COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.1 --vn restore_depth_stencil_spv
        -o "${CMAKE_CURRENT_BINARY_DIR}/restore_depth_stencil_spv.h"
        "${CMAKE_CURRENT_LIST_DIR}/restore_depth_stencil.frag"
    DEPENDS restore_depth_stencil.frag VERBATIM)
target_sources(gow3-native-pass-test PRIVATE "${GOW3_ROOT}/src/renderer/native/texture_data.cpp")
target_include_directories(gow3-native-scene-replay PRIVATE
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/inja/third_party/include"
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/picosha2")
add_executable(gow3-native-shader-test shader_test.cpp "${CMAKE_CURRENT_BINARY_DIR}/fullscreen_spv.h")
add_executable(gow3-native-mesh-replay mesh_replay.cpp "${GOW3_ROOT}/src/renderer/native/texture_data.cpp"
    "${GOW3_ROOT}/src/renderer/native/texture_upload.cpp" "${GOW3_ROOT}/src/renderer/native/draw_state.cpp")
foreach(target gow3-native-shader-test gow3-native-mesh-replay gow3-native-pass-test gow3-native-scene-replay)
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" "${GOW3_ROOT}/src")
    target_link_libraries(${target} PRIVATE plume ${CMAKE_DL_LIBS})
    if(TARGET X11::X11)
        target_link_libraries(${target} PRIVATE X11::X11)
    endif()
endforeach()
target_sources(gow3-native-shader-test PRIVATE "${GOW3_ROOT}/src/renderer/native/texture_data.cpp"
    "${GOW3_ROOT}/src/renderer/native/texture_upload.cpp" "${GOW3_ROOT}/src/renderer/native/draw_state.cpp")
# Plume's internal checks remain active in validation builds.
target_compile_options(plume PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/UNDEBUG,-UNDEBUG>)

include("${GOW3_ROOT}/src/renderer/native/shader_compiler.cmake")
# Separate byte-pinned phase producer; preserve the canonical compiler recipe.
target_sources(gow3-native-shader-compiler PRIVATE
    "${GOW3_ROOT}/src/renderer/native/retained_opaque_phase_compiler.cpp"
    "${GOW3_ROOT}/src/renderer/native/retained_opaque_phase_module.cpp")
add_executable(gow3-native-retained-opaque-phase-policy-test retained_opaque_phase_policy_test.cpp)
target_include_directories(gow3-native-retained-opaque-phase-policy-test PRIVATE "${GOW3_ROOT}/src")
add_executable(gow3-native-retained-opaque-phase-pass-groups-test retained_opaque_phase_pass_groups_test.cpp)
target_include_directories(gow3-native-retained-opaque-phase-pass-groups-test PRIVATE "${GOW3_ROOT}/src")

include("${GOW3_ROOT}/src/renderer/native/content_hash.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/native_overflow_prewarm_fixture.cmake")
add_executable(gow3-native-pipeline-prewarm-test pipeline_prewarm_test.cpp
    "${GOW3_ROOT}/src/renderer/native/pipeline_prewarm.cpp")
target_include_directories(gow3-native-pipeline-prewarm-test PRIVATE "${GOW3_ROOT}/src"
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/inja/third_party/include")
target_link_libraries(gow3-native-pipeline-prewarm-test PRIVATE plume gow3-native-content-hash Threads::Threads)
add_executable(gow3-native-query-poll-yield-test query_poll_yield_test.cpp)
target_include_directories(gow3-native-query-poll-yield-test PRIVATE "${GOW3_ROOT}/src")
add_executable(gow3-native-shader-descriptor-sets-test shader_descriptor_sets_test.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/readonly_depth_attachment_frag_spv.h")
target_include_directories(gow3-native-shader-descriptor-sets-test PRIVATE "${GOW3_ROOT}/src" "${CMAKE_CURRENT_BINARY_DIR}")
add_executable(gow3-native-shader-compiler-test shader_compiler_test.cpp)
add_executable(gow3-native-shader-prewarm-queue-test shader_prewarm_queue_test.cpp)
target_include_directories(gow3-native-shader-prewarm-queue-test PRIVATE "${GOW3_ROOT}/src")
target_link_libraries(gow3-native-shader-prewarm-queue-test PRIVATE Threads::Threads)
add_executable(gow3-native-shader-block-discovery-test shader_block_discovery_test.cpp)
target_include_directories(gow3-native-shader-block-discovery-test PRIVATE "${GOW3_ROOT}/src")
add_executable(gow3-native-shader-prewarm-compiler-test shader_prewarm_compiler_test.cpp)
target_link_libraries(gow3-native-shader-prewarm-compiler-test PRIVATE gow3-native-shader-compiler Threads::Threads)
target_include_directories(gow3-native-shader-prewarm-compiler-test PRIVATE
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/inja/third_party/include")
target_link_libraries(gow3-native-shader-compiler-test PRIVATE gow3-native-shader-compiler)
target_include_directories(gow3-native-shader-compiler-test PRIVATE
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/inja/third_party/include")
add_executable(gow3-native-shader-prewarm shader_prewarm.cpp)
target_link_libraries(gow3-native-shader-prewarm PRIVATE gow3-native-shader-compiler
    gow3-native-content-hash Threads::Threads)
target_include_directories(gow3-native-shader-prewarm PRIVATE
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/inja/third_party/include")

add_library(gow3-native-live-device STATIC "${GOW3_ROOT}/src/renderer/native/live_device.cpp")
target_sources(gow3-native-live-device PRIVATE "${GOW3_ROOT}/src/renderer/native/fsi_device.cpp")
target_include_directories(gow3-native-live-device PUBLIC "${GOW3_ROOT}/src")
target_link_libraries(gow3-native-live-device PUBLIC plume ${CMAKE_DL_LIBS})
if(TARGET X11::X11)
    target_link_libraries(gow3-native-live-device PUBLIC X11::X11)
endif()
add_executable(gow3-native-live-device-test live_device_test.cpp)
target_link_libraries(gow3-native-live-device-test PRIVATE gow3-native-live-device)
foreach(stage vertex fragment)
    if(stage STREQUAL "vertex")
        set(fsi_stage vert)
    else()
        set(fsi_stage frag)
    endif()
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/fsi_${stage}_spv.h"
        COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.2 --vn fsi_${stage}_spv
            "-I${GOW3_ROOT}/src/renderer/native" -o "${CMAKE_CURRENT_BINARY_DIR}/fsi_${stage}_spv.h"
            "${CMAKE_CURRENT_LIST_DIR}/fsi_precision.${fsi_stage}"
        DEPENDS "fsi_precision.${fsi_stage}" "${GOW3_ROOT}/src/renderer/native/fsi_precision.glsl" VERBATIM)
endforeach()
add_executable(gow3-native-fsi-test fsi_precision_test.cpp
    "${GOW3_ROOT}/src/renderer/native/transfer_batch.cpp"
    "${GOW3_ROOT}/src/renderer/native/gpu_work_queue.cpp"
    "${GOW3_ROOT}/src/renderer/native/fsi_precision.cpp"
    "${GOW3_ROOT}/src/renderer/native/query_tracker.cpp"
    "${GOW3_ROOT}/src/renderer/native/resolve_memory.cpp"
    "${GOW3_ROOT}/src/renderer/native/texture_data.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/fsi_vertex_spv.h" "${CMAKE_CURRENT_BINARY_DIR}/fsi_fragment_spv.h")
target_include_directories(gow3-native-fsi-test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(gow3-native-fsi-test PRIVATE gow3-native-live-device)
target_link_libraries(gow3-native-scene-replay PRIVATE gow3-native-live-device)
add_executable(gow3-native-page-version-table-test page_version_table_test.cpp)
target_include_directories(gow3-native-page-version-table-test PRIVATE "${GOW3_ROOT}/src")
add_executable(gow3-native-page-change-tracker-test page_change_tracker_test.cpp)
target_include_directories(gow3-native-page-change-tracker-test PRIVATE "${GOW3_ROOT}/src")

add_executable(gow3-native-live-plan-test live_plan_test.cpp
    "${GOW3_ROOT}/src/renderer/native/live_plan.cpp"
    "${GOW3_ROOT}/src/renderer/native/memory_export_copy.cpp"
    "${GOW3_ROOT}/src/renderer/native/texture_data.cpp")
target_compile_definitions(gow3-native-live-plan-test PRIVATE GOW3_NATIVE_VERIFY_PREFIX_DEPENDENCIES=1)
target_include_directories(gow3-native-live-plan-test PRIVATE "${GOW3_ROOT}/src"
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/inja/third_party/include"
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/picosha2")
add_executable(gow3-native-live-prefix-resources-test live_prefix_resources_test.cpp)
add_executable(gow3-native-boundary-block-pool-test boundary_block_pool_test.cpp)
target_include_directories(gow3-native-boundary-block-pool-test PRIVATE "${GOW3_ROOT}/src")
target_include_directories(gow3-native-live-prefix-resources-test PRIVATE "${GOW3_ROOT}/src"
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/inja/third_party/include")
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/query_test_spv.h"
    COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.1 --vn query_test_spv
        -o "${CMAKE_CURRENT_BINARY_DIR}/query_test_spv.h" "${CMAKE_CURRENT_LIST_DIR}/query_test.frag"
    DEPENDS query_test.frag VERBATIM)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/alias_query_test_spv.h"
    COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.1 --vn alias_query_test_spv
        -o "${CMAKE_CURRENT_BINARY_DIR}/alias_query_test_spv.h" "${CMAKE_CURRENT_LIST_DIR}/alias_query_test.frag"
    DEPENDS alias_query_test.frag VERBATIM)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/alias_pair_query_test_spv.h"
    COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.1 --vn alias_pair_query_test_spv
        -o "${CMAKE_CURRENT_BINARY_DIR}/alias_pair_query_test_spv.h" "${CMAKE_CURRENT_LIST_DIR}/alias_pair_query_test.frag"
    DEPENDS alias_pair_query_test.frag VERBATIM)
add_executable(gow3-native-query-test query_tracker_test.cpp
    "${GOW3_ROOT}/src/renderer/native/fsi_device.cpp"
    "${GOW3_ROOT}/src/renderer/native/live_device.cpp"
    "${GOW3_ROOT}/src/renderer/native/query_tracker.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/fullscreen_spv.h" "${CMAKE_CURRENT_BINARY_DIR}/query_test_spv.h")
add_executable(gow3-native-draw-barriers-test draw_barriers_test.cpp)
add_executable(gow3-native-draw-descriptor-batch-test draw_descriptor_batch_test.cpp)
add_executable(gow3-native-draw-command-pool-test draw_command_pool_test.cpp)
target_include_directories(gow3-native-draw-command-pool-test PRIVATE "${GOW3_ROOT}/src"
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/vulkan-headers/include")
add_executable(gow3-native-immutable-snapshot-cache-test immutable_snapshot_cache_test.cpp)
target_include_directories(gow3-native-immutable-snapshot-cache-test PRIVATE "${GOW3_ROOT}/src")
target_link_libraries(gow3-native-immutable-snapshot-cache-test PRIVATE Threads::Threads)
target_include_directories(gow3-native-draw-descriptor-batch-test PRIVATE "${GOW3_ROOT}/src"
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/vulkan-headers/include")
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/descriptor_batch_test_spv.h"
    COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.1 --vn descriptor_batch_test_spv
        -o "${CMAKE_CURRENT_BINARY_DIR}/descriptor_batch_test_spv.h" "${CMAKE_CURRENT_LIST_DIR}/descriptor_batch_test.frag"
    DEPENDS descriptor_batch_test.frag VERBATIM)
target_include_directories(gow3-native-draw-barriers-test PRIVATE "${GOW3_ROOT}/src"
    "${GOW3_ROOT}/vendor/plume" "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/vulkan-headers/include")
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/fsi_barrier_test_spv.h"
    COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.2 --vn fsi_barrier_test_spv
        -o "${CMAKE_CURRENT_BINARY_DIR}/fsi_barrier_test_spv.h" "${CMAKE_CURRENT_LIST_DIR}/fsi_barrier_test.frag"
    DEPENDS fsi_barrier_test.frag VERBATIM)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/fsi_extent_test_spv.h"
    COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.2 --vn fsi_extent_test_spv
        -o "${CMAKE_CURRENT_BINARY_DIR}/fsi_extent_test_spv.h" "${CMAKE_CURRENT_LIST_DIR}/fsi_extent_test.frag"
    DEPENDS fsi_extent_test.frag VERBATIM)
target_include_directories(gow3-native-query-test PRIVATE "${GOW3_ROOT}/src" "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(gow3-native-query-test PRIVATE plume ${CMAKE_DL_LIBS})
if(TARGET X11::X11)
    target_link_libraries(gow3-native-query-test PRIVATE X11::X11)
endif()
add_executable(gow3-native-query-comparison-test query_comparison_test.cpp
    "${GOW3_ROOT}/src/renderer/native/query_comparison.cpp")
target_include_directories(gow3-native-query-comparison-test PRIVATE "${GOW3_ROOT}/src"
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/inja/third_party/include")
add_executable(gow3-native-memory-copy-test memory_export_copy_test.cpp
    "${GOW3_ROOT}/src/renderer/native/transfer_batch.cpp"
    "${GOW3_ROOT}/src/renderer/native/gpu_work_queue.cpp"
    "${GOW3_ROOT}/src/renderer/native/live_plan.cpp"
    "${GOW3_ROOT}/src/renderer/native/memory_export_copy.cpp"
    "${GOW3_ROOT}/src/renderer/native/resolve_memory.cpp"
    "${GOW3_ROOT}/src/renderer/native/texture_data.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/memory_store_spv.h" "${CMAKE_CURRENT_BINARY_DIR}/memory_load_spv.h")
target_include_directories(gow3-native-memory-copy-test PRIVATE "${GOW3_ROOT}/src" "${CMAKE_CURRENT_BINARY_DIR}"
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/inja/third_party/include"
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/picosha2")
target_link_libraries(gow3-native-memory-copy-test PRIVATE plume ${CMAKE_DL_LIBS} xxHash::xxhash)
if(TARGET X11::X11)
    target_link_libraries(gow3-native-memory-copy-test PRIVATE X11::X11)
endif()
find_package(Threads REQUIRED)
add_executable(gow3-native-parallel-snapshot-compare-test parallel_snapshot_compare_test.cpp)
add_executable(gow3-native-submission-capacity-test submission_capacity_test.cpp)
target_include_directories(gow3-native-submission-capacity-test PRIVATE "${GOW3_ROOT}/src")
add_executable(gow3-native-texture-layout-cache-test texture_layout_cache_test.cpp
    "${GOW3_ROOT}/src/renderer/native/texture_data.cpp")
target_include_directories(gow3-native-texture-layout-cache-test PRIVATE "${GOW3_ROOT}/src"
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/inja/third_party/include")
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(gow3-native-texture-layout-cache-test PRIVATE -Wno-mismatched-new-delete)
endif()
add_executable(gow3-native-associative-texture-layout-cache-test associative_texture_layout_cache_test.cpp
    "${GOW3_ROOT}/src/renderer/native/texture_data.cpp")
target_include_directories(gow3-native-associative-texture-layout-cache-test PRIVATE "${GOW3_ROOT}/src"
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/inja/third_party/include")
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(gow3-native-associative-texture-layout-cache-test PRIVATE -Wno-mismatched-new-delete)
endif()
target_include_directories(gow3-native-parallel-snapshot-compare-test PRIVATE "${GOW3_ROOT}/src")
target_link_libraries(gow3-native-parallel-snapshot-compare-test PRIVATE Threads::Threads)
add_executable(gow3-native-frame-disposal-test frame_disposal_test.cpp)
target_include_directories(gow3-native-frame-disposal-test PRIVATE "${GOW3_ROOT}/src"
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/inja/third_party/include")
target_link_libraries(gow3-native-frame-disposal-test PRIVATE Threads::Threads)
target_link_libraries(gow3-native-live-plan-test PRIVATE gow3-native-content-hash xxHash::xxhash)
add_executable(gow3-native-fsi-draw-pass-groups-test fsi_draw_pass_groups_test.cpp)
target_include_directories(gow3-native-fsi-draw-pass-groups-test PRIVATE "${GOW3_ROOT}/src")
# Shader compile stage timing on real containers (DXC used directly).
add_executable(gow3-native-shader-compile-bench shader_compile_bench.cpp)
target_include_directories(gow3-native-shader-compile-bench PRIVATE "${CMAKE_CURRENT_BINARY_DIR}"
    "${GOW3_XENOS_ROOT}/XenosRecomp" "${GOW3_XENOS_ROOT}/thirdparty/smol-v/source" "${GOW3_XENOS_ROOT}/thirdparty/xxHash")
target_link_libraries(gow3-native-shader-compile-bench PRIVATE gow3-native-shader-compiler Microsoft::DirectXShaderCompiler fmt::fmt)
target_precompile_headers(gow3-native-shader-compile-bench PRIVATE "${GOW3_ROOT}/src/renderer/native/shader_recompiler_pch.h")
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(gow3-native-shader-compile-bench PRIVATE -Wno-switch -Wno-unused-variable -Wno-null-arithmetic -fms-extensions)
endif()
# Producer-cost benchmark (no verification oracle): see live_plan_bench.cpp.
add_executable(gow3-native-live-plan-bench live_plan_bench.cpp
    "${GOW3_ROOT}/src/renderer/native/live_plan.cpp"
    "${GOW3_ROOT}/src/renderer/native/memory_export_copy.cpp"
    "${GOW3_ROOT}/src/renderer/native/texture_data.cpp")
target_include_directories(gow3-native-live-plan-bench PRIVATE "${GOW3_ROOT}/src"
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/inja/third_party/include"
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/picosha2")
target_link_libraries(gow3-native-live-plan-bench PRIVATE gow3-native-live-draw-json gow3-native-content-hash xxHash::xxhash)
add_executable(gow3-native-shader-byte-segments-test shader_byte_segments_test.cpp)
target_include_directories(gow3-native-shader-byte-segments-test PRIVATE "${GOW3_ROOT}/src"
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/picosha2")
target_link_libraries(gow3-native-shader-byte-segments-test PRIVATE gow3-native-content-hash)
add_executable(gow3-native-owned-payload-test owned_payload_test.cpp)
target_include_directories(gow3-native-owned-payload-test PRIVATE "${GOW3_ROOT}/src")
add_executable(gow3-native-shader-requirement-masks-test shader_requirement_masks_test.cpp)
target_include_directories(gow3-native-shader-requirement-masks-test PRIVATE "${GOW3_ROOT}/src")
add_executable(gow3-native-shader-input-cache-test shader_input_cache_test.cpp)
target_include_directories(gow3-native-shader-input-cache-test PRIVATE "${GOW3_ROOT}/src")
target_link_libraries(gow3-native-shader-input-cache-test PRIVATE xxHash::xxhash)
add_executable(gow3-native-sampled-alias-index-test sampled_alias_index_test.cpp)
target_include_directories(gow3-native-sampled-alias-index-test PRIVATE "${GOW3_ROOT}/src"
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/inja/third_party/include")
target_link_libraries(gow3-native-memory-copy-test PRIVATE gow3-native-content-hash)
add_executable(gow3-native-content-hash-test content_hash_test.cpp)
target_link_libraries(gow3-native-content-hash-test PRIVATE gow3-native-content-hash Threads::Threads)
# Exercise the dependency-free implementation even on accelerated build hosts.
add_executable(gow3-native-content-hash-fallback-test content_hash_test.cpp)
target_include_directories(gow3-native-content-hash-fallback-test PRIVATE "${GOW3_ROOT}/src"
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/picosha2")
target_compile_definitions(gow3-native-content-hash-fallback-test PRIVATE GOW3_NATIVE_USE_OPENSSL=0)
target_link_libraries(gow3-native-content-hash-fallback-test PRIVATE Threads::Threads)
add_executable(gow3-native-memory-coherence-test "${GOW3_ROOT}/tests/native_memory_coherence_test.cpp")
add_executable(gow3-native-gpu-batch-attribution-test gpu_batch_attribution_test.cpp)
target_include_directories(gow3-native-gpu-batch-attribution-test PRIVATE
    "${GOW3_ROOT}/src" "${GOW3_ROOT}/src/renderer/native"
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/inja/third_party/include")
add_executable(gow3-native-index-data-test index_data_test.cpp)
target_include_directories(gow3-native-index-data-test PRIVATE "${GOW3_ROOT}/src")
add_executable(gow3-native-index-geometry-cache-test index_geometry_cache_test.cpp)
target_include_directories(gow3-native-index-geometry-cache-test PRIVATE "${GOW3_ROOT}/src"
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/inja/third_party/include")
add_executable(gow3-native-texture-upload-conversion-test texture_upload_conversion_test.cpp
    "${GOW3_ROOT}/src/renderer/native/texture_data.cpp")
target_include_directories(gow3-native-texture-upload-conversion-test PRIVATE "${GOW3_ROOT}/src")
add_executable(gow3-native-texture-memory-ranges-test texture_memory_ranges_test.cpp
    "${GOW3_ROOT}/src/renderer/native/texture_data.cpp")
target_include_directories(gow3-native-texture-memory-ranges-test PRIVATE "${GOW3_ROOT}/src"
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/inja/third_party/include")
add_executable(gow3-native-dormant-cache-test dormant_cache_test.cpp)
target_include_directories(gow3-native-dormant-cache-test PRIVATE "${GOW3_ROOT}/src")
add_executable(gow3-native-constant-data-test "${GOW3_ROOT}/tests/native_constant_data_test.cpp")
target_include_directories(gow3-native-constant-data-test PRIVATE "${GOW3_ROOT}/src")
add_executable(gow3-native-texture-probe-test texture_probe_test.cpp)
target_include_directories(gow3-native-texture-probe-test PRIVATE "${GOW3_ROOT}/src"
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/inja/third_party/include"
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/picosha2")
add_executable(gow3-native-texture-probe-compare texture_probe_compare.cpp
    "${GOW3_ROOT}/src/renderer/native/texture_data.cpp")
target_include_directories(gow3-native-texture-probe-compare PRIVATE "${GOW3_ROOT}/src"
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/inja/third_party/include"
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/picosha2")
target_include_directories(gow3-native-memory-coherence-test PRIVATE "${GOW3_ROOT}/src")
target_link_libraries(gow3-native-memory-coherence-test PRIVATE Threads::Threads)
add_executable(gow3-native-packet-ownership-test packet_ownership_test.cpp)
target_include_directories(gow3-native-packet-ownership-test PRIVATE "${GOW3_ROOT}/src")
add_executable(gow3-native-completion-test completion_test.cpp)
target_include_directories(gow3-native-completion-test PRIVATE "${GOW3_ROOT}/src"
  "${GOW3_ROOT}/vendor/rexglue-sdk/include")
add_executable(gow3-native-query-tickets-test query_tickets_test.cpp)
target_include_directories(gow3-native-query-tickets-test PRIVATE "${GOW3_ROOT}/vendor/rexglue-sdk/include")
add_executable(gow3-native-deferred-query-completion-test deferred_query_completion_test.cpp)
target_include_directories(gow3-native-deferred-query-completion-test PRIVATE "${GOW3_ROOT}/vendor/rexglue-sdk/include")

add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/state_transfer_spv.h"
    COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.1 --vn state_transfer_spv
        -o "${CMAKE_CURRENT_BINARY_DIR}/state_transfer_spv.h" "${CMAKE_CURRENT_LIST_DIR}/state_transfer.comp"
    DEPENDS state_transfer.comp VERBATIM)
add_executable(gow3-native-state-transfer-test state_transfer_test.cpp
    "${GOW3_ROOT}/src/renderer/native/state_transfer.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/state_transfer_spv.h")
target_sources(gow3-native-scene-replay PRIVATE "${GOW3_ROOT}/src/renderer/native/state_transfer.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/state_transfer_spv.h")
target_include_directories(gow3-native-state-transfer-test PRIVATE "${GOW3_ROOT}/src" "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(gow3-native-state-transfer-test PRIVATE plume ${CMAKE_DL_LIBS})
if(TARGET X11::X11)
    target_link_libraries(gow3-native-state-transfer-test PRIVATE X11::X11)
endif()

target_sources(gow3-native-scene-replay PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/memory_load_r32_spv.h" "${CMAKE_CURRENT_BINARY_DIR}/memory_load_rg32_spv.h")
foreach(mode restore store)
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/retained_opaque_${mode}_spv.h"
        COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.2
            --vn retained_opaque_${mode}_spv "-I${CMAKE_CURRENT_LIST_DIR}"
            -o "${CMAKE_CURRENT_BINARY_DIR}/retained_opaque_${mode}_spv.h"
            "${CMAKE_CURRENT_LIST_DIR}/retained_opaque_${mode}.comp"
        DEPENDS "retained_opaque_${mode}.comp" readonly_depth_attachment.glsl VERBATIM)
endforeach()
target_sources(gow3-native-scene-replay PRIVATE
    "${GOW3_ROOT}/src/renderer/native/retained_opaque_writer.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/retained_opaque_restore_spv.h"
    "${CMAKE_CURRENT_BINARY_DIR}/retained_opaque_store_spv.h")
# V4 phase modules are separate from the unchanged legacy retained pair.
foreach(mode restore store)
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/retained_opaque_phase_${mode}_spv.h"
        COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.2
            --vn retained_opaque_phase_${mode}_spv "-I${CMAKE_CURRENT_LIST_DIR}"
            -o "${CMAKE_CURRENT_BINARY_DIR}/retained_opaque_phase_${mode}_spv.h"
            "${CMAKE_CURRENT_LIST_DIR}/retained_opaque_phase_${mode}.comp"
        DEPENDS "retained_opaque_phase_${mode}.comp" retained_opaque_phase_depth.glsl VERBATIM)
endforeach()
target_sources(gow3-native-scene-replay PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/retained_opaque_phase_restore_spv.h"
    "${CMAKE_CURRENT_BINARY_DIR}/retained_opaque_phase_store_spv.h")
add_executable(gow3-native-retained-opaque-authority-test retained_opaque_authority_test.cpp)
target_include_directories(gow3-native-retained-opaque-authority-test PRIVATE "${GOW3_ROOT}/src")
foreach(mode restore store)
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/retained_fp24_depth_${mode}_spv.h"
        COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.2 --vn retained_fp24_depth_${mode}_spv
            -o "${CMAKE_CURRENT_BINARY_DIR}/retained_fp24_depth_${mode}_spv.h"
            "${CMAKE_CURRENT_LIST_DIR}/retained_fp24_depth_${mode}.comp"
        DEPENDS "retained_fp24_depth_${mode}.comp" VERBATIM)
endforeach()
target_sources(gow3-native-scene-replay PRIVATE "${GOW3_ROOT}/src/renderer/native/retained_fp24_depth_writer.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/retained_fp24_depth_restore_spv.h" "${CMAKE_CURRENT_BINARY_DIR}/retained_fp24_depth_store_spv.h")
add_executable(gow3-native-retained-fp24-depth-authority-test retained_fp24_depth_authority_test.cpp)
target_include_directories(gow3-native-retained-fp24-depth-authority-test PRIVATE "${GOW3_ROOT}/src")
get_target_property(GOW3_STATE_EXPORT_SOURCES gow3-native-scene-replay SOURCES)
list(FILTER GOW3_STATE_EXPORT_SOURCES EXCLUDE REGEX "scene_replay[.]cpp$")
add_executable(gow3-native-packed-zero-depth-clear-test packed_zero_depth_clear_test.cpp)
target_include_directories(gow3-native-packed-zero-depth-clear-test PRIVATE "${GOW3_ROOT}/src")
add_executable(gow3-native-packed-zero-depth-clear-gpu-test
    packed_zero_depth_clear_gpu_test.cpp ${GOW3_STATE_EXPORT_SOURCES})
target_include_directories(gow3-native-packed-zero-depth-clear-gpu-test PRIVATE
    "${GOW3_ROOT}/src" "${CMAKE_CURRENT_BINARY_DIR}"
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/inja/third_party/include"
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/picosha2")
target_link_libraries(gow3-native-packed-zero-depth-clear-gpu-test PRIVATE
    gow3-native-live-device gow3-native-live-draw-json)
add_executable(gow3-native-full-color-clear-gpu-test
    full_color_clear_gpu_test.cpp ${GOW3_STATE_EXPORT_SOURCES})
target_include_directories(gow3-native-full-color-clear-gpu-test PRIVATE
    "${GOW3_ROOT}/src" "${CMAKE_CURRENT_BINARY_DIR}"
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/inja/third_party/include"
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/picosha2")
target_link_libraries(gow3-native-full-color-clear-gpu-test PRIVATE
    gow3-native-live-device gow3-native-live-draw-json)
add_executable(gow3-native-state-export-test state_export_test.cpp ${GOW3_STATE_EXPORT_SOURCES})
get_target_property(GOW3_TRANSFER_TEST_SOURCES gow3-native-pass-test SOURCES)
list(FILTER GOW3_TRANSFER_TEST_SOURCES EXCLUDE REGEX "pass_test[.]cpp$")
add_executable(gow3-native-stencil-export-test stencil_export_test.cpp ${GOW3_TRANSFER_TEST_SOURCES}
    "${GOW3_ROOT}/src/renderer/native/query_tracker.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/restore_depth_stencil_spv.h"
    "${CMAKE_CURRENT_BINARY_DIR}/query_test_spv.h")
target_include_directories(gow3-native-stencil-export-test PRIVATE "${GOW3_ROOT}/src" "${CMAKE_CURRENT_BINARY_DIR}"
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/inja/third_party/include")
target_link_libraries(gow3-native-stencil-export-test PRIVATE gow3-native-live-device plume ${CMAKE_DL_LIBS})
if(TARGET X11::X11)
    target_link_libraries(gow3-native-stencil-export-test PRIVATE X11::X11)
endif()
add_executable(gow3-native-retired-image-pool-test retired_image_pool_test.cpp)
target_include_directories(gow3-native-retired-image-pool-test PRIVATE "${GOW3_ROOT}/src")
target_link_libraries(gow3-native-retired-image-pool-test PRIVATE Threads::Threads)
add_executable(gow3-native-retired-image-pool-gpu-test retired_image_pool_gpu_test.cpp ${GOW3_TRANSFER_TEST_SOURCES})
target_include_directories(gow3-native-retired-image-pool-gpu-test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" "${GOW3_ROOT}/src")
target_link_libraries(gow3-native-retired-image-pool-gpu-test PRIVATE plume ${CMAKE_DL_LIBS})
if(TARGET X11::X11)
    target_link_libraries(gow3-native-retired-image-pool-gpu-test PRIVATE X11::X11)
endif()
add_executable(gow3-native-compact-alias-format-test compact_alias_format_test.cpp)
target_include_directories(gow3-native-compact-alias-format-test PRIVATE "${GOW3_ROOT}/src")
target_link_libraries(gow3-native-compact-alias-format-test PRIVATE Threads::Threads)
# Actual GPU sampling/readback fixture:
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/compact_alias_sample_spv.h"
    COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.1 --vn compact_alias_sample_spv
        -o "${CMAKE_CURRENT_BINARY_DIR}/compact_alias_sample_spv.h"
        "${CMAKE_CURRENT_LIST_DIR}/compact_alias_sample.comp"
    DEPENDS compact_alias_sample.comp VERBATIM)
add_executable(gow3-native-compact-alias-gpu-test compact_alias_gpu_test.cpp ${GOW3_TRANSFER_TEST_SOURCES}
    "${GOW3_ROOT}/src/renderer/native/texture_upload.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/memory_load_r32_spv.h" "${CMAKE_CURRENT_BINARY_DIR}/memory_load_rg32_spv.h"
    "${CMAKE_CURRENT_BINARY_DIR}/compact_alias_sample_spv.h")
target_include_directories(gow3-native-compact-alias-gpu-test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" "${GOW3_ROOT}/src")
target_link_libraries(gow3-native-compact-alias-gpu-test PRIVATE plume ${CMAKE_DL_LIBS})
if(TARGET X11::X11)
    target_link_libraries(gow3-native-compact-alias-gpu-test PRIVATE X11::X11)
endif()
add_executable(gow3-native-transfer-batch-test transfer_batch_test.cpp ${GOW3_TRANSFER_TEST_SOURCES})
target_include_directories(gow3-native-transfer-batch-test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" "${GOW3_ROOT}/src"
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/inja/third_party/include")
target_link_libraries(gow3-native-transfer-batch-test PRIVATE plume ${CMAKE_DL_LIBS})
if(TARGET X11::X11)
    target_link_libraries(gow3-native-transfer-batch-test PRIVATE X11::X11)
endif()
target_include_directories(gow3-native-state-export-test PRIVATE "${GOW3_ROOT}/src" "${CMAKE_CURRENT_BINARY_DIR}"
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/inja/third_party/include"
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/picosha2")
target_link_libraries(gow3-native-state-export-test PRIVATE gow3-native-live-device)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/retired_vertex_test_spv.h"
    COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.2 --vn retired_vertex_test_spv
        -o "${CMAKE_CURRENT_BINARY_DIR}/retired_vertex_test_spv.h" "${CMAKE_CURRENT_LIST_DIR}/retired_vertex_test.vert"
    DEPENDS retired_vertex_test.vert VERBATIM)
foreach(raw_float_probe raw_float_sample raw_float_scene)
    if(raw_float_probe STREQUAL "raw_float_scene")
        set(raw_float_extension frag)
    else()
        set(raw_float_extension comp)
    endif()
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${raw_float_probe}_spv.h"
        COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.1 --vn "${raw_float_probe}_spv"
            -o "${CMAKE_CURRENT_BINARY_DIR}/${raw_float_probe}_spv.h" "${CMAKE_CURRENT_LIST_DIR}/${raw_float_probe}.${raw_float_extension}"
        DEPENDS "${raw_float_probe}.${raw_float_extension}" VERBATIM)
endforeach()
add_executable(gow3-native-scene-deferred-query-test scene_deferred_query_test.cpp
    raw_float_scene_test.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/raw_float_sample_spv.h"
    "${CMAKE_CURRENT_BINARY_DIR}/raw_float_scene_spv.h"
    "${CMAKE_CURRENT_BINARY_DIR}/retired_vertex_test_spv.h"
    "${CMAKE_CURRENT_BINARY_DIR}/descriptor_batch_test_spv.h"
    "${CMAKE_CURRENT_BINARY_DIR}/fsi_barrier_test_spv.h"
    "${CMAKE_CURRENT_BINARY_DIR}/fsi_extent_test_spv.h"
    ${GOW3_STATE_EXPORT_SOURCES} "${CMAKE_CURRENT_BINARY_DIR}/query_test_spv.h"
    "${CMAKE_CURRENT_BINARY_DIR}/alias_query_test_spv.h"
    "${CMAKE_CURRENT_BINARY_DIR}/alias_pair_query_test_spv.h")
target_include_directories(gow3-native-scene-deferred-query-test PRIVATE "${GOW3_ROOT}/src" "${CMAKE_CURRENT_BINARY_DIR}"
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/inja/third_party/include"
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/picosha2")
target_link_libraries(gow3-native-scene-deferred-query-test PRIVATE
    gow3-native-live-device gow3-native-shader-compiler)

add_executable(gow3-native-live-draw-test live_draw_test.cpp)
target_link_libraries(gow3-native-live-draw-test PRIVATE gow3-native-live-draw-json
    gow3-native-content-hash Threads::Threads)
foreach(target gow3-native-scene-replay gow3-native-state-export-test gow3-native-scene-deferred-query-test
    gow3-native-live-plan-test gow3-native-live-prefix-resources-test gow3-native-frame-disposal-test
    gow3-native-memory-copy-test)
    target_link_libraries(${target} PRIVATE gow3-native-live-draw-json)
endforeach()

add_executable(gow3-native-cache-bookkeeping-test cache_bookkeeping_test.cpp)
target_include_directories(gow3-native-cache-bookkeeping-test PRIVATE "${GOW3_ROOT}/src")

add_executable(gow3-native-draw-state-test "${GOW3_ROOT}/tests/native_draw_state_test.cpp"
    "${GOW3_ROOT}/src/renderer/native/draw_state.cpp")
target_include_directories(gow3-native-draw-state-test PRIVATE "${GOW3_ROOT}/src" "${GOW3_ROOT}/vendor/plume")

add_executable(gow3-native-sampler-test sampler_test.cpp
    "${GOW3_ROOT}/src/renderer/native/texture_upload.cpp"
    "${GOW3_ROOT}/src/renderer/native/texture_data.cpp")
target_include_directories(gow3-native-sampler-test PRIVATE "${GOW3_ROOT}/src")
target_link_libraries(gow3-native-sampler-test PRIVATE plume ${CMAKE_DL_LIBS})
if(TARGET X11::X11)
    target_link_libraries(gow3-native-sampler-test PRIVATE X11::X11)
endif()

add_executable(gow3-native-texture-overlay-test texture_overlay_test.cpp
    "${GOW3_ROOT}/src/renderer/native/transfer_batch.cpp"
    "${GOW3_ROOT}/src/renderer/native/gpu_work_queue.cpp"
    "${GOW3_ROOT}/src/renderer/native/resolve_memory.cpp"
    "${GOW3_ROOT}/src/renderer/native/texture_data.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/memory_store_spv.h" "${CMAKE_CURRENT_BINARY_DIR}/memory_load_spv.h")
target_include_directories(gow3-native-texture-overlay-test PRIVATE "${GOW3_ROOT}/src" "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(gow3-native-texture-overlay-test PRIVATE plume gow3-native-live-draw-json ${CMAKE_DL_LIBS})
if(TARGET X11::X11)
    target_link_libraries(gow3-native-texture-overlay-test PRIVATE X11::X11)
endif()

# A cold diagnostic oracle for the real API->live-plan->Scene typed path.
get_target_property(GOW3_LIVE_CAPTURE_REPLAY_SOURCES gow3-native-scene-replay SOURCES)
list(FILTER GOW3_LIVE_CAPTURE_REPLAY_SOURCES EXCLUDE REGEX "scene_replay[.]cpp$")
list(APPEND GOW3_LIVE_CAPTURE_REPLAY_SOURCES
    "${GOW3_ROOT}/src/renderer/native/live_plan.cpp"
    "${GOW3_ROOT}/src/renderer/native/memory_export_copy.cpp")
list(REMOVE_DUPLICATES GOW3_LIVE_CAPTURE_REPLAY_SOURCES)
add_executable(gow3-native-live-capture-replay live_capture_replay.cpp retained_fp24_depth_scene_fixture.cpp retained_opaque_scene_fixture.cpp retained_opaque_phase_owner_fixture.cpp retained_opaque_phase_marker_fixture.cpp retained_opaque_phase_pass_failure_fixture.cpp retained_queued_draw_export_fixture.cpp retained_resource_pool_scene_fixture.cpp ${GOW3_LIVE_CAPTURE_REPLAY_SOURCES}
    "${GOW3_ROOT}/src/renderer/native/fsi_state_recipe_cache.cpp")
target_include_directories(gow3-native-live-capture-replay PRIVATE "${GOW3_ROOT}/src" "${CMAKE_CURRENT_BINARY_DIR}"
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/inja/third_party/include"
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/picosha2")
target_link_libraries(gow3-native-live-capture-replay PRIVATE gow3-native-live-device gow3-native-live-draw-json
    gow3-native-content-hash Threads::Threads xxHash::xxhash)
add_executable(gow3-native-api-capture-reader-test api_capture_reader_test.cpp)
target_include_directories(gow3-native-api-capture-reader-test PRIVATE "${GOW3_ROOT}/src")

add_executable(gow3-native-callback-test native_callback_test.cpp)
target_include_directories(gow3-native-callback-test PRIVATE "${GOW3_ROOT}/vendor/rexglue-sdk/include")

add_executable(gow3-native-fsi-shader-variant-test fsi_shader_variant_test.cpp
    "${GOW3_ROOT}/src/renderer/native/fsi_shader_variant.cpp")

add_executable(gow3-native-builtin-atomic-depth-test builtin_atomic_depth_test.cpp
    "${GOW3_ROOT}/src/renderer/native/builtin_atomic_depth.cpp"
    "${GOW3_ROOT}/src/renderer/native/fsi_shader_variant.cpp")
target_include_directories(gow3-native-builtin-atomic-depth-test PRIVATE "${GOW3_ROOT}/src")
target_link_libraries(gow3-native-builtin-atomic-depth-test PRIVATE gow3-native-content-hash)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/builtin_atomic_depth_test.spv"
    COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.2
        -o "${CMAKE_CURRENT_BINARY_DIR}/builtin_atomic_depth_test.spv"
        "${CMAKE_CURRENT_LIST_DIR}/builtin_atomic_depth_test.vert"
    DEPENDS builtin_atomic_depth_test.vert VERBATIM)
add_executable(gow3-native-builtin-atomic-depth-gpu-test builtin_atomic_depth_gpu_test.cpp
    ${GOW3_STATE_EXPORT_SOURCES} "${CMAKE_CURRENT_BINARY_DIR}/builtin_atomic_depth_test.spv")
target_include_directories(gow3-native-builtin-atomic-depth-gpu-test PRIVATE
    "${GOW3_ROOT}/src" "${CMAKE_CURRENT_BINARY_DIR}"
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/inja/third_party/include"
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/picosha2")
target_link_libraries(gow3-native-builtin-atomic-depth-gpu-test PRIVATE
    gow3-native-live-device gow3-native-live-draw-json gow3-native-shader-compiler
    gow3-native-content-hash Threads::Threads xxHash::xxhash)
target_include_directories(gow3-native-fsi-shader-variant-test PRIVATE "${GOW3_ROOT}/src")

add_executable(gow3-native-fsi-variant-scene-test fsi_variant_scene_test.cpp ${GOW3_STATE_EXPORT_SOURCES})
target_include_directories(gow3-native-fsi-variant-scene-test PRIVATE "${GOW3_ROOT}/src" "${CMAKE_CURRENT_BINARY_DIR}"
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/inja/third_party/include"
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/picosha2")
target_link_libraries(gow3-native-fsi-variant-scene-test PRIVATE gow3-native-live-device gow3-native-live-draw-json
    gow3-native-content-hash Threads::Threads xxHash::xxhash)


add_executable(gow3-native-cpu-footprint-lru-test cpu_footprint_lru_test.cpp
    "${GOW3_ROOT}/src/renderer/native/texture_data.cpp")
target_include_directories(gow3-native-cpu-footprint-lru-test PRIVATE "${GOW3_ROOT}/src"
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/inja/third_party/include")

# Paired exact packed-color experiment; compiling fixtures does not run Vulkan.
add_executable(gow3-native-opaque-color-overwrite-source-test opaque_color_overwrite_source_test.cpp)
target_include_directories(gow3-native-opaque-color-overwrite-source-test PRIVATE "${GOW3_ROOT}/src")
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/opaque_color_overwrite_baseline.hlsl"
                          "${CMAKE_CURRENT_BINARY_DIR}/opaque_color_overwrite_candidate.hlsl"
    COMMAND $<TARGET_FILE:gow3-native-opaque-color-overwrite-source-test>
        "${GOW3_ROOT}/src/renderer/native/fsi_precision.hlsl"
        "${CMAKE_CURRENT_LIST_DIR}/opaque_color_overwrite_fixture.hlsl"
        "${CMAKE_CURRENT_BINARY_DIR}/opaque_color_overwrite_baseline.hlsl"
        "${CMAKE_CURRENT_BINARY_DIR}/opaque_color_overwrite_candidate.hlsl"
    DEPENDS gow3-native-opaque-color-overwrite-source-test opaque_color_overwrite_fixture.hlsl
        "${GOW3_ROOT}/src/renderer/native/fsi_precision.hlsl"
        "${GOW3_ROOT}/src/renderer/native/opaque_color_overwrite.h" VERBATIM)
foreach(variant baseline candidate)
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/opaque_color_overwrite_${variant}.spv"
        COMMAND "${CMAKE_COMMAND}" -E env ${DIRECTX_DXC_TOOL}
            -T ps_6_0 -E main -HV 2021 -spirv -fvk-use-dx-layout -D GOW3_NATIVE_FSI=1
            -fspv-target-env=vulkan1.2 -fspv-extension=SPV_EXT_fragment_shader_interlock
            -fspv-extension=SPV_EXT_descriptor_indexing -fspv-extension=SPV_KHR_physical_storage_buffer
            -Fo "${CMAKE_CURRENT_BINARY_DIR}/opaque_color_overwrite_${variant}.spv"
            "${CMAKE_CURRENT_BINARY_DIR}/opaque_color_overwrite_${variant}.hlsl"
        DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/opaque_color_overwrite_${variant}.hlsl" VERBATIM)
endforeach()
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/opaque_color_overwrite_vertex.spv"
    COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.2
        -o "${CMAKE_CURRENT_BINARY_DIR}/opaque_color_overwrite_vertex.spv"
        "${CMAKE_CURRENT_LIST_DIR}/fsi_precision.vert"
    DEPENDS fsi_precision.vert VERBATIM)
add_executable(gow3-native-opaque-color-overwrite-gpu-test opaque_color_overwrite_gpu_test.cpp
    "${GOW3_ROOT}/src/renderer/native/query_tracker.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/opaque_color_overwrite_vertex.spv"
    "${CMAKE_CURRENT_BINARY_DIR}/opaque_color_overwrite_baseline.spv"
    "${CMAKE_CURRENT_BINARY_DIR}/opaque_color_overwrite_candidate.spv")
target_link_libraries(gow3-native-opaque-color-overwrite-gpu-test PRIVATE gow3-native-live-device)

add_executable(gow3-native-opaque-color-overwrite-guard-test opaque_color_overwrite_guard_test.cpp)
target_include_directories(gow3-native-opaque-color-overwrite-guard-test PRIVATE "${GOW3_ROOT}/src")
add_executable(gow3-native-opaque-color-overwrite-compiler-test opaque_color_overwrite_compiler_test.cpp)
target_link_libraries(gow3-native-opaque-color-overwrite-compiler-test PRIVATE gow3-native-shader-compiler)
# Explicit replay experiment compiles before any Vulkan Scene mutation.
target_link_libraries(gow3-native-live-capture-replay PRIVATE gow3-native-shader-compiler)

add_executable(gow3-native-forward-control-flow-test forward_control_flow_test.cpp)
target_include_directories(gow3-native-forward-control-flow-test PRIVATE "${GOW3_ROOT}/src")
add_executable(gow3-native-forward-control-flow-compiler-test forward_control_flow_compiler_test.cpp)
target_link_libraries(gow3-native-forward-control-flow-compiler-test PRIVATE gow3-native-shader-compiler)

add_executable(gow3-native-forward-vertex-control-flow-test forward_vertex_control_flow_test.cpp)
target_include_directories(gow3-native-forward-vertex-control-flow-test PRIVATE "${GOW3_ROOT}/src")
add_executable(gow3-native-forward-vertex-compiler-test forward_vertex_compiler_test.cpp)
target_link_libraries(gow3-native-forward-vertex-compiler-test PRIVATE gow3-native-shader-compiler)

# Separate compiler/source fixtures; no game content is included.
add_executable(gow3-native-forward-pixel-catalog-test forward_pixel_catalog_test.cpp)
target_compile_features(gow3-native-forward-pixel-catalog-test PRIVATE cxx_std_23)
target_include_directories(gow3-native-forward-pixel-catalog-test PRIVATE "${GOW3_ROOT}/src")
add_executable(gow3-native-forward-pixel-catalog-compiler-test forward_pixel_catalog_compiler_test.cpp)
target_compile_features(gow3-native-forward-pixel-catalog-compiler-test PRIVATE cxx_std_23)
target_link_libraries(gow3-native-forward-pixel-catalog-compiler-test PRIVATE gow3-native-shader-compiler)
target_include_directories(gow3-native-forward-pixel-catalog-compiler-test PRIVATE "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/inja/third_party/include")

# Compile-only owned-device diagnostics: no game runtime feature enable or submission.
add_executable(gow3-native-pipeline-executables pipeline_executables.cpp
    "${GOW3_ROOT}/src/renderer/native/pipeline_prewarm.cpp")
target_include_directories(gow3-native-pipeline-executables PRIVATE "${GOW3_ROOT}/src"
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/inja/third_party/include")
target_link_libraries(gow3-native-pipeline-executables PRIVATE
    gow3-native-live-device gow3-native-content-hash Threads::Threads)

# CPU-only catalog residency accounting; no Vulkan device or shader compile.
add_executable(gow3-native-catalog-shader-cache-headroom-test catalog_shader_cache_headroom_test.cpp)
target_include_directories(gow3-native-catalog-shader-cache-headroom-test PRIVATE "${GOW3_ROOT}/src")

add_executable(gow3-native-draw-command-groups-test draw_command_groups_test.cpp)
target_include_directories(gow3-native-draw-command-groups-test PRIVATE "${GOW3_ROOT}/src")

target_sources(gow3-native-scene-deferred-query-test PRIVATE multi_draw_scene_test.cpp)

# CPU-only exact dummy-pass description and fresh render-pass owner transaction.
add_executable(gow3-native-dummy-fsi-render-pass-test dummy_fsi_render_pass_test.cpp)
target_include_directories(gow3-native-dummy-fsi-render-pass-test PRIVATE "${GOW3_ROOT}/src"
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/vulkan-headers/include")

# Pinned read-only query candidate fixture does not change canonical compiler inputs.
add_executable(gow3-native-builtin-readonly-query-test builtin_readonly_query_test.cpp
    "${GOW3_ROOT}/src/renderer/native/builtin_readonly_query.cpp"
    "${GOW3_ROOT}/src/renderer/native/fsi_shader_variant.cpp")
target_include_directories(gow3-native-builtin-readonly-query-test PRIVATE "${GOW3_ROOT}/src")
target_link_libraries(gow3-native-builtin-readonly-query-test PRIVATE gow3-native-content-hash)

add_executable(gow3-native-builtin-readonly-query-gpu-test builtin_readonly_query_gpu_test.cpp
    ${GOW3_STATE_EXPORT_SOURCES} "${CMAKE_CURRENT_BINARY_DIR}/fsi_vertex_spv.h")
target_include_directories(gow3-native-builtin-readonly-query-gpu-test PRIVATE
    "${GOW3_ROOT}/src" "${CMAKE_CURRENT_BINARY_DIR}"
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/inja/third_party/include"
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/picosha2")
target_link_libraries(gow3-native-builtin-readonly-query-gpu-test PRIVATE
    gow3-native-live-device gow3-native-live-draw-json gow3-native-content-hash Threads::Threads)

# CPU-only exact tuple/cache lifecycle fixture and all-source DXC fixture.
# Neither target creates a Vulkan device or executes a shader.
add_executable(gow3-native-fsi-material-shader-cache-headroom-test fsi_material_shader_cache_headroom_test.cpp)
target_include_directories(gow3-native-fsi-material-shader-cache-headroom-test PRIVATE "${GOW3_ROOT}/src")

add_executable(gow3-native-fsi-state-material-composition-test fsi_state_material_composition_test.cpp)
target_include_directories(gow3-native-fsi-state-material-composition-test PRIVATE "${GOW3_ROOT}/src")
add_executable(gow3-native-fsi-state-specialization-test fsi_state_specialization_test.cpp
    "${GOW3_ROOT}/src/renderer/native/pipeline_prewarm.cpp"
    "${GOW3_ROOT}/src/renderer/native/fsi_state_recipe_cache.cpp")
target_include_directories(gow3-native-fsi-state-specialization-test PRIVATE "${GOW3_ROOT}/src"
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/inja/third_party/include")
target_link_libraries(gow3-native-fsi-state-specialization-test PRIVATE plume gow3-native-content-hash Threads::Threads)
add_executable(gow3-native-fsi-state-specialization-compiler-test fsi_state_specialization_compiler_test.cpp)
target_include_directories(gow3-native-fsi-state-specialization-compiler-test PRIVATE
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/inja/third_party/include")
target_link_libraries(gow3-native-fsi-state-specialization-compiler-test PRIVATE gow3-native-shader-compiler gow3-native-content-hash)

# Real Scene startup refusal/residency and full-memory/query GPU fixture.
add_executable(gow3-native-fsi-state-specialization-budget-gpu-test
    fsi_state_specialization_budget_gpu_test.cpp ${GOW3_STATE_EXPORT_SOURCES})
target_include_directories(gow3-native-fsi-state-specialization-budget-gpu-test PRIVATE
    "${GOW3_ROOT}/src" "${CMAKE_CURRENT_BINARY_DIR}"
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/inja/third_party/include"
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/picosha2")
target_link_libraries(gow3-native-fsi-state-specialization-budget-gpu-test PRIVATE
    gow3-native-live-device gow3-native-live-draw-json gow3-native-shader-compiler
    gow3-native-content-hash Threads::Threads)

add_executable(gow3-native-descriptor-content-cache-test descriptor_content_cache_test.cpp)
target_include_directories(gow3-native-descriptor-content-cache-test PRIVATE "${GOW3_ROOT}/src")
target_sources(gow3-native-scene-deferred-query-test PRIVATE descriptor_content_scene_test.cpp)

# Pinned full-state + guest forward-CFG source/compiler fixtures (no GPU in either).
add_executable(gow3-native-fsi-state-forward-cfg-test fsi_state_forward_cfg_test.cpp
    "${GOW3_ROOT}/src/renderer/native/pipeline_prewarm.cpp")
target_include_directories(gow3-native-fsi-state-forward-cfg-test PRIVATE "${GOW3_ROOT}/src"
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/inja/third_party/include")
target_link_libraries(gow3-native-fsi-state-forward-cfg-test PRIVATE plume gow3-native-content-hash Threads::Threads)
add_executable(gow3-native-fsi-state-forward-cfg-compiler-test fsi_state_forward_cfg_compiler_test.cpp)
target_include_directories(gow3-native-fsi-state-forward-cfg-compiler-test PRIVATE
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/inja/third_party/include")
target_link_libraries(gow3-native-fsi-state-forward-cfg-compiler-test PRIVATE gow3-native-shader-compiler gow3-native-content-hash)

# Isolated readonly D32 query prototype correctness fixtures.
include("${CMAKE_CURRENT_LIST_DIR}/readonly_depth_attachment_fixture.cmake")

add_executable(gow3-native-readonly-depth-version-test readonly_depth_version_test.cpp)
target_include_directories(gow3-native-readonly-depth-version-test PRIVATE "${GOW3_ROOT}/src")

add_executable(gow3-native-readonly-depth-scene-test readonly_depth_attachment_scene_test.cpp
    ${GOW3_STATE_EXPORT_SOURCES})
target_include_directories(gow3-native-readonly-depth-scene-test PRIVATE
    "${GOW3_ROOT}/src" "${CMAKE_CURRENT_BINARY_DIR}"
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/inja/third_party/include"
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/picosha2")
target_link_libraries(gow3-native-readonly-depth-scene-test PRIVATE
    gow3-native-live-device gow3-native-live-draw-json gow3-native-shader-compiler
    gow3-native-content-hash Threads::Threads xxHash::xxhash)

add_executable(gow3-native-retained-opaque-catalog-policy-test retained_opaque_catalog_policy_test.cpp)
target_include_directories(gow3-native-retained-opaque-catalog-policy-test PRIVATE "${GOW3_ROOT}/src")

# Actual three-source compiler/cache fixture; no Vulkan device is created.
add_executable(gow3-native-retained-opaque-catalog-compiler-test retained_opaque_catalog_compiler_test.cpp)
target_include_directories(gow3-native-retained-opaque-catalog-compiler-test PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}" "${GOW3_ROOT}/vendor/xenos-recomp/XenosRecomp"
    "${GOW3_ROOT}/vendor/xenos-recomp/thirdparty/smol-v/source"
    "${GOW3_ROOT}/vendor/xenos-recomp/thirdparty/xxHash"
    "${GOW3_ROOT}/vendor/xenos-recomp/thirdparty/zstd/lib")
target_link_libraries(gow3-native-retained-opaque-catalog-compiler-test PRIVATE
    gow3-native-shader-compiler gow3-native-content-hash fmt::fmt Microsoft::DirectXShaderCompiler)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(gow3-native-retained-opaque-catalog-compiler-test PRIVATE -fms-extensions)
endif()

# Append-only fixture plumbing. No production shader/compiler identity inputs.
add_executable(gow3-native-retained-opaque-catalog-state-fixture-test
  retained_opaque_state_fixture_test.cpp)
target_compile_features(gow3-native-retained-opaque-catalog-state-fixture-test PRIVATE cxx_std_23)
target_include_directories(gow3-native-retained-opaque-catalog-state-fixture-test PRIVATE
  "${GOW3_ROOT}/src" "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/inja/third_party/include")
target_link_libraries(gow3-native-retained-opaque-catalog-state-fixture-test PRIVATE
  gow3-native-live-draw-json gow3-native-content-hash)

# Default-OFF retained depth sharing source and real Scene fixtures.
include("${CMAKE_CURRENT_LIST_DIR}/retained_shared_depth_fixture.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/fsi_exact_stores_fixture.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/fsi_immutable_state_hoist_fixture.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/constant_delta_tests.cmake")

# Dedicated disjoint-query retention fixtures use the approved actual live replay.
include("${CMAKE_CURRENT_LIST_DIR}/retained_disjoint_query_policy_fixture.cmake")
target_sources(gow3-native-live-capture-replay PRIVATE retained_disjoint_query_scene_fixture.cpp)

# Required-view grouped texture snapshot CPU correctness fixture.
include("${CMAKE_CURRENT_LIST_DIR}/batch_texture_snapshots_fixture.cmake")

# Full original captured CPU producer differential; no GPU device.
include("${CMAKE_CURRENT_LIST_DIR}/batch_texture_capture_fixture.cmake")


add_executable(gow3-native-packed-color-pass-groups-test packed_color_pass_groups_test.cpp
    "${GOW3_ROOT}/src/renderer/native/fsi_shader_variant.cpp")
target_include_directories(gow3-native-packed-color-pass-groups-test PRIVATE "${GOW3_ROOT}/src")
target_link_libraries(gow3-native-packed-color-pass-groups-test PRIVATE gow3-native-content-hash)

add_executable(gow3-native-packed-color-pass-scene-test packed_color_pass_scene_test.cpp ${GOW3_STATE_EXPORT_SOURCES})
target_include_directories(gow3-native-packed-color-pass-scene-test PRIVATE
    "${GOW3_ROOT}/src" "${CMAKE_CURRENT_BINARY_DIR}"
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/inja/third_party/include"
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/picosha2")
target_link_libraries(gow3-native-packed-color-pass-scene-test PRIVATE
    gow3-native-live-device gow3-native-live-draw-json gow3-native-shader-compiler
    gow3-native-content-hash Threads::Threads xxHash::xxhash)

# Separate default-OFF exact blend cohort. Building does not execute tests/Vulkan.
add_executable(gow3-native-packed-color-blend-variant8-test packed_color_blend_variant8_test.cpp)
target_include_directories(gow3-native-packed-color-blend-variant8-test PRIVATE "${GOW3_ROOT}/src")
target_link_libraries(gow3-native-packed-color-blend-variant8-test PRIVATE gow3-native-content-hash)

add_executable(gow3-native-packed-color-blend-pass-scene-test
    packed_color_blend_pass_scene_test.cpp ${GOW3_STATE_EXPORT_SOURCES})
target_include_directories(gow3-native-packed-color-blend-pass-scene-test PRIVATE
    "${GOW3_ROOT}/src" "${CMAKE_CURRENT_BINARY_DIR}"
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/inja/third_party/include"
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/picosha2")
target_link_libraries(gow3-native-packed-color-blend-pass-scene-test PRIVATE
    gow3-native-live-device gow3-native-live-draw-json gow3-native-shader-compiler
    gow3-native-content-hash Threads::Threads xxHash::xxhash)

add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/packed_color_pass_fixture.spv"
    COMMAND "${CMAKE_COMMAND}" -E env ${DIRECTX_DXC_TOOL}
        -T ps_6_0 -E main -HV 2021 -spirv -fvk-use-dx-layout -D GOW3_NATIVE_FSI=1
        -fspv-target-env=vulkan1.2 -fspv-extension=SPV_EXT_fragment_shader_interlock
        -fspv-extension=SPV_EXT_descriptor_indexing -fspv-extension=SPV_KHR_physical_storage_buffer
        -I "${GOW3_ROOT}/src/renderer/native"
        -Fo "${CMAKE_CURRENT_BINARY_DIR}/packed_color_pass_fixture.spv"
        "${CMAKE_CURRENT_LIST_DIR}/packed_color_pass_fixture.hlsl"
    DEPENDS packed_color_pass_fixture.hlsl "${GOW3_ROOT}/src/renderer/native/fsi_precision.hlsl" VERBATIM)
target_sources(gow3-native-packed-color-pass-scene-test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/packed_color_pass_fixture.spv")

# Conservative material early-depth prototype, separately keyed from canonical SPIR-V.
include("${CMAKE_CURRENT_LIST_DIR}/native_early_depth_shader_fixture.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/early_depth_lower_bound_fixture.cmake")
get_directory_property(GOW3_EARLY_DEPTH_SCENE_TARGETS BUILDSYSTEM_TARGETS)
foreach(target IN LISTS GOW3_EARLY_DEPTH_SCENE_TARGETS)
    get_target_property(target_sources ${target} SOURCES)
    if("${target_sources}" MATCHES "scene_executor[.]cpp")
        target_sources(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/early_depth_lower_bound_comp_spv.h")
        target_link_libraries(${target} PRIVATE gow3-native-early-depth-shader)
    endif()
endforeach()

include("${CMAKE_CURRENT_LIST_DIR}/early_depth_lower_bound_gpu_fixture.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../../../src/renderer/native/native_early_depth_catalog.cmake")
target_link_libraries(gow3-native-shader-compiler PUBLIC gow3-native-early-depth-catalog)
include("${CMAKE_CURRENT_LIST_DIR}/native_early_depth_catalog_fixture.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/early_depth_catalog_scene_fixture.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/retired_descriptor_pool_fixture.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/retired_descriptor_pool_gpu_fixture.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/null_depth_constant_footprint_fixture.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/pinned_null_constant_fixture.cmake")

include(${CMAKE_CURRENT_LIST_DIR}/atlas_depth_region_fixture.cmake)

include("${CMAKE_CURRENT_LIST_DIR}/atlas_depth_clear_shader.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/atlas_depth_clear_fixture.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/fused_atlas_depth_clear_guard_fixture.cmake")
target_sources(gow3-native-live-capture-replay PRIVATE fused_atlas_depth_clear_scene_fixture.cpp)

include("${CMAKE_CURRENT_LIST_DIR}/atlas_rgba_stencil_depth_store_shader.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/fused_packed_atlas_resolve_shader.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/canonical_store_shader.cmake")
target_sources(gow3-native-live-capture-replay PRIVATE canonical_store_scene_fixture.cpp)
add_executable(gow3-native-canonical-store-gpu-test canonical_store_gpu_test.cpp
    ${GOW3_TRANSFER_TEST_SOURCES} "${GOW3_CANONICAL_STORE_HEADER}")
target_include_directories(gow3-native-canonical-store-gpu-test PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}" "${GOW3_ROOT}/src"
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/inja/third_party/include")
target_link_libraries(gow3-native-canonical-store-gpu-test PRIVATE plume ${CMAKE_DL_LIBS} xxHash::xxhash)
if(TARGET X11::X11)
    target_link_libraries(gow3-native-canonical-store-gpu-test PRIVATE X11::X11)
endif()
include("${CMAKE_CURRENT_LIST_DIR}/atlas_rgba_stencil_depth_store_fixture.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/direct_atlas_depth_resolve_guard_fixture.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/packed_primary_stencil_clear_shader.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/packed_primary_stencil_clear_fixture.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/packed_primary_stencil_clear_guard_fixture.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/direct_packed_color_resolve_guard_fixture.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/packed_secondary_stencil_clear_shader.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/packed_secondary_stencil_clear_fixture.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/packed_secondary_stencil_clear_guard_fixture.cmake")

add_executable(gow3-native-submission-worker-test submission_worker_test.cpp)
target_include_directories(gow3-native-submission-worker-test PRIVATE "${GOW3_ROOT}/src")
target_link_libraries(gow3-native-submission-worker-test PRIVATE Threads::Threads)

include("${CMAKE_CURRENT_LIST_DIR}/fragment_invocation_census.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/stencil_material_cutoff_replay.cmake")
include("${GOW3_ROOT}/reports/rendering/native-renderer/performance-60fps/room-fix-20261006-02/fragment-invocations-20261009/stencil-material-cutoff-stage/isa/CMakeLists.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/stencil_material_scene_fixture.cmake")

add_executable(gow3-native-early-depth-material-retention-test early_depth_material_retention_test.cpp)
target_include_directories(gow3-native-early-depth-material-retention-test PRIVATE "${GOW3_ROOT}/src")
target_compile_features(gow3-native-early-depth-material-retention-test PRIVATE cxx_std_23)
target_sources(gow3-native-live-capture-replay PRIVATE early_depth_material_retention_scene_fixture.cpp)
add_executable(gow3-native-monotonic-depth-module-test native_monotonic_depth_module_test.cpp
    "${GOW3_ROOT}/src/renderer/native/native_monotonic_depth_module.cpp")
target_include_directories(gow3-native-monotonic-depth-module-test PRIVATE
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/inja/third_party/include")
target_link_libraries(gow3-native-monotonic-depth-module-test PRIVATE gow3-native-shader-compiler gow3-native-content-hash)

# Applied last so it reaches every target above: executor tests need the
# generated restore shader and xxHash, and shared renderer headers include
# nlohmann/json.
get_directory_property(GOW3_ALL_TEST_TARGETS BUILDSYSTEM_TARGETS)
foreach(target IN LISTS GOW3_ALL_TEST_TARGETS)
    get_target_property(target_type ${target} TYPE)
    if(NOT target_type MATCHES "EXECUTABLE|STATIC_LIBRARY|OBJECT_LIBRARY")
        continue()
    endif()
    target_include_directories(${target} PRIVATE
        "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/inja/third_party/include")
    get_target_property(target_sources ${target} SOURCES)
    if("${target_sources}" MATCHES "scene_executor[.]cpp")
        target_sources(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/memory_restore_frag_spv.h")
        target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
        target_link_libraries(${target} PRIVATE xxHash::xxhash)
    endif()
endforeach()
