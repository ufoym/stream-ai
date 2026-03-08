# streamcam

`streamcam` is a zero-dependency webcam capture library that targets low-latency
native camera access with a small C API.

This initial version focuses on:

- Zero third-party runtime dependencies
- A portable C ABI for easy language bindings
- A fast "latest frame" capture model
- A working macOS backend built on AVFoundation
- A working Linux backend built on V4L2
- A working Windows backend built on Media Foundation

## Design notes

The library intentionally exposes the latest available frame instead of a fully
managed queue. This keeps the hot path short:

- The platform backend receives frames from the OS camera stack
- Frames are copied into a tiny internal ring of reusable slots
- Consumers poll the newest frame without extra synchronization primitives in the
  public API

This model is a good default for realtime inference, preview, and streaming
pipelines where stale frames are less useful than the most recent frame.

The default config prefers `STREAMCAM_PIXEL_FORMAT_NATIVE`, so each platform can
stay on its fastest native path unless the caller explicitly requests a specific
format.

## Build

```bash
cmake -S . -B build
cmake --build build
```

The default build now produces:

- `libstreamcam.a` (or platform equivalent static library) for native C/C++ linking
- `libstreamcam.{dylib,so}` / `streamcam.dll` for FFI use such as Python `ctypes`
- `examples/streamcam.*` as a CPython extension module for the Python examples

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

- C++:
  - `examples/list_devices.cpp`
  - `examples/capture_benchmark.cpp`
- Python:
  - `import streamcam`
  - `examples/list_devices.py`
  - `examples/capture_benchmark.py`

Run the C++ examples after building:

```bash
./build/streamcam_list_devices
./build/streamcam_capture_benchmark
```

Run the Python examples after building:

```bash
python3 examples/list_devices.py
python3 examples/capture_benchmark.py
```

The build places the compiled `streamcam` extension module next to the Python
examples so they can `import streamcam` directly without additional packaging or
`PYTHONPATH` setup.

## Current status

- macOS: implemented with `AVCaptureSession + AVCaptureVideoDataOutput`
- Linux: implemented with `V4L2 + mmap + polling capture thread`
- Windows: implemented with `Media Foundation + IMFSourceReader` asynchronous capture

## Next steps

- Support more native pixel formats with zero conversion on hot paths
- Expose camera controls such as exposure, focus, and white balance
- Add CI coverage on native Linux runners with real V4L2 smoke tests
