#pragma once

#include "../includes/common.h"
#include "memory_allocator.hpp"
#include "scratchpad_memory.hpp"
#include <optional>
#include <stdexcept>

namespace espiral {

class PageFaultException : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

class AddressSpaceManager {
public:
  AddressSpaceManager(MemoryAllocator *pageAllocator,
                      ScratchpadMemory *scratchpadMemory)
      : page_allocator_(pageAllocator), scratchpad_(scratchpadMemory) {}

  auto allocate_page_table() -> std::optional<satp_t> {
    if (auto pgtbl_pa = allocate_page_table_node()) {
      scratchpad_->write_atomic(*pgtbl_pa, std::vector<uint8_t>(PAGE_SIZE, 0));
      return make_satp(*pgtbl_pa);
    }
    return std::nullopt;
  }

  void map_addr(uint32_t va, uint32_t pa, satp_t satp,
                uint32_t flags = pte_flags::R | pte_flags::W) {
    map_addr_inner(va, pa, from_satp(satp), flags);
  }

  auto translate_or_else_allocate(uint32_t va, satp_t satp) -> int32_t {
    const auto pa = translate(va, satp).value_or(0);
    if (pa == 0) {
      allocate_one_vm(va, satp);
      return translate(va, satp).value();
    }
    return pa;
  }

  auto translate(uint32_t va, satp_t satp) -> std::optional<uint32_t> {
    try {
      return page_table_walk(va, from_satp(satp));
    } catch (const PageFaultException &e) {
      return std::nullopt;
    }
  }

  void free_page_table(satp_t satp) {
    free_page_table_inner(from_satp(satp), PT_LEVELS - 1);
  }

  void allocate_vm(uint32_t va, uint32_t size, satp_t satp,
                   uint32_t flags = pte_flags::R | pte_flags::W) {
    for (uint32_t offset = 0; offset < size; offset += PAGE_SIZE) {
      allocate_one_vm(va + offset, satp, flags);
    }
  }

  void allocate_one_vm(uint32_t va, satp_t satp, uint32_t flags = pte_flags::R | pte_flags::W) {
    auto pa = page_allocator_->allocate_atomic(PAGE_SIZE).value_or(0);
    if (pa == 0) {
      throw std::runtime_error("Failed to allocate physical page for VM");
    }
    // check if the va is already mapped
    map_addr(va, pa, satp, flags);
  }

  auto dump_page_table(satp_t satp) -> sparse_scratchpad_t {
    sparse_scratchpad_t dump;
    dump_page_table_inner(from_satp(satp), dump);
    return dump;
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

  void map_addr_inner(uint32_t va, uint32_t pa, uint32_t pgtbl_pa,
                      uint32_t flags) {
    uint32_t current_pa = pgtbl_pa;

    scratchpad_->begin_transaction();
    for (int level = PT_LEVELS - 1; level > 0; --level) {
      const uint32_t vpn_idx = va_vpn(va, level);
      const uint32_t pte_addr = current_pa + vpn_idx * PTE_SIZE;
      uint32_t pte = read_pte(pte_addr);

      if (!pte_valid(pte)) {
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
    uint32_t leaf_pte =
        make_pte(pa, flags | pte_flags::V | pte_flags::A | pte_flags::D);
    if (pte_valid(read_pte(pte_addr))) {
      scratchpad_->end_transaction();
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
    const uint64_t addr_64 =
        page_allocator_->allocate_atomic(PAGE_SIZE).value_or(0);
    if (addr_64 != 0) {
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
      if (pte_valid(pte) && !pte_is_leaf(pte) && level > 0) {
        free_page_table_inner(pte_pa(pte), level - 1);
      }
    }
    page_allocator_->release_atomic(pa);
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
  
  // SV32 configuration

  static auto make_satp(uint32_t pgtbl_pa) -> satp_t { return pgtbl_pa; }
  static auto from_satp(satp_t satp) -> uint32_t { return satp; }

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

  MemoryAllocator *page_allocator_;
  ScratchpadMemory *scratchpad_;
};
} // namespace espiral
