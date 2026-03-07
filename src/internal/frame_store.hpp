#ifndef STREAMCAM_SRC_INTERNAL_FRAME_STORE_HPP_
#define STREAMCAM_SRC_INTERNAL_FRAME_STORE_HPP_

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include "streamcam/streamcam.h"

namespace streamcam {

class FrameStore {
 public:
  streamcam_status Publish(const uint8_t* data,
                           size_t size_bytes,
                           uint32_t width,
                           uint32_t height,
                           uint32_t stride_bytes,
                           streamcam_pixel_format pixel_format,
                           uint64_t timestamp_ns) {
    if (data == nullptr || size_bytes == 0) {
      return STREAMCAM_STATUS_INVALID_ARGUMENT;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const size_t next_index = (latest_index_ + 1U) % slots_.size();
    Slot& slot = slots_[next_index];
    slot.bytes.resize(size_bytes);
    std::memcpy(slot.bytes.data(), data, size_bytes);
    slot.width = width;
    slot.height = height;
    slot.stride_bytes = stride_bytes;
    slot.timestamp_ns = timestamp_ns;
    slot.pixel_format = pixel_format;
    slot.sequence = ++sequence_;
    latest_index_ = next_index;
    has_frame_ = true;
    return STREAMCAM_STATUS_OK;
  }

  streamcam_status Latest(streamcam_frame_view* out_frame) const {
    if (out_frame == nullptr) {
      return STREAMCAM_STATUS_INVALID_ARGUMENT;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!has_frame_) {
      return STREAMCAM_STATUS_NO_FRAME;
    }

    const Slot& slot = slots_[latest_index_];
    out_frame->data = slot.bytes.data();
    out_frame->size_bytes = slot.bytes.size();
    out_frame->width = slot.width;
    out_frame->height = slot.height;
    out_frame->stride_bytes = slot.stride_bytes;
    out_frame->timestamp_ns = slot.timestamp_ns;
    out_frame->sequence = slot.sequence;
    out_frame->pixel_format = slot.pixel_format;
    return STREAMCAM_STATUS_OK;
  }

 private:
  struct Slot {
    std::vector<uint8_t> bytes;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride_bytes = 0;
    uint64_t timestamp_ns = 0;
    uint64_t sequence = 0;
    streamcam_pixel_format pixel_format = STREAMCAM_PIXEL_FORMAT_UNKNOWN;
  };

  mutable std::mutex mutex_;
  std::array<Slot, 3> slots_;
  size_t latest_index_ = 0;
  uint64_t sequence_ = 0;
  bool has_frame_ = false;
};

}  // namespace streamcam

#endif  // STREAMCAM_SRC_INTERNAL_FRAME_STORE_HPP_
