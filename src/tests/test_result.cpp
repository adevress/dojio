#include <doctest/doctest.h>

#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <variant>

#include <dojio/error.hpp>
#include <dojio/result.hpp>

TEST_CASE("Result carries a value on success") {
  dojio::Result<std::int32_t> r = 42;
  CHECK(r.has_value());
  CHECK(r.value() == 42);
  CHECK(*r == 42);
}

TEST_CASE("Result carries the mapped I/O code on failure") {
  dojio::Result<std::int32_t> r = tl::unexpected(dojio::IoError{-ENOENT});
  CHECK_FALSE(r.has_value());
  const dojio::ErrorCode& err = r.error();
  CHECK(std::holds_alternative<dojio::IoError>(err));
  CHECK(std::get<dojio::IoError>(err).value == -ENOENT);
  CHECK(std::get<dojio::IoError>(err).message() == std::string{"No such file or directory"});
}

TEST_CASE("Result carries the cancel code as a variant alternative") {
  dojio::Result<std::int32_t> r = tl::unexpected(dojio::Cancelled{});
  CHECK_FALSE(r.has_value());
  CHECK(std::holds_alternative<dojio::Cancelled>(r.error()));
}

TEST_CASE("Result can carry a registered item (buffer)") {
  std::vector<std::byte> buf(4);
  dojio::Result<std::vector<std::byte>> r = std::move(buf);
  REQUIRE(r.has_value());
  CHECK(r->size() == 4);
}

TEST_CASE("result_or_fail returns the value of a successful Result") {
  dojio::Result<std::int32_t> r = 42;
  CHECK(dojio::result_or_fail(std::move(r), "test: operation") == 42);
}

TEST_CASE("result_or_fail returns the value of a successful DojioError expected") {
  tl::expected<std::string, dojio::DojioError> r = std::string{"payload"};
  CHECK(dojio::result_or_fail(std::move(r), "test: creation") == "payload");
}

TEST_CASE("result_or_fail exits with EXIT_FAILURE on an error Result") {
  const std::int32_t pid = static_cast<std::int32_t>(::fork());
  REQUIRE(pid >= 0);
  if (pid == 0) {
    // child: report and stop. Silence the inherited buffered streams first so the
    // parent's output is not duplicated by the std::exit flush.
    ::freopen("/dev/null", "w", stdout);
    ::freopen("/dev/null", "w", stderr);
    dojio::result_or_fail(dojio::Result<std::int32_t>(tl::unexpected(dojio::IoError{-ENOENT})), "test: open");
    ::_exit(0); // not reached
  }
  std::int32_t status = 0;
  REQUIRE(::waitpid(static_cast<pid_t>(pid), &status, 0) == pid);
  CHECK(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == EXIT_FAILURE);
}
