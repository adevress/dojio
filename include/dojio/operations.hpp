#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

#include <tl/expected.hpp>

#include <dojio/buffer.hpp>
#include <dojio/error.hpp>
#include <dojio/fd.hpp>
#include <dojio/result.hpp>
#include <dojio/socket.hpp>
#include <dojio/sqe.hpp>

// FstatResult is a struct stat. It is forward declared here so the public headers stay
// free of the POSIX <sys/stat.h> include; a caller that submits an fstat must include it.
struct stat;

namespace dojio {

/// Size of the kernel struct statx (fixed at 256 bytes by the uapi).
inline constexpr std::uint64_t k_statx_size = 256;

/// Result value of an OpenAtOp: the descriptor of the opened file.
struct OpenAtResult {
  FileDescriptor fd;
};

/// Result value of a ReadOp: the submitted buffer, holding the received content, and
/// the number of bytes read.
template <class Buffer> struct ReadResult {
  Buffer buffer;
  std::uint64_t size = 0;
};

/// Result value of a PreadOp: the submitted buffer, holding the received content, and
/// the number of bytes read at the requested offset.
template <class Buffer> struct PreadResult {
  Buffer buffer;
  std::uint64_t size = 0;
  std::uint64_t offset = 0;
};

/// Result value of a WriteOp: the submitted buffer, returned for reuse, and the number
/// of bytes written.
template <class Buffer> struct WriteResult {
  Buffer buffer;
  std::uint64_t size = 0;
};

/// Result value of a PwriteOp: the submitted buffer, returned for reuse, and the number
/// of bytes written at the requested offset.
template <class Buffer> struct PwriteResult {
  Buffer buffer;
  std::uint64_t size = 0;
  std::uint64_t offset = 0;
};

/// Result value of an FtruncateOp: the new length of the file.
struct FtruncateResult {
  std::uint64_t length = 0;
};

/// Result value of an FstatOp: the stat of the descriptor.
using FstatResult = struct stat;

/// Result value of a CloseOp: the descriptor that was closed.
struct CloseResult {
  FileDescriptor fd;
};

/// Result value of a ListenOp: the descriptor now listening and its backlog.
struct ListenResult {
  FileDescriptor fd;
  std::int32_t backlog = 0;
};

/// Result value of an AcceptOp: the descriptor of the accepted connection.
struct AcceptResult {
  FileDescriptor fd;
};

/// Result value of a SendOp: the submitted buffer, returned for reuse, and the number of
/// bytes sent.
template <class Buffer> struct SendResult {
  Buffer buffer;
  std::uint64_t size = 0;
};

/// Result value of a RecvOp: the submitted buffer, holding the received content, and the
/// number of bytes received.
template <class Buffer> struct RecvResult {
  Buffer buffer;
  std::uint64_t size = 0;
};

/// Result value of a TimeoutOp: empty, a fired timer carries no data.
struct TimeoutResult {};

/// Result value of an FsyncOp: the descriptor that was synced.
struct FsyncResult {
  FileDescriptor fd;
  bool use_datasync = false;
};

/// Result value of a ConnectOp: the descriptor that was connected.
struct ConnectResult {
  FileDescriptor fd;
};

/// Result value of a BindOp: the descriptor that was bound.
struct BindResult {
  FileDescriptor fd;
};

/// Number of file descriptors produced by a Pipe operation.
inline constexpr std::uint64_t k_pipe_fd_count = 2;

/// Buffer receiving the two descriptors created by a Pipe operation: index 0 is the
/// read end, index 1 the write end.
using PipeBuffer = std::array<FileDescriptor, k_pipe_fd_count>;

/// Result value of a PipeOp: the two descriptors of the pipe.
struct PipeResult {
  PipeBuffer fds;
};

/// Opens path with flags/mode (openat(2) semantics; open(2) when dirfd is AT_FDCWD).
/// Completes with the new FileDescriptor.
struct OpenAtOp {
  FileDescriptor dirfd{k_at_fdcwd};
  std::string path;
  std::int32_t flags = 0;
  std::uint32_t mode = 0;
  std::monostate buffer{};
  using buffer_type = std::monostate;
  using result_value = OpenAtResult;
  using result = tl::expected<result_value, ErrorCode>;
};

/// Constructs an OpenAtOp operation with open(2) semantics (path relative to the
/// current working directory).
inline OpenAtOp open_at(std::string path, std::int32_t flags, std::uint32_t mode = static_cast<std::uint32_t>(0644)) {
  return OpenAtOp{FileDescriptor{k_at_fdcwd}, std::move(path), flags, mode, {}};
}

/// Constructs an OpenAtOp operation with openat(2) semantics (path relative to dirfd).
inline OpenAtOp open_at(FileDescriptor dirfd, std::string path, std::int32_t flags,
                        std::uint32_t mode = static_cast<std::uint32_t>(0644)) {
  return OpenAtOp{dirfd, std::move(path), flags, mode, {}};
}

/// Reads from fd at the current file position (read(2) semantics). The result returns the
/// buffer, holding the data read.
template <class Buffer> struct ReadOp {
  FileDescriptor fd;
  Buffer buffer;
  using buffer_type = Buffer;
  using result_value = ReadResult<Buffer>;
  using result = tl::expected<result_value, ErrorCode>;
};

/// Constructs a ReadOp operation (read at the current file position).
template <BufferLike Buffer> ReadOp<Buffer> read(FileDescriptor fd, Buffer buffer) {
  return ReadOp<Buffer>{fd, std::move(buffer)};
}

/// Reads from fd at an explicit offset (pread(2) semantics). The result returns the
/// buffer, holding the data read.
template <class Buffer> struct PreadOp {
  FileDescriptor fd;
  std::uint64_t offset = 0;
  Buffer buffer;
  using buffer_type = Buffer;
  using result_value = PreadResult<Buffer>;
  using result = tl::expected<result_value, ErrorCode>;
};

/// Constructs a PreadOp operation (read at an explicit offset).
template <BufferLike Buffer> PreadOp<Buffer> pread(FileDescriptor fd, std::uint64_t offset, Buffer buffer) {
  return PreadOp<Buffer>{fd, offset, std::move(buffer)};
}

/// Writes the buffer to fd at the current file position (write(2) semantics). The result
/// returns the buffer for reuse.
template <class Buffer> struct WriteOp {
  FileDescriptor fd;
  Buffer buffer;
  using buffer_type = Buffer;
  using result_value = WriteResult<Buffer>;
  using result = tl::expected<result_value, ErrorCode>;
};

/// Constructs a WriteOp operation (write at the current file position).
template <BufferLike Buffer> WriteOp<Buffer> write(FileDescriptor fd, Buffer buffer) {
  return WriteOp<Buffer>{fd, std::move(buffer)};
}

/// Writes the buffer to fd at an explicit offset (pwrite(2) semantics). The result
/// returns the buffer for reuse.
template <class Buffer> struct PwriteOp {
  FileDescriptor fd;
  std::uint64_t offset = 0;
  Buffer buffer;
  using buffer_type = Buffer;
  using result_value = PwriteResult<Buffer>;
  using result = tl::expected<result_value, ErrorCode>;
};

/// Constructs a PwriteOp operation (write at an explicit offset).
template <BufferLike Buffer> PwriteOp<Buffer> pwrite(FileDescriptor fd, std::uint64_t offset, Buffer buffer) {
  return PwriteOp<Buffer>{fd, offset, std::move(buffer)};
}

/// Truncates fd to length bytes (ftruncate(2) semantics).
///
/// The RingEngine uses IORING_OP_FTRUNCATE when the running kernel exposes it
/// (Linux 6.9+) and falls back to a synchronous ftruncate(2) otherwise.
struct FtruncateOp {
  FileDescriptor fd;
  std::uint64_t length = 0;
  std::monostate buffer{};
  using buffer_type = std::monostate;
  using result_value = FtruncateResult;
  using result = tl::expected<result_value, ErrorCode>;
};

/// Constructs a FtruncateOp operation (ftruncate(2) semantics).
inline FtruncateOp ftruncate(FileDescriptor fd, std::uint64_t length) { return FtruncateOp{fd, length, {}}; }

/// Creates a pipe (pipe2(2) semantics). The two descriptors are written into the
/// operation and the result returns them; index 0 is the read end, index 1 the write end.
///
/// The RingEngine uses IORING_OP_PIPE when the running kernel exposes it (Linux 6.16+)
/// and falls back to a synchronous pipe2(2) otherwise.
struct PipeOp {
  std::int32_t flags = 0;
  std::monostate buffer{};
  PipeBuffer fds{};
  using buffer_type = std::monostate;
  using result_value = PipeResult;
  using result = tl::expected<result_value, ErrorCode>;
};

/// Constructs a PipeOp operation (pipe2(2) semantics); flags are the pipe2(2) flags.
inline PipeOp pipe(std::int32_t flags = 0) { return PipeOp{flags, {}, {}}; }

/// Stats fd (fstat(2) semantics) through IORING_OP_STATX with AT_EMPTY_PATH.
///
/// The statx scratch buffer is embedded in the operation and the result returns the
/// equivalent struct stat.
struct FstatOp {
  FileDescriptor fd;
  std::monostate buffer{};
  StaticBuffer<k_statx_size> statx{};
  using buffer_type = std::monostate;
  using result_value = FstatResult;
  using result = tl::expected<result_value, ErrorCode>;
};

/// Constructs an FstatOp operation (fstat(2) semantics).
inline FstatOp fstat(FileDescriptor fd) { return FstatOp{fd, {}, {}}; }

/// Closes fd (close(2) semantics).
struct CloseOp {
  FileDescriptor fd;
  std::monostate buffer{};
  using buffer_type = std::monostate;
  using result_value = CloseResult;
  using result = tl::expected<result_value, ErrorCode>;
};

/// Constructs a CloseOp operation.
inline CloseOp close(FileDescriptor fd) { return CloseOp{fd, {}}; }

/// Marks a socket as passive (listen(2) semantics).
///
/// The RingEngine uses IORING_OP_LISTEN when the running kernel exposes it
/// (Linux 6.11+) and falls back to a synchronous listen(2) otherwise.
struct ListenOp {
  FileDescriptor fd;
  std::int32_t backlog = 0;
  std::monostate buffer{};
  using buffer_type = std::monostate;
  using result_value = ListenResult;
  using result = tl::expected<result_value, ErrorCode>;
};

/// Constructs a ListenOp operation (listen(2) semantics).
inline ListenOp listen(FileDescriptor fd, std::int32_t backlog) { return ListenOp{fd, backlog, {}}; }

/// Accepts a connection on a listening socket (accept(2) semantics). Completes with the
/// accepted FileDescriptor. The peer address buffer is embedded in the operation, so no
/// buffer is passed to submit().
struct AcceptOp {
  FileDescriptor fd;
  std::int32_t flags = 0;
  std::uint32_t addr_len = 0; // filled by the RingEngine from the embedded buffer size
  std::monostate buffer{};
  AcceptBuffer peer{};
  using buffer_type = std::monostate;
  using result_value = AcceptResult;
  using result = tl::expected<result_value, ErrorCode>;
};

/// Constructs an AcceptOp operation.
inline AcceptOp accept(FileDescriptor fd, std::int32_t flags = 0) { return AcceptOp{fd, flags, 0, {}, {}}; }

/// Sends the buffer on a connected socket (send(2) semantics). The result returns the
/// buffer for reuse.
template <class Buffer> struct SendOp {
  FileDescriptor fd;
  std::int32_t flags = 0;
  Buffer buffer;
  using buffer_type = Buffer;
  using result_value = SendResult<Buffer>;
  using result = tl::expected<result_value, ErrorCode>;
};

/// Constructs a SendOp operation (send(2) semantics on a connected socket).
template <BufferLike Buffer> SendOp<Buffer> send(FileDescriptor fd, Buffer buffer, std::int32_t flags = 0) {
  return SendOp<Buffer>{fd, flags, std::move(buffer)};
}

/// Receives into the buffer from a connected socket (recv(2) semantics). The result
/// returns the buffer, holding the received content.
template <class Buffer> struct RecvOp {
  FileDescriptor fd;
  std::int32_t flags = 0;
  Buffer buffer;
  using buffer_type = Buffer;
  using result_value = RecvResult<Buffer>;
  using result = tl::expected<result_value, ErrorCode>;
};

/// Constructs a RecvOp operation (recv(2) semantics on a connected socket).
template <BufferLike Buffer> RecvOp<Buffer> recv(FileDescriptor fd, Buffer buffer, std::int32_t flags = 0) {
  return RecvOp<Buffer>{fd, flags, std::move(buffer)};
}

/// Plain mirror of __kernel_timespec (identical layout: two 64-bit fields).
struct Timespec {
  std::int64_t sec = 0;
  std::int64_t nsec = 0;
};

/// fsync(2) on fd; datasync selects fdatasync(2) semantics.
struct FsyncOp {
  FileDescriptor fd;
  bool use_datasync = false;
  std::monostate buffer{};
  using buffer_type = std::monostate;
  using result_value = FsyncResult;
  using result = tl::expected<result_value, ErrorCode>;
};

/// Constructs an FsyncOp operation; pass use_datasync = true for fdatasync(2) semantics.
inline FsyncOp fsync(FileDescriptor fd, bool use_datasync = false) { return FsyncOp{fd, use_datasync, {}}; }

/// bind(2) on fd to an IPv4 or IPv6 endpoint.
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
  std::monostate buffer{};
  using buffer_type = std::monostate;
  using result_value = BindResult;
  using result = tl::expected<result_value, ErrorCode>;
};

/// Constructs a BindOp operation for an IPv4 endpoint (implemented in the library,
/// where the sockaddr_in is built from the endpoint).
BindOp bind(FileDescriptor fd, Ipv4Endpoint endpoint);

/// Constructs a BindOp operation for an IPv6 endpoint (implemented in the library,
/// where the sockaddr_in6 is built from the endpoint).
BindOp bind(FileDescriptor fd, Ipv6Endpoint endpoint);

/// connect(2) on fd to an IPv4 endpoint.
///
/// The sockaddr_in is stored inside the operation (static buffer), so no C socket
/// type appears in the public API.
struct ConnectOp {
  FileDescriptor fd;
  Ipv4Endpoint endpoint;
  StaticBuffer<k_sockaddr_in_size> addr{}; // sockaddr_in storage, filled by the factory
  std::monostate buffer{};
  using buffer_type = std::monostate;
  using result_value = ConnectResult;
  using result = tl::expected<result_value, ErrorCode>;
};

/// Constructs a ConnectOp operation (implemented in the library, where the sockaddr_in
/// is built from the endpoint).
ConnectOp connect(FileDescriptor fd, Ipv4Endpoint endpoint);

/// Backing operation of Timer: completes when the timeout expires.
struct TimeoutOp {
  std::monostate buffer{};
  Timespec ts{};
  using buffer_type = std::monostate;
  using result_value = TimeoutResult;
  using result = tl::expected<result_value, ErrorCode>;
};

/// Constructs a TimeoutOp operation firing after the given duration.
inline TimeoutOp timeout(std::chrono::nanoseconds duration) {
  TimeoutOp op{};
  const std::int64_t ns = duration.count();
  op.ts.sec = ns / 1000000000LL;
  op.ts.nsec = ns % 1000000000LL;
  return op;
}

/// Concept of a dojio operation: every operation exposes a result_value alias and its
/// tl::expected result alias.
template <typename Op>
concept OperationType =
    requires {
      typename std::remove_cvref_t<Op>::result_value;
      typename std::remove_cvref_t<Op>::result;
    } && std::is_same_v<typename std::remove_cvref_t<Op>::result,
                        tl::expected<typename std::remove_cvref_t<Op>::result_value, ErrorCode>>;

/// True for the operations that have no io_uring opcode and are therefore executed
/// synchronously by the RingEngine when the current batch is processed. An operation
/// opts in by declaring a `static constexpr bool is_inline = true` tag member.
template <typename Op>
concept InlineOperation = std::decay_t<Op>::is_inline;

} // namespace dojio
