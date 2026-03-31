#pragma once

#include "includes/espiral_common.h"
#include "upload_buffer.h"
#include "download_buffer.h"

#include <cstdint>
#include <string>
#include <optional>

namespace espiral {

namespace backend {
constexpr int SIMX     = 0;
constexpr int VERILATOR = 1;
}

class Spinner;               // pImpl — callers never see this
class HostAcceleratorInterface; // pImpl — callers never see this

class Espiral {
public:
  explicit Espiral(int backend_type);
  ~Espiral();

  auto allocate_kernel(std::string vxbin_path) -> kernel_id_t;
  void start_kernel(kernel_id_t kid);
  void wait_kernel(kernel_id_t kid, uint64_t timeout);
  void free_kernel(kernel_id_t kid);

  auto allocate_upload_buffer(kernel_id_t kid, size_t size,
                               uint32_t flags = pte_flags::R | pte_flags::W) -> UploadBuffer;
  auto allocate_dev_buffer(kernel_id_t kid, size_t size) -> uint32_t;
  template<typename T>
  void upload_args(kernel_id_t kid, const T *args) {
    upload_args_(kid, static_cast<const void *>(args), sizeof(T));
  }
  void upload(kernel_id_t kid, const UploadBuffer &upload_buf);
  void download(kernel_id_t kid, const DownloadBuffer &download_buf);
  auto get_caps(uint32_t caps_id) -> std::optional<uint64_t>;

private:
  void upload_args_(kernel_id_t kid, const void *args, size_t size);
  Spinner *spinner_;
  HostAcceleratorInterface *accelerator_;
};

} // namespace espiral
