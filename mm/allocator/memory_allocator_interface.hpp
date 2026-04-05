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
  virtual auto allocate(size_t size) -> std::optional<addr_t> {
    std::cout << "MemoryAllocatorInterface::allocate called with size: " << size << std::endl;
    auto addr = _allocate_ignorant_of_holes(size);
    std::cout << "MemoryAllocatorInterface::allocate returned address: " << (addr.has_value() ? std::to_string(*addr) : "nullopt") << std::endl;
    if (!addr.has_value()) {
      return std::nullopt;
    }
    if (addr_within_hole(*addr)) {
      return _allocate_ignorant_of_holes(size);
    } else {
      return addr;
    }
  };
  virtual auto release(addr_t addr) -> bool {
    if (!addr_within_hole(addr)) {
      return _release_ignorant_of_holes(addr);
    }
    return true;
  };
  // allocator size
  virtual void set_growable(bool growable) = 0;
  virtual void grow_capacity(uint64_t additional_capacity) {
    throw std::runtime_error("This allocator does not support grow_capacity");
  };
  virtual auto shrink_capacity(uint64_t reduced_capacity) -> bool {
    throw std::runtime_error("This allocator does not support shrink_capacity");
  };
  // hole list
  virtual void set_hole_list(const std::unordered_map<addr_t, size_t> &holes) {
    throw std::runtime_error("This allocator does not support hole list");
  };
  virtual auto addr_within_hole(addr_t addr) -> bool {
    const auto hole_list = _get_hole_list();
    if (!hole_list.has_value()) {
      return false;  // < no holes
    }
    for (const auto &[hole_start, hole_size] : *hole_list) {
      if (addr >= hole_start && addr < (hole_start + hole_size)) {
        return true;
      }
    }
    return false;
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

protected:
  // internal hooks: not exposed on the public API
  // TODO: refactor to use strategy?
  virtual auto _allocate_ignorant_of_holes(size_t size) -> std::optional<addr_t> {
    throw std::runtime_error("Allocating ignorant of holes is not implemented");
  };
  virtual auto _release_ignorant_of_holes(addr_t addr) -> bool {
    throw std::runtime_error("Releasing ignorant of holes is not implemented");
  };
  // virtual auto _allocate_hole_aware(size_t size) -> std::optional<addr_t> {
  //   throw std::runtime_error("Hole-aware allocating is not implemented");
  // }
  // virtual auto _release_hole_aware(addr_t addr) -> bool {
  //   throw std::runtime_error("Hole-aware releasing is not implemented");
  // }
  virtual auto _get_hole_list() const -> std::optional<std::unordered_map<addr_t, size_t>> {
    return std::nullopt;
  };
};
} // namespace espiral