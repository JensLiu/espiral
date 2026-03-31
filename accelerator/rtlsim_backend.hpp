#pragma once

#include "host_accelerator_interface.hpp"

#include <bitmanip.h>
#include <common.h>
#include <mem.h>
#include <optional>
#include <rtlsim/processor.h>
#include <vortex.h>
#include <iostream>
#include <chrono>
#include <cstdint>
#include <future>
#include <unordered_map>

namespace espiral {
class RtlSimDevice : public HostAcceleratorInterface {
public:
  RtlSimDevice()
      : ram_(0, RAM_PAGE_SIZE),
        global_mem_(ALLOC_BASE_ADDR,
                    GLOBAL_MEM_SIZE - ALLOC_BASE_ADDR,
                    RAM_PAGE_SIZE,
                    CACHE_BLOCK_SIZE) {
    processor_.attach_ram(&ram_);
  }

  auto init() -> int override {
    return 0;
  }

  auto close() -> int override {
    return 0;
  }

  auto get_caps(uint32_t caps_id) -> std::optional<uint64_t> override {
    switch (caps_id) {
    case VX_CAPS_VERSION:
      return IMPLEMENTATION_ID;
    case VX_CAPS_NUM_THREADS:
      return NUM_THREADS;
    case VX_CAPS_NUM_WARPS:
      return NUM_WARPS;
    case VX_CAPS_NUM_CORES:
      return NUM_CORES * NUM_CLUSTERS;
    case VX_CAPS_CACHE_LINE_SIZE:
      return CACHE_BLOCK_SIZE;
    case VX_CAPS_GLOBAL_MEM_SIZE:
      return GLOBAL_MEM_SIZE;
    case VX_CAPS_LOCAL_MEM_SIZE:
      return (1 << LMEM_LOG_SIZE);
    case VX_CAPS_ISA_FLAGS:
      return ((uint64_t(MISA_EXT)) << 32) | ((vortex::log2floor<uint32_t>(XLEN) - 4) << 30) |
             MISA_STD;
    case VX_CAPS_NUM_MEM_BANKS:
      return PLATFORM_MEMORY_NUM_BANKS;
    case VX_CAPS_MEM_BANK_SIZE:
      return 1ull << (MEM_ADDR_WIDTH / PLATFORM_MEMORY_NUM_BANKS);
    default:
      std::cout << "invalid caps id: " << caps_id << std::endl;
      std::abort();
      return std::nullopt;
    }
  }


  auto upload(addr_t dest_addr, const void *src, size_t size) -> int override {
    uint64_t asize = aligned_size(size, CACHE_BLOCK_SIZE);
    if (dest_addr + asize > GLOBAL_MEM_SIZE)
      return -1;

    ram_.enable_acl(false);
    ram_.write((const uint8_t *)src, dest_addr, size);
    ram_.enable_acl(true);

    return 0;
  }

  auto download(void *dest, addr_t src_addr, size_t size) -> int override {
    uint64_t asize = aligned_size(size, CACHE_BLOCK_SIZE);
    if (src_addr + asize > GLOBAL_MEM_SIZE)
      return -1;

    ram_.enable_acl(false);
    ram_.read((uint8_t *)dest, src_addr, size);
    ram_.enable_acl(true);

    return 0;
  }

  auto start(addr_t krnl_addr, addr_t args_addr, addr_t satp) -> int override {
    // ensure prior run completed
    if (future_.valid()) {
      future_.wait();
    }

    // set kernel info
    const auto krnl_addr64 = static_cast<uint64_t>(krnl_addr);
    const auto args_addr64 = static_cast<uint64_t>(args_addr);
    const auto satp64 = static_cast<uint64_t>(satp);
    this->dcr_write(VX_DCR_BASE_STARTUP_ADDR0, krnl_addr64 & 0xffffffff);
    this->dcr_write(VX_DCR_BASE_STARTUP_ADDR1, krnl_addr64 >> 32);
    this->dcr_write(VX_DCR_BASE_STARTUP_ARG0, args_addr64 & 0xffffffff);
    this->dcr_write(VX_DCR_BASE_STARTUP_ARG1, args_addr64 >> 32);
    this->dcr_write(VX_DCR_BASE_SATP_BASE_ADDR0, satp64 & 0xffffffff);
    this->dcr_write(VX_DCR_BASE_SATP_BASE_ADDR1, satp64 >> 32);

    // start new run
    future_ = std::async(std::launch::async, [&] {
      processor_.run();
    });

    // clear mpm cache
    mpm_cache_.clear();

    return 0;
  }

  auto ready_wait(size_t timeout) -> int override {
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

  auto dcr_write(addr_t addr, uint32_t value) -> int override {
    if (future_.valid()) {
      future_.wait(); // ensure prior run completed
    }
    processor_.dcr_write(addr, value);
    dcrs_.write(addr, value);
    return 0;
  }

  auto dcr_read(addr_t addr) -> std::optional<uint32_t> override {
    uint32_t value = 0;
    if (dcrs_.read(addr, &value)) {
      return value;
    }
    return std::nullopt;
  }

  auto mpm_query(addr_t addr, uint32_t core_id) -> std::optional<uint64_t> override {
    uint32_t offset = addr - VX_CSR_MPM_BASE;
    if (offset > 31)
      return std::nullopt;
    if (mpm_cache_.count(core_id) == 0) {
      uint64_t mpm_mem_addr = IO_MPM_ADDR + core_id * 32 * sizeof(uint64_t);
      CHECK_ERR(this->download(mpm_cache_[core_id].data(), mpm_mem_addr, 32 * sizeof(uint64_t)), {
        return std::nullopt;
      });
    }
    return mpm_cache_.at(core_id).at(offset);
  }

private:
  vortex::RAM ram_;
  vortex::rtlsim::Processor processor_;
  vortex::MemoryAllocator global_mem_;
  DeviceConfig dcrs_;
  std::future<void> future_;
  std::unordered_map<uint32_t, std::array<uint64_t, 32>> mpm_cache_;
};
} // namespace espiral