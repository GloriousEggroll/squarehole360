# Fixture-only plumbing; no canonical compiler/hash input changes.
add_executable(gow3-native-retained-shared-depth-authority-test
  "${CMAKE_CURRENT_LIST_DIR}/retained_shared_depth_authority_test.cpp")
target_include_directories(gow3-native-retained-shared-depth-authority-test PRIVATE
  "${GOW3_ROOT}/src/renderer/native")
target_compile_features(gow3-native-retained-shared-depth-authority-test PRIVATE cxx_std_23)
target_sources(gow3-native-live-capture-replay PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/retained_shared_depth_scene_fixture.cpp")
