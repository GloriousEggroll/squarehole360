# Source-only controlled Scene fixture; root owns the exclusive replay CLI.
target_sources(gow3-native-live-capture-replay PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/early_depth_catalog_scene_fixture.cpp")
