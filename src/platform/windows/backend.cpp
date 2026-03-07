#include "platform/windows/backend.hpp"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace streamcam {

using Microsoft::WRL::ComPtr;

namespace {

class ScopedComInitialization {
 public:
  ScopedComInitialization() : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}

  ~ScopedComInitialization() {
    if (SUCCEEDED(result_)) {
      CoUninitialize();
    }
  }

  HRESULT result() const {
    if (result_ == RPC_E_CHANGED_MODE) {
      return S_OK;
    }
    return result_;
  }

 private:
  HRESULT result_;
};

class ScopedMediaFoundationStartup {
 public:
  ScopedMediaFoundationStartup() : result_(MFStartup(MF_VERSION)), started_(SUCCEEDED(result_)) {}

  ~ScopedMediaFoundationStartup() {
    if (started_) {
      MFShutdown();
    }
  }

  HRESULT result() const { return result_; }

 private:
  HRESULT result_;
  bool started_;
};

uint64_t HundredNanosecondsToNanoseconds(const LONGLONG value) {
  if (value <= 0) {
    return 0;
  }
  return static_cast<uint64_t>(value) * 100ULL;
}

std::string WideToUtf8(const wchar_t* value) {
  if (value == nullptr || value[0] == L'\0') {
    return {};
  }

  const int required = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
  if (required <= 1) {
    return {};
  }

  std::string result(static_cast<size_t>(required), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), required, nullptr, nullptr);
  result.pop_back();
  return result;
}

streamcam_status StatusFromHresult(const HRESULT hr) {
  if (SUCCEEDED(hr)) {
    return STREAMCAM_STATUS_OK;
  }
  if (hr == MF_E_NOT_FOUND || hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) {
    return STREAMCAM_STATUS_NOT_FOUND;
  }
  if (hr == MF_E_INVALIDMEDIATYPE || hr == MF_E_INVALIDTYPE || hr == MF_E_TOPO_CODEC_NOT_FOUND) {
    return STREAMCAM_STATUS_NOT_SUPPORTED;
  }
  return STREAMCAM_STATUS_PLATFORM_ERROR;
}

streamcam_pixel_format StreamcamFormatFromSubtype(const GUID& subtype) {
  if (subtype == MFVideoFormat_NV12) {
    return STREAMCAM_PIXEL_FORMAT_NV12;
  }
  if (subtype == MFVideoFormat_YUY2) {
    return STREAMCAM_PIXEL_FORMAT_YUY2;
  }
  if (subtype == MFVideoFormat_MJPG) {
    return STREAMCAM_PIXEL_FORMAT_MJPEG;
  }
  if (subtype == MFVideoFormat_RGB32 || subtype == MFVideoFormat_ARGB32) {
    return STREAMCAM_PIXEL_FORMAT_BGRA32;
  }
  return STREAMCAM_PIXEL_FORMAT_UNKNOWN;
}

uint32_t DefaultStrideBytes(const streamcam_pixel_format pixel_format, const uint32_t width) {
  switch (pixel_format) {
    case STREAMCAM_PIXEL_FORMAT_BGRA32:
      return width * 4U;
    case STREAMCAM_PIXEL_FORMAT_YUY2:
      return width * 2U;
    case STREAMCAM_PIXEL_FORMAT_NV12:
    case STREAMCAM_PIXEL_FORMAT_MJPEG:
    case STREAMCAM_PIXEL_FORMAT_NATIVE:
    case STREAMCAM_PIXEL_FORMAT_UNKNOWN:
    default:
      return width;
  }
}

std::vector<const GUID*> PreferredSubtypeOrder(const streamcam_pixel_format preferred) {
  switch (preferred) {
    case STREAMCAM_PIXEL_FORMAT_BGRA32:
      return {&MFVideoFormat_RGB32, &MFVideoFormat_YUY2, &MFVideoFormat_NV12,
              &MFVideoFormat_MJPG};
    case STREAMCAM_PIXEL_FORMAT_NV12:
      return {&MFVideoFormat_NV12, &MFVideoFormat_YUY2, &MFVideoFormat_MJPG,
              &MFVideoFormat_RGB32};
    case STREAMCAM_PIXEL_FORMAT_MJPEG:
      return {&MFVideoFormat_MJPG, &MFVideoFormat_YUY2, &MFVideoFormat_NV12,
              &MFVideoFormat_RGB32};
    case STREAMCAM_PIXEL_FORMAT_YUY2:
      return {&MFVideoFormat_YUY2, &MFVideoFormat_NV12, &MFVideoFormat_MJPG,
              &MFVideoFormat_RGB32};
    case STREAMCAM_PIXEL_FORMAT_NATIVE:
    case STREAMCAM_PIXEL_FORMAT_UNKNOWN:
    default:
      return {&MFVideoFormat_YUY2, &MFVideoFormat_NV12, &MFVideoFormat_MJPG,
              &MFVideoFormat_RGB32};
  }
}

int FormatRank(const GUID& subtype, const streamcam_pixel_format preferred) {
  const std::vector<const GUID*> order = PreferredSubtypeOrder(preferred);
  for (size_t i = 0; i < order.size(); ++i) {
    if (subtype == *order[i]) {
      return static_cast<int>(i);
    }
  }
  return static_cast<int>(order.size()) + 1;
}

uint64_t ResolutionDistance(const uint32_t lhs, const uint32_t rhs) {
  if (lhs >= rhs) {
    return static_cast<uint64_t>(lhs - rhs);
  }
  return static_cast<uint64_t>(rhs - lhs);
}

uint64_t FrameRateDistance(const UINT32 numerator, const UINT32 denominator,
                           const uint32_t target_fps) {
  if (numerator == 0 || denominator == 0 || target_fps == 0) {
    return 0;
  }
  const double fps = static_cast<double>(numerator) / static_cast<double>(denominator);
  return static_cast<uint64_t>(std::llround(std::abs(fps - static_cast<double>(target_fps)) *
                                            1000.0));
}

HRESULT EnumerateVideoDevices(std::vector<ComPtr<IMFActivate>>* out_devices) {
  if (out_devices == nullptr) {
    return E_POINTER;
  }

  out_devices->clear();

  ComPtr<IMFAttributes> attributes;
  HRESULT hr = MFCreateAttributes(&attributes, 1);
  if (FAILED(hr)) {
    return hr;
  }

  hr = attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                           MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
  if (FAILED(hr)) {
    return hr;
  }

  IMFActivate** activates = nullptr;
  UINT32 count = 0;
  hr = MFEnumDeviceSources(attributes.Get(), &activates, &count);
  if (FAILED(hr)) {
    return hr;
  }

  out_devices->reserve(count);
  for (UINT32 i = 0; i < count; ++i) {
    ComPtr<IMFActivate> activate;
    activate.Attach(activates[i]);
    out_devices->push_back(std::move(activate));
  }
  CoTaskMemFree(activates);
  return S_OK;
}

HRESULT GetActivateAllocatedString(IMFActivate* activate, const GUID& key, std::string* out_value) {
  if (activate == nullptr || out_value == nullptr) {
    return E_POINTER;
  }

  wchar_t* raw_value = nullptr;
  UINT32 raw_length = 0;
  const HRESULT hr = activate->GetAllocatedString(key, &raw_value, &raw_length);
  if (FAILED(hr)) {
    return hr;
  }

  (void)raw_length;
  out_value->assign(WideToUtf8(raw_value));
  CoTaskMemFree(raw_value);
  return S_OK;
}

struct MediaTypeCandidate {
  ComPtr<IMFMediaType> media_type;
  streamcam_pixel_format pixel_format = STREAMCAM_PIXEL_FORMAT_UNKNOWN;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t stride_bytes = 0;
  uint64_t score = std::numeric_limits<uint64_t>::max();
};

HRESULT ChooseMediaType(IMFSourceReader* reader, const streamcam_config& config,
                        MediaTypeCandidate* out_candidate) {
  if (reader == nullptr || out_candidate == nullptr) {
    return E_POINTER;
  }

  std::vector<MediaTypeCandidate> candidates;
  for (DWORD index = 0;; ++index) {
    ComPtr<IMFMediaType> media_type;
    const HRESULT hr =
        reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, index, &media_type);
    if (hr == MF_E_NO_MORE_TYPES) {
      break;
    }
    if (FAILED(hr)) {
      return hr;
    }

    GUID major_type = GUID_NULL;
    GUID subtype = GUID_NULL;
    if (FAILED(media_type->GetGUID(MF_MT_MAJOR_TYPE, &major_type)) ||
        major_type != MFMediaType_Video ||
        FAILED(media_type->GetGUID(MF_MT_SUBTYPE, &subtype))) {
      continue;
    }

    const streamcam_pixel_format pixel_format = StreamcamFormatFromSubtype(subtype);
    if (pixel_format == STREAMCAM_PIXEL_FORMAT_UNKNOWN) {
      continue;
    }

    UINT32 width = 0;
    UINT32 height = 0;
    if (FAILED(MFGetAttributeSize(media_type.Get(), MF_MT_FRAME_SIZE, &width, &height))) {
      continue;
    }

    UINT32 fps_numerator = 0;
    UINT32 fps_denominator = 0;
    MFGetAttributeRatio(media_type.Get(), MF_MT_FRAME_RATE, &fps_numerator, &fps_denominator);

    uint32_t stride_bytes = 0;
    UINT32 raw_stride = 0;
    if (SUCCEEDED(media_type->GetUINT32(MF_MT_DEFAULT_STRIDE, &raw_stride))) {
      const LONG signed_stride = static_cast<LONG>(raw_stride);
      stride_bytes =
          static_cast<uint32_t>(signed_stride >= 0 ? signed_stride : -signed_stride);
    } else {
      LONG computed_stride = 0;
      if (SUCCEEDED(MFGetStrideForBitmapInfoHeader(subtype.Data1,
                                                   static_cast<LONG>(width), &computed_stride))) {
        stride_bytes =
            static_cast<uint32_t>(computed_stride >= 0 ? computed_stride : -computed_stride);
      } else {
        stride_bytes = DefaultStrideBytes(pixel_format, width);
      }
    }

    MediaTypeCandidate candidate;
    candidate.media_type = media_type;
    candidate.pixel_format = pixel_format;
    candidate.width = width;
    candidate.height = height;
    candidate.stride_bytes = stride_bytes;
    candidate.score =
        static_cast<uint64_t>(FormatRank(subtype, config.preferred_format)) * 1000000000000ULL +
        ResolutionDistance(width, config.width) * 1000000ULL +
        ResolutionDistance(height, config.height) * 1000ULL +
        FrameRateDistance(fps_numerator, fps_denominator, config.fps);
    candidates.push_back(std::move(candidate));
  }

  if (candidates.empty()) {
    return MF_E_INVALIDMEDIATYPE;
  }

  std::sort(candidates.begin(), candidates.end(),
            [](const MediaTypeCandidate& lhs, const MediaTypeCandidate& rhs) {
              return lhs.score < rhs.score;
            });

  for (const MediaTypeCandidate& candidate : candidates) {
    const HRESULT hr =
        reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr,
                                    candidate.media_type.Get());
    if (SUCCEEDED(hr)) {
      *out_candidate = candidate;
      return S_OK;
    }
  }

  return MF_E_INVALIDMEDIATYPE;
}

}  // namespace

class WindowsSourceReaderCallback final : public IMFSourceReaderCallback {
 public:
  explicit WindowsSourceReaderCallback(class WindowsBackendImpl* owner) : owner_(owner) {}

  STDMETHODIMP QueryInterface(REFIID riid, void** out_object) override;
  STDMETHODIMP_(ULONG) AddRef() override;
  STDMETHODIMP_(ULONG) Release() override;
  STDMETHODIMP OnReadSample(HRESULT status, DWORD stream_index, DWORD stream_flags,
                            LONGLONG timestamp, IMFSample* sample) override;
  STDMETHODIMP OnEvent(DWORD stream_index, IMFMediaEvent* event) override;
  STDMETHODIMP OnFlush(DWORD stream_index) override;

 private:
  std::atomic<ULONG> ref_count_{1};
  class WindowsBackendImpl* owner_ = nullptr;
};

class WindowsBackendImpl {
 public:
  WindowsBackendImpl(WindowsBackend* owner, const char* device_id, const streamcam_config& config)
      : owner_(owner), device_id_(device_id ? device_id : ""), config_(config) {}

  ~WindowsBackendImpl() { Stop(); }

  streamcam_status Start() {
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (worker_thread_.joinable()) {
        return STREAMCAM_STATUS_OK;
      }
      stop_requested_ = false;
      start_completed_ = false;
      start_status_ = STREAMCAM_STATUS_PLATFORM_ERROR;
    }

    worker_thread_ = std::thread([this]() { WorkerMain(); });

    streamcam_status status = STREAMCAM_STATUS_PLATFORM_ERROR;
    {
      std::unique_lock<std::mutex> lock(state_mutex_);
      start_cv_.wait(lock, [this]() { return start_completed_; });
      status = start_status_;
    }

    if (status != STREAMCAM_STATUS_OK && worker_thread_.joinable()) {
      worker_thread_.join();
    }
    return status;
  }

  streamcam_status Stop() {
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (!worker_thread_.joinable()) {
        return STREAMCAM_STATUS_OK;
      }
      running_.store(false);
      stop_requested_ = true;
    }
    stop_cv_.notify_all();

    worker_thread_.join();
    return STREAMCAM_STATUS_OK;
  }

  HRESULT HandleReadSample(const HRESULT status, const DWORD stream_flags,
                           const LONGLONG timestamp, IMFSample* sample) {
    if (!running_.load()) {
      return S_OK;
    }

    if (FAILED(status)) {
      running_.store(false);
      RequestStop();
      return status;
    }

    if (sample != nullptr) {
      ComPtr<IMFMediaBuffer> buffer;
      HRESULT hr = sample->ConvertToContiguousBuffer(&buffer);
      if (FAILED(hr)) {
        running_.store(false);
        RequestStop();
        return hr;
      }

      uint8_t* data = nullptr;
      DWORD max_length = 0;
      DWORD current_length = 0;
      hr = buffer->Lock(&data, &max_length, &current_length);
      if (FAILED(hr)) {
        running_.store(false);
        RequestStop();
        return hr;
      }

      if (data != nullptr && current_length != 0) {
        owner_->PublishFrame(data, current_length, width_, height_, stride_bytes_, pixel_format_,
                             HundredNanosecondsToNanoseconds(timestamp));
      }
      buffer->Unlock();
    }

    if ((stream_flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
      running_.store(false);
      RequestStop();
      return S_OK;
    }

    if (!running_.load()) {
      return S_OK;
    }
    const HRESULT hr = QueueNextSample();
    if (FAILED(hr)) {
      running_.store(false);
      RequestStop();
    }
    return hr;
  }

 private:
  void WorkerMain() {
    const ScopedComInitialization com_scope;
    const HRESULT com_result = com_scope.result();
    if (FAILED(com_result)) {
      SignalStart(StatusFromHresult(com_result));
      return;
    }

    const ScopedMediaFoundationStartup mf_scope;
    const HRESULT mf_result = mf_scope.result();
    if (FAILED(mf_result)) {
      SignalStart(StatusFromHresult(mf_result));
      return;
    }

    HRESULT hr = InitializeReader();
    if (SUCCEEDED(hr)) {
      running_.store(true);
      hr = QueueNextSample();
    }

    SignalStart(StatusFromHresult(hr));
    if (FAILED(hr)) {
      running_.store(false);
      ShutdownReader();
      return;
    }

    std::unique_lock<std::mutex> lock(state_mutex_);
    stop_cv_.wait(lock, [this]() { return stop_requested_; });
    lock.unlock();

    ShutdownReader();
  }

  HRESULT InitializeReader() {
    std::vector<ComPtr<IMFActivate>> devices;
    HRESULT hr = EnumerateVideoDevices(&devices);
    if (FAILED(hr)) {
      return hr;
    }

    ComPtr<IMFActivate> selected_device;
    for (const ComPtr<IMFActivate>& device : devices) {
      std::string symbolic_link;
      if (FAILED(GetActivateAllocatedString(
              device.Get(), MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
              &symbolic_link))) {
        continue;
      }
      if (symbolic_link == device_id_) {
        selected_device = device;
        break;
      }
    }

    if (!selected_device) {
      return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }

    hr = selected_device->ActivateObject(IID_PPV_ARGS(&media_source_));
    if (FAILED(hr)) {
      return hr;
    }

    callback_.Attach(new WindowsSourceReaderCallback(this));

    ComPtr<IMFAttributes> reader_attributes;
    hr = MFCreateAttributes(&reader_attributes, 2);
    if (FAILED(hr)) {
      return hr;
    }

    hr = reader_attributes->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, callback_.Get());
    if (FAILED(hr)) {
      return hr;
    }

    hr = reader_attributes->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA, TRUE);
    if (FAILED(hr)) {
      return hr;
    }

    hr = MFCreateSourceReaderFromMediaSource(media_source_.Get(), reader_attributes.Get(),
                                             &source_reader_);
    if (FAILED(hr)) {
      return hr;
    }

    hr = source_reader_->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
    if (FAILED(hr)) {
      return hr;
    }

    MediaTypeCandidate selected_type;
    hr = ChooseMediaType(source_reader_.Get(), config_, &selected_type);
    if (FAILED(hr)) {
      return hr;
    }

    width_ = selected_type.width;
    height_ = selected_type.height;
    stride_bytes_ = selected_type.stride_bytes;
    pixel_format_ = selected_type.pixel_format;
    return S_OK;
  }

  HRESULT QueueNextSample() {
    std::lock_guard<std::mutex> lock(source_reader_mutex_);
    if (!source_reader_) {
      return MF_E_SHUTDOWN;
    }
    return source_reader_->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, nullptr,
                                      nullptr, nullptr);
  }

  void ShutdownReader() {
    running_.store(false);

    std::lock_guard<std::mutex> lock(source_reader_mutex_);
    if (source_reader_) {
      source_reader_->Flush(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
      source_reader_->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, FALSE);
      source_reader_.Reset();
    }
    if (media_source_) {
      media_source_->Shutdown();
      media_source_.Reset();
    }
    callback_.Reset();
    width_ = 0;
    height_ = 0;
    stride_bytes_ = 0;
    pixel_format_ = STREAMCAM_PIXEL_FORMAT_UNKNOWN;
  }

  void SignalStart(const streamcam_status status) {
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      start_status_ = status;
      start_completed_ = true;
      if (status != STREAMCAM_STATUS_OK) {
        stop_requested_ = true;
      }
    }
    start_cv_.notify_all();
  }

  void RequestStop() {
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      stop_requested_ = true;
    }
    stop_cv_.notify_all();
  }

  WindowsBackend* owner_ = nullptr;
  std::string device_id_;
  streamcam_config config_{};
  std::mutex state_mutex_;
  std::condition_variable start_cv_;
  std::condition_variable stop_cv_;
  std::mutex source_reader_mutex_;
  std::thread worker_thread_;
  bool stop_requested_ = false;
  bool start_completed_ = false;
  streamcam_status start_status_ = STREAMCAM_STATUS_PLATFORM_ERROR;
  std::atomic<bool> running_{false};
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint32_t stride_bytes_ = 0;
  streamcam_pixel_format pixel_format_ = STREAMCAM_PIXEL_FORMAT_UNKNOWN;
  ComPtr<IMFMediaSource> media_source_;
  ComPtr<IMFSourceReader> source_reader_;
  ComPtr<WindowsSourceReaderCallback> callback_;
};

STDMETHODIMP WindowsSourceReaderCallback::QueryInterface(REFIID riid, void** out_object) {
  if (out_object == nullptr) {
    return E_POINTER;
  }
  if (riid == IID_IUnknown || riid == __uuidof(IMFSourceReaderCallback)) {
    *out_object = static_cast<IMFSourceReaderCallback*>(this);
    AddRef();
    return S_OK;
  }
  *out_object = nullptr;
  return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) WindowsSourceReaderCallback::AddRef() {
  return ++ref_count_;
}

STDMETHODIMP_(ULONG) WindowsSourceReaderCallback::Release() {
  const ULONG ref_count = --ref_count_;
  if (ref_count == 0) {
    delete this;
  }
  return ref_count;
}

STDMETHODIMP WindowsSourceReaderCallback::OnReadSample(HRESULT status, DWORD stream_index,
                                                       DWORD stream_flags, LONGLONG timestamp,
                                                       IMFSample* sample) {
  (void)stream_index;
  if (owner_ == nullptr) {
    return S_OK;
  }
  return owner_->HandleReadSample(status, stream_flags, timestamp, sample);
}

STDMETHODIMP WindowsSourceReaderCallback::OnEvent(DWORD stream_index, IMFMediaEvent* event) {
  (void)stream_index;
  (void)event;
  return S_OK;
}

STDMETHODIMP WindowsSourceReaderCallback::OnFlush(DWORD stream_index) {
  (void)stream_index;
  return S_OK;
}

std::vector<streamcam_device_info> ListDevicesWindows() {
  const ScopedComInitialization com_scope;
  if (FAILED(com_scope.result())) {
    return {};
  }

  const ScopedMediaFoundationStartup mf_scope;
  if (FAILED(mf_scope.result())) {
    return {};
  }

  std::vector<ComPtr<IMFActivate>> devices;
  if (FAILED(EnumerateVideoDevices(&devices))) {
    return {};
  }

  std::vector<streamcam_device_info> listed;
  listed.reserve(devices.size());
  for (const ComPtr<IMFActivate>& device : devices) {
    std::string symbolic_link;
    std::string friendly_name;
    if (FAILED(GetActivateAllocatedString(
            device.Get(), MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
            &symbolic_link)) ||
        FAILED(GetActivateAllocatedString(device.Get(), MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME,
                                          &friendly_name))) {
      continue;
    }

    streamcam_device_info info{};
    std::strncpy(info.id, symbolic_link.c_str(), STREAMCAM_DEVICE_ID_MAX - 1);
    std::strncpy(info.name, friendly_name.c_str(), STREAMCAM_DEVICE_NAME_MAX - 1);
    listed.push_back(info);
  }
  return listed;
}

WindowsBackend::WindowsBackend(const char* device_id, const streamcam_config& config)
    : config_(config), impl_(new WindowsBackendImpl(this, device_id, config)) {}

WindowsBackend::~WindowsBackend() { delete impl_; }

streamcam_status WindowsBackend::Start() { return impl_->Start(); }

streamcam_status WindowsBackend::Stop() { return impl_->Stop(); }

streamcam_status WindowsBackend::LatestFrame(streamcam_frame_view* out_frame) {
  return store_.Latest(out_frame);
}

streamcam_status WindowsBackend::PublishFrame(const uint8_t* data, const size_t size_bytes,
                                              const uint32_t width, const uint32_t height,
                                              const uint32_t stride_bytes,
                                              const streamcam_pixel_format pixel_format,
                                              const uint64_t timestamp_ns) {
  return store_.Publish(data, size_bytes, width, height, stride_bytes, pixel_format,
                        timestamp_ns);
}

}  // namespace streamcam
