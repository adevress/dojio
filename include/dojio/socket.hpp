#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include <dojio/buffer.hpp>
#include <dojio/fd.hpp>
#include <dojio/result.hpp>

namespace dojio {

/// Address family for create_socket. The C constants (AF_*) never appear in the
/// public API: they are mapped to these values inside the library.
enum class AddressFamily : std::int32_t {
  Unix = 0,
  Inet = 1,
  Inet6 = 2,
};

/// Socket type for create_socket (mapped to SOCK_STREAM / SOCK_DGRAM / ... in the library).
enum class SocketType : std::int32_t {
  Stream = 0,
  Dgram = 1,
  SeqPacket = 2,
  Raw = 3,
};

/// Socket protocol for create_socket (mapped to IPPROTO_* in the library).
enum class SocketProtocol : std::int32_t {
  Default = 0,
  Tcp = 1,
  Udp = 2,
};

/// Extra flags for create_socket (mapped to SOCK_NONBLOCK / SOCK_CLOEXEC in the library).
enum class SocketFlag : std::uint32_t {
  None = 0,
  NonBlock = 1,
  CloExec = 2,
};

constexpr SocketFlag operator|(SocketFlag a, SocketFlag b) {
  return static_cast<SocketFlag>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}

/// Creates a socket. Implemented in the library, where the dojio enums are mapped
/// onto the C API constants and socket(2) is called.
Result<FileDescriptor> create_socket(AddressFamily family, SocketType type,
                                     SocketProtocol protocol = SocketProtocol::Default,
                                     SocketFlag flags = SocketFlag::None);

/// Size of a sockaddr_in on Linux; used as the address storage of the Connect operation.
inline constexpr std::uint64_t k_sockaddr_in_size = 16;
/// Size of a sockaddr_in6 on Linux.
inline constexpr std::uint64_t k_sockaddr_in6_size = 28;

/// An IPv4 endpoint: address and port in host byte order.
struct Ipv4Endpoint {
  std::uint32_t address = 0;
  std::uint16_t port = 0;

  /// 127.0.0.1:port
  static constexpr Ipv4Endpoint loopback(std::uint16_t port) { return Ipv4Endpoint{0x7F000001u, port}; }

  /// Parses a dotted-quad IPv4 address (e.g. "127.0.0.1") with a port, both in host byte order.
  static Result<Ipv4Endpoint> parse(std::string_view ip, std::uint16_t port);
};

/// An IPv6 endpoint: the 16 address bytes (as produced by inet_pton, i.e. network
/// order) and the port in host byte order.
struct Ipv6Endpoint {
  std::array<std::uint8_t, 16> address{};
  std::uint16_t port = 0;

  /// [::1]:port
  static constexpr Ipv6Endpoint loopback(std::uint16_t port) {
    Ipv6Endpoint ep{};
    ep.address[15] = 1;
    ep.port = port;
    return ep;
  }

  /// Parses an IPv6 address (e.g. "::1") with a port in host byte order.
  static Result<Ipv6Endpoint> parse(std::string_view ip, std::uint16_t port);
};

} // namespace dojio
