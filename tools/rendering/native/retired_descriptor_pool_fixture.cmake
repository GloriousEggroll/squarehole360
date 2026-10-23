# Optional source-only policy fixture; root owns inclusion and execution.
find_package(Threads REQUIRED)
add_executable(gow3-native-retired-descriptor-pool-test
  "${CMAKE_CURRENT_LIST_DIR}/retired_descriptor_pool_test.cpp")
target_include_directories(gow3-native-retired-descriptor-pool-test PRIVATE "${GOW3_ROOT}/src")
target_compile_features(gow3-native-retired-descriptor-pool-test PRIVATE cxx_std_23)
target_link_libraries(gow3-native-retired-descriptor-pool-test PRIVATE Threads::Threads)
