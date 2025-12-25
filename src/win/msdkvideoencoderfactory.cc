// Copyright (C) <2018> Intel Corporation
//
// SPDX-License-Identifier: Apache-2.0

#include "src/win/msdkvideoencoderfactory.h"

#include <string>

#include "absl/strings/match.h"
#include "media/base/media_constants.h"
#include "modules/video_coding/codecs/av1/libaom_av1_encoder.h"
#include "modules/video_coding/codecs/h264/include/h264.h"
#include "modules/video_coding/codecs/vp8/include/vp8.h"
#include "modules/video_coding/codecs/vp9/include/vp9.h"
#include "src/win/codecutils.h"
#include "src/win/msdkvideoencoder.h"
// #include "common_video/h264/profile_level_id.h"
// #include "media/base/vp9_profile.h"

namespace owt {
namespace base {

MSDKVideoEncoderFactory::MSDKVideoEncoderFactory() {
  supported_codec_types_.clear();
  MediaCapabilities* media_capability = MediaCapabilities::Get();
  std::vector<owt::base::VideoCodec> codecs_to_check;
  codecs_to_check.push_back(owt::base::VideoCodec::kH264);
#if 0
  codecs_to_check.push_back(owt::base::VideoCodec::kVp9);
  codecs_to_check.push_back(owt::base::VideoCodec::kAv1);
  codecs_to_check.push_back(owt::base::VideoCodec::kVp8);
#endif

  std::vector<VideoEncoderCapability> capabilities =
      media_capability->SupportedCapabilitiesForVideoEncoder(codecs_to_check);
  // TODO(jianlin): use the check result from MSDK.
  supported_codec_types_.push_back(webrtc::kVideoCodecH264);
  supported_codec_types_.push_back(webrtc::kVideoCodecVP8);
  supported_codec_types_.push_back(webrtc::kVideoCodecVP9);
  supported_codec_types_.push_back(webrtc::kVideoCodecAV1);
}

std::unique_ptr<webrtc::VideoEncoder>
MSDKVideoEncoderFactory::Create(const webrtc::Environment& env,
                                const webrtc::SdpVideoFormat& format) {
  bool vp9_hw = false, vp8_hw = false, av1_hw = false, h264_hw = false;
  for (auto& codec : supported_codec_types_) {
    if (codec == webrtc::kVideoCodecAV1)
      av1_hw = false;
    else if (codec == webrtc::kVideoCodecH264)
      h264_hw = true;
    else if (codec == webrtc::kVideoCodecVP8)
      vp8_hw = false;
    else if (codec == webrtc::kVideoCodecVP9)
      vp9_hw = false;
  }
  // VP8 encoding will always use SW impl.
  if (absl::EqualsIgnoreCase(format.name, cricket::kVp8CodecName) && !vp8_hw)
    return webrtc::CreateVp8Encoder(env);
  // VP9 encoding will only be enabled on ICL+;
  else if (absl::EqualsIgnoreCase(format.name, cricket::kVp9CodecName))
    return webrtc::CreateVp9Encoder(env);
  // TODO: Replace with AV1 HW encoder post ADL.
  else if (absl::EqualsIgnoreCase(format.name, cricket::kAv1CodecName))
    return webrtc::CreateLibaomAv1Encoder(env);
  webrtc::VideoCodec codec;
  codec.codecType = owt::base::CodecUtils::ConvertSdpFormatToCodecType(format);
  return MSDKVideoEncoder::Create(codec);
}

std::vector<webrtc::SdpVideoFormat>
MSDKVideoEncoderFactory::GetSupportedFormats() const {
  std::vector<webrtc::SdpVideoFormat> supported_codecs;
  // TODO: We should combine the codec profiles that hardware H.264 encoder
  // supports with those provided by built-in H.264 encoder
  for (const webrtc::SdpVideoFormat& format :
       owt::base::CodecUtils::SupportedH264Codecs())
    supported_codecs.push_back(format);
  supported_codecs.push_back(webrtc::SdpVideoFormat(cricket::kVp8CodecName));
  for (const webrtc::SdpVideoFormat& format : webrtc::SupportedVP9Codecs())
    supported_codecs.push_back(format);
  supported_codecs.push_back(webrtc::SdpVideoFormat(cricket::kAv1CodecName));

  return supported_codecs;
}

webrtc::VideoEncoderFactory::CodecSupport
MSDKVideoEncoderFactory::QueryCodecSupport(
    const webrtc::SdpVideoFormat& format,
    absl::optional<std::string> scalability_mode) const {
  webrtc::VideoEncoderFactory::CodecSupport info;
  info.is_supported = format.IsCodecInList(GetSupportedFormats());
  if (absl::EqualsIgnoreCase(format.name, cricket::kH264CodecName)) {
    info.is_power_efficient = true;
  }
  return info;
}

}  // namespace base
}  // namespace owt
