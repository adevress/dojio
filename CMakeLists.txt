cmake_minimum_required(VERSION 3.22)
project(ringu VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
include(GNUInstallDirs)

option(RINGU_COVERAGE "Enable Ringu code coverage" OFF)
option(RINGU_SANITIZER "Enable the sanitizer for testing and debug" OFF)
option(RINGU_LTO_OPT "Enable LTO optimization" ON)
option(RINGU_STATIC_BUILD "Produce static binaries" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

if(NOT CMAKE_BUILD_TYPE)
set(CMAKE_BUILD_TYPE "RelWithDebInfo")
endif()

list(APPEND RINGU_COMMON_FLAGS
    "-Wall"
    "-Wextra"
    "-Wno-missing-field-initializers"
    "-Werror=conversion"
    "-Wformat-security"
    "-Wshadow"
    "-fno-omit-frame-pointer"
    "-fwrapv"
)

if(RINGU_STATIC_BUILD)
list(APPEND RINGU_COMMON_LINK_FLAGS
    "-static"
)
endif()

if(RINGU_LTO_OPT)
list(APPEND RINGU_COMMON_FLAGS
    "-flto"
)
endif()

if(RINGU_SANITIZER)
list(APPEND RINGU_COMMON_FLAGS
    "-fsanitize=address"
    "-fsanitize=undefined"
)
list(APPEND RINGU_COMMON_LINK_FLAGS
    "-fsanitize=address"
    "-fsanitize=undefined"
)
endif()

if(RINGU_COVERAGE)
list(APPEND RINGU_COMMON_FLAGS
    "-fprofile-arcs"
    "-ftest-coverage"
)
list(APPEND RINGU_COMMON_LINK_FLAGS
    "gcov"
)
endif()

find_package(Threads REQUIRED)

add_subdirectory(external)
add_subdirectory(src)

##
## install: public headers, static library, CMake package config and pkgconfig
##

# the tl::expected header is installed alongside the ringu headers (it is a
# header-only dependency); the build-tree target is not needed by consumers
install(TARGETS ringu EXPORT ringuTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT ringuTargets
    NAMESPACE ringu::
    FILE ringuTargets.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ringu
)

include(CMakePackageConfigHelpers)
configure_package_config_file(
    cmake/ringuConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/ringuConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ringu
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/ringuConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/ringuConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/ringuConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ringu
)

configure_file(ringu.pc.in ${CMAKE_CURRENT_BINARY_DIR}/ringu.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ringu.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
)
