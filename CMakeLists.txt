cmake_minimum_required(VERSION 3.25)
project(gow3 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(REXSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/rexglue-sdk" CACHE PATH "ReXGlue source snapshot")
set(REXGLUE_REGISTER_PACKAGE OFF CACHE BOOL "Do not alter user package registry")
set(REXGLUE_ENABLE_TRACY OFF CACHE BOOL "Disable optional profiler")
option(GOW3_ENABLE_GUEST_TRACE "Compile per-function guest diagnostic tracing" OFF)
option(GOW3_ENABLE_NATIVE_RENDERER "Build the Native renderer alongside the separately selectable Xenia renderer" ON)

# sdk keeps the original toolchain model. These alternatives are isolated
# experiments for this executable; SDK libraries and guest DLLs stay unchanged.
set(GOW3_EXPERIMENTAL_MAIN_CODE_MODEL "sdk" CACHE STRING
    "Experimental x86-64 main code model: sdk, small, or medium")
set_property(CACHE GOW3_EXPERIMENTAL_MAIN_CODE_MODEL PROPERTY STRINGS sdk small medium)
if(NOT GOW3_EXPERIMENTAL_MAIN_CODE_MODEL MATCHES "^(sdk|small|medium)$")
    message(FATAL_ERROR "GOW3_EXPERIMENTAL_MAIN_CODE_MODEL must be sdk, small, or medium")
endif()
if(NOT GOW3_EXPERIMENTAL_MAIN_CODE_MODEL STREQUAL "sdk")
    if(NOT (UNIX AND NOT APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$"
            AND CMAKE_CXX_COMPILER_ID MATCHES "^(Clang|GNU)$"))
        message(FATAL_ERROR "Experimental main code models require an x86-64 ELF Clang/GNU toolchain")
    endif()
endif()

# The SDK already bundles the CAS/FSR 1 shaders and host constants.
add_compile_definitions(REX_HAS_FIDELITYFX_SDK=1)
# FSR 3.1 temporal upscaling and frame generation come from our FidelityFX
# fork (vendor/fidelityfx-sdk). Its shader compiler is a Windows program;
# Linux builds run it through Wine.
option(GOW3_ENABLE_FIDELITYFX "Build FSR 3.1 temporal upscaling and frame generation" ON)
if(GOW3_ENABLE_FIDELITYFX AND CMAKE_HOST_UNIX)
    find_program(GOW3_WINE wine)
    if(NOT GOW3_WINE)
        message(WARNING "Wine was not found: building without FSR 3.1 and frame generation "
                        "(FidelityFX's shader compiler is a Windows program)")
        set(GOW3_ENABLE_FIDELITYFX OFF CACHE BOOL "" FORCE)
    endif()
endif()
if(GOW3_ENABLE_FIDELITYFX)
    set(REXGLUE_ENABLE_FIDELITYFX ON CACHE BOOL "" FORCE)
    set(FETCHCONTENT_SOURCE_DIR_FIDELITYFX "${CMAKE_CURRENT_SOURCE_DIR}/vendor/fidelityfx-sdk" CACHE PATH "" FORCE)
    if(WIN32 AND NOT CMAKE_HOST_WIN32)
        # Cross builds have no Vulkan SDK: FidelityFX links the loader through
        # an import library made from cmake/windows/vulkan-1.def and the SDK's
        # bundled headers. The real vulkan-1.dll ships with the GPU driver.
        set(GOW3_VULKAN_IMPLIB "${CMAKE_BINARY_DIR}/vulkan-1.lib")
        if(NOT EXISTS "${GOW3_VULKAN_IMPLIB}")
            execute_process(COMMAND "${CMAKE_AR}" "/def:${CMAKE_CURRENT_SOURCE_DIR}/cmake/windows/vulkan-1.def"
                    /machine:x64 "/out:${GOW3_VULKAN_IMPLIB}"
                RESULT_VARIABLE gow3_vulkan_implib_result)
            if(NOT gow3_vulkan_implib_result EQUAL 0)
                message(FATAL_ERROR "Cannot create the vulkan-1 import library")
            endif()
        endif()
        set(Vulkan_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/rexglue-sdk/thirdparty/vulkan-headers/include"
            CACHE PATH "" FORCE)
        set(Vulkan_LIBRARY "${GOW3_VULKAN_IMPLIB}" CACHE FILEPATH "" FORCE)
    endif()
endif()

# Vendor upscaler SDKs fetched by tools/fetch-vendor-sdks.sh (not part of
# this repository). Present SDKs are built in; absent ones are skipped.
option(GOW3_VENDOR_UPSCALERS "Build DLSS and XeSS when their SDKs are in vendor-sdks/" ON)
if(GOW3_VENDOR_UPSCALERS)
    set(GOW3_DLSS_SDK "${CMAKE_CURRENT_SOURCE_DIR}/vendor-sdks/dlss-v310.9.16")
    set(GOW3_XESS_SDK "${CMAKE_CURRENT_SOURCE_DIR}/vendor-sdks/xess-v3.0.2")
    if(EXISTS "${GOW3_DLSS_SDK}/.complete")
        set(REXGLUE_DLSS_SDK_DIR "${GOW3_DLSS_SDK}" CACHE PATH "" FORCE)
    endif()
    if(EXISTS "${GOW3_XESS_SDK}/.complete")
        set(REXGLUE_XESS_SDK_DIR "${GOW3_XESS_SDK}" CACHE PATH "" FORCE)
    endif()
endif()

include(generated/rexglue.cmake)

# Applies to this directory's targets only, not the SDK added above: keep
# <windows.h> from defining min/max over std::min/std::max.
if(WIN32)
    add_compile_definitions(NOMINMAX)
endif()
# Renderer and game sources name variables `interface`; see windows_compat.h.
function(gow3_windows_compat target)
    if(WIN32 AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        target_compile_options(${target} PRIVATE
            "$<$<COMPILE_LANGUAGE:CXX>:/FI${CMAKE_CURRENT_SOURCE_DIR}/src/platform/windows_compat.h>")
    endif()
endfunction()

if(GOW3_ENABLE_NATIVE_RENDERER)
    include(src/renderer/native/runtime.cmake)
    gow3_windows_compat(gow3-native-renderer)
    add_executable(gow3-temporal-inputs-test tests/temporal_inputs_test.cpp)
    target_include_directories(gow3-temporal-inputs-test PRIVATE src)
    target_link_libraries(gow3-temporal-inputs-test PRIVATE gow3-native-renderer rex::runtime)
    add_executable(gow3-native-host-device-test tools/rendering/native/live_device_test.cpp)
    target_link_libraries(gow3-native-host-device-test PRIVATE gow3-native-renderer)
endif()

add_executable(gow3-texture-layout-test tests/native_texture_test.cpp src/renderer/native/texture_data.cpp)
add_executable(gow3-certified-snapshot-test tools/rendering/native/certified_snapshot_test.cpp)
target_include_directories(gow3-certified-snapshot-test PRIVATE src)
target_link_libraries(gow3-certified-snapshot-test PRIVATE rex::runtime)
add_executable(gow3-certified-snapshot-platform-test tools/rendering/native/certified_snapshot_platform_test.cpp)
target_link_libraries(gow3-certified-snapshot-platform-test PRIVATE rex::runtime)
add_executable(gow3-certified-memory-test tools/rendering/native/certified_memory_test.cpp)
target_link_libraries(gow3-certified-memory-test PRIVATE rex::runtime)
add_executable(gow3-proc-maps-parser-test tools/rendering/native/proc_maps_parser_test.cpp)
target_include_directories(gow3-proc-maps-parser-test PRIVATE "${REXSDK_DIR}/include")
add_executable(gow3-native-coherence-wait-test tools/rendering/native/coherence_wait_test.cpp)
target_link_libraries(gow3-native-coherence-wait-test PRIVATE rex::runtime)
add_executable(gow3-type0-constants-test tools/rendering/native/type0_constants_test.cpp)
target_link_libraries(gow3-type0-constants-test PRIVATE rex::runtime)
add_executable(gow3-native-strict-scene-test tools/rendering/native/strict_native_scene_test.cpp)
target_link_libraries(gow3-native-strict-scene-test PRIVATE rex::runtime)
# Porting tool: dump a loaded XEX image for signature extraction/scanning.
add_executable(gow3-xex-image-dump tools/porting/xex_image_dump.cpp)
target_link_libraries(gow3-xex-image-dump PRIVATE rexcodegen)
add_executable(gow3-page-access-cache-test tools/rendering/native/page_access_cache_test.cpp)
target_link_libraries(gow3-page-access-cache-test PRIVATE rex::runtime)
add_executable(gow3-coherence-snapshot-memory-test tools/rendering/native/coherence_snapshot_memory_test.cpp)
target_include_directories(gow3-coherence-snapshot-memory-test PRIVATE src)
target_link_libraries(gow3-coherence-snapshot-memory-test PRIVATE rex::runtime)
add_executable(gow3-external-write-completion-test tools/rendering/native/external_write_completion_test.cpp)
target_include_directories(gow3-external-write-completion-test PRIVATE src)
target_link_libraries(gow3-external-write-completion-test PRIVATE rex::runtime)
target_include_directories(gow3-texture-layout-test PRIVATE src)
target_link_libraries(gow3-texture-layout-test PRIVATE rex::runtime)
add_executable(gow3-trace-residency-test tests/trace_residency_test.cpp)
target_link_libraries(gow3-trace-residency-test PRIVATE rex::runtime)
add_executable(gow3-reference-stacked-lerp-test tests/reference_stacked_lerp_test.cpp)
target_link_libraries(gow3-reference-stacked-lerp-test PRIVATE rex::runtime)
target_include_directories(gow3-reference-stacked-lerp-test PRIVATE "${REXSDK_DIR}/thirdparty/glslang")

if(TARGET rex_hdr_detection)
    find_package(Vulkan REQUIRED)
    add_executable(gow3-hdr-probe tools/hdr_probe.cpp)
    target_link_libraries(gow3-hdr-probe PRIVATE rex_hdr_detection Vulkan::Vulkan)
    find_program(GLSLANG_VALIDATOR glslangValidator)
    if(GLSLANG_VALIDATOR)
        add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/hdr_transfer_test.h
            COMMAND ${GLSLANG_VALIDATOR} -V --target-env vulkan1.1
                -I${REXSDK_DIR}/src/ui/shaders --vn hdr_transfer_test
                -o ${CMAKE_BINARY_DIR}/hdr_transfer_test.h
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/hdr_transfer.comp
            DEPENDS tests/hdr_transfer.comp ${REXSDK_DIR}/src/ui/shaders/hdr_transfer.glsl
            VERBATIM)
        add_executable(hdr_transfer_test tests/hdr_transfer_test.cpp ${CMAKE_BINARY_DIR}/hdr_transfer_test.h)
        target_include_directories(hdr_transfer_test PRIVATE ${CMAKE_BINARY_DIR})
        target_link_libraries(hdr_transfer_test PRIVATE Vulkan::Vulkan)
    endif()
endif()

if(WIN32 AND REXGLUE_XESS_SDK_DIR AND GOW3_VULKAN_IMPLIB)
    add_executable(gow3-xess-context-test tests/xess_context_test.cpp)
    target_include_directories(gow3-xess-context-test PRIVATE "${REXGLUE_XESS_SDK_DIR}/inc"
        "${REXSDK_DIR}/thirdparty/vulkan-headers/include")
    target_link_libraries(gow3-xess-context-test PRIVATE "${GOW3_VULKAN_IMPLIB}")
endif()

if(TARGET amd_fidelityfx_vk AND NOT WIN32)
    find_package(Vulkan)
    if(Vulkan_FOUND)
        add_executable(gow3-fidelityfx-context-test tests/fidelityfx_context_test.cpp)
        target_include_directories(gow3-fidelityfx-context-test PRIVATE
            "${FETCHCONTENT_SOURCE_DIR_FIDELITYFX}/ffx-api/include")
        target_link_libraries(gow3-fidelityfx-context-test PRIVATE amd_fidelityfx_vk Vulkan::Vulkan)
    endif()
endif()

if(TARGET rex_adaptive_sync)
    add_executable(gow3-vrr-probe tools/vrr_probe.cpp)
    target_link_libraries(gow3-vrr-probe PRIVATE rex_adaptive_sync)
endif()

if(TARGET unit_tests)
    target_sources(unit_tests PRIVATE tests/branch_context_test.cpp tests/menu_guide_test.cpp tests/menu_navigation_test.cpp)
    target_sources(unit_tests PRIVATE tests/shader_storage_progress_test.cpp)
    target_sources(unit_tests PRIVATE tests/frame_limiter_test.cpp)
    target_sources(unit_tests PRIVATE tests/settings_precedence_test.cpp)
    target_sources(unit_tests PRIVATE tests/wait_pacing_test.cpp)
    target_sources(unit_tests PRIVATE tests/pending_query_results_test.cpp)
    target_sources(unit_tests PRIVATE tests/async_shader_translation_test.cpp)
    target_sources(unit_tests PRIVATE tests/network_test.cpp src/gow3_identity.cpp)
    target_include_directories(unit_tests PRIVATE src)
    target_include_directories(unit_tests PRIVATE "${REXSDK_DIR}/thirdparty/disasm")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/sources.cmake")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_executable(gow3-getdata-inline-selection-test tools/rendering/native/getdata_inline_selection_test.cpp)
    target_include_directories(gow3-getdata-inline-selection-test PRIVATE src "${CMAKE_CURRENT_SOURCE_DIR}"
        "${REXSDK_DIR}/include" "${REXSDK_DIR}/thirdparty/simde")
    add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/gow3_settings_schema.h
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/settings/generate_menu_schema.py
            ${CMAKE_BINARY_DIR}/gow3_settings_schema.h
        DEPENDS tools/settings/generate_menu_schema.py tools/settings/model.py
        VERBATIM)
    add_executable(gow3-renderer-settings-test tests/renderer_settings_test.cpp
        ${CMAKE_BINARY_DIR}/gow3_settings_schema.h)
    target_include_directories(gow3-renderer-settings-test PRIVATE src ${CMAKE_BINARY_DIR})
    if(TARGET unit_tests)
        target_sources(unit_tests PRIVATE tests/renderer_settings_config_test.cpp src/gow3_settings.cpp
            ${CMAKE_BINARY_DIR}/gow3_settings_schema.h)
        target_include_directories(unit_tests PRIVATE src ${CMAKE_BINARY_DIR})
    endif()
    # Self-hostable lobby and relay for internet System Link; header-only use of
    # the SDK's protocol definition.
    add_executable(squarehole-lobby tools/lobby/lobby_server.cpp)
    target_include_directories(squarehole-lobby PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/vendor/rexglue-sdk/include")
    target_compile_features(squarehole-lobby PRIVATE cxx_std_20)
    if(WIN32)
        target_link_libraries(squarehole-lobby PRIVATE ws2_32)
    endif()
    add_executable(gow3 src/main.cpp src/gameplay_hooks.cpp src/gow3_identity.cpp src/gow3_settings.cpp src/gow3_settings_live.cpp src/gow3_settings_menu.cpp src/gow3_vrr.cpp src/gow3_host.cpp
        src/renderer/gears_bridge.cpp src/renderer/native/getdata_inline_leaf.cpp
        ${CMAKE_BINARY_DIR}/gow3_settings_schema.h)
    rexglue_setup_target(gow3)
    gow3_windows_compat(gow3)
    include(cmake/host_allocator.cmake)
    add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/gow3_getdata_inline_source_auth.json
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/rendering/getdata_inline_source_guard.py
            --root ${CMAKE_CURRENT_SOURCE_DIR}
            --stamp ${CMAKE_BINARY_DIR}/gow3_getdata_inline_source_auth.json
        DEPENDS tools/rendering/getdata_inline_source_guard.py
            generated/default/gow3_recomp.71.cpp generated/default/gow3_recomp.85.cpp
            generated/native-getdata/getdata_leaf_body.inc generated/native-getdata/getdata_register_helpers.inc
            generated/native-getdata/getdata_published_body.inc
        VERBATIM)
    add_custom_target(gow3-getdata-inline-source-auth DEPENDS ${CMAKE_BINARY_DIR}/gow3_getdata_inline_source_auth.json)
    add_dependencies(gow3 gow3-getdata-inline-source-auth)
    add_dependencies(gow3-getdata-inline-selection-test gow3-getdata-inline-source-auth)
    if(TARGET gow3-hdr-probe)
        add_dependencies(gow3 gow3-hdr-probe)
    endif()
    if(TARGET gow3-vrr-probe)
        add_dependencies(gow3 gow3-vrr-probe)
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(GOW3_GTK REQUIRED IMPORTED_TARGET gtk+-3.0)
        add_executable(gow3-window-size-test tests/window_size_test.cpp)
        target_link_libraries(gow3-window-size-test PRIVATE rex::runtime PkgConfig::GOW3_GTK)
    endif()
    target_link_libraries(gow3 PRIVATE tomlplusplus xxHash::xxhash)
    if(TARGET rex_adaptive_sync)
        # Static, not exported from rexruntime.dll on Windows; gow3_vrr.cpp calls it.
        target_link_libraries(gow3 PRIVATE rex_adaptive_sync)
    endif()
    if(WIN32)
        # The SDK's Windows entry point is wWinMain.
        set_target_properties(gow3 PROPERTIES WIN32_EXECUTABLE ON)
    endif()
    if(GOW3_ENABLE_NATIVE_RENDERER)
        target_link_libraries(gow3 PRIVATE gow3-native-renderer)
        target_compile_definitions(gow3 PRIVATE GOW3_HAS_NATIVE_RENDERER=1)
        add_custom_command(TARGET gow3 POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:Microsoft::DirectXShaderCompiler>" "$<TARGET_FILE_DIR:gow3>"
            VERBATIM)
    endif()
    foreach(runtime ${REXGLUE_VENDOR_UPSCALER_RUNTIMES})
        add_custom_command(TARGET gow3 POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${runtime}" "$<TARGET_FILE_DIR:gow3>"
            VERBATIM)
    endforeach()
    if(GOW3_ENABLE_GUEST_TRACE)
        target_compile_definitions(gow3 PRIVATE REXGLUE_ENABLE_GUEST_FUNCTION_TRACE=1)
    endif()
    if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        target_compile_options(gow3 PRIVATE -fno-strict-aliasing /fp:strict /Zc:char8_t-
            "/FI${CMAKE_CURRENT_SOURCE_DIR}/src/gameplay_hooks.h")
    else()
        target_compile_options(gow3 PRIVATE -fno-strict-aliasing -ffp-model=strict -fno-char8_t)
        target_compile_options(gow3 PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/src/gameplay_hooks.h")
    endif()
    if(NOT GOW3_EXPERIMENTAL_MAIN_CODE_MODEL STREQUAL "sdk")
        # This is deliberately after rexglue_setup_target's original model.
        # Keep the SDK's --no-relax link setting to isolate this experiment.
        target_compile_options(gow3 PRIVATE "-mcmodel=${GOW3_EXPERIMENTAL_MAIN_CODE_MODEL}")
        message(STATUS "Experimental gow3 main code model: ${GOW3_EXPERIMENTAL_MAIN_CODE_MODEL}")
    endif()
    # rex_app.cpp is compiled by the host and needs SDK implementation headers.
    target_include_directories(gow3 PRIVATE ${CMAKE_BINARY_DIR}
        "${REXSDK_DIR}/thirdparty/imgui"
        "${REXSDK_DIR}/thirdparty/renderdoc"
        "${CMAKE_BINARY_DIR}/rexglue-sdk/include")
endif()

# Independent physical snapshot batch CPU correctness fixtures.
include("${CMAKE_CURRENT_LIST_DIR}/tools/rendering/native/snapshot_batch_fixture.cmake")

# Optional host renderer PGO. Generated guest code keeps its original options.
include("${CMAKE_CURRENT_LIST_DIR}/cmake/native_pgo.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/cmake/sdk_ir_pgo.cmake")

# Focused actual SDK robust condition-wait recovery fixtures.
include("${CMAKE_CURRENT_LIST_DIR}/tools/rendering/native/robust_condition_fixture.cmake")
