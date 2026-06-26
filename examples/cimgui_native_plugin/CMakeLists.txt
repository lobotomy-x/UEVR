cmake_minimum_required(VERSION 3.20)
project(cimgui_native_plugin LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(cimgui_native_plugin SHARED Plugin.cpp)

target_include_directories(cimgui_native_plugin PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)

if(MSVC)
    target_compile_options(cimgui_native_plugin PRIVATE /W3 /permissive- /Zc:__cplusplus)
endif()

set_target_properties(cimgui_native_plugin PROPERTIES
    OUTPUT_NAME "cimgui_native_plugin"
    SUFFIX ".dll"
)
