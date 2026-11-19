#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <stdexcept>
#include <utility>

#include <ringu/box_ptr.hpp>

namespace {

/// Counts allocation / deallocation on a backing resource, to check that the
/// allocator stored in the box is the one actually used.
class CountingResource final : public std::pmr::memory_resource {
public:
  explicit CountingResource(std::pmr::memory_resource* upstream = std::pmr::new_delete_resource())
      : upstream_(upstream) {}

  std::uint64_t allocations = 0;
  std::uint64_t deallocations = 0;
  std::size_t last_deallocation_size = 0;

private:
  void* do_allocate(std::size_t bytes, std::size_t alignment) override {
    ++allocations;
    return upstream_->allocate(bytes, alignment);
  }

  void do_deallocate(void* p, std::size_t bytes, std::size_t alignment) override {
    ++deallocations;
    last_deallocation_size = bytes;
    upstream_->deallocate(p, bytes, alignment);
  }

  [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
    return this == &other;
  }

  std::pmr::memory_resource* upstream_;
};

/// Counts live instances and records the constructor arguments it received.
struct Widget {
  static inline std::int32_t alive = 0;

  Widget(std::int32_t a, std::int32_t b) : sum(a + b) { ++alive; }
  explicit Widget(std::int32_t a) : sum(a) { ++alive; }
  ~Widget() { --alive; }

  [[nodiscard]] std::int32_t doubled() const { return sum * 2; }

  std::int32_t sum;
};

} // namespace

TEST_CASE("BoxPtr default initializes to an empty box") {
  ringu::BoxPtr<Widget> box;
  CHECK_FALSE(static_cast<bool>(box));
  CHECK(box.get() == nullptr);
}

TEST_CASE("BoxPtr::make allocates and forwards the arguments to the constructor") {
  CountingResource resource;
  std::pmr::polymorphic_allocator<Widget> allocator{&resource};

  CHECK(Widget::alive == 0);
  {
    auto box = ringu::BoxPtr<Widget>::make(allocator, 20, 22);

    CHECK(static_cast<bool>(box));
    CHECK(box.get() != nullptr);
    CHECK(Widget::alive == 1);
    CHECK(box->sum == 42);
    CHECK(box->doubled() == 84);
    CHECK((*box).sum == 42);

    CHECK(resource.allocations == 1);
    CHECK(resource.deallocations == 0);
  }
  CHECK(Widget::alive == 0);
  CHECK(resource.allocations == 1);
  CHECK(resource.deallocations == 1);
}

TEST_CASE("BoxPtr stores and exposes the allocator it was built with") {
  CountingResource resource;
  std::pmr::polymorphic_allocator<Widget> allocator{&resource};

  auto box = ringu::BoxPtr<Widget>::make(allocator, 7);
  CHECK(box.get_allocator().resource() == &resource);
}

TEST_CASE("BoxPtr move construction transfers ownership") {
  std::pmr::polymorphic_allocator<Widget> allocator{std::pmr::new_delete_resource()};

  auto source = ringu::BoxPtr<Widget>::make(allocator, 5);
  Widget* raw = source.get();

  ringu::BoxPtr<Widget> destination{std::move(source)};
  CHECK_FALSE(static_cast<bool>(source));
  CHECK(destination.get() == raw);
  CHECK(destination->sum == 5);
  CHECK(Widget::alive == 1);
}

TEST_CASE("BoxPtr move assignment destroys the previous object") {
  std::pmr::polymorphic_allocator<Widget> allocator{std::pmr::new_delete_resource()};

  auto first = ringu::BoxPtr<Widget>::make(allocator, 1);
  Widget* first_raw = first.get();
  auto second = ringu::BoxPtr<Widget>::make(allocator, 2);
  Widget* second_raw = second.get();

  CHECK(Widget::alive == 2);
  first = std::move(second);
  CHECK(Widget::alive == 1);
  CHECK_FALSE(static_cast<bool>(second));
  CHECK(first.get() == second_raw);
  CHECK(first.get() != first_raw);
  CHECK(first->sum == 2);
}

TEST_CASE("BoxPtr reset destroys and deallocates the object") {
  CountingResource resource;
  std::pmr::polymorphic_allocator<Widget> allocator{&resource};

  auto box = ringu::BoxPtr<Widget>::make(allocator, 3);
  CHECK(Widget::alive == 1);

  box.reset();
  CHECK_FALSE(static_cast<bool>(box));
  CHECK(box.get() == nullptr);
  CHECK(Widget::alive == 0);
  CHECK(resource.deallocations == 1);

  // reset on an empty box is a no-op
  box.reset();
  CHECK(resource.deallocations == 1);
}

TEST_CASE("BoxPtr::make returns the storage when the constructor throws") {
  struct Throwing {
    explicit Throwing(bool fail) {
      if (fail) {
        throw std::runtime_error{"boom"};
      }
    }
  };

  CountingResource resource;
  std::pmr::polymorphic_allocator<Throwing> allocator{&resource};

  CHECK_THROWS_AS(ringu::BoxPtr<Throwing>::make(allocator, true), std::runtime_error);
  CHECK(resource.allocations == 1);
  CHECK(resource.deallocations == 1);
}

namespace {
// A default-constructible type exercising the empty-argument make overload.
struct Empty {};
} // namespace

TEST_CASE("BoxPtr::make works without constructor arguments") {
  std::pmr::polymorphic_allocator<Empty> allocator{std::pmr::new_delete_resource()};
  auto box = ringu::BoxPtr<Empty>::make(allocator);
  CHECK(static_cast<bool>(box));
  CHECK(box.get() != nullptr);
}

namespace {
struct Base {
  virtual ~Base() = default;
  explicit Base(std::int32_t v) : value(v) {}
  std::int32_t value = 0;
};

struct Derived final : Base {
  explicit Derived(std::int32_t v) : Base(v) {}
  std::uint64_t payload[8]{};
};
} // namespace

TEST_CASE("BoxPtr converting move construction deallocates the concrete object size") {
  CountingResource resource;
  std::pmr::polymorphic_allocator<Derived> allocator{&resource};

  CHECK(sizeof(Derived) > sizeof(Base));
  {
    auto derived = ringu::BoxPtr<Derived>::make(allocator, 42);
    Derived* raw = derived.get();

    ringu::BoxPtr<Base> base{std::move(derived)};
    CHECK_FALSE(static_cast<bool>(derived));
    CHECK(base.get() == static_cast<Base*>(raw));
    CHECK(base->value == 42);
    CHECK(resource.allocations == 1);
  }
  CHECK(resource.deallocations == 1);
  // the storage is returned with the size of the derived object, not the base
  CHECK(resource.last_deallocation_size == sizeof(Derived));
}

TEST_CASE("BoxPtr converting move assignment deallocates the previous and concrete object sizes") {
  CountingResource resource;
  std::pmr::polymorphic_allocator<Derived> derived_allocator{&resource};
  std::pmr::polymorphic_allocator<Base> base_allocator{&resource};

  auto base = ringu::BoxPtr<Base>::make(base_allocator, 1);
  auto derived = ringu::BoxPtr<Derived>::make(derived_allocator, 2);
  Derived* raw = derived.get();

  base = std::move(derived);
  CHECK_FALSE(static_cast<bool>(derived));
  CHECK(base.get() == static_cast<Base*>(raw));
  CHECK(base->value == 2);
  // the previous Base was deallocated with sizeof(Base)
  CHECK(resource.deallocations == 1);
  CHECK(resource.last_deallocation_size == sizeof(Base));

  base.reset();
  CHECK(resource.deallocations == 2);
  CHECK(resource.last_deallocation_size == sizeof(Derived));
}
