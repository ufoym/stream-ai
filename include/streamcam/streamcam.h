#ifndef STREAMCAM_STREAMCAM_H_
#define STREAMCAM_STREAMCAM_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STREAMCAM_DEVICE_ID_MAX 256
#define STREAMCAM_DEVICE_NAME_MAX 256

typedef enum streamcam_status {
  STREAMCAM_STATUS_OK = 0,
  STREAMCAM_STATUS_INVALID_ARGUMENT = -1,
  STREAMCAM_STATUS_NOT_FOUND = -2,
  STREAMCAM_STATUS_NOT_SUPPORTED = -3,
  STREAMCAM_STATUS_PLATFORM_ERROR = -4,
  STREAMCAM_STATUS_NO_FRAME = -5
} streamcam_status;

typedef enum streamcam_pixel_format {
  STREAMCAM_PIXEL_FORMAT_UNKNOWN = 0,
  STREAMCAM_PIXEL_FORMAT_NATIVE = 1,
  STREAMCAM_PIXEL_FORMAT_BGRA32 = 2,
  STREAMCAM_PIXEL_FORMAT_NV12 = 3,
  STREAMCAM_PIXEL_FORMAT_MJPEG = 4,
  STREAMCAM_PIXEL_FORMAT_YUY2 = 5
} streamcam_pixel_format;

typedef struct streamcam_device_info {
  char id[STREAMCAM_DEVICE_ID_MAX];
  char name[STREAMCAM_DEVICE_NAME_MAX];
} streamcam_device_info;

typedef struct streamcam_config {
  uint32_t width;
  uint32_t height;
  uint32_t fps;
  streamcam_pixel_format preferred_format;
} streamcam_config;

typedef struct streamcam_frame_view {
  const uint8_t* data;
  size_t size_bytes;
  uint32_t width;
  uint32_t height;
  uint32_t stride_bytes;
  uint64_t timestamp_ns;
  uint64_t sequence;
  streamcam_pixel_format pixel_format;
} streamcam_frame_view;

typedef struct streamcam_reader streamcam_reader;

const char* streamcam_version(void);
const char* streamcam_status_string(streamcam_status status);
streamcam_config streamcam_default_config(void);

streamcam_status streamcam_list_devices(streamcam_device_info* devices,
                                        size_t capacity,
                                        size_t* count);

streamcam_status streamcam_open(const char* device_id,
                                const streamcam_config* config,
                                streamcam_reader** out_reader);

streamcam_status streamcam_start(streamcam_reader* reader);
streamcam_status streamcam_stop(streamcam_reader* reader);

/*
 * Returns a zero-copy view of the latest frame owned by the reader.
 * The pointer stays valid until the next successful capture or until the
 * reader is closed.
 */
streamcam_status streamcam_get_latest_frame(streamcam_reader* reader,
                                            streamcam_frame_view* out_frame);

void streamcam_close(streamcam_reader* reader);

#ifdef __cplusplus
}
#endif

#endif  // STREAMCAM_STREAMCAM_H_
