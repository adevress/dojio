#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

#include <dojio/buffer.hpp>
#include <dojio/fd.hpp>
#include <dojio/socket.hpp>
#include <dojio/sqe.hpp>

namespace dojio {

/// Size of the kernel struct statx (fixed at 256 bytes by the uapi). An Fstat
/// buffer must hold at least this many bytes; on success its first
/// sizeof(struct stat) bytes are rewritten as a struct stat.
inline constexpr std::uint64_t k_statx_size = 256;

/// Opens path with flags/mode (openat(2) semantics; open(2) when dirfd is AT_FDCWD).
/// Completes with the new FileDescriptor.
struct OpenAtOp {
  FileDescriptor dirfd{k_at_fdcwd};
  std::string path;
  std::int32_t flags = 0;
  std::uint32_t mode = 0;
};

/// Constructs an OpenAtOp operation with open(2) semantics (path relative to the
/// current working directory).
inline OpenAtOp open_at(std::string path, std::int32_t flags, std::uint32_t mode = static_cast<std::uint32_t>(0644)) {
  return OpenAtOp{FileDescriptor{k_at_fdcwd}, std::move(path), flags, mode};
}

/// Constructs an OpenAtOp operation with openat(2) semantics (path relative to dirfd).
inline OpenAtOp open_at(FileDescriptor dirfd, std::string path, std::int32_t flags,
                        std::uint32_t mode = static_cast<std::uint32_t>(0644)) {
  return OpenAtOp{dirfd, std::move(path), flags, mode};
}

/// Reads from fd at the current file position (read(2) semantics). The buffer
/// receives the data; on success it is shrunk to the number of bytes read.
struct ReadOp {
  FileDescriptor fd;
};

/// Constructs a ReadOp operation (read at the current file position).
inline ReadOp read(FileDescriptor fd) { return ReadOp{fd}; }

/// Reads from fd at an explicit offset (pread(2) semantics). The buffer receives
/// the data; on success it is shrunk to the number of bytes read.
struct PreadOp {
  FileDescriptor fd;
  std::uint64_t offset = 0;
};

/// Constructs a PreadOp operation (read at an explicit offset).
inline PreadOp pread(FileDescriptor fd, std::uint64_t offset) { return PreadOp{fd, offset}; }

/// Writes the buffer to fd at the current file position (write(2) semantics).
struct WriteOp {
  FileDescriptor fd;
};

/// Constructs a WriteOp operation (write at the current file position).
inline WriteOp write(FileDescriptor fd) { return WriteOp{fd}; }

/// Writes the buffer to fd at an explicit offset (pwrite(2) semantics).
struct PwriteOp {
  FileDescriptor fd;
  std::uint64_t offset = 0;
};

/// Constructs a PwriteOp operation (write at an explicit offset).
inline PwriteOp pwrite(FileDescriptor fd, std::uint64_t offset) { return PwriteOp{fd, offset}; }

/// Truncates fd to length bytes (ftruncate(2) semantics). Completes with 0.
///
/// The RingEngine uses IORING_OP_FTRUNCATE when the running kernel exposes it
/// (Linux 6.9+) and falls back to a synchronous ftruncate(2) otherwise.
struct FtruncateOp {
  FileDescriptor fd;
  std::uint64_t length = 0;
};

/// Constructs a FtruncateOp operation (ftruncate(2) semantics).
inline FtruncateOp ftruncate(FileDescriptor fd, std::uint64_t length) { return FtruncateOp{fd, length}; }

/// Number of file descriptors produced by a Pipe operation.
inline constexpr std::uint64_t k_pipe_fd_count = 2;

/// Buffer receiving the two descriptors created by a Pipe operation: index 0 is
/// the read end, index 1 the write end.
using PipeBuffer = std::array<FileDescriptor, k_pipe_fd_count>;

/// Creates a pipe (pipe2(2) semantics). The two descriptors are written into the
/// submitted PipeBuffer and the operation completes with 0.
///
/// The RingEngine uses IORING_OP_PIPE when the running kernel exposes it
/// (Linux 6.16+) and falls back to a synchronous pipe2(2) otherwise.
struct PipeOp {
  std::int32_t flags = 0;
};

/// Constructs a PipeOp operation (pipe2(2) semantics); flags are the pipe2(2) flags.
inline PipeOp pipe(std::int32_t flags = 0) { return PipeOp{flags}; }

/// Stats fd (fstat(2) semantics) through IORING_OP_STATX with AT_EMPTY_PATH.
///
/// The submitted buffer must hold at least k_statx_size bytes; on success its
/// first sizeof(struct stat) bytes are rewritten as a struct stat.
struct FstatOp {
  FileDescriptor fd;
};

/// Constructs an FstatOp operation (fstat(2) semantics).
inline FstatOp fstat(FileDescriptor fd) { return FstatOp{fd}; }

/// Closes fd (close(2) semantics).
struct CloseOp {
  FileDescriptor fd;
};

/// Constructs a CloseOp operation.
inline CloseOp close(FileDescriptor fd) { return CloseOp{fd}; }

/// Marks a socket as passive (listen(2) semantics).
///
/// The RingEngine uses IORING_OP_LISTEN when the running kernel exposes it
/// (Linux 6.11+) and falls back to a synchronous listen(2) otherwise.
struct ListenOp {
  FileDescriptor fd;
  std::int32_t backlog = 0;
};

/// Constructs a ListenOp operation (listen(2) semantics).
inline ListenOp listen(FileDescriptor fd, std::int32_t backlog) { return ListenOp{fd, backlog}; }

/// Accepts a connection on a listening socket (accept(2) semantics). Completes
/// with the accepted FileDescriptor. When a buffer is submitted it receives the
/// peer address (a sockaddr).
struct AcceptOp {
  FileDescriptor fd;
  std::int32_t flags = 0;
  std::uint32_t addr_len = 0; // filled by the RingEngine from the buffer size
};

/// Constructs an AcceptOp operation. Submit it with an AcceptBuffer to receive the
/// peer address (a sockaddr_storage), e.g.:
///   engine.submit(accept(lfd), AcceptBuffer{}, callback);
inline AcceptOp accept(FileDescriptor fd, std::int32_t flags = 0) { return AcceptOp{fd, flags, 0}; }

/// Sends the buffer on a connected socket (send(2) semantics).
struct SendOp {
  FileDescriptor fd;
  std::int32_t flags = 0;
};

/// Constructs a SendOp operation (send(2) semantics on a connected socket).
inline SendOp send(FileDescriptor fd, std::int32_t flags = 0) { return SendOp{fd, flags}; }

/// Receives into the buffer from a connected socket (recv(2) semantics). On
/// success the buffer is shrunk to the number of bytes received.
struct RecvOp {
  FileDescriptor fd;
  std::int32_t flags = 0;
};

/// Constructs a RecvOp operation (recv(2) semantics on a connected socket).
inline RecvOp recv(FileDescriptor fd, std::int32_t flags = 0) { return RecvOp{fd, flags}; }

/// Plain mirror of __kernel_timespec (identical layout: two 64-bit fields).
struct Timespec {
  std::int64_t sec = 0;
  std::int64_t nsec = 0;
};

/// fsync(2) on fd; datasync selects fdatasync(2) semantics. Completes with 0.
struct FsyncOp {
  FileDescriptor fd;
  bool use_datasync = false;
};

/// Constructs an FsyncOp operation; pass use_datasync = true for fdatasync(2) semantics.
inline FsyncOp fsync(FileDescriptor fd, bool use_datasync = false) { return FsyncOp{fd, use_datasync}; }

/// bind(2) on fd to an IPv4 or IPv6 endpoint. Completes with 0 on success.
///
/// The RingEngine uses IORING_OP_BIND when the running kernel exposes it
/// (Linux 6.11+) and falls back to a synchronous bind(2) otherwise.
///
/// The sockaddr_in / sockaddr_in6 is stored inside the operation (static buffer),
/// so no C socket type appears in the public API.
struct BindOp {
  FileDescriptor fd;
  std::variant<Ipv4Endpoint, Ipv6Endpoint> endpoint;
  StaticBuffer<k_sockaddr_storage_size> addr{}; // sockaddr_in / sockaddr_in6 storage, filled by the factory
  std::uint32_t addr_len = 0;                   // filled by the factory
};

/// Constructs a BindOp operation for an IPv4 endpoint (implemented in the library,
/// where the sockaddr_in is built from the endpoint).
BindOp bind(FileDescriptor fd, Ipv4Endpoint endpoint);

/// Constructs a BindOp operation for an IPv6 endpoint (implemented in the library,
/// where the sockaddr_in6 is built from the endpoint).
BindOp bind(FileDescriptor fd, Ipv6Endpoint endpoint);

/// connect(2) on fd to an IPv4 endpoint. Completes with 0 on success.
///
/// The sockaddr_in is stored inside the operation (static buffer), so no C socket
/// type appears in the public API.
struct ConnectOp {
  FileDescriptor fd;
  Ipv4Endpoint endpoint;
  StaticBuffer<k_sockaddr_in_size> addr{}; // sockaddr_in storage, filled by the factory
};

/// Constructs a ConnectOp operation (implemented in the library, where the sockaddr_in
/// is built from the endpoint).
ConnectOp connect(FileDescriptor fd, Ipv4Endpoint endpoint);

/// Backing operation of Timer: completes when the timeout expires.
struct TimeoutOp {
  Timespec ts{};
};

/// Constructs a TimeoutOp operation firing after the given duration.
inline TimeoutOp timeout(std::chrono::nanoseconds duration) {
  TimeoutOp op{};
  const std::int64_t ns = duration.count();
  op.ts.sec = ns / 1000000000LL;
  op.ts.nsec = ns % 1000000000LL;
  return op;
}

/// Variant of every operation supported by the RingEngine. Each operation is its own
/// struct held by this variant.
using Operation = std::variant<OpenAtOp, ReadOp, PreadOp, WriteOp, PwriteOp, FtruncateOp, FstatOp, CloseOp, ListenOp,
                               AcceptOp, SendOp, RecvOp, TimeoutOp, FsyncOp, ConnectOp, BindOp, PipeOp>;

/// True when Op is one of the operation structs of the Operation variant.
template <typename Op>
inline constexpr bool is_operation_v =
    std::is_same_v<std::decay_t<Op>, OpenAtOp> || std::is_same_v<std::decay_t<Op>, FsyncOp> ||
    std::is_same_v<std::decay_t<Op>, ConnectOp> || std::is_same_v<std::decay_t<Op>, ReadOp> ||
    std::is_same_v<std::decay_t<Op>, PreadOp> || std::is_same_v<std::decay_t<Op>, WriteOp> ||
    std::is_same_v<std::decay_t<Op>, PwriteOp> || std::is_same_v<std::decay_t<Op>, FtruncateOp> ||
    std::is_same_v<std::decay_t<Op>, FstatOp> || std::is_same_v<std::decay_t<Op>, CloseOp> ||
    std::is_same_v<std::decay_t<Op>, ListenOp> || std::is_same_v<std::decay_t<Op>, AcceptOp> ||
    std::is_same_v<std::decay_t<Op>, SendOp> || std::is_same_v<std::decay_t<Op>, RecvOp> ||
    std::is_same_v<std::decay_t<Op>, TimeoutOp> || std::is_same_v<std::decay_t<Op>, BindOp> ||
    std::is_same_v<std::decay_t<Op>, PipeOp>;

/// True when the operation transfers data through the submitted buffer.
template <typename Op>
inline constexpr bool uses_buffer_v =
    std::is_same_v<std::decay_t<Op>, ReadOp> || std::is_same_v<std::decay_t<Op>, PreadOp> ||
    std::is_same_v<std::decay_t<Op>, WriteOp> || std::is_same_v<std::decay_t<Op>, PwriteOp> ||
    std::is_same_v<std::decay_t<Op>, FstatOp> || std::is_same_v<std::decay_t<Op>, AcceptOp> ||
    std::is_same_v<std::decay_t<Op>, SendOp> || std::is_same_v<std::decay_t<Op>, RecvOp> ||
    std::is_same_v<std::decay_t<Op>, PipeOp>;

/// True for the operations that have no io_uring opcode and are therefore executed
/// synchronously by the RingEngine when the current batch is processed. An operation
/// opts in by declaring a `static constexpr bool is_inline = true` tag member.
template <typename Op>
concept InlineOperation = std::decay_t<Op>::is_inline;

} // namespace dojio
