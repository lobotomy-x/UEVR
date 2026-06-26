# Install script for directory: I:/code/lobotomy-x/UEVR/build-hunt/_deps/directxtk12-src

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "C:/Program Files/uevr-proj")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "Release")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Dd][Ee][Bb][Uu][Gg])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY FILES "I:/code/lobotomy-x/UEVR/build-hunt/bin/CMake/Debug/DirectXTK12.lib")
  elseif(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY FILES "I:/code/lobotomy-x/UEVR/build-hunt/bin/CMake/Release/DirectXTK12.lib")
  elseif(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Mm][Ii][Nn][Ss][Ii][Zz][Ee][Rr][Ee][Ll])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY FILES "I:/code/lobotomy-x/UEVR/build-hunt/bin/CMake/MinSizeRel/DirectXTK12.lib")
  elseif(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Rr][Ee][Ll][Ww][Ii][Tt][Hh][Dd][Ee][Bb][Ii][Nn][Ff][Oo])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY FILES "I:/code/lobotomy-x/UEVR/build-hunt/bin/CMake/RelWithDebInfo/DirectXTK12.lib")
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/share/directxtk12/DirectXTK12-targets.cmake")
    file(DIFFERENT _cmake_export_file_changed FILES
         "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/share/directxtk12/DirectXTK12-targets.cmake"
         "I:/code/lobotomy-x/UEVR/build-hunt/_deps/directxtk12-build/CMakeFiles/Export/dc79b1416ca4e830525952dfb83a96c7/DirectXTK12-targets.cmake")
    if(_cmake_export_file_changed)
      file(GLOB _cmake_old_config_files "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/share/directxtk12/DirectXTK12-targets-*.cmake")
      if(_cmake_old_config_files)
        string(REPLACE ";" ", " _cmake_old_config_files_text "${_cmake_old_config_files}")
        message(STATUS "Old export file \"$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/share/directxtk12/DirectXTK12-targets.cmake\" will be replaced.  Removing files [${_cmake_old_config_files_text}].")
        unset(_cmake_old_config_files_text)
        file(REMOVE ${_cmake_old_config_files})
      endif()
      unset(_cmake_old_config_files)
    endif()
    unset(_cmake_export_file_changed)
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/directxtk12" TYPE FILE FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/directxtk12-build/CMakeFiles/Export/dc79b1416ca4e830525952dfb83a96c7/DirectXTK12-targets.cmake")
  if(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Dd][Ee][Bb][Uu][Gg])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/directxtk12" TYPE FILE FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/directxtk12-build/CMakeFiles/Export/dc79b1416ca4e830525952dfb83a96c7/DirectXTK12-targets-debug.cmake")
  endif()
  if(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Mm][Ii][Nn][Ss][Ii][Zz][Ee][Rr][Ee][Ll])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/directxtk12" TYPE FILE FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/directxtk12-build/CMakeFiles/Export/dc79b1416ca4e830525952dfb83a96c7/DirectXTK12-targets-minsizerel.cmake")
  endif()
  if(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Rr][Ee][Ll][Ww][Ii][Tt][Hh][Dd][Ee][Bb][Ii][Nn][Ff][Oo])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/directxtk12" TYPE FILE FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/directxtk12-build/CMakeFiles/Export/dc79b1416ca4e830525952dfb83a96c7/DirectXTK12-targets-relwithdebinfo.cmake")
  endif()
  if(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/directxtk12" TYPE FILE FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/directxtk12-build/CMakeFiles/Export/dc79b1416ca4e830525952dfb83a96c7/DirectXTK12-targets-release.cmake")
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/directxtk12" TYPE FILE FILES
    "I:/code/lobotomy-x/UEVR/build-hunt/_deps/directxtk12-src/Inc/BufferHelpers.h"
    "I:/code/lobotomy-x/UEVR/build-hunt/_deps/directxtk12-src/Inc/CommonStates.h"
    "I:/code/lobotomy-x/UEVR/build-hunt/_deps/directxtk12-src/Inc/DDSTextureLoader.h"
    "I:/code/lobotomy-x/UEVR/build-hunt/_deps/directxtk12-src/Inc/DescriptorHeap.h"
    "I:/code/lobotomy-x/UEVR/build-hunt/_deps/directxtk12-src/Inc/DirectXHelpers.h"
    "I:/code/lobotomy-x/UEVR/build-hunt/_deps/directxtk12-src/Inc/Effects.h"
    "I:/code/lobotomy-x/UEVR/build-hunt/_deps/directxtk12-src/Inc/EffectPipelineStateDescription.h"
    "I:/code/lobotomy-x/UEVR/build-hunt/_deps/directxtk12-src/Inc/GeometricPrimitive.h"
    "I:/code/lobotomy-x/UEVR/build-hunt/_deps/directxtk12-src/Inc/GraphicsMemory.h"
    "I:/code/lobotomy-x/UEVR/build-hunt/_deps/directxtk12-src/Inc/Model.h"
    "I:/code/lobotomy-x/UEVR/build-hunt/_deps/directxtk12-src/Inc/PostProcess.h"
    "I:/code/lobotomy-x/UEVR/build-hunt/_deps/directxtk12-src/Inc/PrimitiveBatch.h"
    "I:/code/lobotomy-x/UEVR/build-hunt/_deps/directxtk12-src/Inc/RenderTargetState.h"
    "I:/code/lobotomy-x/UEVR/build-hunt/_deps/directxtk12-src/Inc/ResourceUploadBatch.h"
    "I:/code/lobotomy-x/UEVR/build-hunt/_deps/directxtk12-src/Inc/ScreenGrab.h"
    "I:/code/lobotomy-x/UEVR/build-hunt/_deps/directxtk12-src/Inc/SpriteBatch.h"
    "I:/code/lobotomy-x/UEVR/build-hunt/_deps/directxtk12-src/Inc/SpriteFont.h"
    "I:/code/lobotomy-x/UEVR/build-hunt/_deps/directxtk12-src/Inc/VertexTypes.h"
    "I:/code/lobotomy-x/UEVR/build-hunt/_deps/directxtk12-src/Inc/WICTextureLoader.h"
    "I:/code/lobotomy-x/UEVR/build-hunt/_deps/directxtk12-src/Inc/GamePad.h"
    "I:/code/lobotomy-x/UEVR/build-hunt/_deps/directxtk12-src/Inc/Keyboard.h"
    "I:/code/lobotomy-x/UEVR/build-hunt/_deps/directxtk12-src/Inc/Mouse.h"
    "I:/code/lobotomy-x/UEVR/build-hunt/_deps/directxtk12-src/Inc/SimpleMath.h"
    "I:/code/lobotomy-x/UEVR/build-hunt/_deps/directxtk12-src/Inc/SimpleMath.inl"
    "I:/code/lobotomy-x/UEVR/build-hunt/_deps/directxtk12-src/Inc/Audio.h"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/directxtk12" TYPE FILE FILES
    "I:/code/lobotomy-x/UEVR/build-hunt/_deps/directxtk12-build/directxtk12-config.cmake"
    "I:/code/lobotomy-x/UEVR/build-hunt/_deps/directxtk12-build/directxtk12-config-version.cmake"
    )
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "I:/code/lobotomy-x/UEVR/build-hunt/_deps/directxtk12-build/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
