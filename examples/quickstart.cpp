#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

#include "streamcam/streamcam.h"

int main() {
  size_t count = 0;
  streamcam_status status = streamcam_list_devices(nullptr, 0, &count);
  if (status != STREAMCAM_STATUS_OK) {
    std::cerr << "list_devices failed: " << streamcam_status_string(status) << "\n";
    return 1;
  }

  std::vector<streamcam_device_info> devices(count);
  status = streamcam_list_devices(devices.data(), devices.size(), &count);
  if (status != STREAMCAM_STATUS_OK) {
    std::cerr << "list_devices failed: " << streamcam_status_string(status) << "\n";
    return 1;
  }

  if (devices.empty()) {
    std::cout << "no camera available\n";
    return 1;
  }

  const streamcam_device_info& device = devices.front();
  std::cout << "streamcam " << streamcam_version() << "\n";
  std::cout << "opening: " << device.name << " [" << device.id << "]\n";

  streamcam_config config = streamcam_default_config();
  streamcam_reader* reader = nullptr;
  status = streamcam_open(device.id, &config, &reader);
  if (status != STREAMCAM_STATUS_OK) {
    std::cerr << "open failed: " << streamcam_status_string(status) << "\n";
    return 1;
  }

  status = streamcam_start(reader);
  if (status != STREAMCAM_STATUS_OK) {
    std::cerr << "start failed: " << streamcam_status_string(status) << "\n";
    streamcam_close(reader);
    return 1;
  }

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  streamcam_frame_view frame{};

  while (std::chrono::steady_clock::now() < deadline) {
    status = streamcam_get_latest_frame(reader, &frame);
    if (status == STREAMCAM_STATUS_OK) {
      std::cout << "frame: " << frame.width << "x" << frame.height
                << ", stride=" << frame.stride_bytes
                << ", bytes=" << frame.size_bytes
                << ", sequence=" << frame.sequence
                << ", format=" << frame.pixel_format << "\n";
      streamcam_stop(reader);
      streamcam_close(reader);
      return 0;
    }

    if (status != STREAMCAM_STATUS_NO_FRAME) {
      std::cerr << "get_latest_frame failed: " << streamcam_status_string(status)
                << "\n";
      streamcam_stop(reader);
      streamcam_close(reader);
      return 1;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  std::cout << "timed out waiting for the first frame\n";
  streamcam_stop(reader);
  streamcam_close(reader);
  return 1;
}
