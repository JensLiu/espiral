#pragma once

#include "accelerator/host_accelerator_interface.hpp"
#include "includes/espiral_common.h"
#include "logger.hpp"
#include "mm/address_space_manager.hpp"
#include "mm/allocator/vortex_memory_allocator.hpp"
#include "mm/heap_manager.hpp"
#include "mm/scratchpad_memory.hpp"
#include "vortex.h"
#include <common.h> // IO_BASE_ADDR, STACK_BASE_ADDR, NUM_THREADS, NUM_WARPS, etc.

#include <cassert>
#include <cstdio>
#include <fstream>
#include <optional>
#include <string>
#include <cmath>

namespace espiral {
class Spinner {
  struct KernelControlBlock {
    addr_t top_pgtbl_pa;
    std::string vxbin_path;
    size_t vxbin_size;
    addr_t args_va;
    addr_t start_pc_va;
    // memory layout
    addr_t kernel_end_va;
    addr_t kernel_start_va;
    // heap virtual memory management
    HeapManager *heap;
  };

public:
  Spinner(HostAcceleratorInterface *accelerator) : accelerator_(accelerator), logger_("espiral::Spinner") {
    logger_.set_verbose(true);
    logger_.log("ctor begin, accelerator=%p", (void *)accelerator_);
    // address space manager
    logger_.log("creating AddressSpaceManager");
    aspace_ = new AddressSpaceManager(new VortexMemoryAllocator(), new ScratchpadMemory());
    logger_.log("AddressSpaceManager created=%p", (void *)aspace_);
  }

  ~Spinner() {
    // TODO: safety checks
    delete aspace_;
  }

  void start_kernel(kernel_id_t kid) {
    logger_.log("start_kernel begin, kid=%u, top_pgtbl_pa=%x", kid, kcbs_.at(kid).value().top_pgtbl_pa);
    // mapping
    allocate_vxbin_segments_(kid);
    allocate_user_stack_(kid);
    allocate_io_pages_(kid);
    // upload
    upload_vxbin_(kid);
    upload_page_table_(kid);
    // debug: print the address mapping
    const auto kcb = kcbs_.at(kid).value();
    const auto mapping = aspace_->dump_address_mapping(kcb.top_pgtbl_pa);
    for (const auto &[va, pa] : mapping) {
      logger_.log("Mapping: 0x%x -> 0x%x", va, pa);
    }

    start_kernel_(kid);
  }

  void wait_kernel(kernel_id_t kid, uint64_t timeout) {
    const auto kcb = kcbs_.at(kid).value();
    accelerator_->ready_wait(timeout);
  }

  void free_kernel(kernel_id_t kid) {
    const auto kcb = kcbs_.at(kid).value();
    delete kcb.heap;
    aspace_->free_page_table(kcb.top_pgtbl_pa);
    free_kcb_(kid);
  }

  auto allocate_kernel(std::string vxbin_path) -> kernel_id_t {
    const kernel_id_t kid = allocate_kcb_();
    const auto pgtbl_opt = aspace_->allocate_page_table();
    if (!pgtbl_opt.has_value()) {
      free_kcb_(kid);
      throw std::runtime_error("Failed to allocate page table for kernel");
    }
    const satp_t pgtbl_pa = *pgtbl_opt;
    logger_.log("Allocated page table with satp: %x for kernel id: %u", pgtbl_pa, kid);

    // parse vxbin and load segments to memory
    const auto vxbin_content = read_vxbin_(vxbin_path);
    const auto *bytes =
        reinterpret_cast<const uint64_t *>(vxbin_content.data());
    const auto min_vma = *bytes++;
    const auto max_vma = *bytes++;
    const auto bin_size = vxbin_content.size() - 2 * 8;
    const auto runtime_size = (max_vma - min_vma);

    KernelControlBlock kcb{
        .top_pgtbl_pa = pgtbl_pa,
        .vxbin_path = vxbin_path,
        .vxbin_size = bin_size,
        .args_va = 0,
        .start_pc_va = 0x80000000,
        .kernel_end_va = static_cast<addr_t>(max_vma),
        .kernel_start_va = static_cast<addr_t>(min_vma),
        .heap = new HeapManager(aspace_, pgtbl_pa, (max_vma + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1)),
    };

    kcbs_.at(kid) = kcb;
    return kid;
  }

  // Allocate a VA in the kernel's heap. Must be called before start_kernel().
  auto allocate_heap(kernel_id_t kid, size_t size,
                     uint32_t flags = pte_flags::R | pte_flags::W) -> addr_t {
    (void)flags; // HeapManager always maps R|W; reserved for future fine-grained control
    const auto kcb = kcbs_.at(kid).value();
    return kcb.heap->allocate(size).value();
  }

  void upload(kernel_id_t kid, addr_t va, const void *ptr, size_t size) {
    copy_host_to_dev_(kid, va, ptr, size);
  }

  void download(kernel_id_t kid, addr_t va, void *ptr, size_t size) {
    copy_dev_to_host_(kid, va, ptr, size);
  }

  void upload_args(kernel_id_t kid, const void *args, size_t size) {
    auto &kcb = kcbs_.at(kid).value();
    kcb.args_va = kcb.heap->allocate(size).value();
    copy_host_to_dev_(kid, kcb.args_va, args, size);
  }

private:
  void allocate_io_pages_(kernel_id_t kid) {
    const auto kcb = kcbs_.at(kid).value();
    const auto top_pgtbl_pa = kcb.top_pgtbl_pa;
    const auto io_size = IO_END_ADDR - IO_BASE_ADDR;
    aspace_->allocate_vm_pages(
      IO_BASE_ADDR,
      io_size,
      top_pgtbl_pa,
      pte_flags::R | pte_flags::W | pte_flags::U | pte_flags::A | pte_flags::D);
  }

  void allocate_vxbin_segments_(kernel_id_t kid) {
    const auto kcb = kcbs_.at(kid).value();
    const auto top_pgtbl_pa = kcb.top_pgtbl_pa;
    const auto min_vma = kcb.kernel_start_va;
    const auto max_vma = kcb.kernel_end_va;
    const auto bin_size = kcb.vxbin_size;

    // FIXME: different pages for code and data for finer access control
    //        This is not possible now because of the memory layout of Vortex
    //        since the .text and .data section are NOT seperated when compiled
    const addr_t code_end = min_vma + bin_size;
    const bool has_data = (max_vma > code_end);
    const addr_t last_code_page = (code_end - 1) & ~(PAGE_SIZE - 1);
    const bool shared_boundary = has_data && (last_code_page == (code_end & ~(PAGE_SIZE - 1)));

    if (shared_boundary) {
      if (last_code_page > min_vma) {
        aspace_->allocate_vm_pages(
            min_vma,
            last_code_page - min_vma,
            top_pgtbl_pa,
            pte_flags::R | pte_flags::W | pte_flags::X | pte_flags::U | pte_flags::A | pte_flags::D);
      }
      aspace_->allocate_one_vm_page(last_code_page, top_pgtbl_pa,
                                    pte_flags::R | pte_flags::W | pte_flags::X | pte_flags::U | pte_flags::A | pte_flags::D);
      const addr_t next_page = last_code_page + PAGE_SIZE;
      if (max_vma > next_page) {
        aspace_->allocate_vm_pages(
            next_page,
            max_vma - next_page,
            top_pgtbl_pa,
            pte_flags::R | pte_flags::W | pte_flags::U | pte_flags::A | pte_flags::D);
      }
    } else {
      // no code-data shared boundary
      const addr_t code_size_aligned = (bin_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
      aspace_->allocate_vm_pages(
          min_vma,
          code_size_aligned,
          top_pgtbl_pa,
          pte_flags::R | pte_flags::X | pte_flags::U | pte_flags::A);
      const addr_t data_va = min_vma + code_size_aligned;
      if (has_data && max_vma > data_va) {
        aspace_->allocate_vm_pages(
            data_va,
            max_vma - data_va,
            top_pgtbl_pa,
            pte_flags::R | pte_flags::W | pte_flags::U | pte_flags::A | pte_flags::D);
      }
    }
  }

  void allocate_user_stack_(kernel_id_t kid) {
    const auto kcb = kcbs_.at(kid).value();
    const uint64_t per_stack_size = (1ull << STACK_LOG2_SIZE);

    // mhartid is encoded as core/warp/thread bitfields in RTL.
    // Size stack by max encoded hart ID to avoid under-mapping sparse IDs.
    // look into `VX_csr_uint.sv` for mheartid encoding details
    const uint32_t num_threads = accelerator_->get_caps(VX_CAPS_NUM_THREADS).value_or( NUM_THREADS);
    const uint32_t num_warps = accelerator_->get_caps(VX_CAPS_NUM_WARPS).value_or(NUM_WARPS);
    const uint32_t num_cores = accelerator_->get_caps(VX_CAPS_NUM_CORES).value_or(NUM_CORES * NUM_CLUSTERS);
    // const uint32_t num_cores = 4;

    const uint32_t nt_bits = std::ceil(std::log2(num_threads));
    const uint32_t nw_bits = std::ceil(std::log2(num_warps));
    const uint64_t max_hartid = ((uint64_t)(num_cores - 1) << (nw_bits + nt_bits))
                              + ((uint64_t)(num_warps - 1) << nt_bits)
                              + (num_threads - 1);
    const uint64_t n_stack_slots = max_hartid + 1;
    const uint64_t stack_size_64 = per_stack_size * n_stack_slots;
    const uint64_t stack_top_va_64 = STACK_BASE_ADDR;

    const uint32_t stack_size = static_cast<uint32_t>(stack_size_64);
    const uint32_t stack_top_va = static_cast<uint32_t>(stack_top_va_64);
    const uint32_t stack_base_va = stack_top_va - stack_size;
    aspace_->allocate_vm_pages(
      stack_base_va,
      stack_size,
      kcb.top_pgtbl_pa,
      pte_flags::R | pte_flags::W | pte_flags::U | pte_flags::A | pte_flags::D);
    logger_.log("Allocated user stack for kernel id: %u, stack_base_va: %x, stack_top_va: %x, stack_size: %u bytes, slots: %u, cores=%u, warps=%u, threads=%u",
                kid, stack_base_va, stack_top_va, stack_size, (uint32_t)n_stack_slots, num_cores, num_warps, num_threads);
  }

  void upload_page_table_(kernel_id_t kid) {
    const auto kcb = kcbs_.at(kid).value();
    const auto top_pgtbl_pa = kcb.top_pgtbl_pa;
    const auto pt_content = aspace_->dump_page_table(top_pgtbl_pa);
    upload_sparse_scratchpad_(kid, pt_content);
  }

  void upload_vxbin_(kernel_id_t kid) {
    const auto kcb = kcbs_.at(kid).value();
    const auto vxbin_content = read_vxbin_(kcb.vxbin_path);
    const auto *bytes =
        reinterpret_cast<const uint64_t *>(vxbin_content.data());
    const auto min_vma = *bytes++;
    const auto max_vma = *bytes++;
    assert(min_vma == kcb.kernel_start_va);
    assert(max_vma == kcb.kernel_end_va);
    copy_host_to_dev_(kid, min_vma, bytes, vxbin_content.size() - 2 * 8);
  }

  void upload_sparse_scratchpad_(kernel_id_t kid,
                                 const sparse_scratchpad_t &data) {
    const auto kcb = kcbs_.at(kid).value();
    logger_.log("Uploading sparse scratchpad with %zu entries", data.size());
    for (const auto &[pa, content] : data) {
      logger_.log("Uploading to scratchpad: pa=%x, size=%zu", pa, content.size());
      accelerator_->upload(pa, content.data(), content.size());
    }
  }

  auto read_vxbin_(const std::string &vxbin_path) -> std::vector<char> {
    std::ifstream ifs(vxbin_path, std::ios::binary);
    if (!ifs) {
      throw std::runtime_error("Error: " + vxbin_path + " not found");
    }

    // read file content
    ifs.seekg(0, ifs.end);
    auto size = ifs.tellg();
    std::vector<char> content(size);
    ifs.seekg(0, ifs.beg);
    ifs.read(content.data(), size);
    return std::move(content);
  }

  auto allocate_kcb_() -> kernel_id_t {
    std::lock_guard<std::mutex> lock(kcb_mutex_);
    for (size_t i = 0; i < kcbs_.size(); ++i) {
      if (!kcbs_.at(i).has_value()) {
        return i;
      }
    }
    kcbs_.emplace_back(KernelControlBlock{});
    return kcbs_.size() - 1;
  }

  void free_kcb_(kernel_id_t kid) {
    std::lock_guard<std::mutex> lock(kcb_mutex_);
    kcbs_.at(kid).reset();
  }

  void copy_dev_to_host_(kernel_id_t kid, uint32_t va, void *dest, size_t size) {
    const auto kcb = kcbs_.at(kid).value();
    const auto top_pgtbl_pa = kcb.top_pgtbl_pa;
    size_t offset = 0;
    while (offset < size) {
      const uint32_t page_off = va & (PAGE_SIZE - 1);
      const size_t chunk_size = std::min(static_cast<size_t>(PAGE_SIZE - page_off), size - offset);
      const auto pa = aspace_->translate(va, top_pgtbl_pa).value();
      accelerator_->download((uint8_t *)dest + offset, pa, chunk_size);
      va += chunk_size;
      offset += chunk_size;
    }
  }

  void copy_host_to_dev_(kernel_id_t kid, uint32_t base_va,
                         const void *content, size_t size) {
    const auto kcb = kcbs_.at(kid).value();
    const auto top_pgtbl_pa = kcb.top_pgtbl_pa;
    size_t offset = 0;
    while (offset < size) {
      const uint32_t page_off = base_va & (PAGE_SIZE - 1);
      const size_t chunk_size = std::min(static_cast<size_t>(PAGE_SIZE - page_off), size - offset);
      const auto pa = aspace_->translate(base_va, top_pgtbl_pa).value();
      accelerator_->upload(pa, (const uint8_t *)content + offset, chunk_size);
      base_va += chunk_size;
      offset += chunk_size;
    }
  }

  auto page_align_down(uint32_t addr) -> uint32_t {
    return addr & ~(PAGE_SIZE - 1);
  }

  void start_kernel_(kernel_id_t kid) {
    const auto kcb = kcbs_.at(kid).value();
    accelerator_->start(kcb.start_pc_va, kcb.args_va, kcb.top_pgtbl_pa);
  }

  std::mutex kcb_mutex_;
  // TODO: protect each KCB with a mutex
  std::vector<std::optional<KernelControlBlock>> kcbs_;
  // Manages virtual page to physical page mappings
  AddressSpaceManager *aspace_;
  // Talks to the Accelerator
  HostAcceleratorInterface *accelerator_;
  Logger logger_;
};

} // namespace espiral