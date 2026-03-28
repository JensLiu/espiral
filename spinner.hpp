#pragma once

#include "includes/common.h"
#include "mm/address_space_manager.hpp"
#include "mm/host_accelerator_interface.hpp"
#include "mm/memory_allocator.hpp"
#include "mm/scratchpad_memory.hpp"

#include <cassert>
#include <fstream>
#include <string>

namespace espiral {

template <typename AcceleratorType> class Spinner {
public:
  Spinner(uint64_t baseAddress, uint64_t capacity, uint32_t pageAlign,
          uint32_t blockAlign) {
    page_allocator_ =
        new MemoryAllocator(baseAddress, capacity, pageAlign, blockAlign);
    scratchpad_ = new ScratchpadMemory();
    aspace_ = new AddressSpaceManager(page_allocator_, scratchpad_);
    accelerator_ = new AcceleratorType();
  }

  ~Spinner() {
    // TODO: safety checks
    delete aspace_;
    delete scratchpad_;
    delete page_allocator_;
    delete accelerator_;
  }

  void start_kernel(kernel_id_t kid) {
    const auto kcb = kcbs_.at(kid).value();
    accelerator_->start(kcb.start_va, kcb.args_va, kcb.satp);
  }

  void wait_kernel(kernel_id_t kid, uint64_t timeout) {
    const auto kcb = kcbs_.at(kid).value();
    accelerator_->ready_wait(timeout);
  }

  void free_kernel(kernel_id_t kid) {
    const auto kcb = kcbs_.at(kid).value();
    aspace_->free_page_table(kcb.satp);
    free_kcb(kid);
  }

  auto allocate_kernel(std::string vxbin_path) -> kernel_id_t {
    const kernel_id_t kid = allocate_kcb();
    const satp_t satp = aspace_->allocate_page_table().value_or(0);
    if (satp == 0) {
      free_kcb(kid);
      throw std::runtime_error("Failed to allocate page table for kernel");
    }

    // parse vxbin and load segments to memory
    const auto vxbin_content = read_vxbin(vxbin_path);
    const auto *bytes =
        reinterpret_cast<const uint64_t *>(vxbin_content.data());
    const auto min_vma = *bytes++;
    const auto max_vma = *bytes++;
    const auto bin_size = vxbin_content.size() - 2 * 8;
    const auto runtime_size = (max_vma - min_vma);

    // memory allocation (reservation)
    // code segment
    aspace_->allocate_vm(min_vma, bin_size, satp, pte_flags::R);
    // global variable segment
    aspace_->allocate_vm(min_vma + bin_size, runtime_size - bin_size, satp,
                         pte_flags::R | pte_flags::W);

    KernelControlBlock kcb{
        .satp = satp,
        .start_va = 0x80000000,                    // < fixed
        .min_vma = static_cast<uint32_t>(min_vma), // < start of the kernel
        .max_vma = static_cast<uint32_t>(max_vma), // < end of the kernel
        .vxbin_path = vxbin_path,
        .args_va = 0,
    };

    kcbs_.at(kid) = kcb;

    upload_vxbin(kid);

    return kid;
  }

  void upload_bytes(kernel_id_t kid, const uint32_t base_va,
                    const void *content, size_t size) {
    const auto kcb = kcbs_.at(kid).value();
    const auto satp = kcb.satp;
    for (size_t offset = 0; offset < size; offset += PAGE_SIZE) {
      const auto chunk_size =
          std::min(static_cast<size_t>(PAGE_SIZE), size - offset);
      const auto va = base_va + offset;
      const auto pa = aspace_->translate_or_else_allocate(va, satp);
      accelerator_->upload(pa, (uint8_t *)content + offset, chunk_size);
    }
  }

  template <typename T> void upload_args(kernel_id_t kid, const T *args) {
    auto &kcb = kcbs_.at(kid).value();
    kcb.args_va = kcb.max_vma; // < place args at the end of the address space
    upload_bytes(kid, kcb.args_va, args, sizeof(T));
  }

  void upload_page_table(kernel_id_t kid) {
    const auto kcb = kcbs_.at(kid).value();
    const auto satp = kcb.satp;
    const auto pt_content = aspace_->dump_page_table(satp);
    upload_sparse_scratchpad(kid, pt_content);
  }

  void download(kernel_id_t kid, uint32_t va, void *dest, size_t size) {
    const auto kcb = kcbs_.at(kid).value();
    const auto satp = kcb.satp;
    for (size_t offset = 0; offset < size; offset += PAGE_SIZE) {
      const auto chunk_size =
          std::min(static_cast<size_t>(PAGE_SIZE), size - offset);
      const auto current_va = va + offset;
      const auto pa = aspace_->translate(current_va, satp).value_or(0);
      if (pa == 0) {
        throw std::runtime_error("Invalid virtual address");
      }
      accelerator_->download((uint8_t *)dest + offset, pa, chunk_size);
    }
  }

private:
  void upload_vxbin(kernel_id_t kid) {
    const auto kcb = kcbs_.at(kid).value();
    const auto vxbin_content = read_vxbin(kcb.vxbin_path);
    const auto *bytes =
        reinterpret_cast<const uint64_t *>(vxbin_content.data());
    const auto min_vma = *bytes++;
    const auto max_vma = *bytes++;
    assert(min_vma == kcb.min_vma);
    assert(max_vma == kcb.max_vma);
    upload_bytes(kid, min_vma, bytes, vxbin_content.size() - 2 * 8);
  }

  void upload_sparse_scratchpad(kernel_id_t kid,
                                const sparse_scratchpad_t &data) {
    const auto kcb = kcbs_.at(kid).value();
    for (const auto &[pa, content] : data) {
      accelerator_->upload(pa, content.data(), content.size());
    }
  }

  auto read_vxbin(const std::string &vxbin_path) -> std::vector<char> {
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

  struct KernelControlBlock {
    uint32_t satp;
    uint32_t start_va;
    uint32_t min_vma;
    uint32_t max_vma;
    std::string vxbin_path;
    uint32_t args_va;
  };

  auto allocate_kcb() -> kernel_id_t {
    std::lock_guard<std::mutex> lock(kcb_mutex_);
    for (size_t i = 0; i < kcbs_.size(); ++i) {
      if (!kcbs_.at(i).has_value()) {
        return i;
      }
    }
    kcbs_.emplace_back(KernelControlBlock{});
    return kcbs_.size() - 1;
  }

  void free_kcb(kernel_id_t kid) {
    std::lock_guard<std::mutex> lock(kcb_mutex_);
    kcbs_.at(kid).reset();
  }

  std::mutex kcb_mutex_;
  // TODO: protect each KCB with a mutex
  std::vector<std::optional<KernelControlBlock>> kcbs_;
  MemoryAllocator *page_allocator_;
  AddressSpaceManager *aspace_;
  ScratchpadMemory *scratchpad_; // < used only for page table storage (for now)
  HostAcceleratorInterface *accelerator_;
};

} // namespace espiral