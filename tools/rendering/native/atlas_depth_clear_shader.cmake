# Include once after GLSLANG_VALIDATOR and existing targets are defined.
foreach(atlas_clear_mode packed host)
    if(atlas_clear_mode STREQUAL "host")
        set(atlas_clear_name atlas_depth_clear_host)
        set(atlas_host_source 1)
    else()
        set(atlas_clear_name atlas_depth_clear)
        set(atlas_host_source 0)
    endif()
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${atlas_clear_name}_spv.h"
        COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.1 -DHOST_SOURCE=${atlas_host_source}
            --vn ${atlas_clear_name}_spv -o "${CMAKE_CURRENT_BINARY_DIR}/${atlas_clear_name}_spv.h"
            "${CMAKE_CURRENT_LIST_DIR}/atlas_depth_clear.comp"
        DEPENDS "${CMAKE_CURRENT_LIST_DIR}/atlas_depth_clear.comp" VERBATIM)
    get_property(atlas_clear_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    foreach(atlas_clear_target IN LISTS atlas_clear_targets)
        get_target_property(atlas_clear_sources ${atlas_clear_target} SOURCES)
        if(atlas_clear_target STREQUAL "gow3-native-stencil-export-test" OR
           "${atlas_clear_sources}" MATCHES "scene_executor[.]cpp")
            target_sources(${atlas_clear_target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/${atlas_clear_name}_spv.h")
        endif()
    endforeach()
endforeach()
