#----------------------------------------------------------------
# Generated CMake target import file for configuration "MinSizeRel".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "Microsoft::DirectXTK12" for configuration "MinSizeRel"
set_property(TARGET Microsoft::DirectXTK12 APPEND PROPERTY IMPORTED_CONFIGURATIONS MINSIZEREL)
set_target_properties(Microsoft::DirectXTK12 PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_MINSIZEREL "CXX"
  IMPORTED_LOCATION_MINSIZEREL "${_IMPORT_PREFIX}/lib/DirectXTK12.lib"
  )

list(APPEND _cmake_import_check_targets Microsoft::DirectXTK12 )
list(APPEND _cmake_import_check_files_for_Microsoft::DirectXTK12 "${_IMPORT_PREFIX}/lib/DirectXTK12.lib" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
