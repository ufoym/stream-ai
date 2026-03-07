# streamcam

`streamcam` is a zero-dependency webcam capture library that targets low-latency
native camera access with a small C API.

This initial version focuses on:

- Zero third-party runtime dependencies
- A portable C ABI for easy language bindings
- A fast "latest frame" capture model
- A working macOS backend built on AVFoundation
- A codebase layout that can be extended to Windows and Linux native backends

## Design notes

The library intentionally exposes the latest available frame instead of a fully
managed queue. This keeps the hot path short:

- The platform backend receives frames from the OS camera stack
- Frames are copied into a tiny internal ring of reusable slots
- Consumers poll the newest frame without extra synchronization primitives in the
  public API

This model is a good default for realtime inference, preview, and streaming
pipelines where stale frames are less useful than the most recent frame.

## Build

```bash
cmake -S . -B build
cmake --build build
```

## Quick start

```cpp
#include <vector>
#include "streamcam/streamcam.h"

size_t count = 0;
streamcam_list_devices(nullptr, 0, &count);

std::vector<streamcam_device_info> devices(count);
streamcam_list_devices(devices.data(), devices.size(), &count);

streamcam_config config = streamcam_default_config();
streamcam_reader* reader = nullptr;
streamcam_open(devices[0].id, &config, &reader);
streamcam_start(reader);

streamcam_frame_view frame{};
if (streamcam_get_latest_frame(reader, &frame) == STREAMCAM_STATUS_OK) {
  // frame.data points to the latest captured image bytes.
}

streamcam_close(reader);
```

## Examples

- `streamcam_list_devices`
- `streamcam_capture_benchmark`

## Current status

- macOS: implemented with `AVCaptureSession + AVCaptureVideoDataOutput`
- Windows: public API and build layout are ready, native backend is not implemented yet
- Linux: public API and build layout are ready, native backend is not implemented yet

## Next steps

- Add Windows Media Foundation backend
- Add Linux V4L2 backend
- Support more native pixel formats with zero conversion on hot paths
- Expose camera controls such as exposure, focus, and white balance
