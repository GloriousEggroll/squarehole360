add_executable(gow3-native-null-depth-constant-footprint-test
  "${GOW3_ROOT}/tools/rendering/native/null_depth_constant_footprint_test.cpp")
target_compile_features(gow3-native-null-depth-constant-footprint-test PRIVATE cxx_std_20)
target_include_directories(gow3-native-null-depth-constant-footprint-test PRIVATE "${GOW3_ROOT}/src")
target_link_libraries(gow3-native-null-depth-constant-footprint-test PRIVATE gow3-native-content-hash)
