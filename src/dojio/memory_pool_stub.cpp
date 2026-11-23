// Non-Linux stub of the registered memory pool: registering fixed buffers needs
// io_uring, so create() and borrow() always fail with a NotSupported DojioError.

#include <dojio/memory_pool.hpp>

#include <dojio/contracts.hpp>

#include <fmt/format.h>

#include <utility>

namespace dojio {

namespace {

DojioError not_supported_error() {
  return DojioError{ErrorOpcode::NotSupported,
                    fmt::format("dojio: RegisteredMemoryPool is not supported on this platform (io_uring requires "
                                "Linux)")};
}

} // namespace

// The pool implementation (pimpl): the stub never creates one, create() always fails,
// so it carries no state.
class RegisteredMemoryPool::Impl {};

RegisteredMemoryPool::RegisteredMemoryPool(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

RegisteredMemoryPool::RegisteredMemoryPool(RegisteredMemoryPool&&) noexcept = default;

RegisteredMemoryPool& RegisteredMemoryPool::operator=(RegisteredMemoryPool&&) noexcept = default;

RegisteredMemoryPool::~RegisteredMemoryPool() = default;

RegisteredMemoryPool::Impl& RegisteredMemoryPool::impl() const noexcept { unreachable(); }

tl::expected<RegisteredMemoryPool, DojioError> RegisteredMemoryPool::create(detail::IoUring&, std::uint64_t,
                                                                            std::uint64_t) {
  return tl::unexpected(not_supported_error());
}

tl::expected<RegisteredBuffer, DojioError> RegisteredMemoryPool::borrow() {
  return tl::unexpected(not_supported_error());
}

std::uint64_t RegisteredMemoryPool::buffer_size() const noexcept { unreachable(); }

std::uint64_t RegisteredMemoryPool::capacity() const noexcept { unreachable(); }

std::uint64_t RegisteredMemoryPool::active_count() const noexcept { unreachable(); }

std::uint64_t RegisteredMemoryPool::available() const noexcept { unreachable(); }

std::byte* RegisteredMemoryPool::base() const noexcept { unreachable(); }

RegisteredBuffer::RegisteredBuffer(RegisteredMemoryPool::Impl& impl, std::uint64_t index) noexcept
    : impl_(&impl), index_(index) {}

RegisteredBuffer::RegisteredBuffer(RegisteredBuffer&& other) noexcept : impl_(other.impl_), index_(other.index_) {
  other.impl_ = nullptr;
}

RegisteredBuffer& RegisteredBuffer::operator=(RegisteredBuffer&& other) noexcept {
  if (this != &other) {
    release();
    impl_ = other.impl_;
    index_ = other.index_;
    other.impl_ = nullptr;
  }
  return *this;
}

RegisteredBuffer::~RegisteredBuffer() { release(); }

void RegisteredBuffer::release() noexcept { impl_ = nullptr; }

std::byte* RegisteredBuffer::data() const {
  enforce_precondition(impl_ != nullptr);
  return nullptr;
}

std::uint64_t RegisteredBuffer::size() const { return 0; }

} // namespace dojio
