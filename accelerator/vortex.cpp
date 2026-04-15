// Copyright © 2019-2023
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include "axictrl.h"
#include <algorithm>
#include <assert.h>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <list>
#include <malloc.h>
#include <memory>
#include <sstream>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <unordered_map>
#include <utils.h>
#include <vortex.h>

#include <VX_config.h>
#include <VX_types.h>
#include <vortex_afu.h>

#ifdef SCOPE
#include "scope.h"
#endif

#ifndef __rtems__
#include "baremetal.h"
#endif

///////////////////////////////////////////////////////////////////////////////

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

#ifndef NDEBUG
#define DBGPRINT(format, ...)                    \
  do {                                           \
    printf("[VXDRV] " format "", ##__VA_ARGS__); \
  } while (0)
#else
#define DBGPRINT(format, ...) ((void)0)
#endif

#define CHECK_HANDLE(handle, _expr, _cleanup)               \
  auto handle = _expr;                                      \
  if (handle == nullptr) {                                  \
    printf("[VXDRV] Error: '%s' returned NULL!\n", #_expr); \
    _cleanup                                                \
  }

#define CHECK_ERR(_expr, _cleanup)                                  \
  do {                                                              \
    auto err = _expr;                                               \
    if (err == 0)                                                   \
      break;                                                        \
    printf("[VXDRV] Error: '%s' returned %d!\n", #_expr, (int)err); \
    _cleanup                                                        \
  } while (false)

///////////////////////////////////////////////////////////////////////////////

class vx_device {
public:
  vx_device(uint64_t base_mmio_addr)
      : axi_(base_mmio_addr) {}

  ~vx_device() {
  }

  int write_register(uint64_t addr, uint64_t value) {
    CHECK_ERR(axi_.write32(addr, (uint32_t)value), { return -1; });
    DBGPRINT("*** write_register: addr=0x%x, value=0x%x\n", addr, value);
    return 0;
  }

  int read_register(uint64_t addr, uint64_t *value) {
    CHECK_ERR(axi_.read32(addr, (uint32_t *)value), { return -1; });
    DBGPRINT("*** read_register: addr=0x%x, value=0x%x\n", addr, *value);
    return 0;
  }

  int write_register64(uint64_t addr, uint64_t value) {
    CHECK_ERR(axi_.write64(addr, value), { return -1; });
    DBGPRINT("*** write_register: addr=0x%x, value=0x%lx\n", addr, value);
    return 0;
  }

  int read_register64(uint64_t addr, uint64_t *value) {
    CHECK_ERR(axi_.read64(addr, value), { return -1; });
    DBGPRINT("*** read_register: addr=0x%x, value=0x%lx\n", addr, *value);
    return 0;
  }
  int upload(uint64_t dev_addr, uint32_t *host_ptr, uint64_t asize) {
    // ensure ready for new command
    if (vx_ready_wait((vx_device_h)this, VX_MAX_TIMEOUT) != 0)
      return -1;

    for (uint64_t i = 0; i < asize / MEM_TRANSF_WIDTH; ++i) {
      uint64_t value = host_ptr[i];
      CHECK_ERR(write_register(MMIO_CMD_DATA, (uint32_t)value), { return -1; });
      CHECK_ERR(write_register(MMIO_CMD_ADDR, (uint32_t)dev_addr + i * MEM_TRANSF_WIDTH), { return -1; });
      CHECK_ERR(write_register(MMIO_CMD_SIZE, MEM_TRANSF_WIDTH), { return -1; });
      CHECK_ERR(write_register(MMIO_CMD_TYPE, CMD_MEM_WRITE), { return -1; });
      // ensure transfer completed
      if (vx_ready_wait((vx_device_h)this, VX_MAX_TIMEOUT) != 0)
        return -1;
    }
    return 0;
  }

  int download(uint32_t *host_ptr, uint64_t dev_addr, uint64_t asize) {
    // ensure ready for new command
    if (vx_ready_wait((vx_device_h)this, VX_MAX_TIMEOUT) != 0)
      return -1;

    for (uint64_t i = 0; i < asize / MEM_TRANSF_WIDTH; ++i) {
      uint64_t value;
      CHECK_ERR(write_register(MMIO_CMD_ADDR, (uint32_t)dev_addr + i * MEM_TRANSF_WIDTH), { return -1; });
      CHECK_ERR(write_register(MMIO_CMD_SIZE, MEM_TRANSF_WIDTH), { return -1; });
      CHECK_ERR(write_register(MMIO_CMD_TYPE, CMD_MEM_READ), { return -1; });
      // ensure transfer completed
      if (vx_ready_wait((vx_device_h)this, VX_MAX_TIMEOUT) != 0)
        return -1;

      CHECK_ERR(read_register(MMIO_DATA_READ, &value), { return -1; });
      host_ptr[i] = value;
    }
    return 0;
  }

  AxiCtrl axi_;
  std::shared_ptr<vortex::MemoryAllocator> global_mem;
  std::shared_ptr<vortex::MemoryAllocator> local_mem;
  DeviceConfig dcrs;
  uint64_t dev_caps;
  uint64_t isa_caps;
  uint64_t global_mem_size;
};

///////////////////////////////////////////////////////////////////////////////

extern int vx_dev_caps(vx_device_h hdevice, uint32_t caps_id, uint64_t *value) {
  if (nullptr == hdevice)
    return -1;

  auto device = ((vx_device *)hdevice);

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

  return 0;
}

extern int vx_dev_open(vx_device_h *hdevice) {
  if (nullptr == hdevice)
    return -1;

  vx_device *device;

  device = new vx_device(MMIO_BASE_ADDR);
  if (nullptr == device)
    return -1;

  {
    // assume 8GB as default
    device->global_mem_size = GLOBAL_MEM_SIZE;

    // Load ISA CAPS
    CHECK_ERR(device->read_register64(MMIO_ISA_CAPS, &device->isa_caps), {
      return -1;
    });
    // Load device CAPS
    CHECK_ERR(device->read_register64(MMIO_DEV_CAPS, &device->dev_caps), {
      return -1;
    });
  }

  device->global_mem = std::make_shared<vortex::MemoryAllocator>(
      ALLOC_BASE_ADDR, ALLOC_MAX_ADDR - ALLOC_BASE_ADDR, RAM_PAGE_SIZE, CACHE_BLOCK_SIZE);

  uint64_t local_mem_size = 0;
  vx_dev_caps(device, VX_CAPS_LOCAL_MEM_SIZE, &local_mem_size);
  if (local_mem_size <= 1) {
    device->local_mem = std::make_shared<vortex::MemoryAllocator>(
        SMEM_BASE_ADDR, local_mem_size, RAM_PAGE_SIZE, 1);
  }

  int err = dcr_initialize(device);
  if (err != 0) {
    delete device;
    return err;
  }

#ifdef SCOPE
  {
    scope_callback_t callback;
    callback.registerWrite = [](vx_device_h hdevice, uint64_t value) -> int {
      auto device = (vx_device *)hdevice;
      CHECK_ERR(device->write_register64(MMIO_SCOPE_WRITE, value), {
        return -1;
      });
      return 0;
    };
    callback.registerRead = [](vx_device_h hdevice, uint64_t *value) -> int {
      auto device = (vx_device *)hdevice;
      CHECK_ERR(device->read_register64(MMIO_SCOPE_READ, &value), {
        return -1;
      });
      return 0;
    };
    int ret = vx_scope_start(&callback, device, 0, -1);
    if (ret != 0) {
      delete device;
      return ret;
    }
  }
#endif

#ifdef DUMP_PERF_STATS
  perf_add_device(device);
#endif

  *hdevice = device;

  DBGPRINT("device creation complete!\n", NULL);
  return 0;
}

extern int vx_dev_close(vx_device_h hdevice) {
  if (nullptr == hdevice)
    return -1;

  auto device = ((vx_device *)hdevice);

#ifdef SCOPE
  vx_scope_stop(hdevice);
#endif

#ifdef DUMP_PERF_STATS
  perf_remove_device(hdevice);
#endif

  delete device;

  DBGPRINT("device destroyed!\n", NULL);

  return 0;
}

extern int vx_mem_alloc(vx_device_h hdevice, uint64_t size, int type, uint64_t *dev_addr) {
  if (nullptr == hdevice || nullptr == dev_addr || 0 == size)
    return -1;

  auto device = ((vx_device *)hdevice);
  if (type == VX_MEM_TYPE_GLOBAL) {
    return device->global_mem->allocate(size, dev_addr);
  } else if (type == VX_MEM_TYPE_LOCAL) {
    return device->local_mem->allocate(size, dev_addr);
  }
  return -1;
}

extern int vx_mem_free(vx_device_h hdevice, uint64_t dev_addr) {
  if (nullptr == hdevice)
    return -1;

  if (0 == dev_addr)
    return 0;

  auto device = ((vx_device *)hdevice);
  if (dev_addr >= SMEM_BASE_ADDR) {
    return device->local_mem->release(dev_addr);
  } else {
    return device->global_mem->release(dev_addr);
  }
}

extern int vx_mem_info(vx_device_h hdevice, int type, uint64_t *mem_free, uint64_t *mem_used) {
  if (nullptr == hdevice)
    return -1;

  auto device = ((vx_device *)hdevice);
  if (type == VX_MEM_TYPE_GLOBAL) {
    if (mem_free)
      *mem_free = device->global_mem->free();
    if (mem_used)
      *mem_used = device->global_mem->allocated();
  } else if (type == VX_MEM_TYPE_LOCAL) {
    if (mem_free)
      *mem_free = device->local_mem->free();
    if (mem_used)
      *mem_free = device->local_mem->allocated();
  } else {
    return -1;
  }
  return 0;
}

extern int vx_copy_to_dev(vx_device_h hdevice, uint64_t dev_addr, const void *host_ptr, uint64_t size) {
  if (nullptr == hdevice)
    return -1;

  auto device = (vx_device *)hdevice;

  // check alignment
  if (!is_aligned(dev_addr, CACHE_BLOCK_SIZE))
    return -1;

  auto asize = aligned_size(size, CACHE_BLOCK_SIZE);

  // bound checking
  if (dev_addr + asize > device->global_mem_size)
    return -1;

  CHECK_ERR(device->upload(dev_addr, (uint32_t *)host_ptr, asize), {
    return -1;
  });

  DBGPRINT("COPY_TO_DEV: dev_addr=0x%lx, host_addr=0x%lx, size=%ld bytes\n", dev_addr, (uintptr_t)host_ptr, asize);

  return 0;
}

extern int vx_copy_from_dev(vx_device_h hdevice, void *host_ptr, uint64_t dev_addr, uint64_t size) {
  if (nullptr == hdevice)
    return -1;

  auto device = (vx_device *)hdevice;

  // check alignment
  if (!is_aligned(dev_addr, CACHE_BLOCK_SIZE))
    return -1;

  auto asize = aligned_size(size, CACHE_BLOCK_SIZE);

  // bound checking
  if (dev_addr + asize > device->global_mem_size)
    return -1;

  CHECK_ERR(device->download((uint32_t *)host_ptr, dev_addr, size), {
    return -1;
  });

  DBGPRINT("COPY_FROM_DEV: dev_addr=0x%lx, host_addr=0x%lx, size=%ld bytes\n", dev_addr, (uintptr_t)host_ptr, size);

  return 0;
}

extern int vx_start(vx_device_h hdevice) {
  if (nullptr == hdevice)
    return -1;

  auto device = (vx_device *)hdevice;

  CHECK_ERR(device->write_register(MMIO_CMD_TYPE, CMD_RUN), {
    return -1;
  });

  DBGPRINT("START\n", NULL);

  return 0;
}

extern int vx_ready_wait(vx_device_h hdevice, uint64_t timeout) {
  if (nullptr == hdevice)
    return -1;

  auto device = (vx_device *)hdevice;

  struct timespec sleep_time;

#ifndef NDEBUG
  sleep_time.tv_sec = 1;
  sleep_time.tv_nsec = 0;
#else
  sleep_time.tv_sec = 0;
  sleep_time.tv_nsec = 1000000;
#endif

  // to milliseconds
  uint64_t sleep_time_ms = (sleep_time.tv_sec * 1000) + (sleep_time.tv_nsec / 1000000);

  for (;;) {
    uint64_t status = 0;
    CHECK_ERR(device->read_register(MMIO_STATUS, &status), {
      return -1;
    });
    status &= (0x01 << STATE_BITS) - 1;
    bool is_done = status == STATE_IDLE;
    if (is_done || 0 == timeout) {
      break;
    }
    nanosleep(&sleep_time, nullptr);

    timeout -= sleep_time_ms;
  };

  return 0;
}

extern int vx_dcr_write(vx_device_h hdevice, uint32_t addr, uint64_t value) {
  if (nullptr == hdevice)
    return -1;

  auto device = (vx_device *)hdevice;

  CHECK_ERR(device->write_register(MMIO_CMD_ADDR, addr), { return -1; });
  CHECK_ERR(device->write_register(MMIO_CMD_DATA, value), { return -1; });
  CHECK_ERR(device->write_register(MMIO_CMD_TYPE, CMD_DCR_WRITE), { return -1; });

  // save the value
  DBGPRINT("DCR_WRITE: addr=0x%x, value=0x%lx\n", addr, value);
  device->dcrs.write(addr, value);

  return 0;
}
