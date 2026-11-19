#pragma once

#include <cstdint>

namespace dojio {

/// io_uring opcode values used by dojio.
///
/// These are the stable kernel uapi ABI values of the io_uring opcodes; the library
/// maps them onto the real kernel SQE, so no <linux/io_uring.h> is needed here.
enum class IoOp : std::uint8_t {
  Fsync = 3,        // IORING_OP_FSYNC
  ReadFixed = 4,    // IORING_OP_READ_FIXED
  WriteFixed = 5,   // IORING_OP_WRITE_FIXED
  Timeout = 11,     // IORING_OP_TIMEOUT
  Accept = 13,      // IORING_OP_ACCEPT
  AsyncCancel = 14, // IORING_OP_ASYNC_CANCEL
  Connect = 16,     // IORING_OP_CONNECT
  OpenAt = 18,      // IORING_OP_OPENAT
  Close = 19,       // IORING_OP_CLOSE
  Statx = 21,       // IORING_OP_STATX
  Read = 22,        // IORING_OP_READ
  Write = 23,       // IORING_OP_WRITE
  Send = 26,        // IORING_OP_SEND
  Recv = 27,        // IORING_OP_RECV
  Ftruncate = 55,   // IORING_OP_FTRUNCATE
  Bind = 56,        // IORING_OP_BIND
  Listen = 57,      // IORING_OP_LISTEN
  Pipe = 62,        // IORING_OP_PIPE
};

/// Plain-old-data mirror of the kernel io_uring SQE fields used by dojio.
///
/// Actions fill this struct at submit time; the library copies it field by field
/// onto the real kernel SQE (see detail::copy_sqe in the library). Keeping this
/// mirror in the public header lets the public API stay free of any Linux
/// internal type.
struct Sqe {
  std::uint8_t opcode = 0;    // IoOp
  std::int32_t fd = -1;       // target file descriptor (k_at_fdcwd for open(2)-style open_at)
  std::uint64_t off = 0;      // file offset (-1 for the current position)
  std::uint64_t addr = 0;     // buffer / path / timespec pointer
  std::uint64_t addr2 = 0;    // secondary pointer (statx buffer, accept addr_len)
  std::uint32_t len = 0;      // buffer length / mode / timeout count / statx mask
  std::uint32_t op_flags = 0; // open_flags / statx_flags / accept_flags / msg_flags / timeout_flags
  std::uint64_t user_data = 0;
  std::uint16_t buf_index = 0; // fixed buffer index for READ_FIXED / WRITE_FIXED
};

/// AT_FDCWD (stable kernel uapi value).
inline constexpr std::int32_t k_at_fdcwd = -100;
/// AT_EMPTY_PATH (stable kernel uapi value), used for the Fstat operation.
inline constexpr std::uint32_t k_at_empty_path = 0x1000;
/// STATX_BASIC_STATS (stable kernel uapi value), used for the Fstat operation.
inline constexpr std::uint32_t k_statx_basic_stats = 0x7ff;
/// IORING_FSYNC_DATASYNC (stable kernel uapi value), used for the Fsync operation.
inline constexpr std::uint32_t k_fsync_datasync = 1;

} // namespace dojio
