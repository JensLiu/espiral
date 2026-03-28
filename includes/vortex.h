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

#ifndef __VX_VORTEX_H__
#define __VX_VORTEX_H__

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <VX_config.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void* vx_device_h;
typedef void* vx_buffer_h;

// device caps ids
#define VX_CAPS_VERSION             0x0
#define VX_CAPS_NUM_THREADS         0x1
#define VX_CAPS_NUM_WARPS           0x2
#define VX_CAPS_NUM_CORES           0x3
#define VX_CAPS_CACHE_LINE_SIZE     0x4
#define VX_CAPS_GLOBAL_MEM_SIZE     0x5
#define VX_CAPS_LOCAL_MEM_SIZE      0x6
#define VX_CAPS_ISA_FLAGS           0x7
#define VX_CAPS_NUM_MEM_BANKS       0x8
#define VX_CAPS_MEM_BANK_SIZE       0x9

// device isa flags
#define VX_ISA_STD_A                (1ull << ISA_STD_A)
#define VX_ISA_STD_C                (1ull << ISA_STD_C)
#define VX_ISA_STD_D                (1ull << ISA_STD_D)
#define VX_ISA_STD_E                (1ull << ISA_STD_E)
#define VX_ISA_STD_F                (1ull << ISA_STD_F)
#define VX_ISA_STD_H                (1ull << ISA_STD_H)
#define VX_ISA_STD_I                (1ull << ISA_STD_I)
#define VX_ISA_STD_N                (1ull << ISA_STD_N)
#define VX_ISA_STD_Q                (1ull << ISA_STD_Q)
#define VX_ISA_STD_S                (1ull << ISA_STD_S)
#define VX_ISA_STD_V                (1ull << ISA_STD_V)
#define VX_ISA_ARCH(flags)          (1ull << (((flags >> 30) & 0x3) + 4))
#define VX_ISA_EXT_ICACHE           (1ull << (32+ISA_EXT_ICACHE))
#define VX_ISA_EXT_DCACHE           (1ull << (32+ISA_EXT_DCACHE))
#define VX_ISA_EXT_L2CACHE          (1ull << (32+ISA_EXT_L2CACHE))
#define VX_ISA_EXT_L3CACHE          (1ull << (32+ISA_EXT_L3CACHE))
#define VX_ISA_EXT_LMEM             (1ull << (32+ISA_EXT_LMEM))
#define VX_ISA_EXT_ZICOND           (1ull << (32+ISA_EXT_ZICOND))
#define VX_ISA_EXT_TEX              (1ull << (32+ISA_EXT_TEX))
#define VX_ISA_EXT_RASTER           (1ull << (32+ISA_EXT_RASTER))
#define VX_ISA_EXT_OM               (1ull << (32+ISA_EXT_OM))
#define VX_ISA_EXT_TCU              (1ull << (32+ISA_EXT_TCU))

// ready wait timeout
#define VX_MAX_TIMEOUT              (24*60*60*1000)   // 24 Hr

// device memory access
#define VX_MEM_READ                 0x1
#define VX_MEM_WRITE                0x2
#define VX_MEM_READ_WRITE           0x3
#define VX_MEM_PIN_MEMORY           0x4

#ifdef __cplusplus
}
#endif

#endif // __VX_VORTEX_H__
