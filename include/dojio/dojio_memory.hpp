#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <type_traits>
#include <utility>

namespace dojio {

namespace detail {

/// Reinterprets the object representation of from as To, without reinterpret_cast.
///
/// Both types must be trivially copyable and of the same size. This is how the library
/// views io_uring ABI data (kernel `__u64` address fields, the mmap'd ring pages)
/// through a different type.
template <typename To, typename From> [[nodiscard]] constexpr To uring_cast(From from) noexcept {
  static_assert(std::is_trivially_copyable_v<From>, "uring_cast requires a trivially copyable source");
  static_assert(std::is_trivially_copyable_v<To>, "uring_cast requires a trivially copyable target");
  static_assert(sizeof(To) == sizeof(From), "uring_cast requires same-size types");
  return std::bit_cast<To>(from);
}

} // namespace detail

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

namespace detail {

/// Header of a SharedValue allocation: the reference count, the memory resource the
/// single allocation came from, and the size / alignment needed to return it. The boxed
/// object is constructed in the same allocation, right after this header.
struct SharedControl {
  std::uint64_t count = 1;
  std::pmr::memory_resource* resource = nullptr;
  std::size_t size = 0;
  std::size_t alignment = 0;
};

/// Offset of the boxed object inside a SharedValue allocation, rounded up so the object
/// is correctly aligned after the control header.
template <typename T> [[nodiscard]] constexpr std::size_t shared_object_offset() noexcept {
  return (sizeof(SharedControl) + alignof(T) - 1U) / alignof(T) * alignof(T);
}

} // namespace detail

/// A move-only reference-counted owning pointer, the shared counterpart of BoxPtr.
///
/// SharedValue uses the same allocation model as BoxPtr (created with the static
/// factory make(), whose first argument is the std::pmr memory resource to allocate
/// from and whose remaining arguments are forwarded to T's constructor), but it adds a
/// non-atomic reference count. A single memory region is allocated for the reference
/// count, the memory resource it came from and the boxed object itself; the object is
/// constructed inside that region and destroyed with the last reference.
///
/// Copying shares the boxed object: the copy constructor and the copy assignment
/// increment the reference count without touching the object, and the last reference
/// destroys and deallocates it. Moves transfer the reference without touching the count.
///
/// A SharedValue<U> whose U* converts to T* (e.g. a derived class to its base)
/// converts to SharedValue<T>, and the recorded allocation is always returned to the
/// resource that produced it. The base class must have a virtual destructor for the
/// conversion to destroy the right object.
///
/// The reference count is not atomic: a SharedValue (and the objects it points to) is
/// not thread safe.
///
/// Example:
///   std::pmr::monotonic_buffer_resource resource;
///   auto box = SharedValue<Widget>::make(&resource, 42);
///   auto shared = box;
///   box.reset(); // the object stays alive through shared
template <typename T> class SharedValue {
  static_assert(!std::is_void_v<T>, "SharedValue requires a complete object type");
  static_assert(!std::is_array_v<T>, "SharedValue does not support array types");
  static_assert(!std::is_const_v<T> && !std::is_volatile_v<T>, "SharedValue requires a non cv-qualified object type");

public:
  using element_type = T;

  /// Default initialization: an empty box.
  SharedValue() noexcept = default;

  SharedValue(SharedValue&& other) noexcept
      : control_(std::exchange(other.control_, nullptr)), ptr_(std::exchange(other.ptr_, nullptr)) {}

  /// Converting move construction (e.g. SharedValue<Derived> to SharedValue<Base>):
  /// the reference passes to the base box without touching the reference count.
  template <typename U>
    requires(std::is_convertible_v<U*, T*> && !std::is_same_v<U, T>)
  SharedValue(SharedValue<U>&& other) noexcept
      : control_(std::exchange(other.control_, nullptr)), ptr_(std::exchange(other.ptr_, nullptr)) {}

  /// Move assignment: the current reference is dropped, then the reference of other is
  /// taken.
  SharedValue& operator=(SharedValue&& other) noexcept {
    if (this != &other) {
      reset();
      control_ = std::exchange(other.control_, nullptr);
      ptr_ = std::exchange(other.ptr_, nullptr);
    }
    return *this;
  }

  /// Converting move assignment (e.g. SharedValue<Derived> to SharedValue<Base>).
  template <typename U>
    requires(std::is_convertible_v<U*, T*> && !std::is_same_v<U, T>)
  SharedValue& operator=(SharedValue<U>&& other) noexcept {
    reset();
    control_ = std::exchange(other.control_, nullptr);
    ptr_ = std::exchange(other.ptr_, nullptr);
    return *this;
  }

  SharedValue(const SharedValue& other) noexcept : control_(other.control_), ptr_(other.ptr_) {
    if (control_ != nullptr) {
      control_->count += 1;
    }
  }

  /// Copy assignment shares the object of other, dropping the current reference.
  SharedValue& operator=(const SharedValue& other) noexcept {
    if (this != &other) {
      reset();
      control_ = other.control_;
      ptr_ = other.ptr_;
      if (control_ != nullptr) {
        control_->count += 1;
      }
    }
    return *this;
  }

  ~SharedValue() { reset(); }

  /// Creates an object in a single allocation holding the reference count, the memory
  /// resource and the object itself, constructed with the remaining arguments. resource
  /// is the std::pmr memory resource the single allocation is made from (e.g.
  /// std::pmr::new_delete_resource(), a monotonic resource or a pool). On constructor
  /// failure the storage is returned to the resource and the exception is propagated.
  template <typename... Args>
  [[nodiscard]] static SharedValue make(std::pmr::memory_resource* resource, Args&&... args) {
    constexpr std::size_t offset = detail::shared_object_offset<T>();
    constexpr std::size_t total = offset + sizeof(T);
    constexpr std::size_t alignment =
        alignof(T) > alignof(detail::SharedControl) ? alignof(T) : alignof(detail::SharedControl);
    void* block = resource->allocate(total, alignment);
    auto* control = std::construct_at(static_cast<detail::SharedControl*>(block));
    control->count = 1;
    control->resource = resource;
    control->size = total;
    control->alignment = alignment;
    auto* object_memory = static_cast<std::byte*>(block) + offset;
    T* object = nullptr;
    try {
      object = std::construct_at(reinterpret_cast<T*>(object_memory), std::forward<Args>(args)...);
    } catch (...) {
      std::destroy_at(control);
      resource->deallocate(block, total, alignment);
      throw;
    }
    return SharedValue{control, object};
  }

  /// Pointer to the boxed object, nullptr when the box is empty (moved-from).
  [[nodiscard]] T* get() const noexcept { return ptr_; }

  /// Reference to the boxed object. Precondition: the box is not empty.
  [[nodiscard]] T& operator*() const noexcept { return *ptr_; }

  /// Pointer to the boxed object. Precondition: the box is not empty.
  [[nodiscard]] T* operator->() const noexcept { return ptr_; }

  [[nodiscard]] explicit operator bool() const noexcept { return ptr_ != nullptr; }

  /// Number of SharedValue sharing the boxed object (0 when empty).
  [[nodiscard]] std::uint64_t use_count() const noexcept { return control_ != nullptr ? control_->count : 0; }

  /// The memory resource the single allocation came from (nullptr when empty).
  [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
    return control_ != nullptr ? control_->resource : nullptr;
  }

  /// The allocator of the memory resource the object was allocated from.
  [[nodiscard]] std::pmr::polymorphic_allocator<T> get_allocator() const noexcept {
    return std::pmr::polymorphic_allocator<T>{resource()};
  }

  /// Drops a reference, destroying and deallocating the object with the last one.
  void reset() noexcept {
    if (control_ == nullptr) {
      return;
    }
    control_->count -= 1;
    if (control_->count == 0) {
      std::destroy_at(ptr_);
      control_->resource->deallocate(control_, control_->size, control_->alignment);
    }
    control_ = nullptr;
    ptr_ = nullptr;
  }

private:
  SharedValue(detail::SharedControl* control, T* ptr) noexcept : control_(control), ptr_(ptr) {}

  template <typename U> friend class SharedValue;

  detail::SharedControl* control_ = nullptr;
  T* ptr_ = nullptr;
};

} // namespace dojio
