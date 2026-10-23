add_executable(gow3-native-packed-secondary-stencil-clear-guard-test
    "${CMAKE_CURRENT_LIST_DIR}/packed_secondary_stencil_clear_guard_test.cpp")
target_compile_features(gow3-native-packed-secondary-stencil-clear-guard-test PRIVATE cxx_std_20)
target_include_directories(gow3-native-packed-secondary-stencil-clear-guard-test PRIVATE "${GOW3_ROOT}/src")
