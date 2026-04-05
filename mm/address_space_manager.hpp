#pragma once

#include "../common/common.h"

#include "../includes/espiral_common.h"
#include "accelerator/host_accelerator_interface.hpp"
#include "allocator/memory_allocator_interface.hpp"
#include "hole_list.hpp"
#include "scratchpad_memory.hpp"

#include "logger.hpp"
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <unordered_map>

namespace espiral {

class PageFaultException : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

// The Address Space Manager maintains the page tables for all kernels
// It provides page-grained virtual memory management
class AddressSpaceManager {
public:
  AddressSpaceManager(MemoryAllocatorInterface *pageAllocator,
                      ScratchpadMemory *scratchpadMemory, HostAcceleratorInterface *accelerator)
      : page_allocator_(pageAllocator), scratchpad_(scratchpadMemory), logger_("espiral::AddressSpaceManager") {
    logger_.set_verbose(true);
    logger_.log("ctor begin, page_allocator=%p scratchpad=%p", (void *)page_allocator_, (void *)scratchpad_);
    page_allocator_->init_base_address(ALLOC_BASE_ADDR);
    logger_.log("init_base_address done");
    page_allocator_->init_capacity(GLOBAL_MEM_SIZE - ALLOC_BASE_ADDR);
    logger_.log("init_capacity done");
    page_allocator_->init_page_alignment(MEM_PAGE_SIZE);
    logger_.log("init_page_alignment done");
    page_allocator_->init_block_alignment(CACHE_BLOCK_SIZE);
    logger_.log("init_block_alignment done");
    logger_.log("Initialized with base address: %lx, capacity: %lx, page alignment: %d, block alignment: %d", ALLOC_BASE_ADDR, (GLOBAL_MEM_SIZE - ALLOC_BASE_ADDR), MEM_PAGE_SIZE, CACHE_BLOCK_SIZE);
    page_allocator_->set_growable(false);
    // reserve these regions so that we can do identity mapping for them later
    hole_list_.add_hole(IO_BASE_ADDR, IO_END_ADDR - IO_BASE_ADDR);
    hole_list_.add_hole(LMEM_BASE_ADDR, accelerator->get_caps(VX_CAPS_LOCAL_MEM_SIZE).value_or(0));
    logger_.log("Set hole list for IO and LMEM");
  }

  ~AddressSpaceManager() {
    delete page_allocator_;
    delete scratchpad_;
  }

  auto allocate_page_table() -> std::optional<addr_t> {
    if (auto pgtbl_pa = allocate_page_table_node()) {
      scratchpad_->write_atomic(*pgtbl_pa, std::vector<uint8_t>(PAGE_SIZE, 0));
      logger_.log("Allocated page table at physical address: %x", *pgtbl_pa);
      return *pgtbl_pa;
    }
    logger_.log("Failed to allocate page table");
    return std::nullopt;
  }

  void unsafe_map_identity(addr_t addr, uint32_t size, addr_t top_pgtbl_pa, uint32_t flags = pte_flags::R | pte_flags::W) {
    logger_.log("unsafe_map_identity: addr=%x, size=%x, top_pgtbl_pa=%x, flags=%x", addr, size, top_pgtbl_pa, flags);
    for (uint32_t offset = 0; offset < size; offset += PAGE_SIZE) {
      map_addr_inner(addr + offset, addr + offset, top_pgtbl_pa, flags);
    }
  }

  auto translate_or_else_allocate_page(uint32_t va, addr_t top_pgtbl_pa) -> int32_t {
    logger_.log("translate_or_else_allocate_page: va=%x, top_pgtbl_pa=%x", va, top_pgtbl_pa);
    const auto pa = translate(va, top_pgtbl_pa);
    if (!pa.has_value()) {
      allocate_one_vm_page(va, top_pgtbl_pa);
      return translate(va, top_pgtbl_pa).value();
    }
    return *pa;
  }

  auto translate(uint32_t va, addr_t top_pgtbl_pa) -> std::optional<uint32_t> {
    logger_.log("translate: va=%x, top_pgtbl_pa=%x", va, top_pgtbl_pa);
    try {
      return page_table_walk(va, top_pgtbl_pa);
    } catch (const PageFaultException &e) {
      return std::nullopt;
    }
  }

  void free_page_table(addr_t top_pgtbl_pa) {
    logger_.log("free_page_table: top_pgtbl_pa=%x", top_pgtbl_pa);
    free_page_table_inner(top_pgtbl_pa, PT_LEVELS - 1);
  }

  void allocate_vm_pages(uint32_t va, uint32_t size, addr_t top_pgtbl_pa,
                         uint32_t flags = pte_flags::R | pte_flags::W) {
    logger_.log("allocate_vm_pages: va=%x, size=%x, top_pgtbl_pa=%x, flags=%x", va, size, top_pgtbl_pa, flags);
    for (uint32_t offset = 0; offset < size; offset += PAGE_SIZE) {
      allocate_one_vm_page(va + offset, top_pgtbl_pa, flags);
    }
  }

  void allocate_one_vm_page(uint32_t va, addr_t top_pgtbl_pa, uint32_t flags = pte_flags::R | pte_flags::W) {
    logger_.log("allocate_one_vm_page: va=%x, top_pgtbl_pa=%x, flags=%x", va, top_pgtbl_pa, flags);
    auto pa = allocate_one_page().value_or(0);
    if (pa == 0) {
      throw std::runtime_error("Failed to allocate physical page for VM");
    }
    // check if the va is already mapped
    map_addr_inner(va, pa, top_pgtbl_pa, flags);
  }

  auto dump_page_table(addr_t top_pgtbl_pa) -> sparse_scratchpad_t {
    logger_.log("dump_page_table: top_pgtbl_pa=%x", top_pgtbl_pa);
    sparse_scratchpad_t dump;
    dump_page_table_inner(top_pgtbl_pa, dump);
    return dump;
  }

  auto dump_address_mapping(addr_t top_pgtbl_pa) -> std::vector<std::pair<addr_t, addr_t>> {
    logger_.log("dump_address_mapping: top_pgtbl_pa=%x", top_pgtbl_pa);
    std::vector<std::pair<addr_t, addr_t>> mapping;
    dump_address_mapping_inner(top_pgtbl_pa, PT_LEVELS - 1, 0, mapping);
    return mapping;
  }

  void dump_address_mapping_inner(uint32_t node_pa, int level, addr_t va_prefix,
                                  std::vector<std::pair<addr_t, addr_t>> &mapping) {
    const auto current_node = scratchpad_->read_atomic(node_pa, PAGE_SIZE);
    for (size_t i = 0; i < PAGE_SIZE; i += PTE_SIZE) {
      uint32_t pte = *reinterpret_cast<const uint32_t *>(&current_node[i]);
      if (pte_valid(pte)) {
        const uint32_t vpn_idx = i / PTE_SIZE;
        const addr_t va = va_prefix | (vpn_idx << (12 + level * VPN_BITS));
        if (pte_is_leaf(pte)) {
          mapping.push_back({va, pte_pa(pte)});
        } else {
          dump_address_mapping_inner(pte_pa(pte), level - 1, va, mapping);
        }
      }
    }
  }

private:
  auto read_pte(uint32_t addr) -> uint32_t {
    auto bytes = scratchpad_->read(addr, PTE_SIZE);
    return *reinterpret_cast<const uint32_t *>(bytes.data());
  }

  auto read_pte_atomic(uint32_t addr) -> uint32_t {
    auto bytes = scratchpad_->read_atomic(addr, PTE_SIZE);
    return *reinterpret_cast<const uint32_t *>(bytes.data());
  }

  void write_pte(uint32_t addr, uint32_t pte) {
    std::vector<uint8_t> buf(PTE_SIZE);
    *reinterpret_cast<uint32_t *>(buf.data()) = pte;
    scratchpad_->write(addr, buf);
  }

  void map_addr_inner(uint32_t va, uint32_t pa, uint32_t top_pgtbl_pa,
                      uint32_t flags) {
    logger_.log("va: %x, pa: %x, top_pgtbl_pa: %x, flags: %x", va, pa, top_pgtbl_pa, flags);
    uint32_t current_pa = top_pgtbl_pa;

    scratchpad_->begin_transaction();
    for (int level = PT_LEVELS - 1; level > 0; --level) {
      const uint32_t vpn_idx = va_vpn(va, level);
      const uint32_t pte_addr = current_pa + vpn_idx * PTE_SIZE;
      uint32_t pte = read_pte(pte_addr);
      logger_.log("level: %d, vpn_idx: %d, pte_addr: %x, pte: %x", level, vpn_idx, pte_addr, pte);
      if (!pte_valid(pte)) {
        logger_.log("PTE not valid, allocating new page table node");
        const auto new_pa = allocate_page_table_node();
        if (!new_pa) {
          scratchpad_->end_transaction();
          throw std::runtime_error("Failed to allocate page table node");
        }
        scratchpad_->write(*new_pa, std::vector<uint8_t>(PAGE_SIZE, 0));
        pte = make_pte(*new_pa, pte_flags::V);
        write_pte(pte_addr, pte);
      }
      current_pa = pte_pa(pte);
    }

    const uint32_t vpn_idx = va_vpn(va, 0);
    const uint32_t pte_addr = current_pa + vpn_idx * PTE_SIZE;
    logger_.log("leaf_pte_addr: %x", pte_addr);
    uint32_t leaf_pte =
        make_pte(pa, flags | pte_flags::V | pte_flags::A | pte_flags::D);
    logger_.log("leaf_pte: %x", leaf_pte);
    if (pte_valid(read_pte(pte_addr))) {
      scratchpad_->end_transaction();
      // print the existing mapping for debugging
      uint32_t existing_pte = read_pte(pte_addr);
      const auto existing_pa = translate(va, top_pgtbl_pa);
      logger_.println("Error: VA %x is already mapped to PA %x with PTE %x", va, existing_pa.has_value() ? *existing_pa : 0, existing_pte);
      throw std::runtime_error("Virtual address already mapped");
    }
    write_pte(pte_addr, leaf_pte);
    scratchpad_->end_transaction();
  }

  auto page_table_walk(uint32_t va, uint32_t root_pa) -> uint32_t {
    uint32_t current_pa = root_pa;

    for (int level = PT_LEVELS - 1; level >= 0; --level) {
      const uint32_t vpn_idx = va_vpn(va, level);
      const uint32_t pte_addr = current_pa + vpn_idx * PTE_SIZE;
      uint32_t pte = read_pte_atomic(pte_addr);

      if (!pte_valid(pte)) {
        throw PageFaultException("Invalid PTE: valid bit not set");
      }
      if (!(pte & pte_flags::R) && (pte & pte_flags::W)) {
        throw PageFaultException("Invalid PTE: W set without R");
      }

      if (pte_is_leaf(pte)) {
        if (!(pte & pte_flags::R)) {
          throw PageFaultException("Leaf PTE not readable");
        }
        if (level > 0) {
          uint32_t ppn_low_mask = (1u << (level * VPN_BITS)) - 1;
          if (pte_ppn(pte) & ppn_low_mask) {
            throw PageFaultException("Misaligned superpage");
          }
          uint32_t offset_mask = (1u << (12 + level * VPN_BITS)) - 1;
          return (pte_pa(pte) & ~offset_mask) | (va & offset_mask);
        }
        return pte_pa(pte) | va_pgoff(va);
      }

      current_pa = pte_pa(pte);
    }
    throw PageFaultException("No leaf PTE found after walking all levels");
  }

  auto allocate_page_table_node() -> std::optional<uint32_t> {
    const uint64_t addr_64 = allocate_one_page().value_or(0);
    if (addr_64 != 0) {
      logger_.log("Allocated page table node at physical address: %x", addr_64);
      return static_cast<uint32_t>(addr_64);
    }
    return std::nullopt;
  }

  void free_page_table_inner(uint32_t pa, int level) {
    if (level < 0)
      return;

    auto page_data = scratchpad_->read_atomic(pa, PAGE_SIZE);
    for (size_t i = 0; i < PAGE_SIZE; i += PTE_SIZE) {
      uint32_t pte = *reinterpret_cast<const uint32_t *>(&page_data[i]);
      if (pte_valid(pte)) {
        if (!pte_is_leaf(pte)) {
          free_page_table_inner(pte_pa(pte), level - 1);
        } else {
          release_one_page(pte_pa(pte));
        }
      }
    }
    release_one_page(pa);
  }

  void dump_page_table_inner(uint32_t root_pa, sparse_scratchpad_t &dump) {
    const auto current_node = scratchpad_->read_atomic(root_pa, PAGE_SIZE);
    dump[root_pa] = current_node;
    for (size_t i = 0; i < PAGE_SIZE; i += PTE_SIZE) {
      uint32_t pte = *reinterpret_cast<const uint32_t *>(&current_node[i]);
      if (pte_valid(pte) && !pte_is_leaf(pte)) {
        dump_page_table_inner(pte_pa(pte), dump);
      }
    }
  }

  auto allocate_one_page() -> std::optional<addr_t> {
    const auto pa_opt = page_allocator_->atomic_allocate(PAGE_SIZE);
    if (!pa_opt.has_value()) {
      return std::nullopt;
    }
    // ignore allocation inside holes, but mark them as released as well
    if (hole_list_.addr_in_hole(*pa_opt)) {
      return page_allocator_->atomic_allocate(PAGE_SIZE);
    } else {
      return pa_opt;
    }
  }

  void release_one_page(addr_t addr) {
    // don't release addresses within the hole
    if (!hole_list_.addr_in_hole(addr)) {
      page_allocator_->atomic_release(addr);
    }
  }

  // SV32 configuration
public:
  // SV32 satp: bit[31]=1 (SV32 mode), bits[30:22]=ASID=0, bits[21:0]=PPN
  static auto make_satp_sv32(addr_t pgtbl_pa) -> uint32_t {
    return (1u << 31) | ((pgtbl_pa >> 12) & 0x3FFFFFu);
  }
  
  static auto from_satp_sv32(satp_t satp) -> addr_t {
    return (satp & 0x3FFFFFu) << 12;
  }

  static auto make_satp_sv64(addr_t pgtbl_pa) -> uint64_t {
    return (8ull << 60) | ((pgtbl_pa >> 12) & 0xFFFFFFFFFFFu);
  }

  static auto from_satp_sv64(satp_t satp) -> addr_t {
    return (satp & 0xFFFFFFFFFFFu) << 12;
  }

private:
  // SV32 PTE layout: PPN[31:10], flags[9:0]
  static auto make_pte(uint32_t pa, uint32_t flags) -> uint32_t {
    return ((pa >> 12) << 10) | (flags & 0x3FF);
  }
  static auto pte_pa(uint32_t pte) -> uint32_t {
    return ((pte >> 10) & 0x3FFFFF) << 12;
  }
  static auto pte_ppn(uint32_t pte) -> uint32_t {
    return (pte >> 10) & 0x3FFFFF;
  }
  static auto pte_valid(uint32_t pte) -> bool {
    return (pte & pte_flags::V) != 0;
  }
  static auto pte_is_leaf(uint32_t pte) -> bool {
    return (pte & (pte_flags::R | pte_flags::W | pte_flags::X)) != 0;
  }
  static auto va_vpn(uint32_t va, int level) -> uint32_t {
    return (va >> (12 + level * VPN_BITS)) & VPN_MASK;
  }
  static auto va_pgoff(uint32_t va) -> uint32_t { return va & 0xFFF; }

  MemoryAllocatorInterface *page_allocator_;
  ScratchpadMemory *scratchpad_;
  Logger logger_;
  HoleList hole_list_;
};
} // namespace espiral
