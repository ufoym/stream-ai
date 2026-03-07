#include "platform/linux/backend.hpp"

#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace streamcam {

namespace {

struct MappedBuffer {
  void* data = nullptr;
  size_t length = 0;
};

int RetryIoctl(int fd, unsigned long request, void* arg) {
  int result = 0;
  do {
    result = ioctl(fd, request, arg);
  } while (result == -1 && errno == EINTR);
  return result;
}

uint64_t TimevalToNanoseconds(const timeval& tv) {
  return static_cast<uint64_t>(tv.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(tv.tv_usec) * 1000ULL;
}

streamcam_pixel_format StreamcamFormatFromV4l2(uint32_t pixel_format) {
  switch (pixel_format) {
    case V4L2_PIX_FMT_YUYV:
      return STREAMCAM_PIXEL_FORMAT_YUY2;
    case V4L2_PIX_FMT_NV12:
      return STREAMCAM_PIXEL_FORMAT_NV12;
    case V4L2_PIX_FMT_MJPEG:
      return STREAMCAM_PIXEL_FORMAT_MJPEG;
    default:
      return STREAMCAM_PIXEL_FORMAT_NATIVE;
  }
}

uint32_t DefaultStrideBytes(streamcam_pixel_format pixel_format, uint32_t width) {
  switch (pixel_format) {
    case STREAMCAM_PIXEL_FORMAT_YUY2:
      return width * 2U;
    case STREAMCAM_PIXEL_FORMAT_NV12:
    case STREAMCAM_PIXEL_FORMAT_MJPEG:
    case STREAMCAM_PIXEL_FORMAT_NATIVE:
    case STREAMCAM_PIXEL_FORMAT_BGRA32:
    case STREAMCAM_PIXEL_FORMAT_UNKNOWN:
    default:
      return width;
  }
}

uint32_t PreferredV4l2PixelFormat(streamcam_pixel_format preferred) {
  switch (preferred) {
    case STREAMCAM_PIXEL_FORMAT_NV12:
      return V4L2_PIX_FMT_NV12;
    case STREAMCAM_PIXEL_FORMAT_MJPEG:
      return V4L2_PIX_FMT_MJPEG;
    case STREAMCAM_PIXEL_FORMAT_YUY2:
      return V4L2_PIX_FMT_YUYV;
    case STREAMCAM_PIXEL_FORMAT_BGRA32:
    case STREAMCAM_PIXEL_FORMAT_NATIVE:
    case STREAMCAM_PIXEL_FORMAT_UNKNOWN:
    default:
      return V4L2_PIX_FMT_YUYV;
  }
}

std::vector<std::string> EnumerateDevicePaths() {
  std::vector<std::string> device_paths;
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator("/dev", error)) {
    if (error) {
      break;
    }
    if (!entry.is_character_file(error)) {
      continue;
    }
    const std::string name = entry.path().filename().string();
    if (name.rfind("video", 0) != 0) {
      continue;
    }
    device_paths.push_back(entry.path().string());
  }
  std::sort(device_paths.begin(), device_paths.end());
  return device_paths;
}

bool QueryCaptureCapabilities(int fd, v4l2_capability* out_capability) {
  if (out_capability == nullptr) {
    return false;
  }
  std::memset(out_capability, 0, sizeof(*out_capability));
  if (RetryIoctl(fd, VIDIOC_QUERYCAP, out_capability) != 0) {
    return false;
  }
  const bool supports_capture =
      (out_capability->device_caps & V4L2_CAP_VIDEO_CAPTURE) != 0 ||
      (out_capability->capabilities & V4L2_CAP_VIDEO_CAPTURE) != 0;
  const bool supports_streaming =
      (out_capability->device_caps & V4L2_CAP_STREAMING) != 0 ||
      (out_capability->capabilities & V4L2_CAP_STREAMING) != 0;
  return supports_capture && supports_streaming;
}

bool IsSupportedFormat(int fd, uint32_t pixel_format) {
  v4l2_fmtdesc desc{};
  desc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  for (desc.index = 0; RetryIoctl(fd, VIDIOC_ENUM_FMT, &desc) == 0; ++desc.index) {
    if (desc.pixelformat == pixel_format) {
      return true;
    }
  }
  return false;
}

}  // namespace

class LinuxBackendImpl {
 public:
  LinuxBackendImpl(LinuxBackend* owner, const char* device_id, const streamcam_config& config)
      : owner_(owner), device_id_(device_id ? device_id : ""), config_(config) {}

  ~LinuxBackendImpl() { Stop(); }

  streamcam_status Start() {
    if (running_) {
      return STREAMCAM_STATUS_OK;
    }

    fd_ = open(device_id_.c_str(), O_RDWR | O_NONBLOCK);
    if (fd_ < 0) {
      return STREAMCAM_STATUS_NOT_FOUND;
    }

    v4l2_capability capability{};
    if (!QueryCaptureCapabilities(fd_, &capability)) {
      CleanupFileDescriptor();
      return STREAMCAM_STATUS_NOT_SUPPORTED;
    }

    streamcam_status status = ConfigureFormat();
    if (status != STREAMCAM_STATUS_OK) {
      CleanupFileDescriptor();
      return status;
    }

    status = RequestAndMapBuffers();
    if (status != STREAMCAM_STATUS_OK) {
      CleanupBuffers();
      CleanupFileDescriptor();
      return status;
    }

    status = QueueAllBuffers();
    if (status != STREAMCAM_STATUS_OK) {
      CleanupBuffers();
      CleanupFileDescriptor();
      return status;
    }

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (RetryIoctl(fd_, VIDIOC_STREAMON, &type) != 0) {
      CleanupBuffers();
      CleanupFileDescriptor();
      return STREAMCAM_STATUS_PLATFORM_ERROR;
    }

    running_ = true;
    capture_thread_ = std::thread([this]() { CaptureLoop(); });
    return STREAMCAM_STATUS_OK;
  }

  streamcam_status Stop() {
    if (!running_ && fd_ < 0) {
      return STREAMCAM_STATUS_OK;
    }

    running_ = false;

    if (fd_ >= 0) {
      int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      RetryIoctl(fd_, VIDIOC_STREAMOFF, &type);
    }

    if (capture_thread_.joinable()) {
      capture_thread_.join();
    }

    CleanupBuffers();
    CleanupFileDescriptor();
    return STREAMCAM_STATUS_OK;
  }

  void CaptureLoop() {
    while (running_) {
      pollfd descriptor{};
      descriptor.fd = fd_;
      descriptor.events = POLLIN;

      const int poll_result = poll(&descriptor, 1, 100);
      if (!running_) {
        break;
      }
      if (poll_result <= 0) {
        continue;
      }
      if ((descriptor.revents & POLLIN) == 0) {
        continue;
      }

      v4l2_buffer buffer{};
      buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buffer.memory = V4L2_MEMORY_MMAP;
      if (RetryIoctl(fd_, VIDIOC_DQBUF, &buffer) != 0) {
        if (errno == EAGAIN) {
          continue;
        }
        break;
      }

      if (buffer.index < buffers_.size()) {
        const MappedBuffer& mapped = buffers_[buffer.index];
        const uint8_t* data = static_cast<const uint8_t*>(mapped.data);
        if (data != nullptr && buffer.bytesused != 0) {
          owner_->PublishFrame(data, buffer.bytesused, width_, height_, stride_bytes_,
                               pixel_format_, TimevalToNanoseconds(buffer.timestamp));
        }
      }

      RetryIoctl(fd_, VIDIOC_QBUF, &buffer);
    }
  }

 private:
  streamcam_status ConfigureFormat() {
    const std::array<uint32_t, 4> candidates = {
        PreferredV4l2PixelFormat(config_.preferred_format),
        V4L2_PIX_FMT_YUYV,
        V4L2_PIX_FMT_NV12,
        V4L2_PIX_FMT_MJPEG,
    };

    uint32_t selected_pixel_format = 0;
    for (uint32_t candidate : candidates) {
      if (candidate != 0 && IsSupportedFormat(fd_, candidate)) {
        selected_pixel_format = candidate;
        break;
      }
    }
    if (selected_pixel_format == 0) {
      return STREAMCAM_STATUS_NOT_SUPPORTED;
    }

    v4l2_format format{};
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = config_.width;
    format.fmt.pix.height = config_.height;
    format.fmt.pix.pixelformat = selected_pixel_format;
    format.fmt.pix.field = V4L2_FIELD_ANY;

    if (RetryIoctl(fd_, VIDIOC_S_FMT, &format) != 0) {
      return STREAMCAM_STATUS_PLATFORM_ERROR;
    }

    if (config_.fps > 0) {
      v4l2_streamparm streamparm{};
      streamparm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      streamparm.parm.capture.timeperframe.numerator = 1;
      streamparm.parm.capture.timeperframe.denominator = config_.fps;
      RetryIoctl(fd_, VIDIOC_S_PARM, &streamparm);
    }

    width_ = format.fmt.pix.width;
    height_ = format.fmt.pix.height;
    pixel_format_ = StreamcamFormatFromV4l2(format.fmt.pix.pixelformat);
    stride_bytes_ = format.fmt.pix.bytesperline;
    if (stride_bytes_ == 0) {
      stride_bytes_ = DefaultStrideBytes(pixel_format_, width_);
    }
    return STREAMCAM_STATUS_OK;
  }

  streamcam_status RequestAndMapBuffers() {
    v4l2_requestbuffers request{};
    request.count = 4;
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request.memory = V4L2_MEMORY_MMAP;

    if (RetryIoctl(fd_, VIDIOC_REQBUFS, &request) != 0 || request.count < 2) {
      return STREAMCAM_STATUS_PLATFORM_ERROR;
    }

    buffers_.clear();
    buffers_.resize(request.count);

    for (uint32_t index = 0; index < request.count; ++index) {
      v4l2_buffer buffer{};
      buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buffer.memory = V4L2_MEMORY_MMAP;
      buffer.index = index;

      if (RetryIoctl(fd_, VIDIOC_QUERYBUF, &buffer) != 0) {
        return STREAMCAM_STATUS_PLATFORM_ERROR;
      }

      void* mapped = mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_,
                          static_cast<off_t>(buffer.m.offset));
      if (mapped == MAP_FAILED) {
        buffers_[index] = {};
        return STREAMCAM_STATUS_PLATFORM_ERROR;
      }

      buffers_[index].data = mapped;
      buffers_[index].length = buffer.length;
    }
    return STREAMCAM_STATUS_OK;
  }

  streamcam_status QueueAllBuffers() {
    for (uint32_t index = 0; index < buffers_.size(); ++index) {
      v4l2_buffer buffer{};
      buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buffer.memory = V4L2_MEMORY_MMAP;
      buffer.index = index;
      if (RetryIoctl(fd_, VIDIOC_QBUF, &buffer) != 0) {
        return STREAMCAM_STATUS_PLATFORM_ERROR;
      }
    }
    return STREAMCAM_STATUS_OK;
  }

  void CleanupBuffers() {
    for (MappedBuffer& buffer : buffers_) {
      if (buffer.data != nullptr && buffer.length != 0) {
        munmap(buffer.data, buffer.length);
      }
      buffer = {};
    }
    buffers_.clear();
  }

  void CleanupFileDescriptor() {
    if (fd_ >= 0) {
      close(fd_);
      fd_ = -1;
    }
  }

  LinuxBackend* owner_ = nullptr;
  std::string device_id_;
  streamcam_config config_{};
  int fd_ = -1;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint32_t stride_bytes_ = 0;
  streamcam_pixel_format pixel_format_ = STREAMCAM_PIXEL_FORMAT_UNKNOWN;
  std::vector<MappedBuffer> buffers_;
  std::thread capture_thread_;
  std::atomic<bool> running_{false};
};

std::vector<streamcam_device_info> ListDevicesLinux() {
  std::vector<streamcam_device_info> devices;
  for (const std::string& device_path : EnumerateDevicePaths()) {
    const int fd = open(device_path.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0) {
      continue;
    }

    v4l2_capability capability{};
    if (!QueryCaptureCapabilities(fd, &capability)) {
      close(fd);
      continue;
    }

    streamcam_device_info info{};
    std::strncpy(info.id, device_path.c_str(), STREAMCAM_DEVICE_ID_MAX - 1);
    std::strncpy(info.name, reinterpret_cast<const char*>(capability.card),
                 STREAMCAM_DEVICE_NAME_MAX - 1);
    devices.push_back(info);
    close(fd);
  }
  return devices;
}

LinuxBackend::LinuxBackend(const char* device_id, const streamcam_config& config)
    : config_(config), impl_(new LinuxBackendImpl(this, device_id, config)) {}

LinuxBackend::~LinuxBackend() { delete impl_; }

streamcam_status LinuxBackend::Start() { return impl_->Start(); }

streamcam_status LinuxBackend::Stop() { return impl_->Stop(); }

streamcam_status LinuxBackend::LatestFrame(streamcam_frame_view* out_frame) {
  return store_.Latest(out_frame);
}

streamcam_status LinuxBackend::PublishFrame(const uint8_t* data,
                                            size_t size_bytes,
                                            uint32_t width,
                                            uint32_t height,
                                            uint32_t stride_bytes,
                                            streamcam_pixel_format pixel_format,
                                            uint64_t timestamp_ns) {
  return store_.Publish(data, size_bytes, width, height, stride_bytes, pixel_format,
                        timestamp_ns);
}

}  // namespace streamcam
