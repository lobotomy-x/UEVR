cmake_minimum_required(VERSION 3.20)
project(lua_script_editor_plugin LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(IMCTEDIT_DIR "C:/Users/lbatv/source/repos/ImGuiColorTextEdit"
    CACHE PATH "ImGuiColorTextEdit source dir")

# CRITICAL: must use the EXACT imgui sources + imconfig that UEVR's main
# binary uses. Mismatched version or imconfig defines (e.g.
# IMGUI_USE_WCHAR32) shift ImGuiContext/ImGuiIO/ImWchar layouts and corrupt
# host state when the plugin calls ImGui::SetCurrentContext(host_ctx).
set(UEVR_ROOT       "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(UEVR_IMGUI_DIR  "${UEVR_ROOT}/dependencies/submodules/imgui")
set(UEVR_IMCONFIG   "${UEVR_ROOT}/src/uevr-imgui/uevr_imconfig.hpp")

if(NOT EXISTS "${IMCTEDIT_DIR}/TextEditor.cpp")
    message(FATAL_ERROR
        "ImGuiColorTextEdit not found at ${IMCTEDIT_DIR}. "
        "Pass -DIMCTEDIT_DIR=<path> at configure time.")
endif()
if(NOT EXISTS "${UEVR_IMCONFIG}")
    message(FATAL_ERROR "uevr_imconfig.hpp missing at ${UEVR_IMCONFIG}")
endif()

add_library(lua_script_editor_plugin SHARED
    Plugin.cpp
    "${IMCTEDIT_DIR}/TextEditor.cpp"
    "${IMCTEDIT_DIR}/TextDiff.cpp"
    "${UEVR_IMGUI_DIR}/imgui.cpp"
    "${UEVR_IMGUI_DIR}/imgui_draw.cpp"
    "${UEVR_IMGUI_DIR}/imgui_tables.cpp"
    "${UEVR_IMGUI_DIR}/imgui_widgets.cpp"
)

target_include_directories(lua_script_editor_plugin PRIVATE
    "${UEVR_ROOT}/include"
    "${IMCTEDIT_DIR}"
    "${UEVR_IMGUI_DIR}"
)

# Use UEVR's imconfig so every imgui struct in this DLL matches the host.
target_compile_definitions(lua_script_editor_plugin PRIVATE
    "IMGUI_USER_CONFIG=\"${UEVR_IMCONFIG}\""
    _ENABLE_EXTENDED_ALIGNED_STORAGE
)

if(MSVC)
    target_compile_options(lua_script_editor_plugin PRIVATE
        /W3 /permissive- /Zc:__cplusplus /bigobj /utf-8)
endif()

set_target_properties(lua_script_editor_plugin PROPERTIES
    OUTPUT_NAME "lua_script_editor_plugin"
    SUFFIX ".dll"
)
