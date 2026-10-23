# Include after canonical compiler setup. Separate target; no compiler/V1 edits.
include("${GOW3_ROOT}/src/renderer/native/native_early_depth_catalog.cmake")
add_executable(gow3-native-early-depth-catalog-test native_early_depth_catalog_test.cpp)
target_link_libraries(gow3-native-early-depth-catalog-test PRIVATE gow3-native-early-depth-catalog)
