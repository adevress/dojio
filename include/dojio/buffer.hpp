#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace dojio {

/// Concept for the buffers accepted by the operations: std::vector, std::array,
/// StaticBuffer or RegisteredBuffer (any container of a trivially copyable value type
/// with data()/size()).
template <typename T>
concept BufferLike = requires(T& t) {
  typename T::value_type;
  { t.data() };
  { t.size() };
} && std::is_trivially_copyable_v<typename T::value_type>;

/// Size of a sockaddr_storage on Linux, used for the peer address of Accept.
inline constexpr std::uint64_t k_sockaddr_storage_size = 128;

/// A fixed-size buffer usable as an operation buffer.
///
/// StaticBuffer is an alternative to std::vector / std::array / RegisteredBuffer for
/// operations whose buffer size is known at compile time (e.g. the peer address of
/// an Accept operation). It satisfies the BufferLike concept accepted by
/// RingEngine::submit.
template <std::uint64_t N> class StaticBuffer {
public:
  using value_type = std::byte;
  static constexpr std::uint64_t static_size = N;

  StaticBuffer() = default;

  [[nodiscard]] std::byte* data() noexcept { return storage_.data(); }
  [[nodiscard]] const std::byte* data() const noexcept { return storage_.data(); }
  [[nodiscard]] static constexpr std::uint64_t size() noexcept { return N; }

  /// View of the whole buffer.
  [[nodiscard]] std::span<std::byte> span() noexcept { return std::span<std::byte>{storage_}; }
  /// View of the whole buffer.
  [[nodiscard]] std::span<const std::byte> span() const noexcept { return std::span<const std::byte>{storage_}; }

private:
  // Aligned so that a buffer holding a sockaddr_in / sockaddr_in6 (or any object) can be
  // reinterpreted safely; a plain std::byte array only guarantees alignment 1.
  alignas(std::max_align_t) std::array<std::byte, static_cast<std::size_t>(N)> storage_{};
};

/// Static buffer holding a sockaddr_storage, for the peer address of Accept.
using AcceptBuffer = StaticBuffer<k_sockaddr_storage_size>;

} // namespace dojio
