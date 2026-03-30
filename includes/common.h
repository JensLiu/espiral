#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

// DCR constants for SATP — extend the base VX_types.h layout which ends at 0x005
#ifndef VX_DCR_BASE_STARTUP_SATP0
#define VX_DCR_BASE_STARTUP_SATP0 0x006
#define VX_DCR_BASE_STARTUP_SATP1 0x007
#endif

// FROM VX_config.h for 32-bits: VIRTUAL ADDRESS LAYOUT
#define STACK_BASE_ADDR 0xFFFF0000
#define STARTUP_ADDR 0x80000000
#define USER_BASE_ADDR 0x00010000
#define IO_BASE_ADDR 0x00000040
// #define PAGE_TABLE_BASE_ADDR 0xF0000000 // < we dynamically allocate page tables
#define IO_END_ADDR USER_BASE_ADDR
#define LMEM_LOG_SIZE 14
#define LMEM_BASE_ADDR STACK_BASE_ADDR
#define IO_COUT_ADDR IO_BASE_ADDR
#define IO_COUT_SIZE 64
#define IO_MPM_ADDR (IO_COUT_ADDR + IO_COUT_SIZE)
#define STACK_LOG2_SIZE 13

#define NUM_THREADS 4
#define NUM_WARPS 4
#define NUM_CORES 1
#define NUM_CLUSTERS 1

// PHYSICAL ADDRESS LAYOUT
#define GLOBAL_MEM_BASE_ADDR 0x10000000
#define GLOBAL_MEM_SIZE 0x10000000

namespace espiral {
typedef uint32_t kernel_id_t;
typedef uint32_t addr_t;
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