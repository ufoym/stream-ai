#include "streamcam/streamcam.h"

#include <cstring>
#include <memory>
#include <new>
#include <utility>
#include <vector>

#include "internal/backend.hpp"

struct streamcam_reader {
  explicit streamcam_reader(std::unique_ptr<streamcam::Backend> backend_in)
      : backend(std::move(backend_in)) {}

  std::unique_ptr<streamcam::Backend> backend;
};

namespace {

constexpr const char* kVersion = "0.1.0";

}  // namespace

const char* streamcam_version(void) { return kVersion; }

const char* streamcam_status_string(streamcam_status status) {
  switch (status) {
    case STREAMCAM_STATUS_OK:
      return "ok";
    case STREAMCAM_STATUS_INVALID_ARGUMENT:
      return "invalid argument";
    case STREAMCAM_STATUS_NOT_FOUND:
      return "not found";
    case STREAMCAM_STATUS_NOT_SUPPORTED:
      return "not supported";
    case STREAMCAM_STATUS_PLATFORM_ERROR:
      return "platform error";
    case STREAMCAM_STATUS_NO_FRAME:
      return "no frame available";
  }
  return "unknown error";
}

streamcam_config streamcam_default_config(void) {
  streamcam_config config{};
  config.width = 1280;
  config.height = 720;
  config.fps = 60;
  config.preferred_format = STREAMCAM_PIXEL_FORMAT_NATIVE;
  return config;
}

streamcam_status streamcam_list_devices(streamcam_device_info* devices,
                                        size_t capacity,
                                        size_t* count) {
  if (count == nullptr) {
    return STREAMCAM_STATUS_INVALID_ARGUMENT;
  }

  const std::vector<streamcam_device_info> listed = streamcam::ListDevices();
  *count = listed.size();

  if (devices == nullptr || capacity == 0) {
    return STREAMCAM_STATUS_OK;
  }

  const size_t limit = listed.size() < capacity ? listed.size() : capacity;
  for (size_t i = 0; i < limit; ++i) {
    devices[i] = listed[i];
  }
  return STREAMCAM_STATUS_OK;
}

streamcam_status streamcam_open(const char* device_id,
                                const streamcam_config* config,
                                streamcam_reader** out_reader) {
  if (device_id == nullptr || config == nullptr || out_reader == nullptr) {
    return STREAMCAM_STATUS_INVALID_ARGUMENT;
  }

  streamcam_status status = STREAMCAM_STATUS_OK;
  std::unique_ptr<streamcam::Backend> backend =
      streamcam::CreateBackend(device_id, *config, &status);
  if (!backend) {
    return status;
  }

  try {
    *out_reader = new streamcam_reader(std::move(backend));
  } catch (const std::bad_alloc&) {
    return STREAMCAM_STATUS_PLATFORM_ERROR;
  }
  return STREAMCAM_STATUS_OK;
}

streamcam_status streamcam_start(streamcam_reader* reader) {
  if (reader == nullptr || !reader->backend) {
    return STREAMCAM_STATUS_INVALID_ARGUMENT;
  }
  return reader->backend->Start();
}

streamcam_status streamcam_stop(streamcam_reader* reader) {
  if (reader == nullptr || !reader->backend) {
    return STREAMCAM_STATUS_INVALID_ARGUMENT;
  }
  return reader->backend->Stop();
}

streamcam_status streamcam_get_latest_frame(streamcam_reader* reader,
                                            streamcam_frame_view* out_frame) {
  if (reader == nullptr || !reader->backend) {
    return STREAMCAM_STATUS_INVALID_ARGUMENT;
  }
  return reader->backend->LatestFrame(out_frame);
}

void streamcam_close(streamcam_reader* reader) {
  if (reader == nullptr) {
    return;
  }
  if (reader->backend) {
    reader->backend->Stop();
  }
  delete reader;
}
