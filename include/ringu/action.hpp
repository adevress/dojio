#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <variant>

#include <ringu/contracts.hpp>
#include <ringu/fd.hpp>
#include <ringu/memory_pool.hpp>
#include <ringu/operation_id.hpp>
#include <ringu/operations.hpp>
#include <ringu/result.hpp>
#include <ringu/sqe.hpp>

namespace ringu {

namespace detail {

// Implemented in the library: they require POSIX / Linux headers, which are kept
// out of the public headers.

/// lseek(2) wrapper returning the raw result code (negated errno on failure).
std::int32_t lseek_fd(std::int32_t fd, std::int64_t offset, std::int32_t whence);
/// listen(2) wrapper returning the raw result code (negated errno on failure).
std::int32_t listen_fd(std::int32_t fd, std::int32_t backlog);
/// bind(2) wrapper returning the raw result code (negated errno on failure).
std::int32_t bind_fd(std::int32_t fd, const void* addr, std::uint32_t addr_len);
/// Rewrites the first sizeof(struct stat) bytes of a statx-filled buffer as a struct stat.
void statx_to_stat(std::byte* data, std::uint64_t size);

} // namespace detail

/// Concept for the buffers accepted by Reactor::submit: std::vector, std::array or
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
template <typename Buffer> struct result_value<FsyncOp, Buffer> {
  using type = std::int32_t;
};
template <typename Buffer> struct result_value<ConnectOp, Buffer> {
  using type = std::int32_t;
};
template <typename Buffer> struct result_value<LseekOp, Buffer> {
  using type = std::int64_t;
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

/// Abstract base of every delayed action tracked by the Reactor.
///
/// The ring-facing state is a plain Sqe mirror; the library copies it onto the real
/// kernel SQE, so no io_uring type appears here.
class ActionBase {
public:
  explicit ActionBase(OperationId id, bool timer) : id_(id), timer_(timer) {}
  virtual ~ActionBase() = default;

  [[nodiscard]] OperationId id() const { return id_; }

  /// True when the operation has no io_uring opcode and is executed synchronously
  /// by the Reactor when the current batch is processed.
  [[nodiscard]] virtual bool is_inline() const { return false; }

  /// Executes an inline operation, returning the raw result code.
  virtual std::int32_t execute_inline() { return -ENOSYS; }

  /// Fills the SQE mirror (sqe()) from the stored operation and buffer.
  virtual void fill() = 0;

  /// Dispatches the completion to the callback. res is the raw CQE result.
  virtual void complete(std::int32_t res) = 0;

  /// The SQE mirror of this action, copied onto the kernel ring by the Reactor.
  [[nodiscard]] Sqe& sqe() { return sqe_; }

  /// True once the operation is destined for the io_uring ring (not inline).
  [[nodiscard]] bool in_ring() const { return in_ring_; }
  void set_in_ring() { in_ring_ = true; }

  /// True once the SQE mirror has been copied onto the kernel submission queue.
  [[nodiscard]] bool sqe_submitted() const { return sqe_submitted_; }
  void set_sqe_submitted() { sqe_submitted_ = true; }

  /// True for timer operations, whose -ETIME completion means "fired".
  [[nodiscard]] bool is_timer() const { return timer_; }

private:
  OperationId id_;
  bool timer_;
  bool in_ring_ = false;
  bool sqe_submitted_ = false;
  Sqe sqe_{};
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

  Action(OperationId id, Op op, Buffer buffer, Callback callback, bool timer)
      : ActionBase(id, timer), op_(std::move(op)), buffer_(std::move(buffer)), callback_(std::move(callback)) {}

  [[nodiscard]] bool is_inline() const override { return is_inline_v<Op>; }

  std::int32_t execute_inline() override {
    if constexpr (std::is_same_v<Op, LseekOp>) {
      return detail::lseek_fd(op_.fd.value(), op_.offset, op_.whence);
    } else if constexpr (std::is_same_v<Op, ListenOp>) {
      return detail::listen_fd(op_.fd.value(), op_.backlog);
    } else if constexpr (std::is_same_v<Op, BindOp>) {
      return detail::bind_fd(op_.fd.value(), op_.addr.data(), op_.addr_len);
    } else {
      // only lseek, listen and bind are inline operations; anything else reaching here is a bug
      unreachable();
    }
  }

  void fill() override { fill_sqe(sqe()); }

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
      detail::statx_to_stat(reinterpret_cast<std::byte*>(buffer_.data()), buffer_bytes());
    }
    invoke(Result<Value>(make_value(res)), res);
  }

private:
  std::uint64_t buffer_bytes() const {
    return static_cast<std::uint64_t>(buffer_.size()) * sizeof(typename Buffer::value_type);
  }

  void set_buffer(Sqe& sqe, bool fixed) {
    sqe.addr = reinterpret_cast<std::uint64_t>(buffer_.data());
    sqe.len = static_cast<std::uint32_t>(buffer_bytes());
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
      sqe.addr = reinterpret_cast<std::uint64_t>(op_.path.c_str());
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
      set_buffer(sqe, std::is_same_v<Buffer, RegisteredBuffer>);
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
      set_buffer(sqe, std::is_same_v<Buffer, RegisteredBuffer>);
    } else if constexpr (std::is_same_v<Op, FstatOp>) {
      static const char k_empty_path[] = "";
      sqe.opcode = static_cast<std::uint8_t>(IoOp::Statx);
      sqe.fd = op_.fd.value();
      sqe.addr = reinterpret_cast<std::uint64_t>(k_empty_path);
      sqe.addr2 = reinterpret_cast<std::uint64_t>(buffer_.data());
      sqe.len = k_statx_basic_stats;
      sqe.op_flags = k_at_empty_path;
    } else if constexpr (std::is_same_v<Op, FsyncOp>) {
      sqe.opcode = static_cast<std::uint8_t>(IoOp::Fsync);
      sqe.fd = op_.fd.value();
      sqe.op_flags = op_.use_datasync ? k_fsync_datasync : 0;
    } else if constexpr (std::is_same_v<Op, ConnectOp>) {
      sqe.opcode = static_cast<std::uint8_t>(IoOp::Connect);
      sqe.fd = op_.fd.value();
      sqe.addr = reinterpret_cast<std::uint64_t>(op_.addr.data());
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
        op_.addr_len = static_cast<std::uint32_t>(buffer_bytes());
        sqe.addr = reinterpret_cast<std::uint64_t>(buffer_.data());
        sqe.addr2 = reinterpret_cast<std::uint64_t>(&op_.addr_len);
      }
    } else if constexpr (std::is_same_v<Op, SendOp>) {
      sqe.opcode = static_cast<std::uint8_t>(IoOp::Send);
      sqe.fd = op_.fd.value();
      sqe.op_flags = static_cast<std::uint32_t>(op_.flags);
      set_buffer(sqe, false);
    } else if constexpr (std::is_same_v<Op, RecvOp>) {
      sqe.opcode = static_cast<std::uint8_t>(IoOp::Recv);
      sqe.fd = op_.fd.value();
      sqe.op_flags = static_cast<std::uint32_t>(op_.flags);
      set_buffer(sqe, false);
    } else if constexpr (std::is_same_v<Op, TimeoutOp>) {
      sqe.opcode = static_cast<std::uint8_t>(IoOp::Timeout);
      sqe.addr = reinterpret_cast<std::uint64_t>(&op_.ts);
      sqe.len = 1;
      sqe.op_flags = 0;
    }
    // lseek, listen and bind have no io_uring opcode: they are executed inline.
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
};

} // namespace ringu
