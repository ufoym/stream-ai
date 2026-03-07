#ifndef STREAMCAM_SRC_PLATFORM_LINUX_BACKEND_HPP_
#define STREAMCAM_SRC_PLATFORM_LINUX_BACKEND_HPP_

#include "internal/backend.hpp"

namespace streamcam {

class LinuxBackendImpl;

std::vector<streamcam_device_info> ListDevicesLinux();

class LinuxBackend final : public Backend {
 public:
  LinuxBackend(const char* device_id, const streamcam_config& config);
  ~LinuxBackend() override;

  streamcam_status Start() override;
  streamcam_status Stop() override;
  streamcam_status LatestFrame(streamcam_frame_view* out_frame) override;

  streamcam_status PublishFrame(const uint8_t* data,
                                size_t size_bytes,
                                uint32_t width,
                                uint32_t height,
                                uint32_t stride_bytes,
                                streamcam_pixel_format pixel_format,
                                uint64_t timestamp_ns);

 private:
  streamcam_config config_{};
  FrameStore store_;
  LinuxBackendImpl* impl_ = nullptr;
};

}  // namespace streamcam

#endif  // STREAMCAM_SRC_PLATFORM_LINUX_BACKEND_HPP_
