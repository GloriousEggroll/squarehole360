if(TARGET gow3-native-stencil-material-cutoff)
    return()
endif()
if(NOT TARGET gow3-native-shader-compiler OR NOT DEFINED GOW3_SHADER_VERSION)
    message(FATAL_ERROR "Include canonical shader compiler before stencil-material diagnostic")
endif()
# Producer invariants: one viewport, no sample shading, no multiview; FsiDevice
# actually enables fragmentStoresAndAtomics. A changed backend refuses startup
# compilation until the diagnostic counter-bound/feature proof is reviewed.
file(SHA256 "${GOW3_SHADER_ROOT}/vendor/plume/plume_vulkan.cpp" backend_hash)
if(NOT backend_hash STREQUAL "16543cd8d166d7f5fd26f5a67bf5d542b0a0280953356ccd19cf9d64d6709938")
    message(FATAL_ERROR "Stencil-material pinned backend changed: vendor/plume/plume_vulkan.cpp")
endif()
file(SHA256 "${GOW3_SHADER_ROOT}/src/renderer/native/fsi_device.cpp" backend_hash)
if(NOT backend_hash STREQUAL "867ad666c9852f9899cbba088b453b26586ddf5a091ec0bc780355302afec5f8")
    message(FATAL_ERROR "Stencil-material pinned backend changed: src/renderer/native/fsi_device.cpp")
endif()
file(SHA256 "${CMAKE_CURRENT_LIST_FILE}" producer_hash)
set(version_input "${GOW3_SHADER_VERSION};native-stencil-material-v2;${producer_hash};")
foreach(source stencil_material_cutoff.h stencil_material_cutoff.cpp stencil_material_cutoff_compiler.h stencil_material_cutoff_compiler.cpp)
    set(path "${CMAKE_CURRENT_LIST_DIR}/${source}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
    file(SHA256 "${path}" source_hash)
    string(APPEND version_input "${source_hash};")
endforeach()
string(SHA256 GOW3_STENCIL_MATERIAL_VERSION "${version_input}")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/native_stencil_material_version.h" CONTENT
"#pragma once
inline constexpr char kNativeStencilMaterialVersion[] = \"@GOW3_STENCIL_MATERIAL_VERSION@\";
" @ONLY)
add_library(gow3-native-stencil-material-cutoff STATIC
    "${CMAKE_CURRENT_LIST_DIR}/stencil_material_cutoff.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/stencil_material_cutoff_compiler.cpp")
target_compile_features(gow3-native-stencil-material-cutoff PUBLIC cxx_std_23)
target_compile_definitions(gow3-native-stencil-material-cutoff PRIVATE GOW3_RECOMP)
target_include_directories(gow3-native-stencil-material-cutoff PUBLIC "${GOW3_SHADER_ROOT}/src")
target_include_directories(gow3-native-stencil-material-cutoff PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}" "${GOW3_XENOS_ROOT}/XenosRecomp"
    "${GOW3_XENOS_ROOT}/thirdparty/smol-v/source" "${GOW3_XENOS_ROOT}/thirdparty/xxHash"
    "${GOW3_XENOS_ROOT}/thirdparty/zstd/lib" "${GOW3_SHADER_ROOT}/vendor/rexglue-sdk/thirdparty/picosha2")
target_link_libraries(gow3-native-stencil-material-cutoff PUBLIC gow3-native-shader-compiler
    PRIVATE Microsoft::DirectXShaderCompiler fmt::fmt)
target_precompile_headers(gow3-native-stencil-material-cutoff PRIVATE "${CMAKE_CURRENT_LIST_DIR}/shader_recompiler_pch.h")
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(gow3-native-stencil-material-cutoff PRIVATE -Wno-switch -Wno-unused-variable -Wno-null-arithmetic -fms-extensions)
endif()
if(WIN32)
    target_compile_definitions(gow3-native-stencil-material-cutoff PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()
