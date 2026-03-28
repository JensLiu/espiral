#pragma once

#include "../mm/host_accelerator_interface.hpp"
#include "vortex.h"

#include <chrono>
#include <cstdint>
#include <future>
#include <unordered_map>

// simx `vx_device` from Vortex

namespace espiral {
class SimXDevice : public HostAcceleratorInterface {
public:
  SimXDevice()
      : arch_(NUM_THREADS, NUM_WARPS, NUM_CORES), ram_(0, MEM_PAGE_SIZE),
        processor_(arch_) {
    // attach memory module
    processor_.attach_ram(&ram_);
  }

  ~SimXDevice() {
    if (future_.valid()) {
      future_.wait();
    }
  }

  int upload(uint64_t dest_addr, const void *src, uint64_t size) override {
    uint64_t asize = aligned_size(size, CACHE_BLOCK_SIZE);
    if (dest_addr + asize > GLOBAL_MEM_SIZE)
      return -1;

    ram_.enable_acl(false);
    ram_.write((const uint8_t *)src, dest_addr, size);
    ram_.enable_acl(true);

    return 0;
  }

  int download(void *dest, uint64_t src_addr, uint64_t size) override {
    uint64_t asize = aligned_size(size, CACHE_BLOCK_SIZE);
    if (src_addr + asize > GLOBAL_MEM_SIZE)
      return -1;

    ram_.enable_acl(false);
    ram_.read((uint8_t *)dest, src_addr, size);
    ram_.enable_acl(true);

    /*DBGPRINT("download %ld bytes from 0x%lx\n", size, src_addr);
    for (uint64_t i = 0; i < size && i < 1024; i += 4) {
        DBGPRINT("  0x%lx -> 0x%x\n", src_addr + i, *(uint32_t*)((uint8_t*)dest
    + i));
    }*/

    return 0;
  }

  int start(uint64_t krnl_addr, uint64_t args_addr, uint64_t satp) override {
    // ensure prior run completed
    if (future_.valid()) {
      future_.wait();
    }

    // set kernel info
    this->dcr_write(VX_DCR_BASE_STARTUP_ADDR0, krnl_addr & 0xffffffff);
    this->dcr_write(VX_DCR_BASE_STARTUP_ADDR1, krnl_addr >> 32);
    this->dcr_write(VX_DCR_BASE_STARTUP_ARG0, args_addr & 0xffffffff);
    this->dcr_write(VX_DCR_BASE_STARTUP_ARG1, args_addr >> 32);
    this->dcr_write(VX_DCR_BASE_STARTUP_SATP0, satp & 0xffffffff);
    this->dcr_write(VX_DCR_BASE_STARTUP_SATP1, satp >> 32);

    // start new run
    future_ = std::async(std::launch::async, [&] { processor_.run(); });

    // clear mpm cache
    mpm_cache_.clear();

    return 0;
  }

  int ready_wait(uint64_t timeout) override {
    if (!future_.valid())
      return 0;
    uint64_t timeout_sec = timeout / 1000;
    std::chrono::seconds wait_time(1);
    for (;;) {
      // wait for 1 sec and check status
      auto status = future_.wait_for(wait_time);
      if (status == std::future_status::ready)
        break;
      if (0 == timeout_sec--)
        return -1;
    }
    return 0;
  }

  int dcr_write(uint32_t addr, uint32_t value) override {
    if (future_.valid()) {
      future_.wait(); // ensure prior run completed
    }
    processor_.dcr_write(addr, value);
    dcrs_.write(addr, value);
    return 0;
  }

  int dcr_read(uint32_t addr, uint32_t *value) const override {
    return dcrs_.read(addr, value);
  }

  int mpm_query(uint32_t addr, uint32_t core_id,
                uint64_t *value) const override {
    uint32_t offset = addr - VX_CSR_MPM_BASE;
    if (offset > 31)
      return -1;
    if (mpm_cache_.count(core_id) == 0) {
      uint64_t mpm_mem_addr = IO_MPM_ADDR + core_id * 32 * sizeof(uint64_t);
      CHECK_ERR(this->download(mpm_cache_[core_id].data(), mpm_mem_addr,
                               32 * sizeof(uint64_t)),
                { return err; });
    }
    *value = mpm_cache_.at(core_id).at(offset);
    return 0;
  }

private:
  int get_caps(uint32_t caps_id, uint64_t *value) {
    uint64_t _value;
    switch (caps_id) {
    case VX_CAPS_VERSION:
      _value = IMPLEMENTATION_ID;
      break;
    case VX_CAPS_NUM_THREADS:
      _value = NUM_THREADS;
      break;
    case VX_CAPS_NUM_WARPS:
      _value = NUM_WARPS;
      break;
    case VX_CAPS_NUM_CORES:
      _value = NUM_CORES * NUM_CLUSTERS;
      break;
    case VX_CAPS_CACHE_LINE_SIZE:
      _value = CACHE_BLOCK_SIZE;
      break;
    case VX_CAPS_GLOBAL_MEM_SIZE:
      _value = GLOBAL_MEM_SIZE;
      break;
    case VX_CAPS_LOCAL_MEM_SIZE:
      _value = (1 << LMEM_LOG_SIZE);
      break;
    case VX_CAPS_ISA_FLAGS:
      _value = ((uint64_t(MISA_EXT)) << 32) | ((log2floor(XLEN) - 4) << 30) |
               MISA_STD;
      break;
    case VX_CAPS_NUM_MEM_BANKS:
      _value = PLATFORM_MEMORY_NUM_BANKS;
      break;
    case VX_CAPS_MEM_BANK_SIZE:
      _value = 1ull << (MEM_ADDR_WIDTH / PLATFORM_MEMORY_NUM_BANKS);
      break;
    default:
      std::cout << "invalid caps id: " << caps_id << std::endl;
      std::abort();
      return -1;
    }
    *value = _value;
    return 0;
  }

  Arch arch_;
  RAM ram_;
  Processor processor_;
  DeviceConfig dcrs_;
  std::future<void> future_;
  std::unordered_map<uint32_t, std::array<uint64_t, 32>> mpm_cache_;
};
} // namespace espiral