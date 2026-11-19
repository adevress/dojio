#include <doctest/doctest.h>

#include <chrono>

#include <dojio/fd.hpp>
#include <dojio/timer.hpp>

TEST_CASE("FileDescriptor is a typed, non-owning handle") {
  dojio::FileDescriptor empty;
  CHECK_FALSE(static_cast<bool>(empty));
  CHECK(empty.value() == -1);

  dojio::FileDescriptor fd{7};
  CHECK(static_cast<bool>(fd));
  CHECK(fd.value() == 7);
  CHECK(fd == dojio::FileDescriptor{7});
  CHECK(fd != empty);
}

TEST_CASE("Timer is a typed handler for a duration") {
  dojio::Timer t{std::chrono::milliseconds(50)};
  CHECK(t.duration() == std::chrono::nanoseconds{50000000});

  dojio::Timer t2{std::chrono::seconds{2}};
  CHECK(t2.duration() == std::chrono::nanoseconds{2000000000});
}
