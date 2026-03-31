#pragma once

#include <cstdint>

namespace espiral {

class DownloadBuffer {
public:
  const uint32_t va;
  const uint32_t read_size;
  void *const host_ptr;

  DownloadBuffer(uint32_t va, uint32_t read_size, void *host_ptr)
      : va(va), read_size(read_size), host_ptr(host_ptr) {}
};

} // namespace espiral
