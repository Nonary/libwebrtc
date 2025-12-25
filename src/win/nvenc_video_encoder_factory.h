// Copyright (C) <2025>
//
// SPDX-License-Identifier: Apache-2.0
#ifndef OWT_BASE_WIN_NVENC_VIDEO_ENCODER_FACTORY_H_
#define OWT_BASE_WIN_NVENC_VIDEO_ENCODER_FACTORY_H_

#include <memory>
#include <vector>

#include "api/video_codecs/video_encoder_factory.h"

namespace owt {
namespace base {

class NvencVideoEncoderFactory : public webrtc::VideoEncoderFactory {
 public:
  explicit NvencVideoEncoderFactory(
      std::unique_ptr<webrtc::VideoEncoderFactory> fallback);

  std::unique_ptr<webrtc::VideoEncoder> Create(
      const webrtc::Environment& env,
      const webrtc::SdpVideoFormat& format) override;
  std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override;
  CodecSupport QueryCodecSupport(
      const webrtc::SdpVideoFormat& format,
      absl::optional<std::string> scalability_mode) const override;

 private:
  bool nvenc_available_ = false;
  std::unique_ptr<webrtc::VideoEncoderFactory> fallback_;
};

}  // namespace base
}  // namespace owt

#endif  // OWT_BASE_WIN_NVENC_VIDEO_ENCODER_FACTORY_H_
