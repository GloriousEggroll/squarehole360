# Included only by the standalone tools/rendering/native validation project.
# Isolated correctness fixtures; no game/Scene/runtime integration or flags.
add_executable(gow3-native-readonly-depth-attachment-test readonly_depth_attachment_test.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/readonly_depth_attachment_frag_spv.h")
target_include_directories(gow3-native-readonly-depth-attachment-test PRIVATE "${GOW3_ROOT}/src" "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(gow3-native-readonly-depth-attachment-test PRIVATE gow3-native-content-hash)
foreach(stage comp frag)
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/readonly_depth_attachment_${stage}_spv.h"
        COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.2
            --vn readonly_depth_attachment_${stage}_spv
            "-I${CMAKE_CURRENT_LIST_DIR}"
            -o "${CMAKE_CURRENT_BINARY_DIR}/readonly_depth_attachment_${stage}_spv.h"
            "${CMAKE_CURRENT_LIST_DIR}/readonly_depth_attachment.${stage}"
        DEPENDS "readonly_depth_attachment.${stage}" readonly_depth_attachment.glsl
        VERBATIM)
endforeach()
add_executable(gow3-native-readonly-depth-attachment-gpu-test readonly_depth_attachment_gpu_test.cpp
    "${GOW3_ROOT}/src/renderer/native/query_tracker.cpp"
    "${GOW3_ROOT}/src/renderer/native/builtin_readonly_query.cpp"
    "${GOW3_ROOT}/src/renderer/native/fsi_shader_variant.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/early_depth_lower_bound_comp_spv.h"
    "${CMAKE_CURRENT_BINARY_DIR}/readonly_depth_attachment_comp_spv.h"
    "${CMAKE_CURRENT_BINARY_DIR}/readonly_depth_attachment_frag_spv.h")
target_include_directories(gow3-native-readonly-depth-attachment-gpu-test PRIVATE
    "${GOW3_ROOT}/src" "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(gow3-native-readonly-depth-attachment-gpu-test PRIVATE
    gow3-native-live-device gow3-native-content-hash Threads::Threads)
