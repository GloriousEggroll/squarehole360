# Source-only new CPU target. Append after existing native content/hash/JSON targets.
add_executable(gow3-native-batch-texture-capture-test
    "${GOW3_ROOT}/tools/rendering/native/batch_texture_capture_test.cpp"
    "${GOW3_ROOT}/src/renderer/native/live_plan.cpp"
    "${GOW3_ROOT}/src/renderer/native/memory_export_copy.cpp"
    "${GOW3_ROOT}/src/renderer/native/texture_data.cpp")
target_include_directories(gow3-native-batch-texture-capture-test PRIVATE
    "${GOW3_ROOT}/src" "${GOW3_ROOT}/vendor/rexglue-sdk/include"
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/inja/third_party/include"
    "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/picosha2")
target_link_libraries(gow3-native-batch-texture-capture-test PRIVATE
    gow3-native-content-hash xxHash::xxhash gow3-native-live-draw-json Threads::Threads)
