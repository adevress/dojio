// Non-Linux stub of the registered memory pool: registering fixed buffers needs
// io_uring, so create() and borrow() always fail with a NotSupported DojioError.

#include <dojio/memory_pool.hpp>

#include <dojio/contracts.hpp>

#include <fmt/format.h>

namespace dojio {

namespace {

DojioError not_supported_error() {
  return DojioError{ErrorOpcode::NotSupported,
                    fmt::format("dojio: RegisteredMemoryPool is not supported on this platform (io_uring requires "
                                "Linux)")};
}

} // namespace

RegisteredBuffer::RegisteredBuffer(RegisteredMemoryPool& pool, std::uint64_t index) : pool_(&pool), index_(index) {}

RegisteredBuffer::RegisteredBuffer(RegisteredBuffer&& other) noexcept : pool_(other.pool_), index_(other.index_) {
  other.pool_ = nullptr;
}

RegisteredBuffer& RegisteredBuffer::operator=(RegisteredBuffer&& other) noexcept {
  if (this != &other) {
    release();
    pool_ = other.pool_;
    index_ = other.index_;
    other.pool_ = nullptr;
  }
  return *this;
}

RegisteredBuffer::~RegisteredBuffer() { release(); }

void RegisteredBuffer::release() noexcept { pool_ = nullptr; }

std::byte* RegisteredBuffer::data() const {
  enforce_precondition(pool_ != nullptr);
  return nullptr;
}

std::uint64_t RegisteredBuffer::size() const { return 0; }

tl::expected<std::unique_ptr<RegisteredMemoryPool>, DojioError>
RegisteredMemoryPool::create(detail::IoUring&, std::uint64_t, std::uint64_t, std::pmr::memory_resource*) {
  return tl::unexpected(not_supported_error());
}

RegisteredMemoryPool::~RegisteredMemoryPool() = default;

tl::expected<RegisteredBuffer, DojioError> RegisteredMemoryPool::borrow() {
  return tl::unexpected(not_supported_error());
}

void RegisteredMemoryPool::return_buffer(std::uint64_t) noexcept {}

} // namespace dojio
