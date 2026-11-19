#include <doctest/doctest.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <fmt/format.h>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <dojio/result.hpp>
#include <dojio/ring_engine.hpp>

namespace {

std::string make_tmp_path(const std::string& tag) {
  static std::int32_t counter = 0;
  return (std::filesystem::temp_directory_path() / fmt::format("dojio_test_{}_{}_{}", tag, ::getpid(), counter++))
      .string();
}

std::string make_tmp_file(const std::string& tag, std::string_view content) {
  const std::string path = make_tmp_path(tag);
  const std::int32_t fd = static_cast<std::int32_t>(::open(path.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644));
  REQUIRE(fd >= 0);
  REQUIRE(::write(fd, content.data(), static_cast<std::size_t>(content.size())) ==
          static_cast<::ssize_t>(content.size()));
  ::close(fd);
  return path;
}

std::vector<std::byte> to_bytes(std::string_view s) {
  return {reinterpret_cast<const std::byte*>(s.data()), reinterpret_cast<const std::byte*>(s.data()) + s.size()};
}

std::string to_string(const std::vector<std::byte>& b) { return {reinterpret_cast<const char*>(b.data()), b.size()}; }

} // namespace

TEST_CASE("open, write, pwrite, pread, read, fstat, close on a file") {
  for (const bool sqpoll : {true, false}) {
    CAPTURE(sqpoll);
    auto ring_engine_result = dojio::RingEngine::create(dojio::RingEngineOptions{.entries = 64, .sqpoll = sqpoll});
    REQUIRE(ring_engine_result.has_value());
    auto engine = std::move(ring_engine_result).value();
    CHECK(engine->sqpoll_enabled() == sqpoll);
    using Mode = dojio::RingEngine::ProcessMode;
    using State = dojio::RingEngine::OperationState;
    // "hello world dojio!" = 18 bytes
    const std::string path = make_tmp_file("file_ops", "hello world dojio!");

    // open
    dojio::FileDescriptor fd{-1};
    engine->submit(dojio::open_at(path, O_RDWR),
                   [&](dojio::Result<dojio::FileDescriptor> r, dojio::OperationId, std::int32_t res) {
                     REQUIRE(r.has_value());
                     CHECK(res >= 0);
                     fd = *r;
                   });
    engine->process(Mode::Batch);
    REQUIRE(fd.value() >= 0);

    // write at the current position (0): overwrite the first 4 bytes with "ABCD"
    std::int32_t written = -1;
    engine->submit(dojio::write(fd), to_bytes("ABCD"),
                   [&](dojio::Result<std::vector<std::byte>> r, dojio::OperationId, std::int32_t res) {
                     REQUIRE(r.has_value());
                     written = res;
                   });
    engine->process(Mode::Batch);
    CHECK(written == 4);

    // pwrite at offset 8: "WXYZ" (does not move the current position)
    std::int32_t pwritten = -1;
    engine->submit(dojio::pwrite(fd, 8), to_bytes("WXYZ"),
                   [&](dojio::Result<std::vector<std::byte>> r, dojio::OperationId, std::int32_t res) {
                     REQUIRE(r.has_value());
                     pwritten = res;
                   });
    engine->process(Mode::Batch);
    CHECK(pwritten == 4);
    // file is now "ABCDo woWXYZdojio!"

    // pread 8 bytes at offset 0
    std::string pread_out;
    engine->submit(dojio::pread(fd, 0), std::vector<std::byte>(8),
                   [&](dojio::Result<std::vector<std::byte>> r, dojio::OperationId, std::int32_t res) {
                     REQUIRE(r.has_value());
                     CHECK(res == 8);
                     CHECK(r->size() == 8); // buffer shrunk to the bytes read
                     pread_out = to_string(*r);
                   });
    engine->process(Mode::Batch);
    CHECK(pread_out == "ABCDo wo");

    // read at the current position (4 after the write): 6 bytes
    std::string read_out;
    engine->submit(dojio::read(fd), std::vector<std::byte>(6),
                   [&](dojio::Result<std::vector<std::byte>> r, dojio::OperationId, std::int32_t res) {
                     REQUIRE(r.has_value());
                     CHECK(res == 6);
                     read_out = to_string(*r);
                   });
    engine->process(Mode::Batch);
    CHECK(read_out == "o woWX");

    // fstat: the buffer (>= sizeof(statx)) receives a struct stat on success
    engine->submit(dojio::fstat(fd), std::vector<std::byte>(dojio::k_statx_size),
                   [&](dojio::Result<std::vector<std::byte>> r, dojio::OperationId, std::int32_t res) {
                     REQUIRE(r.has_value());
                     CHECK(res == 0);
                     const auto* st = reinterpret_cast<const struct stat*>(r->data());
                     CHECK(st->st_size == 18);
                     CHECK(S_ISREG(st->st_mode));
                   });
    engine->process(Mode::Batch);

    // close
    bool closed = false;
    engine->submit(dojio::close(fd), [&](dojio::Result<std::int32_t> r, dojio::OperationId, std::int32_t res) {
      REQUIRE(r.has_value());
      CHECK(res == 0);
      closed = true;
    });
    engine->process(Mode::Batch);
    CHECK(closed);

    // open, write, pwrite, pread, read, fstat, close
    CHECK(engine->count_operations(State::Completed) == 7);
    CHECK(engine->count_operations(State::Failed) == 0);
    CHECK(engine->count_operations(State::Pending) == 0);
    ::unlink(path.c_str());
  }
}

TEST_CASE("pread into a std::array buffer") {
  for (const bool sqpoll : {true, false}) {
    CAPTURE(sqpoll);
    auto ring_engine_result = dojio::RingEngine::create(dojio::RingEngineOptions{.entries = 64, .sqpoll = sqpoll});
    REQUIRE(ring_engine_result.has_value());
    auto engine = std::move(ring_engine_result).value();
    CHECK(engine->sqpoll_enabled() == sqpoll);
    using Mode = dojio::RingEngine::ProcessMode;
    const std::string path = make_tmp_file("array_buf", "0123456789");
    dojio::FileDescriptor fd{-1};
    engine->submit(dojio::open_at(path, O_RDONLY, 0),
                   [&](dojio::Result<dojio::FileDescriptor> r, dojio::OperationId, std::int32_t) {
                     REQUIRE(r.has_value());
                     fd = *r;
                   });
    engine->process(Mode::Batch);
    REQUIRE(fd.value() >= 0);

    std::array<std::byte, 4> buf{};
    engine->submit(dojio::pread(fd, 6), std::move(buf),
                   [&](dojio::Result<std::array<std::byte, 4>> r, dojio::OperationId, std::int32_t res) {
                     REQUIRE(r.has_value());
                     CHECK(res == 4);
                     const auto& b = *r;
                     CHECK(std::string{reinterpret_cast<const char*>(b.data()), b.size()} == "6789");
                   });
    engine->process(Mode::Batch);
    engine->submit(dojio::close(fd), [](dojio::Result<std::int32_t>, dojio::OperationId, std::int32_t) {});
    engine->process(Mode::Batch);
    ::unlink(path.c_str());
  }
}

TEST_CASE("open of a missing file fails with the mapped I/O code") {
  for (const bool sqpoll : {true, false}) {
    CAPTURE(sqpoll);
    auto ring_engine_result = dojio::RingEngine::create(dojio::RingEngineOptions{.entries = 64, .sqpoll = sqpoll});
    REQUIRE(ring_engine_result.has_value());
    auto engine = std::move(ring_engine_result).value();
    CHECK(engine->sqpoll_enabled() == sqpoll);
    using State = dojio::RingEngine::OperationState;
    bool called = false;
    engine->submit(dojio::open_at("/tmp/dojio_definitely_missing_file_xyz", O_RDONLY, 0),
                   [&](dojio::Result<dojio::FileDescriptor> r, dojio::OperationId, std::int32_t) {
                     called = true;
                     CHECK_FALSE(r.has_value());
                     REQUIRE(std::holds_alternative<dojio::IoError>(r.error()));
                     CHECK(std::get<dojio::IoError>(r.error()).value == -ENOENT);
                   });
    engine->process(dojio::RingEngine::ProcessMode::Batch);
    CHECK(called);
    CHECK(engine->count_operations(State::Failed) == 1);
    CHECK(engine->count_operations(State::Completed) == 0);
  }
}

TEST_CASE("read on a closed fd fails with EBADF") {
  for (const bool sqpoll : {true, false}) {
    CAPTURE(sqpoll);
    auto ring_engine_result = dojio::RingEngine::create(dojio::RingEngineOptions{.entries = 64, .sqpoll = sqpoll});
    REQUIRE(ring_engine_result.has_value());
    auto engine = std::move(ring_engine_result).value();
    CHECK(engine->sqpoll_enabled() == sqpoll);
    using Mode = dojio::RingEngine::ProcessMode;
    using State = dojio::RingEngine::OperationState;
    const std::string path = make_tmp_file("ebadf", "data");
    dojio::FileDescriptor fd{-1};
    engine->submit(dojio::open_at(path, O_RDONLY, 0),
                   [&](dojio::Result<dojio::FileDescriptor> r, dojio::OperationId, std::int32_t) {
                     REQUIRE(r.has_value());
                     fd = *r;
                   });
    engine->process(Mode::Batch);
    engine->submit(dojio::close(fd), [](dojio::Result<std::int32_t>, dojio::OperationId, std::int32_t) {});
    engine->process(Mode::Batch);

    bool called = false;
    engine->submit(dojio::pread(fd, 0), std::vector<std::byte>(4),
                   [&](dojio::Result<std::vector<std::byte>> r, dojio::OperationId, std::int32_t) {
                     called = true;
                     CHECK_FALSE(r.has_value());
                     REQUIRE(std::holds_alternative<dojio::IoError>(r.error()));
                     CHECK(std::get<dojio::IoError>(r.error()).value == -EBADF);
                   });
    engine->process(Mode::Batch);
    CHECK(called);
    CHECK(engine->count_operations(State::Failed) == 1);
    ::unlink(path.c_str());
  }
}

TEST_CASE("ftruncate shrinks and grows a file") {
  for (const bool sqpoll : {true, false}) {
    CAPTURE(sqpoll);
    auto ring_engine_result = dojio::RingEngine::create(dojio::RingEngineOptions{.entries = 64, .sqpoll = sqpoll});
    REQUIRE(ring_engine_result.has_value());
    auto engine = std::move(ring_engine_result).value();
    CHECK(engine->sqpoll_enabled() == sqpoll);
    using Mode = dojio::RingEngine::ProcessMode;
    using State = dojio::RingEngine::OperationState;
    const std::string path = make_tmp_file("ftruncate", "0123456789"); // 10 bytes

    dojio::FileDescriptor fd{-1};
    engine->submit(dojio::open_at(path, O_RDWR),
                   [&](dojio::Result<dojio::FileDescriptor> r, dojio::OperationId, std::int32_t) {
                     REQUIRE(r.has_value());
                     fd = *r;
                   });
    engine->process(Mode::Batch);
    REQUIRE(fd.value() >= 0);

    // shrink the file to 4 bytes
    bool truncated = false;
    engine->submit(dojio::ftruncate(fd, 4), [&](dojio::Result<std::int32_t> r, dojio::OperationId, std::int32_t res) {
      REQUIRE(r.has_value());
      CHECK(res == 0);
      truncated = true;
    });
    engine->process(Mode::Batch);
    CHECK(truncated);

    // the file is now exactly 4 bytes long
    engine->submit(dojio::fstat(fd), std::vector<std::byte>(dojio::k_statx_size),
                   [&](dojio::Result<std::vector<std::byte>> r, dojio::OperationId, std::int32_t res) {
                     REQUIRE(r.has_value());
                     CHECK(res == 0);
                     const auto* st = reinterpret_cast<const struct stat*>(r->data());
                     CHECK(st->st_size == 4);
                   });
    engine->process(Mode::Batch);

    // reading past the end returns the 4 remaining bytes
    std::string shrunk;
    engine->submit(dojio::pread(fd, 0), std::vector<std::byte>(8),
                   [&](dojio::Result<std::vector<std::byte>> r, dojio::OperationId, std::int32_t res) {
                     REQUIRE(r.has_value());
                     CHECK(res == 4);
                     shrunk = to_string(*r);
                   });
    engine->process(Mode::Batch);
    CHECK(shrunk == "0123");

    // grow the file back to 12 bytes: the new bytes read as zeroes
    engine->submit(dojio::ftruncate(fd, 12), [&](dojio::Result<std::int32_t> r, dojio::OperationId, std::int32_t res) {
      REQUIRE(r.has_value());
      CHECK(res == 0);
    });
    engine->process(Mode::Batch);
    engine->submit(dojio::fstat(fd), std::vector<std::byte>(dojio::k_statx_size),
                   [&](dojio::Result<std::vector<std::byte>> r, dojio::OperationId, std::int32_t res) {
                     REQUIRE(r.has_value());
                     CHECK(res == 0);
                     const auto* st = reinterpret_cast<const struct stat*>(r->data());
                     CHECK(st->st_size == 12);
                   });
    engine->process(Mode::Batch);
    std::string grown;
    engine->submit(dojio::pread(fd, 4), std::vector<std::byte>(8),
                   [&](dojio::Result<std::vector<std::byte>> r, dojio::OperationId, std::int32_t res) {
                     REQUIRE(r.has_value());
                     CHECK(res == 8);
                     grown = to_string(*r);
                   });
    engine->process(Mode::Batch);
    CHECK(grown == std::string(8, '\0'));

    engine->submit(dojio::close(fd), [](dojio::Result<std::int32_t>, dojio::OperationId, std::int32_t) {});
    engine->process(Mode::Batch);
    CHECK(engine->count_operations(State::Failed) == 0);
    ::unlink(path.c_str());
  }
}
