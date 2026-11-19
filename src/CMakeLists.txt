
function(ringu_add_library target_name)
    set(options)
    set(oneValueArgs)
    set(multiValueArgs SOURCES HEADERS PUBLIC_LIBS PRIVATE_LIBS)
    cmake_parse_arguments(LIB "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    # the library is always built as a static library so it can be linked
    # directly into high performance I/O and network applications
    add_library(${target_name} STATIC)
    target_sources(${target_name} PRIVATE ${LIB_SOURCES} ${LIB_HEADERS})

    # public headers live in include/ringu (installed); private headers (the
    # io_uring ring internals) stay in src and are only visible to the library
    target_include_directories(${target_name}
        PUBLIC
            $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
    )

    # tl::expected is header-only and its header is installed next to ringu's;
    # fmt is used in header-only mode inside the library, so the installed static
    # library is self-contained and neither dependency appears in the install interface
    target_link_libraries(${target_name} PUBLIC $<BUILD_INTERFACE:tl::expected> ${LIB_PUBLIC_LIBS} ${RINGU_COMMON_LINK_FLAGS})
    target_link_libraries(${target_name} PRIVATE $<BUILD_INTERFACE:fmt::fmt> ${LIB_PRIVATE_LIBS})

    target_compile_options(${target_name} PRIVATE ${RINGU_COMMON_FLAGS})
endfunction()

function(ringu_add_test target_name)
    set(options)
    set(oneValueArgs)
    set(multiValueArgs SOURCES LIBS DEFINITIONS)
    cmake_parse_arguments(TEST "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    add_executable(${target_name})
    target_sources(${target_name} PRIVATE ${TEST_SOURCES})
    target_link_libraries(${target_name} PUBLIC doctest ${TEST_LIBS} ${RINGU_COMMON_LINK_FLAGS})
    target_compile_options(${target_name} PRIVATE ${RINGU_COMMON_FLAGS})
    target_compile_definitions(${target_name} PRIVATE ${TEST_DEFINITIONS})
    add_test(NAME ${target_name}_exe COMMAND ${target_name})
endfunction()

function(ringu_add_bench target_name)
    set(options)
    set(oneValueArgs)
    set(multiValueArgs SOURCES LIBS)
    cmake_parse_arguments(BENCH "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    add_executable(${target_name})
    target_sources(${target_name} PRIVATE ${BENCH_SOURCES})
    target_link_libraries(${target_name} PRIVATE nanobench::nanobench ${BENCH_LIBS} ${RINGU_COMMON_LINK_FLAGS})
    target_compile_options(${target_name} PRIVATE ${RINGU_COMMON_FLAGS})
endfunction()

function(ringu_add_demo target_name)
    set(options)
    set(oneValueArgs)
    set(multiValueArgs SOURCES LIBS)
    cmake_parse_arguments(DEMO "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    add_executable(${target_name})
    target_sources(${target_name} PRIVATE ${DEMO_SOURCES})
    target_link_libraries(${target_name} PRIVATE ${DEMO_LIBS} Threads::Threads ${RINGU_COMMON_LINK_FLAGS})
    target_compile_options(${target_name} PRIVATE ${RINGU_COMMON_FLAGS})
endfunction()

# ringu static library (private header: ring.hpp holds the io_uring internals)
#
# io_uring is Linux-only: on every other POSIX system the RingEngine and the
# registered memory pool are replaced by stubs that report NotSupported.
set(RINGU_PLATFORM_SOURCES)
set(RINGU_PLATFORM_HEADERS)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND RINGU_PLATFORM_SOURCES "ringu/ring.cpp" "ringu/ring_engine.cpp" "ringu/memory_pool.cpp")
    list(APPEND RINGU_PLATFORM_HEADERS "ringu/ring.hpp")
else()
    list(APPEND RINGU_PLATFORM_SOURCES "ringu/ring_engine_stub.cpp" "ringu/memory_pool_stub.cpp")
    list(APPEND RINGU_PLATFORM_HEADERS "ringu/io_uring_stub.hpp")
endif()

ringu_add_library(ringu
    SOURCES "ringu/version.cpp" "ringu/contracts.cpp" "ringu/ops.cpp" "ringu/result.cpp" "ringu/socket.cpp"
            ${RINGU_PLATFORM_SOURCES}
    HEADERS ${RINGU_PLATFORM_HEADERS}
)

# unit tests (doctest) — public API only
ringu_add_test(ringu_unit_tests
    SOURCES "tests/entry_point_unit_test.cpp" "tests/test_contracts.cpp" "tests/test_result.cpp"
            "tests/test_fd_timer.cpp" "tests/test_memory_pool.cpp" "tests/test_operations.cpp"
            "tests/test_ring_engine.cpp" "tests/test_ops_file.cpp" "tests/test_ops_socket.cpp" "tests/test_ops_pipe.cpp"
            "tests/test_socket.cpp" "tests/test_timer.cpp"
    LIBS ringu fmt::fmt
)

# the syscall-backed ops (bind/listen, ftruncate, pipe) are dispatched to io_uring when
# the running kernel exposes their opcode and executed inline otherwise. On a recent
# kernel the inline path would never run, so this target builds its test translation
# unit with RINGU_FORCE_LINUX_OLD_KERNEL, which fakes an old kernel and forces the
# inline path. io_uring is Linux-only, so the target is only meaningful there.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    ringu_add_test(ringu_old_linux_compat_tests
        SOURCES "tests/entry_point_unit_test.cpp" "tests/test_old_linux_compat.cpp"
        LIBS ringu fmt::fmt
        DEFINITIONS RINGU_FORCE_LINUX_OLD_KERNEL
    )
endif()

# benches (nanobench)
ringu_add_bench(nanobench_ring_engine
    SOURCES "benches/nanobench_ring_engine.cpp"
    LIBS ringu
)
ringu_add_bench(nanobench_memory_pool
    SOURCES "benches/nanobench_memory_pool.cpp"
    LIBS ringu
)

# demos
ringu_add_demo(demo_udp_pingpong
    SOURCES "demos/udp_pingpong.cpp"
    LIBS ringu fmt::fmt
)
ringu_add_demo(demo_tcp_hello
    SOURCES "demos/tcp_hello.cpp"
    LIBS ringu fmt::fmt
)
