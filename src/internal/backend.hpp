#ifndef STREAMCAM_SRC_INTERNAL_BACKEND_HPP_
#define STREAMCAM_SRC_INTERNAL_BACKEND_HPP_

#include <memory>
#include <vector>

#include "internal/frame_store.hpp"

namespace streamcam {

class Backend {
 public:
  virtual ~Backend() = default;
  virtual streamcam_status Start() = 0;
  virtual streamcam_status Stop() = 0;
  virtual streamcam_status LatestFrame(streamcam_frame_view* out_frame) = 0;
};

std::vector<streamcam_device_info> ListDevices();

std::unique_ptr<Backend> CreateBackend(const char* device_id,
                                       const streamcam_config& config,
                                       streamcam_status* out_status);

}  // namespace streamcam

#endif  // STREAMCAM_SRC_INTERNAL_BACKEND_HPP_
