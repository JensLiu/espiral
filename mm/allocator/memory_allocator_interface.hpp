#pragma once

#include "../../includes/espiral_common.h"

#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <iostream>

namespace espiral {
class MemoryAllocatorInterface {
public:
  virtual ~MemoryAllocatorInterface() = default;
  // initialisations
  virtual void init_base_address(uint64_t base) = 0;
  virtual void init_capacity(uint64_t capacity) = 0;
  virtual void init_page_alignment(uint32_t align) = 0;
  virtual void init_block_alignment(uint32_t align) = 0;
  virtual addr_t get_base_address() const = 0;
  virtual auto get_capacity() const -> uint64_t = 0;
  // memory management
  // NOTE: currently we just ignore the hole 
  // (assuming the allocator will have side effects when trying to allocate and release addresses within holes)
  virtual auto allocate(size_t size) -> std::optional<addr_t> = 0;
  virtual auto release(addr_t addr) -> bool = 0;
  // allocator size
  virtual void set_growable(bool growable) = 0;
  virtual void grow_capacity(uint64_t additional_capacity) {
    throw std::runtime_error("This allocator does not support grow_capacity");
  };
  virtual auto shrink_capacity(uint64_t reduced_capacity) -> bool {
    throw std::runtime_error("This allocator does not support shrink_capacity");
  };
  // transactional operations
  virtual void begin_transaction() = 0;
  virtual void end_transaction() = 0;
  auto atomic_allocate(size_t size) -> std::optional<addr_t> {
    begin_transaction();
    const auto res = allocate(size);
    end_transaction();
    return res;
  }
  auto atomic_release(addr_t addr) -> bool {
    begin_transaction();
    const auto res = release(addr);
    end_transaction();
    return res;
  }
};
} // namespace espiral