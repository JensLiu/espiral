#include "espiral.h"
#include "spinner.hpp"
#include "accelerator/simx_backend.hpp"
#include "accelerator/rtlsim_backend.hpp"

#include <stdexcept>

namespace espiral {

Espiral::Espiral(int backend_type) {
  switch (backend_type) {
    case backend::SIMX:      accelerator_ = new SimXDevice();    break;
    case backend::VERILATOR: accelerator_ = new RtlSimDevice();  break;
    default: throw std::invalid_argument("Unknown backend type");
  }
  accelerator_->init();
  spinner_ = new Spinner(accelerator_);
}

Espiral::~Espiral() {
  delete spinner_;
  delete accelerator_;
}

auto Espiral::allocate_kernel(std::string vxbin_path) -> kernel_id_t {
  return spinner_->allocate_kernel(std::move(vxbin_path));
}

void Espiral::start_kernel(kernel_id_t kid) {
  spinner_->start_kernel(kid);
}

void Espiral::wait_kernel(kernel_id_t kid, uint64_t timeout) {
  spinner_->wait_kernel(kid, timeout);
}

void Espiral::free_kernel(kernel_id_t kid) {
  spinner_->free_kernel(kid);
}

auto Espiral::allocate_upload_buffer(kernel_id_t kid, size_t size, uint32_t flags) -> UploadBuffer {
  const addr_t va = spinner_->allocate_heap(kid, size, flags);
  return UploadBuffer(va, static_cast<uint32_t>(size), flags);
}

auto Espiral::allocate_dev_buffer(kernel_id_t kid, size_t size) -> uint32_t {
  return spinner_->allocate_heap(kid, size);
}

void Espiral::upload_args_(kernel_id_t kid, const void *args, size_t size) {
  spinner_->upload_args(kid, args, size);
}

void Espiral::upload(kernel_id_t kid, const UploadBuffer &buf) {
  spinner_->upload(kid, buf.va_, buf.host_ptr_, buf.size_);
}

void Espiral::download(kernel_id_t kid, const DownloadBuffer &buf) {
  spinner_->download(kid, buf.va, buf.host_ptr, buf.read_size);
}

auto Espiral::get_caps(uint32_t caps_id) -> std::optional<uint64_t> {
  return accelerator_->get_caps(caps_id);
}

} // namespace espiral
