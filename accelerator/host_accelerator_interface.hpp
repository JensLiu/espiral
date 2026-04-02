#pragma once

#include "../includes/espiral_common.h"

#include <optional>
#include <cstdint>

namespace espiral {
class HostAcceleratorInterface {
public:
    virtual auto init() -> int = 0;
    virtual auto close() -> int = 0;
    virtual auto get_caps(uint32_t caps_id) -> std::optional<uint64_t> = 0;
    virtual auto upload(addr_t dest_addr, const void *src, size_t size) -> int = 0;
    virtual auto download(void* dest, addr_t src_addr, size_t size) -> int = 0;
    virtual auto start(addr_t krnl_addr, addr_t args_addr, addr_t top_pgtbl_pa) -> int = 0;
    virtual auto ready_wait(size_t timeout) -> int = 0;
    virtual auto dcr_write(addr_t addr, uint32_t value) -> int = 0;
    virtual auto dcr_read(addr_t addr) -> std::optional<uint32_t> = 0;
    virtual auto mpm_query(addr_t addr, uint32_t core_id) -> std::optional<uint64_t> = 0;
    virtual ~HostAcceleratorInterface() = default;
};
}