#include <dojio/action.hpp>

#include <array>
#include <charconv>
#include <string_view>

#if defined(__linux__)
#include <sys/utsname.h>
#endif

// Kernel version detection backing the operation dispatcher: the RingEngine pushes
// a syscall-backed operation (bind/listen/ftruncate/pipe) to io_uring only when the
// running kernel exposes the matching opcode.

namespace dojio::detail {

namespace {

/// Parses a kernel release string ("6.11.0-27-generic", "7.0.0-1024-nvidia") into an
/// OsVersion. Missing fields default to 0. std::string_view materializes the release
/// C string and std::from_chars reads the numeric fields.
OsVersion parse_os_release(std::string_view release) {
  OsVersion version{};
  std::array<std::int32_t*, 3> fields{&version.major, &version.minor, &version.patch};
  std::size_t field = 0;
  while (field < 3 && !release.empty()) {
    const std::size_t digit_count = release.find_first_not_of("0123456789");
    const std::string_view digits = release.substr(0, digit_count);
    if (digits.empty()) {
      break;
    }
    std::int32_t value = 0;
    if (std::from_chars(digits.data(), digits.data() + digits.size(), value).ec != std::errc{}) {
      break;
    }
    *fields[field] = value;
    field += 1;
    if (digit_count == std::string_view::npos) {
      break;
    }
    release.remove_prefix(digit_count + 1); // skip the '.'
  }
  return version;
}

/// Reads uname(2).release and parses it. Returns 0.0.0 when uname fails or when the
/// platform is not Linux (the release string is a Darwin/macOS version there).
OsVersion read_os_version() {
#if defined(__linux__)
  utsname info{};
  if (::uname(&info) != 0) {
    return OsVersion{};
  }
  return parse_os_release(std::string_view{info.release});
#else
  return OsVersion{};
#endif
}

} // namespace

const OsVersion& extract_os_version() {
  // function-local static: initialized once, thread-safe since C++11
  // cppcheck-suppress misra-cpp-2023-6.7.1 ; MISRA C++ deviation DEV-008: immutable, thread-safe OS-version cache
  static const OsVersion version = read_os_version();
  return version;
}

bool kernel_supports_io_uring() {
  const OsVersion& version = extract_os_version();
  // io_uring appeared in Linux 5.1
  return version.major > 5 || (version.major == 5 && version.minor >= 1);
}

bool kernel_supports_bind_listen() {
  const OsVersion& version = extract_os_version();
  return version.major > 6 || (version.major == 6 && version.minor >= 11);
}

bool kernel_supports_ftruncate() {
  const OsVersion& version = extract_os_version();
  return version.major > 6 || (version.major == 6 && version.minor >= 9);
}

bool kernel_supports_pipe() {
  const OsVersion& version = extract_os_version();
  return version.major > 6 || (version.major == 6 && version.minor >= 16);
}

} // namespace dojio::detail
