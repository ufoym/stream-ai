#include <iostream>
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

  std::cout << "streamcam " << streamcam_version() << "\n";
  std::cout << "devices: " << count << "\n";
  for (size_t i = 0; i < count; ++i) {
    std::cout << i << ": " << devices[i].name << " [" << devices[i].id << "]\n";
  }
  return 0;
}
