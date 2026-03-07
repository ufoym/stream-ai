#include "internal/backend.hpp"

#if defined(__APPLE__)
#include "platform/macos/backend.hpp"
#elif defined(__linux__)
#include "platform/linux/backend.hpp"
#elif defined(_WIN32)
#include "platform/windows/backend.hpp"
#endif

namespace streamcam {

std::vector<streamcam_device_info> ListDevices() {
#if defined(__APPLE__)
  return ListDevicesMacos();
#elif defined(__linux__)
  return ListDevicesLinux();
#elif defined(_WIN32)
  return ListDevicesWindows();
#else
  return {};
#endif
}

std::unique_ptr<Backend> CreateBackend(const char* device_id,
                                       const streamcam_config& config,
                                       streamcam_status* out_status) {
  if (out_status == nullptr) {
    return nullptr;
  }

#if defined(__APPLE__)
  *out_status = STREAMCAM_STATUS_OK;
  return std::make_unique<MacosBackend>(device_id, config);
#elif defined(__linux__)
  *out_status = STREAMCAM_STATUS_OK;
  return std::make_unique<LinuxBackend>(device_id, config);
#elif defined(_WIN32)
  *out_status = STREAMCAM_STATUS_OK;
  return std::make_unique<WindowsBackend>(device_id, config);
#else
  (void)device_id;
  (void)config;
  *out_status = STREAMCAM_STATUS_NOT_SUPPORTED;
  return nullptr;
#endif
}

}  // namespace streamcam
