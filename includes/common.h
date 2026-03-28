#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace espiral {
typedef uint32_t kernel_id_t;
using sparse_scratchpad_t = std::unordered_map<uint32_t, std::vector<uint8_t>>;
constexpr size_t PAGE_SIZE = 4096;
constexpr size_t PTE_SIZE = 4;
constexpr size_t PT_LEVELS = 2;
constexpr size_t VPN_BITS = 10;
constexpr size_t VPN_MASK = (1u << VPN_BITS) - 1;

typedef uint32_t satp_t;

namespace pte_flags {
constexpr uint32_t V = 1 << 0;
constexpr uint32_t R = 1 << 1;
constexpr uint32_t W = 1 << 2;
constexpr uint32_t X = 1 << 3;
constexpr uint32_t U = 1 << 4;
constexpr uint32_t G = 1 << 5;
constexpr uint32_t A = 1 << 6;
constexpr uint32_t D = 1 << 7;
} // namespace pte_flags
} // namespace espiral