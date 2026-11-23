#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <variant>

#include <dojio/contracts.hpp>
#include <dojio/dojio_memory.hpp>
#include <dojio/fd.hpp>
#include <dojio/memory_pool.hpp>
#include <dojio/operation_id.hpp>
#include <dojio/operations.hpp>
#include <dojio/result.hpp>
#include <dojio/sqe.hpp>

// FstatOp completes with a struct stat. It is forward declared here so the public header stays
// free of the POSIX <sys/stat.h> include; a caller that submits an fstat must include it.
struct stat;

namespace dojio {

namespace detail {

/// Empty path handed to IORING_OP_STATX together with k_at_empty_path.
inline constexpr char k_empty_path[] = "";

// Implemented in the library: they require POSIX / Linux headers, which are kept
// out of the public headers.

/// listen(2) wrapper returning the raw result code (negated errno on failure).
std::int32_t listen_fd(std::int32_t fd, std::int32_t backlog);
/// bind(2) wrapper returning the raw result code (negated errno on failure).
std::int32_t bind_fd(std::int32_t fd, const void* addr, std::uint32_t addr_len);
/// ftruncate(2) wrapper returning the raw result code (negated errno on failure).
std::int32_t ftruncate_fd(std::int32_t fd, std::uint64_t length);
/// pipe2(2) wrapper writing the two descriptors into fds; raw result code (negated errno on failure).
std::int32_t pipe_fd(std::int32_t* fds, std::int32_t flags);
/// Reads the kernel-filled statx buffer and returns the equivalent struct stat.
[[nodiscard]] struct stat statx_to_stat(const void* data, std::uint64_t size);

} // namespace detail

/// Concept for the buffers accepted by RingEngine::submit: std::vector, std::array or
/// RegisteredBuffer (any container of a trivially copyable value type with data()/size()).
template <typename T>
concept BufferLike = requires(T& t) {
  typename T::value_type;
  { t.data() };
  { t.size() };
} && std::is_trivially_copyable_v<typename T::value_type>;

/// Maps an operation to the value type carried by a successful Result.
/// By default the action returns the associated buffer.
template <typename Op, typename Buffer> struct result_value {
  using type = Buffer;
};
template <typename Buffer> struct result_value<AcceptOp, Buffer> {
  using type = FileDescriptor;
};
template <typename Buffer> struct result_value<OpenAtOp, Buffer> {
  using type = FileDescriptor;
};
template <typename Buffer> struct result_value<FstatOp, Buffer> {
  using type = struct stat;
};
template <typename Buffer> struct result_value<FsyncOp, Buffer> {
  using type = std::int32_t;
};
template <typename Buffer> struct result_value<ConnectOp, Buffer> {
  using type = std::int32_t;
};
template <typename Buffer> struct result_value<FtruncateOp, Buffer> {
  using type = std::int32_t;
};
template <typename Buffer> struct result_value<CloseOp, Buffer> {
  using type = std::int32_t;
};
template <typename Buffer> struct result_value<ListenOp, Buffer> {
  using type = std::int32_t;
};
template <typename Buffer> struct result_value<BindOp, Buffer> {
  using type = std::int32_t;
};
template <typename Buffer> struct result_value<TimeoutOp, Buffer> {
  using type = std::monostate;
};
template <typename Op, typename Buffer> using result_value_t = typename result_value<Op, Buffer>::type;

/// Abstract base of every delayed action tracked by the RingEngine.
///
/// The ring-facing state is filled on demand into a caller-provided Sqe mirror;
/// the library copies it onto the real kernel SQE, so no io_uring type appears here.
class ActionBase {
public:
  explicit ActionBase(OperationId id, bool timer) : id_(id), timer_(timer) {}
  virtual ~ActionBase() = default;

  [[nodiscard]] OperationId id() const { return id_; }

  /// True when the operation is executed synchronously by the RingEngine when the
  /// current batch is processed, instead of being pushed to the kernel ring. The
  /// Action decides this at submission time (see compute_inline): an operation with
  /// an io_uring opcode is inline only when the ring does not expose it (or when the
  /// build simulates an old kernel, see DOJIO_FORCE_LINUX_OLD_KERNEL).
  [[nodiscard]] virtual bool is_inline() const { return false; }

  /// Executes an inline operation, returning the raw result code.
  virtual std::int32_t execute_inline() { return -ENOSYS; }

  /// Fills the caller-provided SQE mirror from the stored operation and buffer.
  virtual void fill(Sqe& out) = 0;

  /// Dispatches the completion to the callback. res is the raw CQE result.
  virtual void complete(std::int32_t res) = 0;

  /// True once the operation's SQE has been written into the kernel submission queue.
  [[nodiscard]] bool sqe_submitted() const { return sqe_submitted_; }
  void set_sqe_submitted() { sqe_submitted_ = true; }

  /// True for timer operations, whose -ETIME completion means "fired".
  [[nodiscard]] bool is_timer() const { return timer_; }

private:
  OperationId id_;
  bool timer_;
  bool sqe_submitted_ = false;
};

/// A delayed action: the completion handler of a submitted operation.
///
/// The action owns the submitted operation and its associated buffer. On completion
/// the associated buffer is returned through the Result passed as first parameter of
/// the callback: callback(Result<Value>, OperationId, raw_result). The callback may
/// also be invoked with only the Result or with (Result, OperationId).
template <typename Op, typename Buffer, typename Callback> class Action final : public ActionBase {
public:
  using Value = result_value_t<Op, Buffer>;

  Action(OperationId id, Op op, Buffer buffer, Callback callback, bool timer, OpCapabilities capabilities)
      : ActionBase(id, timer), op_(std::move(op)), buffer_(std::move(buffer)), callback_(std::move(callback)),
        inline_(compute_inline(capabilities)) {}

  [[nodiscard]] bool is_inline() const override { return inline_; }

  std::int32_t execute_inline() override {
    if constexpr (std::is_same_v<Op, ListenOp>) {
      return detail::listen_fd(op_.fd.value(), op_.backlog);
    } else if constexpr (std::is_same_v<Op, BindOp>) {
      return detail::bind_fd(op_.fd.value(), op_.addr.data(), op_.addr_len);
    } else if constexpr (std::is_same_v<Op, FtruncateOp>) {
      return detail::ftruncate_fd(op_.fd.value(), op_.length);
    } else if constexpr (std::is_same_v<Op, PipeOp>) {
      return detail::pipe_fd(detail::uring_cast<std::int32_t*>(buffer_.data()), op_.flags);
    } else {
      // only listen, bind, ftruncate and pipe can be dispatched inline; anything
      // else reaching here is a bug
      unreachable();
    }
  }

  void fill(Sqe& out) override { fill_sqe(out); }

  void complete(std::int32_t res) override {
    if (res == -ECANCELED) {
      invoke(Result<Value>(tl::unexpected(Cancelled{})), res);
      return;
    }
    if (res < 0 && !(is_timer() && res == -ETIME)) {
      invoke(Result<Value>(tl::unexpected(IoError{res})), res);
      return;
    }
    if constexpr (std::is_same_v<Op, FstatOp>) {
      invoke(Result<Value>(detail::statx_to_stat(buffer_.data(), buffer_capacity_bytes())), res);
    } else {
      invoke(Result<Value>(make_value(res)), res);
    }
  }

private:
  /// Number of bytes held by the buffer: what a write-like operation sends.
  std::uint64_t buffer_bytes() const {
    return static_cast<std::uint64_t>(buffer_.size()) * sizeof(typename Buffer::value_type);
  }

  /// Number of bytes an operation may have the kernel write into the buffer: the whole
  /// capacity of a registered chunk (its length is set to the transferred byte count at
  /// completion), the current size of a regular container.
  std::uint64_t buffer_capacity_bytes() const {
    if constexpr (std::is_same_v<Buffer, RegisteredBuffer>) {
      return buffer_.capacity() * sizeof(typename Buffer::value_type);
    } else {
      return buffer_bytes();
    }
  }

  void set_buffer(Sqe& sqe, bool fixed, bool kernel_writes) {
    sqe.addr = detail::uring_cast<std::uintptr_t>(buffer_.data());
    sqe.len = static_cast<std::uint32_t>(kernel_writes ? buffer_capacity_bytes() : buffer_bytes());
    if constexpr (std::is_same_v<Buffer, RegisteredBuffer>) {
      if (fixed) {
        sqe.buf_index = buffer_.index();
      }
    }
  }

  void fill_sqe(Sqe& sqe) {
    if constexpr (std::is_same_v<Op, OpenAtOp>) {
      sqe.opcode = static_cast<std::uint8_t>(IoOp::OpenAt);
      sqe.fd = op_.dirfd.value();
      sqe.addr = detail::uring_cast<std::uintptr_t>(op_.path.c_str());
      sqe.op_flags = static_cast<std::uint32_t>(op_.flags);
      sqe.len = op_.mode;
    } else if constexpr (std::is_same_v<Op, ReadOp> || std::is_same_v<Op, PreadOp>) {
      if constexpr (std::is_same_v<Buffer, RegisteredBuffer>) {
        sqe.opcode = static_cast<std::uint8_t>(IoOp::ReadFixed);
      } else {
        sqe.opcode = static_cast<std::uint8_t>(IoOp::Read);
      }
      sqe.fd = op_.fd.value();
      if constexpr (std::is_same_v<Op, PreadOp>) {
        sqe.off = op_.offset;
      } else {
        sqe.off = ~static_cast<std::uint64_t>(0); // current file position
      }
      set_buffer(sqe, std::is_same_v<Buffer, RegisteredBuffer>, /*kernel_writes=*/true);
    } else if constexpr (std::is_same_v<Op, WriteOp> || std::is_same_v<Op, PwriteOp>) {
      if constexpr (std::is_same_v<Buffer, RegisteredBuffer>) {
        sqe.opcode = static_cast<std::uint8_t>(IoOp::WriteFixed);
      } else {
        sqe.opcode = static_cast<std::uint8_t>(IoOp::Write);
      }
      sqe.fd = op_.fd.value();
      if constexpr (std::is_same_v<Op, PwriteOp>) {
        sqe.off = op_.offset;
      } else {
        sqe.off = ~static_cast<std::uint64_t>(0); // current file position
      }
      set_buffer(sqe, std::is_same_v<Buffer, RegisteredBuffer>, /*kernel_writes=*/false);
    } else if constexpr (std::is_same_v<Op, FstatOp>) {
      sqe.opcode = static_cast<std::uint8_t>(IoOp::Statx);
      sqe.fd = op_.fd.value();
      sqe.addr = detail::uring_cast<std::uintptr_t>(detail::k_empty_path);
      sqe.addr2 = detail::uring_cast<std::uintptr_t>(buffer_.data());
      sqe.len = k_statx_basic_stats;
      sqe.op_flags = k_at_empty_path;
    } else if constexpr (std::is_same_v<Op, FsyncOp>) {
      sqe.opcode = static_cast<std::uint8_t>(IoOp::Fsync);
      sqe.fd = op_.fd.value();
      sqe.op_flags = op_.use_datasync ? k_fsync_datasync : 0;
    } else if constexpr (std::is_same_v<Op, ConnectOp>) {
      sqe.opcode = static_cast<std::uint8_t>(IoOp::Connect);
      sqe.fd = op_.fd.value();
      sqe.addr = detail::uring_cast<std::uintptr_t>(op_.addr.data());
      sqe.addr2 = k_sockaddr_in_size;
    } else if constexpr (std::is_same_v<Op, CloseOp>) {
      sqe.opcode = static_cast<std::uint8_t>(IoOp::Close);
      sqe.fd = op_.fd.value();
    } else if constexpr (std::is_same_v<Op, AcceptOp>) {
      sqe.opcode = static_cast<std::uint8_t>(IoOp::Accept);
      sqe.fd = op_.fd.value();
      sqe.op_flags = static_cast<std::uint32_t>(op_.flags);
      if constexpr (!std::is_same_v<Buffer, std::monostate>) {
        // the kernel takes the address buffer capacity from *addr_len (addr2) and
        // leaves the length field at 0
        op_.addr_len = static_cast<std::uint32_t>(buffer_capacity_bytes());
        sqe.addr = detail::uring_cast<std::uintptr_t>(buffer_.data());
        sqe.addr2 = detail::uring_cast<std::uintptr_t>(&op_.addr_len);
      }
    } else if constexpr (std::is_same_v<Op, SendOp>) {
      sqe.opcode = static_cast<std::uint8_t>(IoOp::Send);
      sqe.fd = op_.fd.value();
      sqe.op_flags = static_cast<std::uint32_t>(op_.flags);
      set_buffer(sqe, false, /*kernel_writes=*/false);
    } else if constexpr (std::is_same_v<Op, RecvOp>) {
      sqe.opcode = static_cast<std::uint8_t>(IoOp::Recv);
      sqe.fd = op_.fd.value();
      sqe.op_flags = static_cast<std::uint32_t>(op_.flags);
      set_buffer(sqe, false, /*kernel_writes=*/true);
    } else if constexpr (std::is_same_v<Op, TimeoutOp>) {
      sqe.opcode = static_cast<std::uint8_t>(IoOp::Timeout);
      sqe.addr = detail::uring_cast<std::uintptr_t>(&op_.ts);
      sqe.len = 1;
      sqe.op_flags = 0;
    } else if constexpr (std::is_same_v<Op, FtruncateOp>) {
      sqe.opcode = static_cast<std::uint8_t>(IoOp::Ftruncate);
      sqe.fd = op_.fd.value();
      sqe.off = op_.length;
    } else if constexpr (std::is_same_v<Op, ListenOp>) {
      sqe.opcode = static_cast<std::uint8_t>(IoOp::Listen);
      sqe.fd = op_.fd.value();
      sqe.len = static_cast<std::uint32_t>(op_.backlog);
    } else if constexpr (std::is_same_v<Op, BindOp>) {
      sqe.opcode = static_cast<std::uint8_t>(IoOp::Bind);
      sqe.fd = op_.fd.value();
      sqe.addr = detail::uring_cast<std::uintptr_t>(op_.addr.data());
      sqe.addr2 = op_.addr_len;
    } else if constexpr (std::is_same_v<Op, PipeOp>) {
      // IORING_OP_PIPE writes the two descriptors into *addr (fd must be 0)
      sqe.opcode = static_cast<std::uint8_t>(IoOp::Pipe);
      sqe.fd = 0;
      sqe.addr = detail::uring_cast<std::uintptr_t>(buffer_.data());
      sqe.len = 0;
      sqe.op_flags = static_cast<std::uint32_t>(op_.flags);
    }
    // listen, bind, ftruncate and pipe have no io_uring opcode on older kernels:
    // is_inline() decides per operation whether they are executed synchronously.
  }

  /// Dispatcher run at submission time: a syscall-backed operation goes to the
  /// io_uring submission queue when the ring exposes its opcode, and is executed
  /// synchronously (inline) when it does not.
  static bool compute_inline(OpCapabilities capabilities) {
    // DOJIO_FORCE_LINUX_OLD_KERNEL forces the inline path so it is covered on a recent
    // kernel (see the dojio_old_linux_compat_tests target).
    [[maybe_unused]] constexpr auto simulated_old_kernel = [] {
#if defined(DOJIO_FORCE_LINUX_OLD_KERNEL)
      return true;
#else
      return false;
#endif
    };
    if constexpr (InlineOperation<Op>) {
      return true;
    } else if constexpr (std::is_same_v<Op, ListenOp> || std::is_same_v<Op, BindOp>) {
      return simulated_old_kernel() || !capabilities.bind_listen;
    } else if constexpr (std::is_same_v<Op, FtruncateOp>) {
      return simulated_old_kernel() || !capabilities.ftruncate;
    } else if constexpr (std::is_same_v<Op, PipeOp>) {
      return simulated_old_kernel() || !capabilities.pipe;
    } else {
      return false;
    }
  }

  Value make_value(std::int32_t res) {
    if constexpr (std::is_same_v<Value, Buffer>) {
      // shrink read-like buffers to the number of bytes actually transferred
      if constexpr (std::is_same_v<Op, ReadOp> || std::is_same_v<Op, PreadOp> || std::is_same_v<Op, RecvOp>) {
        if constexpr (requires { buffer_.resize(static_cast<std::size_t>(res)); }) {
          if (res >= 0) {
            buffer_.resize(static_cast<std::size_t>(res));
          }
        }
      }
      return Value(std::move(buffer_));
    } else if constexpr (std::is_same_v<Value, FileDescriptor>) {
      return Value(res);
    } else if constexpr (std::is_same_v<Value, std::monostate>) {
      return Value{};
    } else {
      return static_cast<Value>(res);
    }
  }

  void invoke(Result<Value> result, std::int32_t res) {
    if constexpr (std::is_invocable_v<Callback, Result<Value>, OperationId, std::int32_t>) {
      callback_(std::move(result), id(), res);
    } else if constexpr (std::is_invocable_v<Callback, Result<Value>, OperationId>) {
      callback_(std::move(result), id());
    } else {
      callback_(std::move(result));
    }
  }

  Op op_;
  Buffer buffer_;
  Callback callback_;
  bool inline_ = false;
};

/// Upper bound on the size of a single Action, used as the largest_required_pool_block
/// of the RingEngine action pool. It is roughly 50% above the average action size
/// (the demo Send / Recv actions measure about 136 B / 192 B). do_submit() rejects
/// any submission whose Action type exceeds it with a static_assert.
inline constexpr std::size_t max_size_action = 320;

} // namespace dojio
