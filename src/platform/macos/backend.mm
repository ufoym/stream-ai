#import "platform/macos/backend.hpp"

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace streamcam {
class MacosBackendImpl;
}

@interface StreamCamVideoDelegate : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate> {
 @private
  streamcam::MacosBackendImpl* owner_;
}
- (instancetype)initWithOwner:(streamcam::MacosBackendImpl*)owner;
@end

namespace streamcam {

namespace {

uint64_t TimestampToNanoseconds(CMTime time) {
  if (!CMTIME_IS_VALID(time) || time.timescale == 0) {
    return 0;
  }

  const long double seconds =
      static_cast<long double>(time.value) / static_cast<long double>(time.timescale);
  return static_cast<uint64_t>(seconds * 1000000000.0L);
}

streamcam_pixel_format ToPixelFormat(streamcam_pixel_format preferred) {
  switch (preferred) {
    case STREAMCAM_PIXEL_FORMAT_BGRA32:
    case STREAMCAM_PIXEL_FORMAT_NATIVE:
    default:
      return STREAMCAM_PIXEL_FORMAT_BGRA32;
  }
}

OSType ToCoreVideoPixelFormat(streamcam_pixel_format preferred) {
  switch (ToPixelFormat(preferred)) {
    case STREAMCAM_PIXEL_FORMAT_BGRA32:
    default:
      return kCVPixelFormatType_32BGRA;
  }
}

}  // namespace

class MacosBackendImpl {
 public:
  MacosBackendImpl(MacosBackend* owner, const char* device_id, const streamcam_config& config)
      : owner_(owner), device_id_(device_id ? device_id : ""), config_(config) {}

  ~MacosBackendImpl() {
    Stop();
    if (queue_ != nullptr) {
      dispatch_release(queue_);
      queue_ = nullptr;
    }
  }

  streamcam_status Start() {
    @autoreleasepool {
      if (session_ != nil) {
        return STREAMCAM_STATUS_OK;
      }

      AVCaptureDevice* device = FindDevice();
      if (device == nil) {
        return STREAMCAM_STATUS_NOT_FOUND;
      }

      NSError* error = nil;
      AVCaptureDeviceInput* input =
          [AVCaptureDeviceInput deviceInputWithDevice:device error:&error];
      if (input == nil || error != nil) {
        return STREAMCAM_STATUS_PLATFORM_ERROR;
      }

      session_ = [[AVCaptureSession alloc] init];
      if (config_.width >= 1920 || config_.height >= 1080) {
        session_.sessionPreset = AVCaptureSessionPreset1920x1080;
      } else if (config_.width >= 1280 || config_.height >= 720) {
        session_.sessionPreset = AVCaptureSessionPreset1280x720;
      } else {
        session_.sessionPreset = AVCaptureSessionPreset640x480;
      }

      if (![session_ canAddInput:input]) {
        [session_ release];
        session_ = nil;
        return STREAMCAM_STATUS_PLATFORM_ERROR;
      }
      [session_ addInput:input];

      output_ = [[AVCaptureVideoDataOutput alloc] init];
      output_.alwaysDiscardsLateVideoFrames = YES;
      NSDictionary* settings = @{
        (id)kCVPixelBufferPixelFormatTypeKey :
            @(ToCoreVideoPixelFormat(config_.preferred_format))
      };
      output_.videoSettings = settings;

      if (queue_ == nullptr) {
        queue_ = dispatch_queue_create("ai.streamcam.capture", DISPATCH_QUEUE_SERIAL);
      }

      delegate_ = [[StreamCamVideoDelegate alloc] initWithOwner:this];
      [output_ setSampleBufferDelegate:delegate_ queue:queue_];

      if (![session_ canAddOutput:output_]) {
        [output_ setSampleBufferDelegate:nil queue:nullptr];
        [delegate_ release];
        delegate_ = nil;
        [output_ release];
        output_ = nil;
        [session_ release];
        session_ = nil;
        return STREAMCAM_STATUS_PLATFORM_ERROR;
      }
      [session_ addOutput:output_];

      ConfigureDevice(device);
      [session_ startRunning];
      return STREAMCAM_STATUS_OK;
    }
  }

  streamcam_status Stop() {
    @autoreleasepool {
      if (session_ != nil) {
        [session_ stopRunning];
      }

      if (output_ != nil) {
        [output_ setSampleBufferDelegate:nil queue:nullptr];
        [output_ release];
        output_ = nil;
      }

      if (delegate_ != nil) {
        [delegate_ release];
        delegate_ = nil;
      }

      if (session_ != nil) {
        [session_ release];
        session_ = nil;
      }
      return STREAMCAM_STATUS_OK;
    }
  }

  void HandleSampleBuffer(CMSampleBufferRef sample_buffer) {
    CVImageBufferRef image_buffer = CMSampleBufferGetImageBuffer(sample_buffer);
    if (image_buffer == nullptr) {
      return;
    }

    CVPixelBufferRef pixel_buffer = static_cast<CVPixelBufferRef>(image_buffer);
    CVPixelBufferLockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly);

    const uint8_t* base =
        static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(pixel_buffer));
    const size_t bytes_per_row = static_cast<size_t>(CVPixelBufferGetBytesPerRow(pixel_buffer));
    const uint32_t width =
        static_cast<uint32_t>(CVPixelBufferGetWidth(pixel_buffer));
    const uint32_t height =
        static_cast<uint32_t>(CVPixelBufferGetHeight(pixel_buffer));
    const size_t size_bytes = bytes_per_row * static_cast<size_t>(height);
    const uint64_t timestamp_ns =
        TimestampToNanoseconds(CMSampleBufferGetPresentationTimeStamp(sample_buffer));

    if (base != nullptr && size_bytes != 0) {
      owner_->PublishFrame(base, size_bytes, width, height,
                           static_cast<uint32_t>(bytes_per_row),
                           STREAMCAM_PIXEL_FORMAT_BGRA32, timestamp_ns);
    }

    CVPixelBufferUnlockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly);
  }

 private:
  AVCaptureDevice* FindDevice() {
    AVCaptureDeviceDiscoverySession* discovery = [AVCaptureDeviceDiscoverySession
        discoverySessionWithDeviceTypes:@[
          AVCaptureDeviceTypeBuiltInWideAngleCamera,
          AVCaptureDeviceTypeExternal
        ]
        mediaType:AVMediaTypeVideo
        position:AVCaptureDevicePositionUnspecified];

    for (AVCaptureDevice* device in discovery.devices) {
      if (device_id_ == std::string([[device uniqueID] UTF8String])) {
        return device;
      }
    }
    return nil;
  }

  void ConfigureDevice(AVCaptureDevice* device) {
    NSError* error = nil;
    if (![device lockForConfiguration:&error]) {
      return;
    }

    double desired_fps = static_cast<double>(std::max<uint32_t>(1U, config_.fps));
    AVFrameRateRange* selected_range = nil;
    for (AVFrameRateRange* range in device.activeFormat.videoSupportedFrameRateRanges) {
      if (range.minFrameRate <= desired_fps && desired_fps <= range.maxFrameRate) {
        selected_range = range;
        break;
      }
      if (selected_range == nil || range.maxFrameRate > selected_range.maxFrameRate) {
        selected_range = range;
      }
    }

    if (selected_range != nil) {
      if (desired_fps < selected_range.minFrameRate) {
        desired_fps = selected_range.minFrameRate;
      }
      if (desired_fps > selected_range.maxFrameRate) {
        desired_fps = selected_range.maxFrameRate;
      }
      const int32_t timescale = static_cast<int32_t>(desired_fps + 0.5);
      if (timescale > 0) {
        const CMTime frame_duration = CMTimeMake(1, timescale);
        device.activeVideoMinFrameDuration = frame_duration;
        device.activeVideoMaxFrameDuration = frame_duration;
      }
    }

    [device unlockForConfiguration];
  }

  MacosBackend* owner_ = nullptr;
  std::string device_id_;
  streamcam_config config_{};
  AVCaptureSession* session_ = nil;
  AVCaptureVideoDataOutput* output_ = nil;
  StreamCamVideoDelegate* delegate_ = nil;
  dispatch_queue_t queue_ = nullptr;
};

}  // namespace streamcam

@implementation StreamCamVideoDelegate {
}

- (instancetype)initWithOwner:(streamcam::MacosBackendImpl*)owner {
  self = [super init];
  if (self != nil) {
    owner_ = owner;
  }
  return self;
}

- (void)captureOutput:(AVCaptureOutput*)output
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
           fromConnection:(AVCaptureConnection*)connection {
  (void)output;
  (void)connection;
  if (owner_ != nullptr) {
    owner_->HandleSampleBuffer(sampleBuffer);
  }
}

@end

namespace streamcam {

std::vector<streamcam_device_info> ListDevicesMacos() {
  @autoreleasepool {
    std::vector<streamcam_device_info> devices;

    AVCaptureDeviceDiscoverySession* discovery = [AVCaptureDeviceDiscoverySession
        discoverySessionWithDeviceTypes:@[
          AVCaptureDeviceTypeBuiltInWideAngleCamera,
          AVCaptureDeviceTypeExternal
        ]
        mediaType:AVMediaTypeVideo
        position:AVCaptureDevicePositionUnspecified];

    devices.reserve([discovery.devices count]);
    for (AVCaptureDevice* device in discovery.devices) {
      streamcam_device_info info{};
      const char* id = [[device uniqueID] UTF8String];
      const char* name = [[device localizedName] UTF8String];

      if (id != nullptr) {
        std::strncpy(info.id, id, STREAMCAM_DEVICE_ID_MAX - 1);
      }
      if (name != nullptr) {
        std::strncpy(info.name, name, STREAMCAM_DEVICE_NAME_MAX - 1);
      }
      devices.push_back(info);
    }
    return devices;
  }
}

MacosBackend::MacosBackend(const char* device_id, const streamcam_config& config)
    : config_(config), impl_(new MacosBackendImpl(this, device_id, config)) {}

MacosBackend::~MacosBackend() { delete impl_; }

streamcam_status MacosBackend::Start() { return impl_->Start(); }

streamcam_status MacosBackend::Stop() { return impl_->Stop(); }

streamcam_status MacosBackend::LatestFrame(streamcam_frame_view* out_frame) {
  return store_.Latest(out_frame);
}

streamcam_status MacosBackend::PublishFrame(const uint8_t* data,
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
