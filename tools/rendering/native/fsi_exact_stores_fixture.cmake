# Optional exact FSI store/read fixture; shader generation does not submit GPU work.
add_executable(gow3-native-fsi-exact-stores-source-test fsi_exact_stores_source_test.cpp)
target_include_directories(gow3-native-fsi-exact-stores-source-test PRIVATE "${GOW3_ROOT}/src")
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/fsi_exact_stores_baseline.hlsl"
                          "${CMAKE_CURRENT_BINARY_DIR}/fsi_exact_stores_candidate.hlsl"
    COMMAND $<TARGET_FILE:gow3-native-fsi-exact-stores-source-test>
        "${GOW3_ROOT}/src/renderer/native/fsi_precision.hlsl"
        "${CMAKE_CURRENT_LIST_DIR}/opaque_color_overwrite_fixture.hlsl"
        "${CMAKE_CURRENT_BINARY_DIR}/fsi_exact_stores_baseline.hlsl"
        "${CMAKE_CURRENT_BINARY_DIR}/fsi_exact_stores_candidate.hlsl"
    DEPENDS gow3-native-fsi-exact-stores-source-test opaque_color_overwrite_fixture.hlsl
        "${GOW3_ROOT}/src/renderer/native/fsi_precision.hlsl"
        "${GOW3_ROOT}/src/renderer/native/fsi_exact_stores.h" VERBATIM)
foreach(variant baseline candidate)
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/fsi_exact_stores_${variant}.spv"
        COMMAND "${CMAKE_COMMAND}" -E env ${DIRECTX_DXC_TOOL}
            -T ps_6_0 -E main -HV 2021 -spirv -fvk-use-dx-layout -D GOW3_NATIVE_FSI=1
            -fspv-target-env=vulkan1.2 -fspv-extension=SPV_EXT_fragment_shader_interlock
            -fspv-extension=SPV_EXT_descriptor_indexing -fspv-extension=SPV_KHR_physical_storage_buffer
            -Fo "${CMAKE_CURRENT_BINARY_DIR}/fsi_exact_stores_${variant}.spv"
            "${CMAKE_CURRENT_BINARY_DIR}/fsi_exact_stores_${variant}.hlsl"
        DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/fsi_exact_stores_${variant}.hlsl" VERBATIM)
endforeach()
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/fsi_exact_stores_vertex.spv"
    COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.2
        -o "${CMAKE_CURRENT_BINARY_DIR}/fsi_exact_stores_vertex.spv"
        "${CMAKE_CURRENT_LIST_DIR}/fsi_precision.vert"
    DEPENDS fsi_precision.vert VERBATIM)
add_executable(gow3-native-fsi-exact-stores-gpu-test fsi_exact_stores_gpu_test.cpp
    "${GOW3_ROOT}/src/renderer/native/query_tracker.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/fsi_exact_stores_vertex.spv"
    "${CMAKE_CURRENT_BINARY_DIR}/fsi_exact_stores_baseline.spv"
    "${CMAKE_CURRENT_BINARY_DIR}/fsi_exact_stores_candidate.spv")
target_link_libraries(gow3-native-fsi-exact-stores-gpu-test PRIVATE gow3-native-live-device)

add_executable(gow3-native-fsi-exact-stores-policy-test fsi_exact_stores_policy_test.cpp)
target_include_directories(gow3-native-fsi-exact-stores-policy-test PRIVATE "${GOW3_ROOT}/src")
add_executable(gow3-native-fsi-exact-stores-compiler-test fsi_exact_stores_compiler_test.cpp)
target_link_libraries(gow3-native-fsi-exact-stores-compiler-test PRIVATE gow3-native-shader-compiler)
