#pragma once

#include "host_accelerator_interface.hpp"

#include "axictrl.h"
#include "logger.hpp"

#include <vortex.h>

#include <cstdint>
#include <optional>

// AFU Related definitions, generated
// #include <vortex_afu.h>

#define CMD_MEM_READ AFU_IMAGE_CMD_MEM_READ
#define CMD_MEM_WRITE AFU_IMAGE_CMD_MEM_WRITE
#define CMD_RUN AFU_IMAGE_CMD_RUN
#define CMD_DCR_WRITE AFU_IMAGE_CMD_DCR_WRITE

#define MMIO_BASE_ADDR AFU_IMAGE_MMIO_BASE_ADDR
#define MMIO_CMD_TYPE AFU_IMAGE_MMIO_CMD_TYPE
#define MMIO_CMD_ADDR AFU_IMAGE_MMIO_CMD_ADDR
#define MMIO_CMD_DATA AFU_IMAGE_MMIO_CMD_DATA
#define MMIO_CMD_SIZE AFU_IMAGE_MMIO_CMD_SIZE
#define MMIO_DATA_READ AFU_IMAGE_MMIO_DATA_READ
#define MMIO_STATUS AFU_IMAGE_MMIO_STATUS
#define MMIO_DEV_CAPS AFU_IMAGE_MMIO_DEV_CAPS
#define MMIO_DEV_CAPS_S AFU_IMAGE_MMIO_DEV_CAPS_S
#define MMIO_ISA_CAPS AFU_IMAGE_MMIO_ISA_CAPS
#define MMIO_ISA_CAPS_H AFU_IMAGE_MMIO_ISA_CAPS_S
#define MMIO_SCOPE_READ AFU_IMAGE_MMIO_SCOPE_READ
#define MMIO_SCOPE_WRITE AFU_IMAGE_MMIO_SCOPE_WRITE

#define STATE_IDLE AFU_IMAGE_STATE_IDLE
#define STATE_MEM AFU_IMAGE_STATE_MEM
#define STATE_RUN AFU_IMAGE_STATE_RUN
#define STATE_DCR AFU_IMAGE_STATE_DCR
#define STATE_BITS AFU_IMAGE_STATE_BITS

#define RAM_PAGE_SIZE 4096

#ifndef MEM_TRANSF_WIDTH
#define MEM_TRANSF_WIDTH (32 / 8)
#endif

// Memory
#define CACHE_BLOCK_SIZE    64
#define ALLOC_BASE_ADDR     CACHE_BLOCK_SIZE
#define ALLOC_MAX_ADDR      STARTUP_ADDR
#if (XLEN == 64)
#define GLOBAL_MEM_SIZE      0x200000000  // 8 GB
#else
#define GLOBAL_MEM_SIZE      0x100000000  // 4 GB
#endif

namespace espiral {

class SoCDevice : public HostAcceleratorInterface {
public:
  SoCDevice() : axi_(MMIO_BASE_ADDR) {
    // assume 8GB as default
    //   device->global_mem_size = GLOBAL_MEM_SIZE;

    // Load ISA CAPS
    isa_caps = read_register64(MMIO_ISA_CAPS).value();
    // Load device CAPS
    dev_caps = read_register64(MMIO_DEV_CAPS).value();

    device->global_mem = std::make_shared<vortex::MemoryAllocator>(
        ALLOC_BASE_ADDR, ALLOC_MAX_ADDR - ALLOC_BASE_ADDR, RAM_PAGE_SIZE, CACHE_BLOCK_SIZE);

    // ignore local memory allocation for now
    // uint64_t local_mem_size = get_caps(VX_CAPS_LOCAL_MEM_SIZE).value();

    // int err = dcr_initialize(device);
    // DO NOT initialise DCRs here

    logger_.log("device creation complete!\n");
  }

  auto close() -> int override {
    return 0;
  }
  auto get_caps(uint32_t caps_id) -> std::optional<uint64_t> {
    switch (caps_id) {
    case VX_CAPS_VERSION:
      *value = (device->dev_caps >> 0) & 0xff;
      break;
    case VX_CAPS_NUM_THREADS:
      *value = (device->dev_caps >> 8) & 0xff;
      break;
    case VX_CAPS_NUM_WARPS:
      *value = (device->dev_caps >> 16) & 0xff;
      break;
    case VX_CAPS_NUM_CORES:
      *value = (device->dev_caps >> 24) & 0xffff;
      break;
    case VX_CAPS_CACHE_LINE_SIZE:
      *value = CACHE_BLOCK_SIZE;
      break;
    case VX_CAPS_GLOBAL_MEM_SIZE:
      *value = device->global_mem_size;
      break;
    case VX_CAPS_LOCAL_MEM_SIZE:
      *value = 1ull << ((device->dev_caps >> 40) & 0xff);
      break;
    case VX_CAPS_KERNEL_BASE_ADDR:
      *value = (uint64_t(device->dcrs.read(VX_DCR_BASE_STARTUP_ADDR1)) << 32) |
               device->dcrs.read(VX_DCR_BASE_STARTUP_ADDR0);
      break;
    case VX_CAPS_ISA_FLAGS:
      *value = device->isa_caps;
      break;
    default:
      fprintf(stderr, "[VXDRV] Error: invalid caps id: %d\n", caps_id);
      std::abort();
      return -1;
    }
  }

  auto upload(addr_t dest_addr, const void *src, size_t size) -> int override {
    // ensure ready for new command
    if (ready_wait((vx_device_h)this, VX_MAX_TIMEOUT) != 0) {
      return -1;
    }

    for (uint64_t i = 0; i < size / MEM_TRANSF_WIDTH; ++i) {
      uint64_t value = src[i];
      write_register(MMIO_CMD_DATA, (uint32_t)value);
      write_register(MMIO_CMD_ADDR, (uint32_t)dest_addr + i * MEM_TRANSF_WIDTH);
      write_register(MMIO_CMD_SIZE, MEM_TRANSF_WIDTH);
      write_register(MMIO_CMD_TYPE, CMD_MEM_WRITE);
      // ensure transfer completed
      if (ready_wait((vx_device_h)this, VX_MAX_TIMEOUT) != 0) {
        return -1;
      }
    }
    return 0;
  }

  auto download(void *dest, addr_t src_addr, size_t size) -> int {
    // ensure ready for new command
    if (ready_wait((vx_device_h)this, VX_MAX_TIMEOUT) != 0) {
      return -1;
    }

    for (uint64_t i = 0; i < size / MEM_TRANSF_WIDTH; ++i) {
      uint64_t value;
      write_register(MMIO_CMD_ADDR, (uint32_t)src_addr + i * MEM_TRANSF_WIDTH);
      write_register(MMIO_CMD_SIZE, MEM_TRANSF_WIDTH);
      write_register(MMIO_CMD_TYPE, CMD_MEM_READ);
      // ensure transfer completed
      if (ready_wait((vx_device_h)this, VX_MAX_TIMEOUT) != 0) {
        return -1;
      }
      read_register(MMIO_DATA_READ, &value);
      dest[i] = value;
    }
    return 0;
  }

  auto start(addr_t krnl_addr, addr_t args_addr, addr_t top_pgtbl_pa) -> int {
  }

  auto ready_wait(size_t timeout) -> int = 0;
  auto dcr_write(addr_t addr, uint32_t value) -> int = 0;
  auto dcr_read(addr_t addr) -> std::optional<uint32_t> = 0;
  auto mpm_query(addr_t addr, uint32_t core_id) -> std::optional<uint64_t> = 0;
  ~SoCDevice() = default;

private:
  void write_register(addr_t addr, uint32_t value) {
    const auto res = axi_.write32(addr, value);
    if (res != 0) {
      logger_.error("Failed to write to register at address 0x%lx", addr);
    }
  }

  auto read_register(addr_t addr) -> std::optional<uint32_t> {
    auto value = uint32_t{};
    const auto res = axi_.read32(addr, &value);
    if (res != 0) {
      logger_.error("Failed to read from register at address 0x%lx", addr);
      return std::nullopt;
    }
    return value;
  }

  void write_register64(addr_t addr, uint64_t value) {
    const auto res = axi_.write64(addr, value);
    if (res != 0) {
      logger_.error("Failed to write to register at address 0x%lx", addr);
    }
  }

  auto read_register64(addr_t addr) -> std::optional<uint64_t> {
    auto value = uint64_t{};
    const auto res = axi_.read64(addr, &value);
    if (res != 0) {
      logger_.error("Failed to read from register at address 0x%lx", addr);
      return std::nullopt;
    }
    return value;
  }

  AxiCtrl axi_;
  DeviceConfig dcrs;
  uint64_t dev_caps;
  uint64_t isa_caps;
  Logger logger_;
};
} // namespace espiral