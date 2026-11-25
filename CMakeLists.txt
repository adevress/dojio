cmake_minimum_required(VERSION 3.22)

# The version is declared once, in the pkg-config package file, and reused as the
# CMake project version and to generate the version header returned by
# dojio::dojio_version().
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/dojio.pc.in" DOJIO_PC_VERSION_LINE
    REGEX "^Version:[ \t]*[0-9]+\\.[0-9]+\\.[0-9]+")
if(NOT DOJIO_PC_VERSION_LINE)
message(FATAL_ERROR "dojio.pc.in must declare a x.y.z Version field")
endif()
string(REGEX REPLACE "^Version:[ \t]*([0-9]+\\.[0-9]+\\.[0-9]+)" "\\1"
    DOJIO_VERSION "${DOJIO_PC_VERSION_LINE}")

project(dojio VERSION ${DOJIO_VERSION} LANGUAGES CXX)

string(REPLACE "." ";" DOJIO_VERSION_COMPONENTS "${DOJIO_VERSION}")
list(GET DOJIO_VERSION_COMPONENTS 0 DOJIO_VERSION_MAJOR)
list(GET DOJIO_VERSION_COMPONENTS 1 DOJIO_VERSION_MINOR)
list(GET DOJIO_VERSION_COMPONENTS 2 DOJIO_VERSION_PATCH)

include(CTest)
include(GNUInstallDirs)

option(DOJIO_COVERAGE "Enable Dojio code coverage" OFF)
option(DOJIO_SANITIZER_ADDRESS "Enable AddressSanitizer for testing and debug" OFF)
option(DOJIO_SANITIZER_MEMORY "Enable MemorySanitizer for testing and debug (clang + instrumented libc++)" OFF)
option(DOJIO_SANITIZER_UNDEFINED "Enable UndefinedBehaviorSanitizer for testing and debug" OFF)
option(DOJIO_LTO_OPT "Enable LTO optimization" ON)
option(DOJIO_STATIC_BUILD "Produce static binaries" OFF)
option(DOJIO_SYS_DEBUGGING "Enable low-level syscall debugging logs (detail::dojio_debug)" OFF)

# the Address and Memory sanitizers install incompatible runtimes and cannot be
# linked into the same binary
if(DOJIO_SANITIZER_ADDRESS AND DOJIO_SANITIZER_MEMORY)
message(FATAL_ERROR
    "DOJIO_SANITIZER_ADDRESS and DOJIO_SANITIZER_MEMORY cannot be enabled together: "
    "the AddressSanitizer and MemorySanitizer runtimes are mutually exclusive")
endif()

# MemorySanitizer only exists in clang
if(DOJIO_SANITIZER_MEMORY AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
message(FATAL_ERROR "DOJIO_SANITIZER_MEMORY requires clang")
endif()

set(DOJIO_ANY_SANITIZER OFF)
if(DOJIO_SANITIZER_ADDRESS OR DOJIO_SANITIZER_MEMORY OR DOJIO_SANITIZER_UNDEFINED)
set(DOJIO_ANY_SANITIZER ON)
endif()

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

# Link-time optimization is disabled while a sanitizer is enabled: the two work
# together on recent toolchains, but mixing them makes the sanitizer reports much
# harder to read and is not worth the risk on a debug/testing build.
if(DOJIO_LTO_OPT AND DOJIO_ANY_SANITIZER)
message(STATUS "LTO disabled because a sanitizer is enabled")
elseif(DOJIO_LTO_OPT)
list(APPEND DOJIO_COMMON_FLAGS
    "-flto"
)
endif()

# every sanitizer build aborts on the first error instead of recovering, so the
# test suite fails as soon as something is detected
if(DOJIO_ANY_SANITIZER)
list(APPEND DOJIO_COMMON_FLAGS
    "-fno-sanitize-recover=all"
)
endif()

# AddressSanitizer catches address and memory errors (out-of-bounds accesses,
# use-after-free, double-free, ...) and, on Linux, comes with the LeakSanitizer
# enabled by default so leaks are reported at exit as well. use-after-scope turns
# on the stack lifetime tracking that is off by default for performance reasons.
if(DOJIO_SANITIZER_ADDRESS)
list(APPEND DOJIO_COMMON_FLAGS
    "-fsanitize=address"
    "-fsanitize-address-use-after-scope"
)
list(APPEND DOJIO_COMMON_LINK_FLAGS
    "-fsanitize=address"
)
endif()

# MemorySanitizer catches reads of uninitialized memory. It is clang-only and
# requires every translation unit, including the C++ standard library, to be
# instrumented; track-origins keeps the allocation site that produced the
# uninitialized value in the report.
if(DOJIO_SANITIZER_MEMORY)
list(APPEND DOJIO_COMMON_FLAGS
    "-fsanitize=memory"
    "-fsanitize-memory-track-origins=2"
)
list(APPEND DOJIO_COMMON_LINK_FLAGS
    "-fsanitize=memory"
)
endif()

# UndefinedBehaviorSanitizer catches undefined behavior. It is configured to
# abort on the first offence so the test suite fails instead of silently
# continuing past the offending operation.
if(DOJIO_SANITIZER_UNDEFINED)
list(APPEND DOJIO_COMMON_FLAGS
    "-fsanitize=undefined"
)
list(APPEND DOJIO_COMMON_LINK_FLAGS
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
## install: public headers, static library and pkgconfig
##

# the tl::expected header is installed alongside the dojio headers (it is a
# header-only dependency); the build-tree target is not needed by consumers
install(TARGETS dojio
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# pkg-config is the only supported package metadata (no CMake package config)
configure_file(dojio.pc.in ${CMAKE_CURRENT_BINARY_DIR}/dojio.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/dojio.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
)
