# Standalone consumer of XenosRecomp's MIT licensed translator. Plume is unmodified.
if(TARGET gow3-native-shader-compiler)
    return()
endif()
get_filename_component(GOW3_SHADER_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(GOW3_XENOS_ROOT "${GOW3_SHADER_ROOT}/vendor/xenos-recomp")
if(NOT TARGET Microsoft::DirectXShaderCompiler)
    add_subdirectory("${GOW3_XENOS_ROOT}/thirdparty/dxc-bin" "${CMAKE_CURRENT_BINARY_DIR}/native-dxc")
endif()
if(NOT TARGET fmt::fmt)
    add_subdirectory("${GOW3_XENOS_ROOT}/thirdparty/fmt" "${CMAKE_CURRENT_BINARY_DIR}/native-fmt")
endif()
set(GOW3_SHADER_VERSION_INPUT "gow3-native-spirv-v1;vs_6_0;ps_6_0;main;HV2021;spirv;dx-layout;invert-vertex-y;")
foreach(source shader_recompiler.cpp shader_recompiler.h shader_code.h shader_common.h shader.h constant_table.h pch.h)
    set(path "${GOW3_XENOS_ROOT}/XenosRecomp/${source}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
    file(SHA256 "${path}" source_hash)
    string(APPEND GOW3_SHADER_VERSION_INPUT "${source_hash};")
endforeach()
# Revisions that changed only where cache files live, not compiled output,
# hash as the revision they replaced. That keeps the version, so users' caches
# and every compiler pinned to it stay valid; any other edit changes it.
set(GOW3_SHADER_LAYOUT_ONLY_REVISIONS
    # Cache entries moved into two-digit subfolders (shader_compiler.cpp, .h).
    "2681b8b1952d37e68728501a43084d6a238ac89c0ca72c4d99060cfac547efc7=bcbfe52562dd637d88ee1d070f08fc5b9ef0817c68eb64e72a10214476ef1d5e"
    "8ef33a1921fb18065ac87148827e71e132ee988a7883a7694b410d86d9e64ad9=baf614dbd862cf7064dbac527eeb06e8a4fa0c7a989bc4132c0b8437abc2728c"
    # Source hashing through ContentHash, the same SHA-256 (shader_compiler.cpp).
    "4ab679de207a3698f1d85f5ebe9218b77325d8e43883aca55aad396255c1e295=bcbfe52562dd637d88ee1d070f08fc5b9ef0817c68eb64e72a10214476ef1d5e")
foreach(source shader_compiler.cpp shader_compiler.h shader_abi.h shader_recompiler_pch.h fsi_precision.hlsl fsi_state.h)
    set(path "${CMAKE_CURRENT_LIST_DIR}/${source}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
    file(SHA256 "${path}" source_hash)
    foreach(revision IN LISTS GOW3_SHADER_LAYOUT_ONLY_REVISIONS)
        if(revision MATCHES "^${source_hash}=(.+)$")
            set(source_hash "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    string(APPEND GOW3_SHADER_VERSION_INPUT "${source_hash};")
endforeach()
get_target_property(GOW3_DXC_BINARY Microsoft::DirectXShaderCompiler IMPORTED_LOCATION)
file(SHA256 "${GOW3_DXC_BINARY}" GOW3_DXC_HASH)
string(SHA256 GOW3_SHADER_VERSION "${GOW3_SHADER_VERSION_INPUT}${GOW3_DXC_HASH}")
file(READ "${GOW3_XENOS_ROOT}/XenosRecomp/shader_common.h" GOW3_SHADER_COMMON)
file(READ "${CMAKE_CURRENT_LIST_DIR}/fsi_precision.hlsl" GOW3_SHADER_FSI)
string(APPEND GOW3_SHADER_COMMON "\n#ifdef GOW3_NATIVE_FSI\n${GOW3_SHADER_FSI}\n#endif\n")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/native_shader_embedded.h" CONTENT
"#pragma once
inline constexpr char kNativeShaderVersion[] = \"@GOW3_SHADER_VERSION@\";
inline constexpr char kNativeShaderCommon[] = R\"G3SHADER(@GOW3_SHADER_COMMON@)G3SHADER\";
" @ONLY)
add_library(gow3-native-shader-compiler STATIC
    "${CMAKE_CURRENT_LIST_DIR}/shader_compiler.cpp"
    "${GOW3_XENOS_ROOT}/XenosRecomp/shader_recompiler.cpp")
# Experimental output has a separate identity. Keep the explicit canonical
# hash-input list and embedded generic HLSL above unchanged.
set(GOW3_OPAQUE_SHADER_VERSION_INPUT "${GOW3_SHADER_VERSION};native-fsi-opaque-format3-full-word-overwrite-v1;")
foreach(source opaque_color_overwrite.h opaque_color_overwrite_compiler.h opaque_color_overwrite_compiler.cpp)
    set(path "${CMAKE_CURRENT_LIST_DIR}/${source}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
    file(SHA256 "${path}" source_hash)
    string(APPEND GOW3_OPAQUE_SHADER_VERSION_INPUT "${source_hash};")
endforeach()
string(SHA256 GOW3_OPAQUE_SHADER_VERSION "${GOW3_OPAQUE_SHADER_VERSION_INPUT}")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/native_opaque_shader_version.h" CONTENT
"#pragma once
inline constexpr char kNativeOpaqueShaderVersion[] = \"@GOW3_OPAQUE_SHADER_VERSION@\";
" @ONLY)
target_sources(gow3-native-shader-compiler PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/opaque_color_overwrite_compiler.cpp")
# Forward-CFG output has its own identity; canonical inputs above stay unchanged.
set(GOW3_FORWARD_SHADER_VERSION_INPUT "${GOW3_SHADER_VERSION};native-fsi-forward-cfg-49f2-v1;")
foreach(source forward_control_flow.h forward_control_flow_compiler.h forward_control_flow_compiler.cpp)
    set(path "${CMAKE_CURRENT_LIST_DIR}/${source}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
    file(SHA256 "${path}" source_hash)
    string(APPEND GOW3_FORWARD_SHADER_VERSION_INPUT "${source_hash};")
endforeach()
string(SHA256 GOW3_FORWARD_SHADER_VERSION "${GOW3_FORWARD_SHADER_VERSION_INPUT}")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/native_forward_shader_version.h" CONTENT
"#pragma once
inline constexpr char kNativeForwardShaderVersion[] = \"@GOW3_FORWARD_SHADER_VERSION@\";
" @ONLY)
target_sources(gow3-native-shader-compiler PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/forward_control_flow_compiler.cpp")
# Pinned straight-line VS candidate has a distinct experimental version.
set(GOW3_FORWARD_VERTEX_VERSION_INPUT "${GOW3_SHADER_VERSION};native-vs-straight-cfg-f7db-v1;")
foreach(source forward_vertex_control_flow.h forward_vertex_compiler.h forward_vertex_compiler.cpp)
    set(path "${CMAKE_CURRENT_LIST_DIR}/${source}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
    file(SHA256 "${path}" source_hash)
    string(APPEND GOW3_FORWARD_VERTEX_VERSION_INPUT "${source_hash};")
endforeach()
string(SHA256 GOW3_FORWARD_VERTEX_VERSION "${GOW3_FORWARD_VERTEX_VERSION_INPUT}")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/native_forward_vertex_shader_version.h" CONTENT
"#pragma once
inline constexpr char kNativeForwardVertexShaderVersion[] = \"@GOW3_FORWARD_VERTEX_VERSION@\";
" @ONLY)
target_sources(gow3-native-shader-compiler PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/forward_vertex_compiler.cpp")
# Bounded color-only forward catalog: identity does not touch generic inputs.
set(GOW3_FORWARD_PIXEL_CATALOG_VERSION_INPUT "${GOW3_SHADER_VERSION};native-fsi-forward-color-catalog-v1;")
foreach(source forward_control_flow.h forward_pixel_catalog.h forward_pixel_catalog_compiler.h forward_pixel_catalog_compiler.cpp)
    set(path "${CMAKE_CURRENT_LIST_DIR}/${source}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
    file(SHA256 "${path}" source_hash)
    string(APPEND GOW3_FORWARD_PIXEL_CATALOG_VERSION_INPUT "${source_hash};")
endforeach()
string(SHA256 GOW3_FORWARD_PIXEL_CATALOG_VERSION "${GOW3_FORWARD_PIXEL_CATALOG_VERSION_INPUT}")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/native_forward_pixel_catalog_shader_version.h" CONTENT
"#pragma once
inline constexpr char kNativeForwardPixelCatalogShaderVersion[] = \"@GOW3_FORWARD_PIXEL_CATALOG_VERSION@\";
" @ONLY)
target_sources(gow3-native-shader-compiler PRIVATE "${CMAKE_CURRENT_LIST_DIR}/forward_pixel_catalog_compiler.cpp")
# Add after the canonical hash list/embedded generic HLSL in shader_compiler.cmake.
# Generic cache version and its source inputs remain unchanged.
set(GOW3_FSI_STATE_SPEC_VERSION_INPUT "${GOW3_SHADER_VERSION};native-fsi-pso-state-specialization-v1;")
foreach(source fsi_state_specialization.h fsi_state_specialization_descriptor.h fsi_state_specialization_compiler.h fsi_state_specialization_compiler.cpp fsi_state_specialization_recipe.h)
    set(path "${CMAKE_CURRENT_LIST_DIR}/${source}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
    file(SHA256 "${path}" source_hash)
    string(APPEND GOW3_FSI_STATE_SPEC_VERSION_INPUT "${source_hash};")
endforeach()
string(SHA256 GOW3_FSI_STATE_SPEC_VERSION "${GOW3_FSI_STATE_SPEC_VERSION_INPUT}")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/native_fsi_state_specialization_shader_version.h" CONTENT
"#pragma once
inline constexpr char kNativeFsiStateSpecializationShaderVersion[] = \"@GOW3_FSI_STATE_SPEC_VERSION@\";
" @ONLY)
target_sources(gow3-native-shader-compiler PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/fsi_state_specialization_compiler.cpp")

# Composed candidate has a separate namespace after all canonical hash inputs.
set(GOW3_FSI_STATE_FORWARD_VERSION_INPUT "${GOW3_SHADER_VERSION};${GOW3_FSI_STATE_SPEC_VERSION};native-fsi-state-forward-cfg-49f2-v1;")
foreach(source fsi_state_forward_cfg.h fsi_state_forward_cfg_compiler.h fsi_state_forward_cfg_compiler.cpp forward_control_flow.h forward_pixel_catalog.h)
    set(path "${CMAKE_CURRENT_LIST_DIR}/${source}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
    file(SHA256 "${path}" source_hash)
    string(APPEND GOW3_FSI_STATE_FORWARD_VERSION_INPUT "${source_hash};")
endforeach()
string(SHA256 GOW3_FSI_STATE_FORWARD_VERSION "${GOW3_FSI_STATE_FORWARD_VERSION_INPUT}")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/native_fsi_state_forward_cfg_shader_version.h" CONTENT
"#pragma once
inline constexpr char kNativeFsiStateForwardCfgShaderVersion[] = \"@GOW3_FSI_STATE_FORWARD_VERSION@\";
" @ONLY)
target_sources(gow3-native-shader-compiler PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/fsi_state_forward_cfg_compiler.cpp")

# Packed integer color + exact D32 output has a separate startup namespace.
set(GOW3_RETAINED_OPAQUE_VERSION_INPUT "${GOW3_SHADER_VERSION};native-packed-r32uint-d32-opaque-49f2-v1;")
foreach(source native_packed_opaque_shader.h retained_opaque_compiler.h retained_opaque_compiler.cpp)
    set(path "${CMAKE_CURRENT_LIST_DIR}/${source}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
    file(SHA256 "${path}" source_hash)
    string(APPEND GOW3_RETAINED_OPAQUE_VERSION_INPUT "${source_hash};")
endforeach()
string(SHA256 GOW3_RETAINED_OPAQUE_VERSION "${GOW3_RETAINED_OPAQUE_VERSION_INPUT}")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/native_retained_opaque_shader_version.h" CONTENT
"#pragma once
inline constexpr char kNativeRetainedOpaqueShaderVersion[] = \"@GOW3_RETAINED_OPAQUE_VERSION@\";
" @ONLY)
target_sources(gow3-native-shader-compiler PRIVATE "${CMAKE_CURRENT_LIST_DIR}/retained_opaque_compiler.cpp")

# Independent bounded three-material packed-color/D32 catalogue namespace.
# The canonical and V1 hash inputs remain unchanged.
set(GOW3_RETAINED_OPAQUE_CATALOG_VERSION_INPUT "${GOW3_SHADER_VERSION};native-packed-r32uint-d32-opaque-catalog-v2;")
foreach(source IN ITEMS retained_opaque_catalog.h native_packed_opaque_shader.h retained_opaque_catalog_compiler.h retained_opaque_catalog_compiler.cpp)
    set(path "${CMAKE_CURRENT_LIST_DIR}/${source}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
    file(SHA256 "${path}" source_hash)
    string(APPEND GOW3_RETAINED_OPAQUE_CATALOG_VERSION_INPUT "${source_hash};")
endforeach()
string(SHA256 GOW3_RETAINED_OPAQUE_CATALOG_VERSION "${GOW3_RETAINED_OPAQUE_CATALOG_VERSION_INPUT}")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/native_retained_opaque_catalog_shader_version.h" CONTENT
"#pragma once
inline constexpr char kNativeRetainedOpaqueCatalogShaderVersion[] = \"@GOW3_RETAINED_OPAQUE_CATALOG_VERSION@\";
" @ONLY)
target_sources(gow3-native-shader-compiler PRIVATE "${CMAKE_CURRENT_LIST_DIR}/retained_opaque_catalog_compiler.cpp")

# Exact identity-store output is optional and never changes canonical inputs.
# Bind the optional embedding producer and actual embedded common text. This
# affects only the derived namespace; canonical hash inputs remain unchanged.
file(SHA256 "${CMAKE_CURRENT_LIST_FILE}" GOW3_FSI_EXACT_STORES_PRODUCER_HASH)
string(SHA256 GOW3_FSI_EXACT_STORES_COMMON_HASH "${GOW3_SHADER_COMMON}")
set(GOW3_FSI_EXACT_STORES_VERSION_INPUT "${GOW3_SHADER_VERSION};native-fsi-exact-identity-stores-v1;${GOW3_FSI_EXACT_STORES_PRODUCER_HASH};${GOW3_FSI_EXACT_STORES_COMMON_HASH};")
foreach(source fsi_exact_stores_policy.h fsi_exact_stores.h fsi_exact_stores_compiler.h fsi_exact_stores_compiler.cpp)
    set(path "${CMAKE_CURRENT_LIST_DIR}/${source}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
    file(SHA256 "${path}" source_hash)
    string(APPEND GOW3_FSI_EXACT_STORES_VERSION_INPUT "${source_hash};")
endforeach()
string(SHA256 GOW3_FSI_EXACT_STORES_VERSION "${GOW3_FSI_EXACT_STORES_VERSION_INPUT}")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/native_fsi_exact_stores_version.h" CONTENT
"#pragma once
inline constexpr char kNativeFsiExactStoresShaderVersion[] = \"@GOW3_FSI_EXACT_STORES_VERSION@\";
" @ONLY)
target_sources(gow3-native-shader-compiler PRIVATE "${CMAKE_CURRENT_LIST_DIR}/fsi_exact_stores_compiler.cpp")

# Independent optional pure immutable state/incoming depth hoist. Canonical
# compiler files and baseline version inputs stay byte-identical.
file(SHA256 "${CMAKE_CURRENT_LIST_FILE}" GOW3_FSI_HOIST_PRODUCER_HASH)
string(SHA256 GOW3_FSI_HOIST_COMMON_HASH "${GOW3_SHADER_COMMON}")
set(GOW3_FSI_HOIST_VERSION_INPUT "${GOW3_SHADER_VERSION};${GOW3_FSI_HOIST_PRODUCER_HASH};${GOW3_FSI_HOIST_COMMON_HASH};")
foreach(source fsi_immutable_state_hoist.h fsi_immutable_state_hoist_compiler.h fsi_immutable_state_hoist_compiler.cpp)
    set(path "${CMAKE_CURRENT_LIST_DIR}/${source}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
    file(SHA256 "${path}" source_hash)
    string(APPEND GOW3_FSI_HOIST_VERSION_INPUT "${source_hash};")
endforeach()
string(SHA256 GOW3_FSI_HOIST_STATE_VERSION "${GOW3_FSI_HOIST_VERSION_INPUT};native-fsi-immutable-state-load-hoist-v1;")
string(SHA256 GOW3_FSI_HOIST_INCOMING_VERSION "${GOW3_FSI_HOIST_VERSION_INPUT};native-fsi-immutable-state-incoming-depth-hoist-v2;")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/native_fsi_hoist_version.h" CONTENT
"#pragma once
inline constexpr char kNativeFsiHoistBaseline[] = \"@GOW3_SHADER_VERSION@\";
inline constexpr char kNativeFsiHoistStateShaderVersion[] = \"@GOW3_FSI_HOIST_STATE_VERSION@\";
inline constexpr char kNativeFsiHoistIncomingDepthShaderVersion[] = \"@GOW3_FSI_HOIST_INCOMING_VERSION@\";
" @ONLY)
target_sources(gow3-native-shader-compiler PRIVATE "${CMAKE_CURRENT_LIST_DIR}/fsi_immutable_state_hoist_compiler.cpp")

target_compile_features(gow3-native-shader-compiler PUBLIC cxx_std_23)
target_compile_definitions(gow3-native-shader-compiler PRIVATE GOW3_RECOMP)
target_include_directories(gow3-native-shader-compiler PUBLIC "${GOW3_SHADER_ROOT}/src" "${GOW3_SHADER_ROOT}/vendor/plume" "${GOW3_SHADER_ROOT}/vendor/rexglue-sdk/thirdparty/inja/third_party/include")
target_include_directories(gow3-native-shader-compiler PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}" "${GOW3_XENOS_ROOT}/XenosRecomp"
    "${GOW3_XENOS_ROOT}/thirdparty/smol-v/source"
    "${GOW3_XENOS_ROOT}/thirdparty/xxHash"
    "${GOW3_XENOS_ROOT}/thirdparty/zstd/lib"
    "${GOW3_SHADER_ROOT}/vendor/rexglue-sdk/thirdparty/picosha2")
include("${CMAKE_CURRENT_LIST_DIR}/content_hash.cmake")
target_link_libraries(gow3-native-shader-compiler PRIVATE Microsoft::DirectXShaderCompiler fmt::fmt gow3-native-content-hash)
target_precompile_headers(gow3-native-shader-compiler PRIVATE "${CMAKE_CURRENT_LIST_DIR}/shader_recompiler_pch.h")
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(gow3-native-shader-compiler PRIVATE -Wno-switch -Wno-unused-variable -Wno-null-arithmetic -fms-extensions)
endif()
if(WIN32)
    target_compile_definitions(gow3-native-shader-compiler PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()
