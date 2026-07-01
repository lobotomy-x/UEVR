cmake_minimum_required(VERSION 3.20)
project(cimgui_native_plugin LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(UEVR_ROOT       "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(UEVR_IMGUI_DIR  "${UEVR_ROOT}/dependencies/submodules/imgui")
set(UEVR_IMCONFIG   "${UEVR_ROOT}/src/uevr-imgui/uevr_imconfig.hpp")

add_library(cimgui_native_plugin SHARED
    Plugin.cpp
    "${UEVR_IMGUI_DIR}/imgui.cpp"
    "${UEVR_IMGUI_DIR}/imgui_draw.cpp"
    "${UEVR_IMGUI_DIR}/imgui_tables.cpp"
    "${UEVR_IMGUI_DIR}/imgui_widgets.cpp"
)

target_include_directories(cimgui_native_plugin PRIVATE
    "${UEVR_ROOT}/include"
    "${UEVR_IMGUI_DIR}"
)

target_compile_definitions(cimgui_native_plugin PRIVATE
    "IMGUI_USER_CONFIG=\"${UEVR_IMCONFIG}\""
    _ENABLE_EXTENDED_ALIGNED_STORAGE
)

if(MSVC)
    target_compile_options(cimgui_native_plugin PRIVATE
        /W3 /permissive- /Zc:__cplusplus /bigobj /utf-8)
endif()

set_target_properties(cimgui_native_plugin PROPERTIES
    OUTPUT_NAME "cimgui_native_plugin"
    SUFFIX ".dll"
)
