# Include after the separately authenticated null-depth footprint certificate.
add_executable(gow3-native-pinned-null-constant-data-test
  "${GOW3_ROOT}/tools/rendering/native/pinned_null_constant_data_test.cpp")
target_compile_features(gow3-native-pinned-null-constant-data-test PRIVATE cxx_std_23)
target_include_directories(gow3-native-pinned-null-constant-data-test PRIVATE "${GOW3_ROOT}/src")
target_link_libraries(gow3-native-pinned-null-constant-data-test PRIVATE gow3-native-content-hash)
