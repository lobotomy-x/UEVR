cmake_minimum_required(VERSION 3.20)
project(api_test_plugin LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(api_test_plugin SHARED Plugin.cpp)

# uevr/API.h, API.hpp, Plugin.hpp live in the repo's top-level include/.
target_include_directories(api_test_plugin PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)

if(MSVC)
    target_compile_options(api_test_plugin PRIVATE /W3 /permissive- /Zc:__cplusplus)
endif()

# Drop the DLL into <Game>/UnrealVRMod/plugins/ to load.
set_target_properties(api_test_plugin PROPERTIES
    OUTPUT_NAME "api_test_plugin"
    SUFFIX ".dll"
)
