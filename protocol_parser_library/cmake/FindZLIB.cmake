# Shadow for CMake's bundled FindZLIB.cmake, used only while configuring
# subdirectories fetched by this project (currently libxlsxwriter) that call
# find_package(ZLIB REQUIRED). Exposes the zlib already fetched via
# FetchContent (target `zlibstatic`) as ZLIB::ZLIB / ZLIB_* instead of
# searching for (and possibly duplicate-fetching or failing to find) a system
# zlib install.
if(NOT TARGET zlibstatic)
    message(FATAL_ERROR "FindZLIB shim: 'zlibstatic' not defined yet — fetch zlib "
                         "before any subdirectory that calls find_package(ZLIB)")
endif()
if(NOT TARGET ZLIB::ZLIB)
    add_library(ZLIB::ZLIB ALIAS zlibstatic)
endif()
set(ZLIB_FOUND TRUE)
set(ZLIB_LIBRARY zlibstatic)
set(ZLIB_LIBRARIES zlibstatic)
set(ZLIB_INCLUDE_DIR "${zlib_SOURCE_DIR}" "${zlib_BINARY_DIR}")
set(ZLIB_INCLUDE_DIRS "${zlib_SOURCE_DIR}" "${zlib_BINARY_DIR}")
set(ZLIB_VERSION_STRING "1.3.2")
