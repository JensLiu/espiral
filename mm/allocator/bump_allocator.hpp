#pragma once

#include "memory_allocator_interface.hpp"

#include <mutex>
#include <stdexcept>

namespace espiral {
class BumpAllocator : public MemoryAllocatorInterface {
public:
  BumpAllocator() : baseAddress_(0), capacity_(0), nextAddress_(0) {}
  void init_base_address(uint64_t base) override {
    baseAddress_ = base;
    nextAddress_ = base;
  }
  void init_capacity(uint64_t capacity) override {
    capacity_ = capacity;
  }
  auto get_base_address() const -> addr_t override {
    return baseAddress_;
  }
  auto get_capacity() const -> uint64_t override {
    return capacity_;
  }
  void init_page_alignment(uint32_t /*align*/) override {}
  void init_block_alignment(uint32_t /*align*/) override {}
  void set_growable(bool growable) override {
    if (!growable_.has_value()) {
      growable_ = growable;
    } else if (growable != *growable_) {
      throw std::runtime_error("BumpAllocator: cannot change growable after initialization");
    }
  }
  void grow_capacity(uint64_t additional_capacity) override {
    if (growable_.has_value() && *growable_) {
      capacity_ += additional_capacity;
      return;
    }
    throw std::runtime_error("BumpAllocator: this instance does not support growing capacity");
  }
  bool shrink_capacity(uint64_t /*reduced_capacity*/) override {
    throw std::runtime_error("BumpAllocator does not support shrink_capacity");
  }
  void begin_transaction() override {
    mutex_.lock();
  }
  void end_transaction() override {
    mutex_.unlock();
  }
  auto allocate(size_t size) -> std::optional<addr_t> override {
    if (size == 0) {
      if (verbose_) {
        printf("espiral::BumpAllocator: Error: invalid arguments\n");
      }
      return std::nullopt;
    }
    if (nextAddress_ + size > baseAddress_ + capacity_) {
      if (verbose_) {
        printf("espiral::BumpAllocator: Error: out of memory - requested=0x%lx, next=0x%lx, capacity=0x%lx\n", size, nextAddress_, capacity_);
      }
      return std::nullopt;
    }
    addr_t addr = nextAddress_;
    nextAddress_ += size;
    return std::optional<addr_t>(addr);
  }
  bool release(addr_t /*addr*/) override {
    throw std::runtime_error("espiral::BumpAllocator: Error: does not support release");
  }

private:
  uint64_t baseAddress_;
  uint64_t capacity_;
  uint64_t nextAddress_;
  std::mutex mutex_;
  std::optional<bool> growable_ = std::nullopt;
  bool verbose_ = false;
};
} // namespace espiral