# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "I:/code/lobotomy-x/UEVR/build-hunt/_deps/directxtk12-src")
  file(MAKE_DIRECTORY "I:/code/lobotomy-x/UEVR/build-hunt/_deps/directxtk12-src")
endif()
file(MAKE_DIRECTORY
  "I:/code/lobotomy-x/UEVR/build-hunt/_deps/directxtk12-build"
  "I:/code/lobotomy-x/UEVR/build-hunt/_deps/directxtk12-subbuild/directxtk12-populate-prefix"
  "I:/code/lobotomy-x/UEVR/build-hunt/_deps/directxtk12-subbuild/directxtk12-populate-prefix/tmp"
  "I:/code/lobotomy-x/UEVR/build-hunt/_deps/directxtk12-subbuild/directxtk12-populate-prefix/src/directxtk12-populate-stamp"
  "I:/code/lobotomy-x/UEVR/build-hunt/_deps/directxtk12-subbuild/directxtk12-populate-prefix/src"
  "I:/code/lobotomy-x/UEVR/build-hunt/_deps/directxtk12-subbuild/directxtk12-populate-prefix/src/directxtk12-populate-stamp"
)

set(configSubDirs Debug)
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "I:/code/lobotomy-x/UEVR/build-hunt/_deps/directxtk12-subbuild/directxtk12-populate-prefix/src/directxtk12-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "I:/code/lobotomy-x/UEVR/build-hunt/_deps/directxtk12-subbuild/directxtk12-populate-prefix/src/directxtk12-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
