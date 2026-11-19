#pragma once

#include <cstddef>
#include <memory>
#include <memory_resource>
#include <type_traits>
#include <utility>

namespace dojio {

/// A move-only owning pointer that allocates its object through a
/// std::pmr::polymorphic_allocator.
///
/// BoxPtr is unique_ptr-like (move only, non copyable, default initializes to an
/// empty box, destroys the contained object with the box, dereference operators)
/// with two differences:
///  - it always stores the std::pmr::polymorphic_allocator<T> used to allocate the
///    object, and uses that same allocator to deallocate it, so the storage is
///    returned to the std::pmr::memory_resource it came from;
///  - it has no public constructor taking the boxed arguments: the object is
///    created with the static factory make(), whose first argument is the
///    allocator and whose remaining arguments are forwarded to T's constructor.
///
/// A BoxPtr<U> whose U* converts to T* (e.g. a derived class to its base) converts
/// to BoxPtr<T>. The allocation size and alignment recorded by make() are carried
/// over, so the deallocation always returns the exact storage that was allocated,
/// even when the box is destroyed through a base class pointer. The base class
/// must have a virtual destructor for the conversion to destroy the right object.
///
/// The boxed object must be a complete type when the BoxPtr is destroyed.
///
/// Example:
///   std::pmr::monotonic_buffer_resource resource;
///   std::pmr::polymorphic_allocator<Widget> allocator{&resource};
///   auto box = BoxPtr<Widget>::make(allocator, 42, "hello");
///   box->run();
template <typename T> class BoxPtr {
  static_assert(!std::is_void_v<T>, "BoxPtr requires a complete object type");
  static_assert(!std::is_array_v<T>, "BoxPtr does not support array types");
  static_assert(!std::is_const_v<T> && !std::is_volatile_v<T>, "BoxPtr requires a non cv-qualified object type");

public:
  using element_type = T;

  /// Default initialization: an empty box holding the default allocator.
  BoxPtr() noexcept = default;

  BoxPtr(BoxPtr&& other) noexcept
      : allocator_(other.allocator_), ptr_(std::exchange(other.ptr_, nullptr)), size_(std::exchange(other.size_, 0)),
        alignment_(std::exchange(other.alignment_, 0)) {}

  /// Converting move construction (e.g. BoxPtr<Derived> to BoxPtr<Base>): the
  /// boxed pointer and the recorded allocation size / alignment are transferred.
  template <typename U>
    requires(std::is_convertible_v<U*, T*> && !std::is_same_v<U, T>)
  BoxPtr(BoxPtr<U>&& other) noexcept
      : allocator_(other.allocator_.resource()), ptr_(std::exchange(other.ptr_, nullptr)),
        size_(std::exchange(other.size_, 0)), alignment_(std::exchange(other.alignment_, 0)) {}

  /// Move assignment: the current object is destroyed and deallocated with the
  /// currently stored allocator, then the allocator and pointer of other are taken.
  BoxPtr& operator=(BoxPtr&& other) noexcept {
    if (this != &other) {
      reset();
      // std::pmr::polymorphic_allocator is not assignable: rebuild it in place
      // from the allocator of other (which is a cheap memory_resource* handle).
      std::destroy_at(&allocator_);
      std::construct_at(&allocator_, other.allocator_);
      take(other);
    }
    return *this;
  }

  /// Converting move assignment (e.g. BoxPtr<Derived> to BoxPtr<Base>).
  template <typename U>
    requires(std::is_convertible_v<U*, T*> && !std::is_same_v<U, T>)
  BoxPtr& operator=(BoxPtr<U>&& other) noexcept {
    reset();
    std::destroy_at(&allocator_);
    std::construct_at(&allocator_, other.allocator_.resource());
    take(other);
    return *this;
  }

  BoxPtr(const BoxPtr&) = delete;
  BoxPtr& operator=(const BoxPtr&) = delete;

  ~BoxPtr() { reset(); }

  /// Creates a box whose object is allocated from allocator and constructed with
  /// the remaining arguments. On constructor failure the storage is returned to
  /// the allocator and the exception is propagated.
  template <typename... Args>
  [[nodiscard]] static BoxPtr make(std::pmr::polymorphic_allocator<T>& allocator, Args&&... args) {
    T* ptr = allocator.allocate(1);
    try {
      std::construct_at(ptr, std::forward<Args>(args)...);
    } catch (...) {
      allocator.deallocate(ptr, 1);
      throw;
    }
    return BoxPtr{allocator, ptr, sizeof(T), alignof(T)};
  }

  /// Pointer to the boxed object, nullptr when the box is empty (moved-from).
  [[nodiscard]] T* get() const noexcept { return ptr_; }

  /// Reference to the boxed object. Precondition: the box is not empty.
  [[nodiscard]] T& operator*() const noexcept { return *ptr_; }

  /// Pointer to the boxed object. Precondition: the box is not empty.
  [[nodiscard]] T* operator->() const noexcept { return ptr_; }

  [[nodiscard]] explicit operator bool() const noexcept { return ptr_ != nullptr; }

  /// Destroys and deallocates the boxed object; the box becomes empty.
  void reset() noexcept {
    if (ptr_ != nullptr) {
      std::destroy_at(ptr_);
      // deallocate the storage with the size / alignment recorded by make(): after
      // a derived-to-base conversion they differ from sizeof(T) / alignof(T)
      allocator_.resource()->deallocate(ptr_, size_, alignment_);
      ptr_ = nullptr;
    }
  }

  /// The allocator stored in the box, used for allocation and deallocation.
  [[nodiscard]] std::pmr::polymorphic_allocator<T> get_allocator() const noexcept { return allocator_; }

private:
  BoxPtr(std::pmr::polymorphic_allocator<T> allocator, T* ptr, std::size_t size, std::size_t alignment) noexcept
      : allocator_(allocator), ptr_(ptr), size_(size), alignment_(alignment) {}

  /// Steals the boxed pointer and its allocation metadata from other.
  template <typename U> void take(BoxPtr<U>& other) noexcept {
    ptr_ = std::exchange(other.ptr_, nullptr);
    size_ = std::exchange(other.size_, 0);
    alignment_ = std::exchange(other.alignment_, 0);
  }

  template <typename U> friend class BoxPtr;

  std::pmr::polymorphic_allocator<T> allocator_{};
  T* ptr_ = nullptr;
  std::size_t size_ = 0;      // allocation size recorded by make()
  std::size_t alignment_ = 0; // allocation alignment recorded by make()
};

} // namespace dojio