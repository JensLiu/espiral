#pragma once

#include "address_space_manager.hpp"
#include "allocator/bump_allocator.hpp"
#include "allocator/memory_allocator_interface.hpp"
#include "allocator/vortex_memory_allocator.hpp"

#include <cstdio>

namespace espiral {
class HeapManager {

public:
  HeapManager(AddressSpaceManager *aspace, addr_t top_pgtbl_pa, addr_t base_va) : aspace_(aspace), top_pgtbl_pa_(top_pgtbl_pa), logger_("espiral::HeapManager") {
    logger_.set_verbose(true);
    vm_allocator_ = new BumpAllocator();
    vm_allocator_->init_base_address(base_va);
    vm_allocator_->init_capacity(0);
  }

  ~HeapManager() {
    // NOTE: The heap manager does NOT unmap or free any physical memories
    delete vm_allocator_;
  }

  auto allocate(size_t size) -> std::optional<addr_t> {
    // local allocation, no need for mutex
    if (const auto va = vm_allocator_->allocate(size); va.has_value()) {
      return *va;
    }
    const addr_t last_va = vm_allocator_->get_base_address() + vm_allocator_->get_capacity();
    const size_t pages_needed = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    logger_.log("Allocating %zu bytes, need %zu pages", size, pages_needed);
    for (size_t i = 0; i < pages_needed; ++i) {
      aspace_->allocate_one_vm_page(last_va + i * PAGE_SIZE, top_pgtbl_pa_, pte_flags::R | pte_flags::W);
    }
    logger_.log("Allocated %zu bytes", pages_needed * PAGE_SIZE);
    vm_allocator_->grow_capacity(pages_needed * PAGE_SIZE);
    return allocate(size);
  }

  auto free(addr_t addr, size_t size) -> bool {
    throw std::runtime_error("HeapManager::free() is not implemented yet");
  }

private:
  MemoryAllocatorInterface *vm_allocator_;
  addr_t top_pgtbl_pa_;
  AddressSpaceManager *aspace_;
  Logger logger_;
};
} // namespace espiral