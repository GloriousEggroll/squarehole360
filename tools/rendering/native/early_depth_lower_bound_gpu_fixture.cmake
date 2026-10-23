# Include after early_depth_lower_bound_fixture.cmake and native_early_depth_shader_fixture.cmake.
# Reuse the root-owned converter custom command; never create a second producer.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/early_depth_lower_bound_fixture_baseline.hlsl"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/early_depth_lower_bound_fixture_source.py"
        "${GOW3_ROOT}/src/renderer/native/fsi_precision.hlsl"
        "${CMAKE_CURRENT_LIST_DIR}/early_depth_lower_bound_fixture.hlsl"
        "${CMAKE_CURRENT_BINARY_DIR}/early_depth_lower_bound_fixture_baseline.hlsl"
    DEPENDS early_depth_lower_bound_fixture_source.py early_depth_lower_bound_fixture.hlsl
        "${GOW3_ROOT}/src/renderer/native/fsi_precision.hlsl" VERBATIM)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/early_depth_lower_bound_fixture_baseline.spv"
    COMMAND "${CMAKE_COMMAND}" -E env ${DIRECTX_DXC_TOOL}
        -T ps_6_0 -E main -HV 2021 -spirv -fvk-use-dx-layout -D GOW3_NATIVE_FSI=1
        -fspv-target-env=vulkan1.2 -fspv-extension=SPV_EXT_fragment_shader_interlock
        -fspv-extension=SPV_EXT_descriptor_indexing -fspv-extension=SPV_KHR_physical_storage_buffer
        -Fo "${CMAKE_CURRENT_BINARY_DIR}/early_depth_lower_bound_fixture_baseline.spv"
        "${CMAKE_CURRENT_BINARY_DIR}/early_depth_lower_bound_fixture_baseline.hlsl"
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/early_depth_lower_bound_fixture_baseline.hlsl" VERBATIM)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/early_depth_lower_bound_fixture_vertex.spv"
    COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.2
        -o "${CMAKE_CURRENT_BINARY_DIR}/early_depth_lower_bound_fixture_vertex.spv"
        "${CMAKE_CURRENT_LIST_DIR}/early_depth_lower_bound_fixture.vert"
    DEPENDS early_depth_lower_bound_fixture.vert VERBATIM)
add_executable(gow3-native-early-depth-fixture-editor early_depth_lower_bound_fixture_editor.cpp)
target_link_libraries(gow3-native-early-depth-fixture-editor PRIVATE gow3-native-content-hash)
add_executable(gow3-native-early-depth-lower-bound-gpu-test early_depth_lower_bound_gpu_test.cpp
    "${GOW3_ROOT}/src/renderer/native/query_tracker.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/early_depth_lower_bound_comp_spv.h"
    "${CMAKE_CURRENT_BINARY_DIR}/early_depth_lower_bound_fixture_baseline.spv"
    "${CMAKE_CURRENT_BINARY_DIR}/early_depth_lower_bound_fixture_vertex.spv")
target_include_directories(gow3-native-early-depth-lower-bound-gpu-test PRIVATE
    "${GOW3_ROOT}/src" "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(gow3-native-early-depth-lower-bound-gpu-test PRIVATE
    gow3-native-live-device gow3-native-content-hash Threads::Threads)
# The candidate is intentionally not an automatic build output. Root must first
# archive and validate the actual baseline with its source/producer identity,
# then pass that independently authenticated digest to the fixture-only editor.
