#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

#include <ringu/buffer.hpp>
#include <ringu/fd.hpp>
#include <ringu/socket.hpp>

namespace ringu {

/// Size of the kernel struct statx (fixed at 256 bytes by the uapi). An Fstat
/// buffer must hold at least this many bytes; on success its first
/// sizeof(struct stat) bytes are rewritten as a struct stat.
inline constexpr std::uint64_t k_statx_size = 256;

/// Opens path with flags/mode (open(2) semantics). Completes with the new FileDescriptor.
struct Open {
  std::string path;
  std::int32_t flags = 0;
  std::uint32_t mode = 0;
  /// Constructs an Open operation for path with the given open(2) flags and mode.
  static Open make(std::string path, std::int32_t flags, std::uint32_t mode = static_cast<std::uint32_t>(0644)) {
    return Open{std::move(path), flags, mode};
  }
};

/// Reads from fd at the current file position (read(2) semantics). The buffer
/// receives the data; on success it is shrunk to the number of bytes read.
struct Read {
  FileDescriptor fd;
  /// Constructs a Read operation (read at the current file position).
  static Read make(FileDescriptor fd) { return Read{fd}; }
};

/// Reads from fd at an explicit offset (pread(2) semantics). The buffer receives
/// the data; on success it is shrunk to the number of bytes read.
struct Pread {
  FileDescriptor fd;
  std::uint64_t offset = 0;
  /// Constructs a Pread operation (read at an explicit offset).
  static Pread make(FileDescriptor fd, std::uint64_t offset) { return Pread{fd, offset}; }
};

/// Writes the buffer to fd at the current file position (write(2) semantics).
struct Write {
  FileDescriptor fd;
  /// Constructs a Write operation (write at the current file position).
  static Write make(FileDescriptor fd) { return Write{fd}; }
};

/// Writes the buffer to fd at an explicit offset (pwrite(2) semantics).
struct Pwrite {
  FileDescriptor fd;
  std::uint64_t offset = 0;
  /// Constructs a Pwrite operation (write at an explicit offset).
  static Pwrite make(FileDescriptor fd, std::uint64_t offset) { return Pwrite{fd, offset}; }
};

/// Seeks in fd (lseek(2) semantics). Completes with the new offset.
///
/// No io_uring opcode exists for lseek: the Reactor executes it synchronously
/// when the current batch is processed.
struct Lseek {
  FileDescriptor fd;
  std::int64_t offset = 0;
  std::int32_t whence = 0;
  /// Constructs an Lseek operation (lseek(2) semantics).
  static Lseek make(FileDescriptor fd, std::int64_t offset, std::int32_t whence) { return Lseek{fd, offset, whence}; }
};

/// Stats fd (fstat(2) semantics) through IORING_OP_STATX with AT_EMPTY_PATH.
///
/// The submitted buffer must hold at least k_statx_size bytes; on success its
/// first sizeof(struct stat) bytes are rewritten as a struct stat.
struct Fstat {
  FileDescriptor fd;
  /// Constructs an Fstat operation (fstat(2) semantics).
  static Fstat make(FileDescriptor fd) { return Fstat{fd}; }
};

/// Closes fd (close(2) semantics).
struct Close {
  FileDescriptor fd;
  /// Constructs a Close operation.
  static Close make(FileDescriptor fd) { return Close{fd}; }
};

/// Marks a socket as passive (listen(2) semantics).
///
/// No io_uring opcode exists for listen: the Reactor executes it synchronously
/// when the current batch is processed.
struct Listen {
  FileDescriptor fd;
  std::int32_t backlog = 0;
  /// Constructs a Listen operation (listen(2) semantics).
  static Listen make(FileDescriptor fd, std::int32_t backlog) { return Listen{fd, backlog}; }
};

/// Accepts a connection on a listening socket (accept(2) semantics). Completes
/// with the accepted FileDescriptor. When a buffer is submitted it receives the
/// peer address (a sockaddr).
struct Accept {
  FileDescriptor fd;
  std::int32_t flags = 0;
  std::uint32_t addr_len = 0; // filled by the Reactor from the buffer size
  /// Constructs an Accept operation. Submit it with an AcceptBuffer to receive the
  /// peer address (a sockaddr_storage), e.g.:
  ///   reactor.submit(Accept::make(lfd), AcceptBuffer{}, callback);
  static Accept make(FileDescriptor fd, std::int32_t flags = 0) { return Accept{fd, flags, 0}; }
};

/// Sends the buffer on a connected socket (send(2) semantics).
struct Send {
  FileDescriptor fd;
  std::int32_t flags = 0;
  /// Constructs a Send operation (send(2) semantics on a connected socket).
  static Send make(FileDescriptor fd, std::int32_t flags = 0) { return Send{fd, flags}; }
};

/// Receives into the buffer from a connected socket (recv(2) semantics). On
/// success the buffer is shrunk to the number of bytes received.
struct Recv {
  FileDescriptor fd;
  std::int32_t flags = 0;
  /// Constructs a Recv operation (recv(2) semantics on a connected socket).
  static Recv make(FileDescriptor fd, std::int32_t flags = 0) { return Recv{fd, flags}; }
};

/// Plain mirror of __kernel_timespec (identical layout: two 64-bit fields).
struct Timespec {
  std::int64_t sec = 0;
  std::int64_t nsec = 0;
};

/// Opens path relative to dirfd (openat(2) semantics). Completes with the new FileDescriptor.
struct OpenAt {
  FileDescriptor dirfd;
  std::string path;
  std::int32_t flags = 0;
  std::uint32_t mode = 0;

  /// Constructs an OpenAt operation.
  static OpenAt make(FileDescriptor dirfd, std::string path, std::int32_t flags,
                     std::uint32_t mode = static_cast<std::uint32_t>(0644)) {
    return OpenAt{dirfd, std::move(path), flags, mode};
  }
};

/// fsync(2) on fd; datasync selects fdatasync(2) semantics. Completes with 0.
struct Fsync {
  FileDescriptor fd;
  bool use_datasync = false;

  /// Constructs an Fsync operation; pass use_datasync = true for fdatasync(2) semantics.
  static Fsync make(FileDescriptor fd, bool use_datasync = false) { return Fsync{fd, use_datasync}; }
};

/// connect(2) on fd to an IPv4 endpoint. Completes with 0 on success.
///
/// The sockaddr_in is stored inside the operation (static buffer), so no C socket
/// type appears in the public API.
struct Connect {
  FileDescriptor fd;
  Ipv4Endpoint endpoint;
  StaticBuffer<k_sockaddr_in_size> addr{}; // sockaddr_in storage, filled by the factory

  /// Constructs a Connect operation (implemented in the library, where the sockaddr_in
  /// is built from the endpoint).
  static Connect make(FileDescriptor fd, Ipv4Endpoint endpoint);
};

/// Backing operation of Timer: completes when the timeout expires.
struct Timeout {
  Timespec ts{};
  /// Constructs a Timeout operation firing after the given duration.
  static Timeout make(std::chrono::nanoseconds duration) {
    Timeout op{};
    const std::int64_t ns = duration.count();
    op.ts.sec = ns / 1000000000LL;
    op.ts.nsec = ns % 1000000000LL;
    return op;
  }
};

/// Variant of every operation supported by the Reactor. Each operation is its own
/// struct held by this variant.
using Operation = std::variant<Open, OpenAt, Read, Pread, Write, Pwrite, Lseek, Fstat, Close, Listen, Accept, Send,
                               Recv, Timeout, Fsync, Connect>;

/// True when Op is one of the operation structs of the Operation variant.
template <typename Op>
inline constexpr bool is_operation_v =
    std::is_same_v<std::decay_t<Op>, Open> || std::is_same_v<std::decay_t<Op>, OpenAt> ||
    std::is_same_v<std::decay_t<Op>, Fsync> || std::is_same_v<std::decay_t<Op>, Connect> ||
    std::is_same_v<std::decay_t<Op>, Read> || std::is_same_v<std::decay_t<Op>, Pread> ||
    std::is_same_v<std::decay_t<Op>, Write> || std::is_same_v<std::decay_t<Op>, Pwrite> ||
    std::is_same_v<std::decay_t<Op>, Lseek> || std::is_same_v<std::decay_t<Op>, Fstat> ||
    std::is_same_v<std::decay_t<Op>, Close> || std::is_same_v<std::decay_t<Op>, Listen> ||
    std::is_same_v<std::decay_t<Op>, Accept> || std::is_same_v<std::decay_t<Op>, Send> ||
    std::is_same_v<std::decay_t<Op>, Recv> || std::is_same_v<std::decay_t<Op>, Timeout>;

/// True when the operation transfers data through the submitted buffer.
template <typename Op>
inline constexpr bool uses_buffer_v =
    std::is_same_v<std::decay_t<Op>, Read> || std::is_same_v<std::decay_t<Op>, Pread> ||
    std::is_same_v<std::decay_t<Op>, Write> || std::is_same_v<std::decay_t<Op>, Pwrite> ||
    std::is_same_v<std::decay_t<Op>, Fstat> || std::is_same_v<std::decay_t<Op>, Accept> ||
    std::is_same_v<std::decay_t<Op>, Send> || std::is_same_v<std::decay_t<Op>, Recv>;

/// True for the operations that have no io_uring opcode and are therefore executed
/// synchronously by the Reactor when the current batch is processed.
template <typename Op>
inline constexpr bool is_inline_v = std::is_same_v<std::decay_t<Op>, Lseek> || std::is_same_v<std::decay_t<Op>, Listen>;

} // namespace ringu
