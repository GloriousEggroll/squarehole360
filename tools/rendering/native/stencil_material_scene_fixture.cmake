# Deliberately compile Scene without either replay census macro/source.
add_executable(gow3-native-stencil-material-scene-test
  "${CMAKE_CURRENT_LIST_DIR}/stencil_material_scene_test.cpp" ${GOW3_STATE_EXPORT_SOURCES}
  "${GOW3_ROOT}/src/renderer/native/fused_packed_atlas_resolve.cpp")
target_include_directories(gow3-native-stencil-material-scene-test PRIVATE
  "${GOW3_ROOT}/src" "${CMAKE_CURRENT_BINARY_DIR}"
  "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/inja/third_party/include"
  "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/picosha2")
target_link_libraries(gow3-native-stencil-material-scene-test PRIVATE
  gow3-native-live-device gow3-native-live-draw-json gow3-native-stencil-material-cutoff
  gow3-native-content-hash Threads::Threads xxHash::xxhash)
