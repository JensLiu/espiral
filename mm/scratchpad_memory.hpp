#pragma once

#include "../includes/espiral_common.h"

#include <mutex>
#include "logger.hpp"

namespace espiral {

class ScratchpadMemory {
public:
  ScratchpadMemory() : logger_("espiral::ScratchpadMemory") {}

  void begin_transaction() { mutex_.lock(); }

  void end_transaction() { mutex_.unlock(); }

  auto write(uint64_t addr, const std::vector<uint8_t> &data) -> bool {
    logger_.log("Writing %zu bytes to address: %lx", data.size(), addr);
    for (size_t i = 0; i < data.size(); ++i) {
      const uint64_t current_addr = addr + i;
      const uint64_t current_page_base =
          current_addr - (current_addr % PAGE_SIZE);
      const uint64_t offset = current_addr % PAGE_SIZE;
      get_page_or_else_create(current_page_base)[offset] = data[i];
    }
    return true;
  }

  auto write_atomic(uint64_t addr, const std::vector<uint8_t> &data) -> bool {
    std::lock_guard<std::mutex> lock(mutex_);
    return write(addr, data);
  }

  auto read(uint64_t addr, size_t size) -> std::vector<uint8_t> {
    assert(size != 0);
    logger_.log("Reading %zu bytes from address: %lx", size, addr);
    std::vector<uint8_t> result(size);
    for (size_t i = 0; i < size; ++i) {
      const uint64_t current_addr = addr + i;
      const uint64_t current_page_base =
          current_addr - (current_addr % PAGE_SIZE);
      const uint64_t offset = current_addr % PAGE_SIZE;
      result[i] = get_page_or_else_create(current_page_base)[offset];
    }
    return result;
  }

  auto read_atomic(uint64_t addr, size_t size) -> std::vector<uint8_t> {
    std::lock_guard<std::mutex> lock(mutex_);
    return read(addr, size);
  }

private:
  auto get_page_or_else_create(uint64_t page_base) -> std::vector<uint8_t> & {
    if (page_content_.find(page_base) == page_content_.end()) {
      // Initialize with zeros
      page_content_[page_base] = std::vector<uint8_t>(PAGE_SIZE, 0);
    }
    return page_content_[page_base];
  }

  std::mutex mutex_;
  sparse_scratchpad_t page_content_;
  Logger logger_;
};
} // namespace vortex_manager