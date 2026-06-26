# Install script for directory: I:/code/lobotomy-x/UEVR/build-hunt/_deps/asmjit-src

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
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/asmjit-build/Debug/asmjit.lib")
  elseif(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/asmjit-build/Release/asmjit.lib")
  elseif(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Mm][Ii][Nn][Ss][Ii][Zz][Ee][Rr][Ee][Ll])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/asmjit-build/MinSizeRel/asmjit.lib")
  elseif(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Rr][Ee][Ll][Ww][Ii][Tt][Hh][Dd][Ee][Bb][Ii][Nn][Ff][Oo])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/asmjit-build/RelWithDebInfo/asmjit.lib")
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/asmjit/asmjit-config.cmake")
    file(DIFFERENT _cmake_export_file_changed FILES
         "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/asmjit/asmjit-config.cmake"
         "I:/code/lobotomy-x/UEVR/build-hunt/_deps/asmjit-build/CMakeFiles/Export/51e8fad3bd3789d5315a1fbe99c5b64d/asmjit-config.cmake")
    if(_cmake_export_file_changed)
      file(GLOB _cmake_old_config_files "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/asmjit/asmjit-config-*.cmake")
      if(_cmake_old_config_files)
        string(REPLACE ";" ", " _cmake_old_config_files_text "${_cmake_old_config_files}")
        message(STATUS "Old export file \"$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/asmjit/asmjit-config.cmake\" will be replaced.  Removing files [${_cmake_old_config_files_text}].")
        unset(_cmake_old_config_files_text)
        file(REMOVE ${_cmake_old_config_files})
      endif()
      unset(_cmake_old_config_files)
    endif()
    unset(_cmake_export_file_changed)
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/asmjit" TYPE FILE FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/asmjit-build/CMakeFiles/Export/51e8fad3bd3789d5315a1fbe99c5b64d/asmjit-config.cmake")
  if(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Dd][Ee][Bb][Uu][Gg])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/asmjit" TYPE FILE FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/asmjit-build/CMakeFiles/Export/51e8fad3bd3789d5315a1fbe99c5b64d/asmjit-config-debug.cmake")
  endif()
  if(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Mm][Ii][Nn][Ss][Ii][Zz][Ee][Rr][Ee][Ll])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/asmjit" TYPE FILE FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/asmjit-build/CMakeFiles/Export/51e8fad3bd3789d5315a1fbe99c5b64d/asmjit-config-minsizerel.cmake")
  endif()
  if(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Rr][Ee][Ll][Ww][Ii][Tt][Hh][Dd][Ee][Bb][Ii][Nn][Ff][Oo])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/asmjit" TYPE FILE FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/asmjit-build/CMakeFiles/Export/51e8fad3bd3789d5315a1fbe99c5b64d/asmjit-config-relwithdebinfo.cmake")
  endif()
  if(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/asmjit" TYPE FILE FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/asmjit-build/CMakeFiles/Export/51e8fad3bd3789d5315a1fbe99c5b64d/asmjit-config-release.cmake")
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit" TYPE FILE FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/asmjit-src/src/asmjit/asmjit.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit" TYPE FILE FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/asmjit-src/src/asmjit/asmjit-scope-begin.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit" TYPE FILE FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/asmjit-src/src/asmjit/asmjit-scope-end.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit" TYPE FILE FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/asmjit-src/src/asmjit/core.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/asmjit-src/src/asmjit/core/api-config.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/asmjit-src/src/asmjit/core/archtraits.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/asmjit-src/src/asmjit/core/archcommons.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/asmjit-src/src/asmjit/core/assembler.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/asmjit-src/src/asmjit/core/builder.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/asmjit-src/src/asmjit/core/codebuffer.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/asmjit-src/src/asmjit/core/codeholder.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/asmjit-src/src/asmjit/core/compiler.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/asmjit-src/src/asmjit/core/compilerdefs.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/asmjit-src/src/asmjit/core/constpool.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/asmjit-src/src/asmjit/core/cpuinfo.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/asmjit-src/src/asmjit/core/emitter.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/asmjit-src/src/asmjit/core/environment.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/asmjit-src/src/asmjit/core/errorhandler.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/asmjit-src/src/asmjit/core/formatter.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/asmjit-src/src/asmjit/core/func.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/asmjit-src/src/asmjit/core/globals.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/asmjit-src/src/asmjit/core/inst.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/asmjit-src/src/asmjit/core/jitallocator.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/asmjit-src/src/asmjit/core/jitruntime.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/asmjit-src/src/asmjit/core/logger.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/asmjit-src/src/asmjit/core/operand.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/asmjit-src/src/asmjit/core/osutils.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/asmjit-src/src/asmjit/core/string.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/asmjit-src/src/asmjit/core/support.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/asmjit-src/src/asmjit/core/target.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/asmjit-src/src/asmjit/core/type.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/asmjit-src/src/asmjit/core/virtmem.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/asmjit-src/src/asmjit/core/zone.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/asmjit-src/src/asmjit/core/zonehash.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/asmjit-src/src/asmjit/core/zonelist.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/asmjit-src/src/asmjit/core/zonestack.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/asmjit-src/src/asmjit/core/zonestring.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/asmjit-src/src/asmjit/core/zonetree.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/asmjit-src/src/asmjit/core/zonevector.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit" TYPE FILE FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/asmjit-src/src/asmjit/x86.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/x86" TYPE FILE FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/asmjit-src/src/asmjit/x86/x86assembler.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/x86" TYPE FILE FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/asmjit-src/src/asmjit/x86/x86builder.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/x86" TYPE FILE FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/asmjit-src/src/asmjit/x86/x86compiler.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/x86" TYPE FILE FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/asmjit-src/src/asmjit/x86/x86emitter.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/x86" TYPE FILE FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/asmjit-src/src/asmjit/x86/x86globals.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/x86" TYPE FILE FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/asmjit-src/src/asmjit/x86/x86instdb.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/x86" TYPE FILE FILES "I:/code/lobotomy-x/UEVR/build-hunt/_deps/asmjit-src/src/asmjit/x86/x86operand.h")
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "I:/code/lobotomy-x/UEVR/build-hunt/_deps/asmjit-build/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
