#pragma once

#include "includes/espiral_common.h"
#include "logger.hpp"
#include "mm/address_space_manager.hpp"
#include "mm/allocator/vortex_memory_allocator.hpp"
#include "mm/heap_manager.hpp"
#include "mm/host_accelerator_interface.hpp"
#include "mm/scratchpad_memory.hpp"

#include <cassert>
#include <fstream>
#include <string>

namespace espiral {
class UploadBuffer {
  friend class Spinner;

private:
  const uint32_t va;
  const uint32_t device_max_size;
  const uint32_t flags;
  const void *host_ptr;
  size_t size;
  // created by the Spinner
  UploadBuffer(uint32_t va, uint32_t max_size, uint32_t flags = pte_flags::R | pte_flags::W)
      : va(va), device_max_size(max_size), flags(flags), host_ptr(nullptr), size(0) {}

public:
  uint32_t get_va() const { return va; }

  void set_content(const void *data, size_t data_size) {
    if (data_size > device_max_size) {
      throw std::runtime_error("Data size exceeds buffer capacity");
    }
    host_ptr = data;
    size = data_size;
  }
};

class DownloadBuffer {
  friend class Spinner;

private:
  const uint32_t va;
  const uint32_t read_size;
  void *host_ptr;

public:
  DownloadBuffer(uint32_t va, uint32_t read_size, void *host_ptr)
      : va(va), read_size(read_size), host_ptr(host_ptr) {}
};

class Spinner {
  struct KernelControlBlock {
    addr_t satp;
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
    // address space manager
    aspace_ = new AddressSpaceManager(new VortexMemoryAllocator(), new ScratchpadMemory());
  }

  ~Spinner() {
    // TODO: safety checks
    delete aspace_;
    delete accelerator_;
  }

  void start_kernel(kernel_id_t kid) {
    // mapping
    allocate_vxbin_segments_(kid);
    allocate_user_stack_(kid);
    allocate_io_pages_(kid);
    // upload
    upload_vxbin_(kid);
    upload_page_table_(kid);
    // debug: print the address mapping
    const auto kcb = kcbs_.at(kid).value();
    const auto mapping = aspace_->dump_address_mapping(kcb.satp);
    for (const auto &[va, pa] : mapping) {
      logger_.println("Mapping: 0x%x -> 0x%x", va, pa);
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
    aspace_->free_page_table(kcb.satp);
    free_kcb_(kid);
  }

  auto allocate_kernel(std::string vxbin_path) -> kernel_id_t {
    const kernel_id_t kid = allocate_kcb_();
    const auto satp_opt = aspace_->allocate_page_table();
    if (!satp_opt.has_value()) {
      free_kcb_(kid);
      throw std::runtime_error("Failed to allocate page table for kernel");
    }
    const satp_t satp = *satp_opt;

    // parse vxbin and load segments to memory
    const auto vxbin_content = read_vxbin_(vxbin_path);
    const auto *bytes =
        reinterpret_cast<const uint64_t *>(vxbin_content.data());
    const auto min_vma = *bytes++;
    const auto max_vma = *bytes++;
    const auto bin_size = vxbin_content.size() - 2 * 8;
    const auto runtime_size = (max_vma - min_vma);

    KernelControlBlock kcb{
        .satp = satp,
        .vxbin_path = vxbin_path,
        .vxbin_size = bin_size,
        .args_va = 0,
        .start_pc_va = 0x80000000,
        .kernel_end_va = static_cast<addr_t>(max_vma),
        .kernel_start_va = static_cast<addr_t>(min_vma),
        .heap = new HeapManager(aspace_, satp, (max_vma + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1)),
    };

    kcbs_.at(kid) = kcb;
    return kid;
  }

  // Allocate a data buffer in the kernel's virtual address space.
  // Returns the VA. Must be called before upload_page_table().
  auto allocate_upload_buffer(kernel_id_t kid, size_t size,
                              uint32_t flags = pte_flags::R | pte_flags::W) -> UploadBuffer {
    const auto kcb = kcbs_.at(kid).value();
    auto *heap_mngr = kcb.heap;
    const addr_t va = heap_mngr->allocate(size).value();
    // const uint32_t safe_flags = flags & (pte_flags::R | pte_flags::W);
    return UploadBuffer(va, size, pte_flags::R | pte_flags::W);
  }

  auto allocate_dev_buffer(kernel_id_t kid, size_t size, uint32_t flags = pte_flags::R | pte_flags::W) -> addr_t {
    return allocate_upload_buffer(kid, size, flags).va;
  }

  void upload(kernel_id_t kid, const UploadBuffer &buffer) {
    const auto kcb = kcbs_.at(kid).value();
    copy_host_to_dev_(kid, buffer.va, buffer.host_ptr, buffer.size);
  }

  void download(kernel_id_t kid, const DownloadBuffer &buffer) {
    copy_dev_to_host_(kid, buffer.va, buffer.host_ptr, buffer.read_size);
  }

  template <typename T>
  void upload_args(kernel_id_t kid, const T *args) {
    auto &kcb = kcbs_.at(kid).value();
    auto *heap_mngr = kcb.heap;
    kcb.args_va = heap_mngr->allocate(sizeof(T)).value();
    copy_host_to_dev_(kid, kcb.args_va, args, sizeof(T));
  }

private:
  void allocate_io_pages_(kernel_id_t kid) {
    const auto kcb = kcbs_.at(kid).value();
    const auto satp = kcb.satp;
    const auto io_size = IO_END_ADDR - IO_BASE_ADDR;
    aspace_->allocate_vm_pages(IO_BASE_ADDR, io_size, satp,
                               pte_flags::R | pte_flags::W);
  }

  void allocate_vxbin_segments_(kernel_id_t kid) {
    const auto kcb = kcbs_.at(kid).value();
    const auto satp = kcb.satp;
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
        aspace_->allocate_vm_pages(min_vma, last_code_page - min_vma, satp,
                                   pte_flags::R | pte_flags::W | pte_flags::X);
      }
      aspace_->allocate_one_vm_page(last_code_page, satp,
                                    pte_flags::R | pte_flags::W | pte_flags::X);
      const addr_t next_page = last_code_page + PAGE_SIZE;
      if (max_vma > next_page) {
        aspace_->allocate_vm_pages(next_page, max_vma - next_page, satp,
                                   pte_flags::R | pte_flags::W);
      }
    } else {
      // no code-data shared boundary
      const addr_t code_size_aligned = (bin_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
      aspace_->allocate_vm_pages(min_vma, code_size_aligned, satp,
                                 pte_flags::R | pte_flags::X);
      const addr_t data_va = min_vma + code_size_aligned;
      if (has_data && max_vma > data_va) {
        aspace_->allocate_vm_pages(data_va, max_vma - data_va, satp,
                                   pte_flags::R | pte_flags::W);
      }
    }
  }

  void allocate_user_stack_(kernel_id_t kid) {
    const auto kcb = kcbs_.at(kid).value();
    const size_t per_stack_size = 1 << STACK_LOG2_SIZE;
    // reserve maximum possible stack space
    const uint32_t stack_size = per_stack_size * NUM_THREADS * NUM_WARPS * NUM_CORES * NUM_CLUSTERS;
    const uint32_t stack_top_va = STACK_BASE_ADDR;
    const uint32_t stack_base_va = stack_top_va - stack_size;
    aspace_->allocate_vm_pages(stack_base_va, stack_size, kcb.satp,
                               pte_flags::R | pte_flags::W);
  }

  void upload_page_table_(kernel_id_t kid) {
    const auto kcb = kcbs_.at(kid).value();
    const auto satp = kcb.satp;
    const auto pt_content = aspace_->dump_page_table(satp);
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
    for (const auto &[pa, content] : data) {
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
    const auto satp = kcb.satp;
    size_t offset = 0;
    while (offset < size) {
      const uint32_t page_off = va & (PAGE_SIZE - 1);
      const size_t chunk_size = std::min(static_cast<size_t>(PAGE_SIZE - page_off), size - offset);
      const auto pa = aspace_->translate(va, satp).value();
      accelerator_->download((uint8_t *)dest + offset, pa, chunk_size);
      va += chunk_size;
      offset += chunk_size;
    }
  }

  void copy_host_to_dev_(kernel_id_t kid, uint32_t base_va,
                         const void *content, size_t size) {
    const auto kcb = kcbs_.at(kid).value();
    const auto satp = kcb.satp;
    size_t offset = 0;
    while (offset < size) {
      const uint32_t page_off = base_va & (PAGE_SIZE - 1);
      const size_t chunk_size = std::min(static_cast<size_t>(PAGE_SIZE - page_off), size - offset);
      const auto pa = aspace_->translate(base_va, satp).value();
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
    accelerator_->start(kcb.start_pc_va, kcb.args_va, kcb.satp);
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