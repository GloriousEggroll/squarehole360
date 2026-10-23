# Include from the standalone tools/rendering/native/CMakeLists.txt.
add_executable(gow3-native-direct-packed-color-resolve-guard-test direct_packed_color_resolve_guard_test.cpp)
target_include_directories(gow3-native-direct-packed-color-resolve-guard-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../../src")
target_compile_features(gow3-native-direct-packed-color-resolve-guard-test PRIVATE cxx_std_20)
