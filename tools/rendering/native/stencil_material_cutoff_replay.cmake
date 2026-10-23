# Replay executable only. Main gameplay compiles no draw hooks or diagnostics.
include("${GOW3_ROOT}/src/renderer/native/stencil_material_cutoff.cmake")
target_compile_definitions(gow3-native-live-capture-replay PRIVATE GOW3_NATIVE_STENCIL_MATERIAL_REPLAY=1)
target_sources(gow3-native-live-capture-replay PRIVATE "${GOW3_ROOT}/src/renderer/native/stencil_material_census.cpp")
# Scene core references uninstrumented certificate helpers in every consumer.
target_link_libraries(gow3-native-live-device PUBLIC gow3-native-stencil-material-cutoff)
include("${CMAKE_CURRENT_LIST_DIR}/stencil_material_cutoff_cpu_fixture.cmake")
