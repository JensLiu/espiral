#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace espiral {
class Espiral;

class UploadBuffer {
  friend class Espiral;

private:
  const uint32_t va_;
  const uint32_t device_max_size_;
  const uint32_t flags_;
  const void *host_ptr_;
  size_t size_;

  UploadBuffer(uint32_t va, uint32_t max_size, uint32_t flags)
      : va_(va), device_max_size_(max_size), flags_(flags),
        host_ptr_(nullptr), size_(0) {}

public:
  uint32_t get_va() const { return va_; }

  void set_content(const void *data, size_t data_size) {
    if (data_size > device_max_size_)
      throw std::runtime_error("Data size exceeds buffer capacity");
    host_ptr_ = data;
    size_ = data_size;
  }
};

} // namespace espiral
