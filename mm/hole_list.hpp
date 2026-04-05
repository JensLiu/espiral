#pragma once

#include "espiral_common.h"
#include <unordered_map>

namespace espiral {
class HoleList {

public:
  void add_hole(addr_t addr, size_t size) {
    if (size > 0) {
      hole_list_[addr] = size;
    }
  }

  auto addr_in_hole(addr_t addr) -> bool {
    for (const auto [a, s] : hole_list_) {
      if (addr >= a && addr <= a + s) {
        return true;
      }
    }
    return false;
  }

private:
  std::unordered_map<addr_t, size_t> hole_list_;
};
} // namespace espiral