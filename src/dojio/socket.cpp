#include <dojio/socket.hpp>

#include <dojio/contracts.hpp>
#include <dojio/operations.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cerrno>
#include <cstring>
#include <string>

// Library side of the socket helpers: this is where the C API constants (AF_*,
// SOCK_*, IPPROTO_*, sockaddr_in, inet_pton) are used, so they never leak into the
// public headers.

namespace dojio {

namespace {

std::int32_t map_family(AddressFamily family) {
  switch (family) {
  case AddressFamily::Unix:
    return AF_UNIX;
  case AddressFamily::Inet:
    return AF_INET;
  case AddressFamily::Inet6:
    return AF_INET6;
  default:
    unreachable();
  }
}

std::int32_t map_type(SocketType type) {
  switch (type) {
  case SocketType::Stream:
    return SOCK_STREAM;
  case SocketType::Dgram:
    return SOCK_DGRAM;
  case SocketType::SeqPacket:
    return SOCK_SEQPACKET;
  case SocketType::Raw:
    return SOCK_RAW;
  default:
    unreachable();
  }
}

std::int32_t map_protocol(SocketProtocol protocol) {
  switch (protocol) {
  case SocketProtocol::Default:
    return 0;
  case SocketProtocol::Tcp:
    return IPPROTO_TCP;
  case SocketProtocol::Udp:
    return IPPROTO_UDP;
  default:
    unreachable();
  }
}

std::int32_t map_flags(SocketFlag flags) {
  const std::uint32_t f = static_cast<std::uint32_t>(flags);
  std::int32_t out = 0;
  if ((f & static_cast<std::uint32_t>(SocketFlag::NonBlock)) != 0) {
    out |= SOCK_NONBLOCK;
  }
  if ((f & static_cast<std::uint32_t>(SocketFlag::CloExec)) != 0) {
    out |= SOCK_CLOEXEC;
  }
  return out;
}

} // namespace

Result<FileDescriptor> create_socket(AddressFamily family, SocketType type, SocketProtocol protocol, SocketFlag flags) {
  const std::int32_t fd = static_cast<std::int32_t>(
      ::socket(map_family(family), map_type(type) | map_flags(flags), map_protocol(protocol)));
  if (fd < 0) {
    return tl::unexpected(IoError{-errno});
  }
  return FileDescriptor{fd};
}

Result<Ipv4Endpoint> Ipv4Endpoint::parse(std::string_view ip, std::uint16_t port) {
  const std::string ip_str{ip};
  in_addr addr{};
  if (::inet_pton(AF_INET, ip_str.c_str(), &addr) != 1) {
    return tl::unexpected(IoError{-EINVAL});
  }
  return Ipv4Endpoint{static_cast<std::uint32_t>(ntohl(addr.s_addr)), port};
}

Result<Ipv6Endpoint> Ipv6Endpoint::parse(std::string_view ip, std::uint16_t port) {
  const std::string ip_str{ip};
  Ipv6Endpoint ep{};
  ep.port = port;
  if (::inet_pton(AF_INET6, ip_str.c_str(), ep.address.data()) != 1) {
    return tl::unexpected(IoError{-EINVAL});
  }
  return ep;
}

BindOp bind(FileDescriptor fd, Ipv4Endpoint endpoint) {
  BindOp op{};
  op.fd = fd;
  op.endpoint = endpoint;
  sockaddr_in sa{};
  sa.sin_family = AF_INET;
  sa.sin_addr.s_addr = htonl(endpoint.address);
  sa.sin_port = htons(endpoint.port);
  static_assert(sizeof(sa) <= k_sockaddr_storage_size);
  std::memcpy(op.addr.data(), &sa, sizeof(sa));
  op.addr_len = static_cast<std::uint32_t>(sizeof(sa));
  return op;
}

BindOp bind(FileDescriptor fd, Ipv6Endpoint endpoint) {
  BindOp op{};
  op.fd = fd;
  op.endpoint = endpoint;
  sockaddr_in6 sa{};
  sa.sin6_family = AF_INET6;
  std::memcpy(&sa.sin6_addr, endpoint.address.data(), sizeof(sa.sin6_addr));
  sa.sin6_port = htons(endpoint.port);
  static_assert(sizeof(sa) <= k_sockaddr_storage_size);
  std::memcpy(op.addr.data(), &sa, sizeof(sa));
  op.addr_len = static_cast<std::uint32_t>(sizeof(sa));
  return op;
}

ConnectOp connect(FileDescriptor fd, Ipv4Endpoint endpoint) {
  ConnectOp op{};
  op.fd = fd;
  op.endpoint = endpoint;
  sockaddr_in sa{};
  sa.sin_family = AF_INET;
  sa.sin_addr.s_addr = htonl(endpoint.address);
  sa.sin_port = htons(endpoint.port);
  static_assert(sizeof(sa) <= k_sockaddr_in_size);
  std::memcpy(op.addr.data(), &sa, sizeof(sa));
  return op;
}

} // namespace dojio
