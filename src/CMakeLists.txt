
function(dojio_add_library target_name)
    set(options)
    set(oneValueArgs)
    set(multiValueArgs SOURCES HEADERS PUBLIC_LIBS PRIVATE_LIBS)
    cmake_parse_arguments(LIB "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    # the library is always built as a static library so it can be linked
    # directly into high performance I/O and network applications
    add_library(${target_name} STATIC)
    target_sources(${target_name} PRIVATE ${LIB_SOURCES} ${LIB_HEADERS})

    # public headers live in include/dojio (installed); private headers (the
    # io_uring ring internals) stay in src and are only visible to the library
    target_include_directories(${target_name}
        PUBLIC
            $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
    )

    # tl::expected is header-only and its header is installed next to dojio's;
    # fmt is used in header-only mode inside the library, so the installed static
    # library is self-contained and neither dependency appears in the install interface
    target_link_libraries(${target_name} PUBLIC $<BUILD_INTERFACE:tl::expected> ${LIB_PUBLIC_LIBS} ${DOJIO_COMMON_LINK_FLAGS})
    target_link_libraries(${target_name} PRIVATE $<BUILD_INTERFACE:fmt::fmt> ${LIB_PRIVATE_LIBS})

    target_compile_options(${target_name} PRIVATE ${DOJIO_COMMON_FLAGS})
endfunction()

function(dojio_add_test target_name)
    set(options)
    set(oneValueArgs)
    set(multiValueArgs SOURCES LIBS DEFINITIONS)
    cmake_parse_arguments(TEST "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    add_executable(${target_name})
    target_sources(${target_name} PRIVATE ${TEST_SOURCES})
    target_link_libraries(${target_name} PUBLIC doctest ${TEST_LIBS} ${DOJIO_COMMON_LINK_FLAGS})
    target_compile_options(${target_name} PRIVATE ${DOJIO_COMMON_FLAGS})
    target_compile_definitions(${target_name} PRIVATE ${TEST_DEFINITIONS})
    add_test(NAME ${target_name}_exe COMMAND ${target_name})
endfunction()

function(dojio_add_bench target_name)
    set(options)
    set(oneValueArgs)
    set(multiValueArgs SOURCES LIBS)
    cmake_parse_arguments(BENCH "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    add_executable(${target_name})
    target_sources(${target_name} PRIVATE ${BENCH_SOURCES})
    target_link_libraries(${target_name} PRIVATE nanobench::nanobench ${BENCH_LIBS} ${DOJIO_COMMON_LINK_FLAGS})
    target_compile_options(${target_name} PRIVATE ${DOJIO_COMMON_FLAGS})
endfunction()

function(dojio_add_demo target_name)
    set(options)
    set(oneValueArgs)
    set(multiValueArgs SOURCES LIBS)
    cmake_parse_arguments(DEMO "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    add_executable(${target_name})
    target_sources(${target_name} PRIVATE ${DEMO_SOURCES})
    target_link_libraries(${target_name} PRIVATE ${DEMO_LIBS} Threads::Threads ${DOJIO_COMMON_LINK_FLAGS})
    target_compile_options(${target_name} PRIVATE ${DOJIO_COMMON_FLAGS})
endfunction()

# dojio static library (private header: ring.hpp holds the io_uring internals)
#
# io_uring is Linux-only: on every other POSIX system the RingEngine is replaced by a
# stub that reports NotSupported. The registered memory pool is built on every
# platform: it only performs the io_uring fixed-buffer registration on Linux with a
# kernel that exposes it (see memory_pool.cpp).
set(DOJIO_PLATFORM_SOURCES "dojio/memory_pool.cpp")
set(DOJIO_PLATFORM_HEADERS)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND DOJIO_PLATFORM_SOURCES "dojio/ring.cpp" "dojio/ring_engine.cpp")
    list(APPEND DOJIO_PLATFORM_HEADERS "dojio/ring.hpp")
else()
    list(APPEND DOJIO_PLATFORM_SOURCES "dojio/ring_engine_stub.cpp")
    list(APPEND DOJIO_PLATFORM_HEADERS "dojio/io_uring_stub.hpp")
endif()

dojio_add_library(dojio
    SOURCES "dojio/version.cpp" "dojio/contracts.cpp" "dojio/ops.cpp" "dojio/result.cpp" "dojio/socket.cpp"
            ${DOJIO_PLATFORM_SOURCES}
    HEADERS ${DOJIO_PLATFORM_HEADERS}
)

# unit tests (doctest) — public API only
dojio_add_test(dojio_unit_tests
    SOURCES "tests/entry_point_unit_test.cpp" "tests/test_contracts.cpp" "tests/test_result.cpp"
            "tests/test_box_ptr.cpp" "tests/test_fd_timer.cpp" "tests/test_memory_pool.cpp" "tests/test_operations.cpp"
            "tests/test_ring_engine.cpp" "tests/test_ops_file.cpp" "tests/test_ops_socket.cpp" "tests/test_ops_pipe.cpp"
            "tests/test_socket.cpp" "tests/test_timer.cpp"
    LIBS dojio fmt::fmt
)

# the syscall-backed ops (bind/listen, ftruncate, pipe) are dispatched to io_uring when
# the running kernel exposes their opcode and executed inline otherwise. On a recent
# kernel the inline path would never run, so this target builds its test translation
# unit with DOJIO_FORCE_LINUX_OLD_KERNEL, which fakes an old kernel and forces the
# inline path. io_uring is Linux-only, so the target is only meaningful there.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    dojio_add_test(dojio_old_linux_compat_tests
        SOURCES "tests/entry_point_unit_test.cpp" "tests/test_old_linux_compat.cpp"
        LIBS dojio fmt::fmt
        DEFINITIONS DOJIO_FORCE_LINUX_OLD_KERNEL
    )
endif()

# benches (nanobench)
dojio_add_bench(nanobench_ring_engine
    SOURCES "benches/nanobench_ring_engine.cpp"
    LIBS dojio
)
dojio_add_bench(nanobench_memory_pool
    SOURCES "benches/nanobench_memory_pool.cpp"
    LIBS dojio
)

# demos
dojio_add_demo(demo_udp_pingpong
    SOURCES "demos/udp_pingpong.cpp"
    LIBS dojio fmt::fmt
)
dojio_add_demo(demo_tcp_hello
    SOURCES "demos/tcp_hello.cpp"
    LIBS dojio fmt::fmt
)
dojio_add_demo(demo_file_rw
    SOURCES "demos/demo_file_rw.cpp"
    LIBS dojio fmt::fmt
)

# the demos are end-to-end programs: running them through CTest exercises the whole
# io_uring path. They require io_uring, so the tests are limited to Linux; a test
# passes when the demo exits with status 0.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_test(NAME demo_udp_pingpong_test COMMAND demo_udp_pingpong 200 64)
    add_test(NAME demo_tcp_hello_test COMMAND demo_tcp_hello 2 500 42123)
    add_test(NAME demo_file_rw_test COMMAND demo_file_rw "${CMAKE_CURRENT_BINARY_DIR}")

    # the demos use fixed ports and write large files: keep them from running
    # concurrently with each other or with a parallel `ctest -j`
    set_tests_properties(
        demo_udp_pingpong_test demo_tcp_hello_test demo_file_rw_test
        PROPERTIES
            LABELS "demo"
            RUN_SERIAL TRUE
            TIMEOUT 300
    )
endif()
