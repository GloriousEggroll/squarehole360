if(TARGET gow3-native-early-depth-shader)
    return()
endif()
include("${CMAKE_CURRENT_LIST_DIR}/content_hash.cmake")
# Independent optional binary editor: never append to the canonical compiler's
# explicit source-hash inputs or alter its embedded HLSL.
file(SHA256 "${CMAKE_CURRENT_LIST_FILE}" GOW3_EARLY_DEPTH_PRODUCER_HASH)
set(GOW3_EARLY_DEPTH_VERSION_INPUT
    "native-fsi-canonical-49f2-early-fragment-tests-v1;${GOW3_SHADER_VERSION};${GOW3_EARLY_DEPTH_PRODUCER_HASH};")
foreach(source native_early_depth_shader.h native_early_depth_shader.cpp)
    set(path "${CMAKE_CURRENT_LIST_DIR}/${source}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
    file(SHA256 "${path}" source_hash)
    string(APPEND GOW3_EARLY_DEPTH_VERSION_INPUT "${source_hash};")
endforeach()
string(SHA256 GOW3_EARLY_DEPTH_SHADER_VERSION "${GOW3_EARLY_DEPTH_VERSION_INPUT}")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/native_early_depth_shader_version.h" CONTENT
"#pragma once
inline constexpr char kNativeEarlyDepthShaderVersion[] = \"@GOW3_EARLY_DEPTH_SHADER_VERSION@\";
" @ONLY)
add_library(gow3-native-early-depth-shader STATIC
    "${CMAKE_CURRENT_LIST_DIR}/native_early_depth_shader.cpp")
target_compile_features(gow3-native-early-depth-shader PUBLIC cxx_std_23)
target_include_directories(gow3-native-early-depth-shader PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(gow3-native-early-depth-shader PUBLIC gow3-native-content-hash)
if(TARGET gow3-native-shader-compiler)
    target_link_libraries(gow3-native-shader-compiler PUBLIC gow3-native-early-depth-shader)
endif()
