# Independent SDK CPU IR-PGO experiment. Include AFTER native_pgo.cmake.
# OFF performs no tool invocation and changes no compile or link options.
set(GOW3_SDK_IR_PGO_MODE "OFF" CACHE STRING "SDK CPU IR-PGO: OFF, GENERATE, USE")
set_property(CACHE GOW3_SDK_IR_PGO_MODE PROPERTY STRINGS OFF GENERATE USE)
set(GOW3_SDK_IR_PGO_PATH "" CACHE PATH "Absolute fresh raw directory or merged profdata")
set(GOW3_SDK_IR_PGO_MANIFEST "" CACHE FILEPATH "USE: manifest written by sdk_ir_pgo.py merge")
set(GOW3_SDK_IR_PGO_SHA256 "" CACHE STRING "USE: exact merged profile SHA256")
if(NOT GOW3_SDK_IR_PGO_MODE MATCHES "^(OFF|GENERATE|USE)$")
    message(FATAL_ERROR "GOW3_SDK_IR_PGO_MODE must be OFF, GENERATE or USE")
endif()
if(GOW3_SDK_IR_PGO_MODE STREQUAL "OFF")
    return()
endif()
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR
   NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR
   NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "GNU" OR
   NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
    message(FATAL_ERROR "This qualified SDK PGO scope requires Linux x86-64 Clang GNU ABI")
endif()
if(NOT GOW3_ENABLE_NATIVE_RENDERER OR NOT TARGET gow3 OR
   NOT GOW3_NATIVE_PGO_MODE STREQUAL "OFF")
    message(FATAL_ERROR "SDK PGO requires Native renderer with separate Native PGO mode OFF")
endif()
if(NOT CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_INTERPROCEDURAL_OPTIMIZATION OR
   NOT CMAKE_GENERATOR STREQUAL "Ninja")
    message(FATAL_ERROR "SDK PGO requires the existing Release/no-LTO configuration")
endif()
foreach(_sdk_pgo_target rexgraphics rexruntime rexcore)
    if(NOT TARGET ${_sdk_pgo_target})
        message(FATAL_ERROR "SDK PGO requires source target ${_sdk_pgo_target}")
    endif()
endforeach()
if(NOT IS_ABSOLUTE "${GOW3_SDK_IR_PGO_PATH}")
    message(FATAL_ERROR "SDK PGO path must be absolute")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(_sdk_pgo_root "${CMAKE_CURRENT_LIST_DIR}/..")
get_filename_component(_sdk_pgo_root "${_sdk_pgo_root}" ABSOLUTE)
get_filename_component(_sdk_pgo_sdk "${REXSDK_DIR}" REALPATH)
if(NOT _sdk_pgo_sdk STREQUAL "${_sdk_pgo_root}/vendor/rexglue-sdk")
    message(FATAL_ERROR "SDK PGO source identity requires the pinned workspace SDK")
endif()
set(_sdk_pgo_list "${CMAKE_CURRENT_LIST_DIR}/sdk_ir_pgo.profile-list")
set(_sdk_pgo_script "${_sdk_pgo_root}/tools/rendering/sdk_ir_pgo.py")
set(_sdk_pgo_config "${CMAKE_BINARY_DIR}/sdk-ir-pgo/config.json")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${_sdk_pgo_script}" configure
    --root "${_sdk_pgo_root}" --build "${CMAKE_BINARY_DIR}"
    --compiler "${CMAKE_CXX_COMPILER}" --mode "${GOW3_SDK_IR_PGO_MODE}"
    --path "${GOW3_SDK_IR_PGO_PATH}" --profile-sha256 "${GOW3_SDK_IR_PGO_SHA256}"
    --manifest "${GOW3_SDK_IR_PGO_MANIFEST}" --output "${_sdk_pgo_config}"
    RESULT_VARIABLE _sdk_pgo_result ERROR_VARIABLE _sdk_pgo_error)
if(NOT _sdk_pgo_result EQUAL 0)
    message(FATAL_ERROR "SDK PGO identity/admission failed: ${_sdk_pgo_error}")
endif()
set(_sdk_pgo_options "-fprofile-list=${_sdk_pgo_list}")
if(GOW3_SDK_IR_PGO_MODE STREQUAL "GENERATE")
    list(APPEND _sdk_pgo_options "-fprofile-generate=${GOW3_SDK_IR_PGO_PATH}" -fprofile-update=atomic)
    # All five selected translation units end in this shared library. Generated
    # guest and Native static libraries contain no SDK-PGO instrumentation.
    target_link_options(rexruntime PRIVATE "-fprofile-generate=${GOW3_SDK_IR_PGO_PATH}")
else()
    list(APPEND _sdk_pgo_options "-fprofile-use=${GOW3_SDK_IR_PGO_PATH}"
        -Werror=profile-instr-out-of-date -Werror=profile-instr-unprofiled)
endif()
function(_gow3_sdk_pgo_source target relative)
    set(_file "${REXSDK_DIR}/src/${relative}")
    set_property(SOURCE "${_file}" TARGET_DIRECTORY ${target}
        APPEND PROPERTY COMPILE_OPTIONS ${_sdk_pgo_options})
endfunction()
_gow3_sdk_pgo_source(rexgraphics graphics/command_processor.cpp)
_gow3_sdk_pgo_source(rexgraphics graphics/vulkan/command_processor.cpp)
_gow3_sdk_pgo_source(rexruntime system/xmemory.cpp)
_gow3_sdk_pgo_source(rexruntime system/function_dispatcher.cpp)
_gow3_sdk_pgo_source(rexcore core/ring_buffer.cpp)
message(STATUS "SDK CPU IR-PGO ${GOW3_SDK_IR_PGO_MODE}; five source-local TUs; identity ${_sdk_pgo_config}")
unset(_sdk_pgo_target)
unset(_sdk_pgo_result)
unset(_sdk_pgo_error)
unset(_sdk_pgo_options)
unset(_sdk_pgo_config)
unset(_sdk_pgo_root)
unset(_sdk_pgo_list)
unset(_sdk_pgo_script)
unset(_sdk_pgo_sdk)
