# Include from the main gow3 CMakeLists.txt; no GPU/runtime game initialization.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Threads REQUIRED)
    set(_gow3_robust_before "${CMAKE_CURRENT_LIST_DIR}/robust_condition_before_sdk.cpp")
    set(_gow3_robust_after "${REXSDK_DIR}/src/core/threading_posix.cpp")
    file(SHA256 "${_gow3_robust_before}" _gow3_robust_before_sha)
    file(SHA256 "${_gow3_robust_after}" _gow3_robust_after_sha)
    if(NOT _gow3_robust_before_sha STREQUAL "6ac219ac6af4d17d8b5c01e90a28dbf4620f5e1127c582869ae284ffdb02d9e5" OR
       NOT _gow3_robust_after_sha STREQUAL "3d9cf49c06611a2528ccb38589d2e77da50b75f15d22b2c57ea116a7d3a217de")
        message(FATAL_ERROR "Robust-condition fixture requires exact authenticated before/after SDK source")
    endif()
    foreach(_gow3_robust_mode before after)
        set(_gow3_robust_target "gow3-posix-robust-condition-${_gow3_robust_mode}-test")
        add_executable(${_gow3_robust_target}
            "${CMAKE_CURRENT_LIST_DIR}/robust_condition_reacquire_test.cpp")
        if(_gow3_robust_mode STREQUAL "before")
            set(_gow3_robust_source "${_gow3_robust_before}")
            set(_gow3_robust_sha "${_gow3_robust_before_sha}")
            set(_gow3_robust_expect_before 1)
        else()
            set(_gow3_robust_source "${_gow3_robust_after}")
            set(_gow3_robust_sha "${_gow3_robust_after_sha}")
            set(_gow3_robust_expect_before 0)
        endif()
        set_property(SOURCE "${CMAKE_CURRENT_LIST_DIR}/robust_condition_reacquire_test.cpp"
            APPEND PROPERTY OBJECT_DEPENDS "${_gow3_robust_before};${_gow3_robust_after}")
        target_compile_definitions(${_gow3_robust_target} PRIVATE
            GOW3_ROBUST_CONDITION_SDK_SOURCE="${_gow3_robust_source}"
            GOW3_ROBUST_CONDITION_SDK_SHA256="${_gow3_robust_sha}"
            GOW3_ROBUST_CONDITION_EXPECT_BEFORE=${_gow3_robust_expect_before}
            _GLIBCXX_ASSERTIONS=1)
        target_compile_options(${_gow3_robust_target} PRIVATE -UNDEBUG)
        target_compile_features(${_gow3_robust_target} PRIVATE cxx_std_23)
        target_link_libraries(${_gow3_robust_target} PRIVATE rex::runtime Threads::Threads)
    endforeach()
endif()
