#include "axictrl.h"

AxiCtrl::AxiCtrl(uint64_t base_mmio_addr) : base_mmio_addr_(base_mmio_addr) {}

int AxiCtrl::write64(uint64_t addr, uint64_t value) {
    // Assuming a memory-mapped I/O write operation at addr + base_mmio_addr
    uint32_t low = value & 0x0ffffffff;
    uint32_t high = value >> 32;
    uint32_t* addr_low = reinterpret_cast<uint32_t*>(base_mmio_addr_ + addr);
    uint32_t* addr_high = reinterpret_cast<uint32_t*>(base_mmio_addr_ + addr + 4);
    *addr_low = low;
    *addr_high = high;
    return 0;  // Success
}

int AxiCtrl::read64(uint64_t addr, uint64_t* value) const {
    // Make sure not reading into a nullptr
    if ( value == nullptr ) return -1;
    // Assuming a memory-mapped I/O read operation at addr + base_mmio_addr
    const uint32_t* low = reinterpret_cast<const uint32_t*>(base_mmio_addr_ + addr);
    const uint32_t* high = reinterpret_cast<const uint32_t*>(base_mmio_addr_ + addr + 4);
    *value = ((uint64_t)*high << 32) | ((uint64_t)*low & 0x0ffffffff);
    return 0;  // Success
}

int AxiCtrl::write32(uint64_t addr, uint32_t value) {
    // Assuming a memory-mapped I/O write operation at addr + base_mmio_addr
    uint32_t* target_address = reinterpret_cast<uint32_t*>(base_mmio_addr_ + addr);
    *target_address = value;
    return 0;  // Success
}

int AxiCtrl::read32(uint64_t addr, uint32_t* value) const {
    // Make sure not reading into a nullptr
    if ( value == nullptr ) return -1;
    // Assuming a memory-mapped I/O read operation at addr + base_mmio_addr
    const uint32_t* target_address = reinterpret_cast<const uint32_t*>(base_mmio_addr_ + addr);
    *value = *target_address;
    return 0;  // Success
}
