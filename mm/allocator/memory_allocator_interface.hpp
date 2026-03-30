#pragma once

#include "../../includes/espiral_common.h"

#include <optional>

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
  virtual uint64_t get_capacity() const = 0;
  // memory management
  virtual std::optional<addr_t> allocate(size_t size) = 0;
  virtual bool release(addr_t addr) = 0;
  // allocator size
  virtual void grow_capacity(uint64_t additional_capacity) = 0;
  virtual bool shrink_capacity(uint64_t reduced_capacity) = 0;
  // transactional operations
  virtual void begin_transaction() = 0;
  virtual void end_transaction() = 0;
  std::optional<addr_t> atomic_allocate(size_t size) {
    begin_transaction();
    const auto res = allocate(size);
    end_transaction();
    return res;
  }
  bool atomic_release(addr_t addr) {
    begin_transaction();
    const auto res = release(addr);
    end_transaction();
    return res;
  }
};
} // namespace espiral