add_executable(gow3-native-early-depth-lower-bound-test early_depth_lower_bound_test.cpp)
target_include_directories(gow3-native-early-depth-lower-bound-test PRIVATE "${GOW3_ROOT}/src")
add_executable(gow3-native-early-depth-version-test early_depth_lower_bound_version_test.cpp)
target_include_directories(gow3-native-early-depth-version-test PRIVATE "${GOW3_ROOT}/src")
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/early_depth_lower_bound_comp_spv.h"
    COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.2 --vn early_depth_lower_bound_comp_spv
        -o "${CMAKE_CURRENT_BINARY_DIR}/early_depth_lower_bound_comp_spv.h"
        "${CMAKE_CURRENT_LIST_DIR}/early_depth_lower_bound.comp"
    DEPENDS early_depth_lower_bound.comp VERBATIM)
