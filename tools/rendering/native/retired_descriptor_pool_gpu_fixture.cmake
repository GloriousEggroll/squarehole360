# Include after the existing GOW3_TRANSFER_TEST_SOURCES definition. Root owns
# standalone inclusion and execution; no shipping target is modified here.
add_executable(gow3-native-retired-descriptor-pool-gpu-test
  "${CMAKE_CURRENT_LIST_DIR}/retired_descriptor_pool_gpu_test.cpp"
  ${GOW3_TRANSFER_TEST_SOURCES}
  "${CMAKE_CURRENT_BINARY_DIR}/memory_load_r32_spv.h"
  "${CMAKE_CURRENT_BINARY_DIR}/memory_load_rg32_spv.h")
target_compile_features(gow3-native-retired-descriptor-pool-gpu-test PRIVATE cxx_std_23)
target_include_directories(gow3-native-retired-descriptor-pool-gpu-test PRIVATE
  "${GOW3_ROOT}/src" "${CMAKE_CURRENT_BINARY_DIR}"
  "${GOW3_ROOT}/vendor/rexglue-sdk/thirdparty/inja/third_party/include")
target_link_libraries(gow3-native-retired-descriptor-pool-gpu-test PRIVATE
  plume Threads::Threads ${CMAKE_DL_LIBS})
if(TARGET X11::X11)
  target_link_libraries(gow3-native-retired-descriptor-pool-gpu-test PRIVATE X11::X11)
endif()
