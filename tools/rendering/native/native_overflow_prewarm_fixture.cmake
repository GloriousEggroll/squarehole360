# Source-only shared admission fixture; root owns inclusion/build/execution.
add_executable(gow3-native-overflow-prewarm-test
  "${CMAKE_CURRENT_LIST_DIR}/native_overflow_prewarm_test.cpp")
target_include_directories(gow3-native-overflow-prewarm-test PRIVATE "${GOW3_ROOT}/src")
target_compile_features(gow3-native-overflow-prewarm-test PRIVATE cxx_std_20)
