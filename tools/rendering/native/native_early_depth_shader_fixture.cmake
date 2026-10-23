# Include after shader_compiler.cmake has created the canonical compiler target.
include("${GOW3_ROOT}/src/renderer/native/native_early_depth_shader.cmake")
add_executable(gow3-native-early-depth-shader-test native_early_depth_shader_test.cpp)
target_link_libraries(gow3-native-early-depth-shader-test PRIVATE gow3-native-early-depth-shader)
