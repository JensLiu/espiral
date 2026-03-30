#pragma once

#include "../includes/common.h"

#include <cstdint>

namespace espiral {
class HostAcceleratorInterface {
public:
    virtual int upload(addr_t dest_addr, const void *src, size_t size) = 0;
    virtual int download(void* dest, addr_t src_addr, size_t size) = 0;
    virtual int start(addr_t krnl_addr, addr_t args_addr, addr_t satp) = 0;
    virtual int ready_wait(size_t timeout) = 0;
    virtual int dcr_write(addr_t addr, uint32_t value) = 0;
    virtual int dcr_read(addr_t addr, uint32_t* value) const = 0;
    virtual int mpm_query(addr_t addr, uint32_t core_id, uint64_t* value) const = 0;
    virtual ~HostAcceleratorInterface() = default;
};
}