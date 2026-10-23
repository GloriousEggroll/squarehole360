# Standalone CPU fixtures only. Inclusion neither runs nor exposes gameplay controls.
if(NOT TARGET gow3-native-stencil-material-cutoff)
  message(FATAL_ERROR "Include stencil-material producer target before CPU fixtures")
endif()
add_executable(gow3-native-stencil-material-policy-test stencil_material_cutoff_policy_test.cpp)
target_link_libraries(gow3-native-stencil-material-policy-test PRIVATE gow3-native-stencil-material-cutoff)
add_executable(gow3-native-stencil-material-compiler-test stencil_material_cutoff_compiler_test.cpp)
target_link_libraries(gow3-native-stencil-material-compiler-test PRIVATE gow3-native-stencil-material-cutoff)
