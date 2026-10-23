# Source-only optional fixture targets. Include from the root CMakeLists.txt
# after rex::runtime is available, with these three fixture files copied to
# tools/rendering/native. This snippet does not change any production target.
add_executable(gow3-certified-snapshot-batch-tracker-test
    tools/rendering/native/certified_snapshot_batch_tracker_test.cpp)
target_include_directories(gow3-certified-snapshot-batch-tracker-test PRIVATE
    "${REXSDK_DIR}/include")
target_compile_features(gow3-certified-snapshot-batch-tracker-test PRIVATE cxx_std_23)
add_executable(gow3-certified-snapshot-batch-memory-test
    tools/rendering/native/certified_snapshot_batch_memory_test.cpp)
target_link_libraries(gow3-certified-snapshot-batch-memory-test PRIVATE rex::runtime)
target_compile_features(gow3-certified-snapshot-batch-memory-test PRIVATE cxx_std_23)
