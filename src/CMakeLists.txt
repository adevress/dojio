
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
ringu_add_library(ringu
    SOURCES "ringu/ring.cpp" "ringu/ring_engine.cpp" "ringu/memory_pool.cpp" "ringu/contracts.cpp" "ringu/ops.cpp"
            "ringu/result.cpp" "ringu/socket.cpp"
    HEADERS "ringu/ring.hpp"
)

# unit tests (doctest) — public API only
ringu_add_test(ringu_unit_tests
    SOURCES "tests/entry_point_unit_test.cpp" "tests/test_contracts.cpp" "tests/test_result.cpp"
            "tests/test_fd_timer.cpp" "tests/test_memory_pool.cpp" "tests/test_operations.cpp"
            "tests/test_ring_engine.cpp" "tests/test_ops_file.cpp" "tests/test_ops_socket.cpp" "tests/test_socket.cpp"
            "tests/test_timer.cpp"
    LIBS ringu fmt::fmt
)

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
ringu_add_demo(demo_file_rw
    SOURCES "demos/demo_file_rw.cpp"
    LIBS ringu fmt::fmt
)
