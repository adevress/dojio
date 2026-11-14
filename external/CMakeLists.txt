
include(FetchContent)

##
##  nanobench
##
FetchContent_Declare(
    nanobench
    GIT_REPOSITORY https://github.com/martinus/nanobench.git
    GIT_TAG v4.3.11
    GIT_SHALLOW TRUE
)

##
##  doctest
##
set(DOCTEST_NO_INSTALL ON CACHE BOOL "do not install doctest with ringu" FORCE)
FetchContent_Declare(
  doctest
  GIT_REPOSITORY https://github.com/onqtam/doctest.git
  GIT_TAG master
)

##
##  tl expected
##
set(EXPECTED_BUILD_TESTS FALSE CACHE BOOL "disable unit tests for expected" FORCE)
set(EXPECTED_BUILD_PACKAGE OFF CACHE BOOL "disable install rules of expected" FORCE)
FetchContent_Declare(
  tl_expected
  GIT_REPOSITORY https://github.com/TartanLlama/expected
  GIT_TAG v1.1.0
)

##
##  fmt (libfmt) — used for all formatted output and string building
##
set(FMT_INSTALL OFF CACHE BOOL "do not install fmt with ringu" FORCE)
set(FMT_TEST OFF CACHE BOOL "disable fmt tests" FORCE)
FetchContent_Declare(
  fmt
  GIT_REPOSITORY https://github.com/fmtlib/fmt.git
  GIT_TAG 12.1.0
)

# doctest must not install itself into the ringu package; tl_expected installs its
# single header (tl/expected.hpp) into include/, which the public ringu headers need
FetchContent_MakeAvailable(doctest nanobench tl_expected fmt)

# ringu uses fmt in header-only mode: the installed static library is then
# self-contained and consumers do not need libfmt
target_compile_definitions(fmt INTERFACE FMT_HEADER_ONLY)
