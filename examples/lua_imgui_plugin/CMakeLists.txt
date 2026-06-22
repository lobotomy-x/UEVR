cmake_minimum_required(VERSION 3.20)
project(lua_imgui_plugin LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(lua_imgui_plugin SHARED Plugin.cpp)

target_include_directories(lua_imgui_plugin PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)

if(MSVC)
    target_compile_options(lua_imgui_plugin PRIVATE /W3 /permissive- /Zc:__cplusplus)
endif()

set_target_properties(lua_imgui_plugin PROPERTIES
    OUTPUT_NAME "lua_imgui_plugin"
    SUFFIX ".dll"
)
