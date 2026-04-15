#pragma once

#include <cstdint>

class AxiCtrl {
public:
    AxiCtrl(uint64_t base_mmio_addr);

    int write64(uint64_t addr, uint64_t value);

    int read64(uint64_t addr, uint64_t* value) const;

    int write32(uint64_t addr, uint32_t value);

    int read32(uint64_t addr, uint32_t* value) const;

private:
    uint64_t base_mmio_addr_;
};

