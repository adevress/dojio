cmake_minimum_required(VERSION 3.22)
project(dojio VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
include(GNUInstallDirs)

option(DOJIO_COVERAGE "Enable Dojio code coverage" OFF)
option(DOJIO_SANITIZER "Enable the sanitizer for testing and debug" OFF)
option(DOJIO_LTO_OPT "Enable LTO optimization" ON)
option(DOJIO_STATIC_BUILD "Produce static binaries" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

if(NOT CMAKE_BUILD_TYPE)
set(CMAKE_BUILD_TYPE "RelWithDebInfo")
endif()

list(APPEND DOJIO_COMMON_FLAGS
    "-Wall"
    "-Wextra"
    "-Wno-missing-field-initializers"
    "-Werror=conversion"
    "-Wformat-security"
    "-Wshadow"
    "-fno-omit-frame-pointer"
    "-fwrapv"
)

if(DOJIO_STATIC_BUILD)
list(APPEND DOJIO_COMMON_LINK_FLAGS
    "-static"
)
endif()

if(DOJIO_LTO_OPT)
list(APPEND DOJIO_COMMON_FLAGS
    "-flto"
)
endif()

if(DOJIO_SANITIZER)
list(APPEND DOJIO_COMMON_FLAGS
    "-fsanitize=address"
    "-fsanitize=undefined"
)
list(APPEND DOJIO_COMMON_LINK_FLAGS
    "-fsanitize=address"
    "-fsanitize=undefined"
)
endif()

if(DOJIO_COVERAGE)
list(APPEND DOJIO_COMMON_FLAGS
    "-fprofile-arcs"
    "-ftest-coverage"
)
list(APPEND DOJIO_COMMON_LINK_FLAGS
    "gcov"
)
endif()

find_package(Threads REQUIRED)

add_subdirectory(external)
add_subdirectory(src)

##
## install: public headers, static library, CMake package config and pkgconfig
##

# the tl::expected header is installed alongside the dojio headers (it is a
# header-only dependency); the build-tree target is not needed by consumers
install(TARGETS dojio EXPORT dojioTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT dojioTargets
    NAMESPACE dojio::
    FILE dojioTargets.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dojio
)

include(CMakePackageConfigHelpers)
configure_package_config_file(
    cmake/dojioConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/dojioConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dojio
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/dojioConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/dojioConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/dojioConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dojio
)

configure_file(dojio.pc.in ${CMAKE_CURRENT_BINARY_DIR}/dojio.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/dojio.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
)
