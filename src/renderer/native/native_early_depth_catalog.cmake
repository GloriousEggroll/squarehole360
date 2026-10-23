# Optional, separately linked catalogue helper. Including this file does not
# enable its runtime feature or modify the canonical/V1 compiler producers.
if(TARGET gow3-native-early-depth-catalog)
    return()
endif()
include("${CMAKE_CURRENT_LIST_DIR}/content_hash.cmake")
if(NOT DEFINED GOW3_SHADER_VERSION)
    message(FATAL_ERROR "Include canonical shader_compiler.cmake before catalogue helper")
endif()
file(SHA256 "${CMAKE_CURRENT_LIST_FILE}" GOW3_EARLY_CATALOG_CMAKE_HASH)
set(GOW3_EARLY_CATALOG_VERSION_INPUT
    "native-fsi-canonical-two-material-early-fragment-tests-catalog-v2;${GOW3_SHADER_VERSION};${GOW3_EARLY_CATALOG_CMAKE_HASH};")
foreach(source native_early_depth_catalog.h native_early_depth_catalog.cpp native_early_depth_catalog_certificate.h native_early_depth_shader.h)
    set(path "${CMAKE_CURRENT_LIST_DIR}/${source}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
    file(SHA256 "${path}" source_hash)
    string(APPEND GOW3_EARLY_CATALOG_VERSION_INPUT "${source_hash};")
endforeach()
string(SHA256 GOW3_EARLY_DEPTH_CATALOG_VERSION "${GOW3_EARLY_CATALOG_VERSION_INPUT}")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/native_early_depth_catalog_version.h" CONTENT
"#pragma once
inline constexpr char kNativeEarlyDepthCatalogVersion[] = \"@GOW3_EARLY_DEPTH_CATALOG_VERSION@\";
" @ONLY)
add_library(gow3-native-early-depth-catalog STATIC "${CMAKE_CURRENT_LIST_DIR}/native_early_depth_catalog.cpp")
target_compile_features(gow3-native-early-depth-catalog PUBLIC cxx_std_23)
target_include_directories(gow3-native-early-depth-catalog PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(gow3-native-early-depth-catalog PUBLIC gow3-native-content-hash)
