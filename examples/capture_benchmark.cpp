#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

#include "streamcam/streamcam.h"

int main() {
  size_t count = 0;
  streamcam_status status = streamcam_list_devices(nullptr, 0, &count);
  if (status != STREAMCAM_STATUS_OK || count == 0) {
    std::cerr << "no camera available\n";
    return 1;
  }

  std::vector<streamcam_device_info> devices(count);
  status = streamcam_list_devices(devices.data(), devices.size(), &count);
  if (status != STREAMCAM_STATUS_OK) {
    std::cerr << "list_devices failed: " << streamcam_status_string(status) << "\n";
    return 1;
  }

  streamcam_config config = streamcam_default_config();
  streamcam_reader* reader = nullptr;
  status = streamcam_open(devices[0].id, &config, &reader);
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

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  uint64_t last_sequence = 0;
  size_t frames = 0;
  streamcam_frame_view frame{};

  while (std::chrono::steady_clock::now() < deadline) {
    status = streamcam_get_latest_frame(reader, &frame);
    if (status == STREAMCAM_STATUS_OK && frame.sequence != last_sequence) {
      last_sequence = frame.sequence;
      ++frames;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  streamcam_stop(reader);
  streamcam_close(reader);

  std::cout << "frames in 3s: " << frames << "\n";
  std::cout << "approx fps: " << (frames / 3.0) << "\n";
  return 0;
}
