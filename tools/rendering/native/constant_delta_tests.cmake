add_executable(gow3-native-constant-delta-data-test "${GOW3_ROOT}/tests/native_constant_delta_data_test.cpp")
target_compile_features(gow3-native-constant-delta-data-test PRIVATE cxx_std_23)
target_include_directories(gow3-native-constant-delta-data-test PRIVATE "${GOW3_ROOT}/src")
