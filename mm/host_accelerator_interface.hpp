#pragma once
#include <cstdint>

namespace espiral {
class HostAcceleratorInterface {
public:
    virtual int upload(uint64_t dest_addr, const void *src, uint64_t size) = 0;
    virtual int download(void* dest, uint64_t src_addr, uint64_t size) = 0;
    virtual int start(uint64_t krnl_addr, uint64_t args_addr, uint64_t satp) = 0;
    virtual int ready_wait(uint64_t timeout) = 0;
    virtual int dcr_write(uint32_t addr, uint32_t value) = 0;
    virtual int dcr_read(uint32_t addr, uint32_t* value) const = 0;
    virtual int mpm_query(uint32_t addr, uint32_t core_id, uint64_t* value) const = 0;
    virtual ~HostAcceleratorInterface() = default;
};
}