// Copyright (C) <2025>
//
// SPDX-License-Identifier: Apache-2.0
#include "passthrough_video_encoder.h"

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <sstream>

#include "api/video/encoded_image.h"
#include "api/video_codecs/video_codec.h"
#include "media/base/media_constants.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/logging.h"
#include "rtc_base/time_utils.h"

namespace owt {
namespace base {

namespace {

// H.264 NAL unit types
constexpr uint8_t kH264NalTypeSps = 7;
constexpr uint8_t kH264NalTypePps = 8;
constexpr uint8_t kH264NalTypeIdr = 5;
constexpr uint8_t kH264NalTypeMask = 0x1F;

// HEVC NAL unit types
constexpr uint8_t kHevcNalTypeVps = 32;
constexpr uint8_t kHevcNalTypeSps = 33;
constexpr uint8_t kHevcNalTypePps = 34;
constexpr uint8_t kHevcNalTypeIdrWRadl = 19;
constexpr uint8_t kHevcNalTypeIdrNLp = 20;

// Get H.264 NAL type from first byte
inline uint8_t GetH264NalType(uint8_t first_byte) {
  return first_byte & kH264NalTypeMask;
}

// Get HEVC NAL type from first byte (NAL header is 2 bytes, type is in bits 1-6)
inline uint8_t GetHevcNalType(uint8_t first_byte) {
  return (first_byte >> 1) & 0x3F;
}

bool StartsWithAnnexBStartCode(const uint8_t* data, size_t size) {
  if (!data || size < 3) {
    return false;
  }
  if (data[0] == 0 && data[1] == 0 && data[2] == 1) {
    return true;
  }
  if (size >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1) {
    return true;
  }
  return false;
}

std::string HexPrefix(const uint8_t* data, size_t size, size_t max_bytes = 8) {
  if (!data || size == 0) {
    return "";
  }
  std::ostringstream oss;
  const size_t count = std::min(size, max_bytes);
  for (size_t i = 0; i < count; ++i) {
    if (i) {
      oss << ' ';
    }
    oss << std::hex << std::setw(2) << std::setfill('0')
        << static_cast<int>(data[i]);
  }
  return oss.str();
}

}  // namespace

PassthroughVideoEncoder::PassthroughVideoEncoder(
    webrtc::VideoCodecType codec_type,
    PassthroughVideoEncoderFactory* factory)
    : codec_type_(codec_type), factory_(factory) {
  RTC_LOG(LS_INFO) << "PassthroughVideoEncoder created for codec type: "
                   << static_cast<int>(codec_type);
}

PassthroughVideoEncoder::~PassthroughVideoEncoder() {
  RTC_LOG(LS_INFO) << "PassthroughVideoEncoder destroyed";
  if (factory_) {
    factory_->OnEncoderDestroyed(this);
  }
}

int PassthroughVideoEncoder::InitEncode(
    const webrtc::VideoCodec* codec_settings,
    int /*number_of_cores*/,
    size_t /*max_payload_size*/) {
  if (!codec_settings) {
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  width_ = codec_settings->width;
  height_ = codec_settings->height;
  target_bitrate_bps_ = codec_settings->startBitrate * 1000;
  frame_rate_ = codec_settings->maxFramerate > 0 ? codec_settings->maxFramerate : 30;
  timestamp_offset_us_.reset();
  last_mapped_timestamp_us_ = 0;

  RTC_LOG(LS_INFO) << "PassthroughVideoEncoder::InitEncode: "
                   << width_ << "x" << height_ << " @ " << frame_rate_ << "fps";

  return WEBRTC_VIDEO_CODEC_OK;
}

int PassthroughVideoEncoder::Encode(
    const webrtc::VideoFrame& /*input_image*/,
    const std::vector<webrtc::VideoFrameType>* frame_types) {
  // Check if a keyframe is requested (PLI/FIR from receiver)
  if (frame_types) {
    for (auto type : *frame_types) {
      if (type == webrtc::VideoFrameType::kVideoFrameKey) {
        // Trigger IDR request callback
        std::lock_guard<std::mutex> lock(mutex_);
        if (keyframe_request_cb_) {
          RTC_LOG(LS_INFO) << "PassthroughVideoEncoder: keyframe requested (PLI/FIR)";
          keyframe_request_cb_();
        }
        break;
      }
    }
  }

  // We don't actually encode here - frames come via PushEncodedFrame
  // This method is called by WebRTC but we ignore the raw frame
  return WEBRTC_VIDEO_CODEC_OK;
}

int PassthroughVideoEncoder::RegisterEncodeCompleteCallback(
    webrtc::EncodedImageCallback* callback) {
  callback_ = callback;
  return WEBRTC_VIDEO_CODEC_OK;
}

void PassthroughVideoEncoder::SetRates(const RateControlParameters& parameters) {
  target_bitrate_bps_ = parameters.bitrate.get_sum_bps();
  if (parameters.framerate_fps > 0) {
    frame_rate_ = static_cast<uint32_t>(parameters.framerate_fps);
  }
}

void PassthroughVideoEncoder::OnPacketLossRateUpdate(float /*packet_loss_rate*/) {
  // Could trigger more frequent IDRs based on packet loss
}

void PassthroughVideoEncoder::OnRttUpdate(int64_t /*rtt_ms*/) {
  // Could adjust behavior based on RTT
}

void PassthroughVideoEncoder::OnLossNotification(
    const LossNotification& /*loss_notification*/) {
  // Could request IDR on significant loss
}

webrtc::VideoEncoder::EncoderInfo PassthroughVideoEncoder::GetEncoderInfo() const {
  EncoderInfo info;
  info.implementation_name = "PassthroughVideoEncoder";
  info.is_hardware_accelerated = true;  // Pre-encoded, so effectively HW
  info.supports_native_handle = false;
  info.scaling_settings = ScalingSettings::kOff;  // No scaling - pre-encoded
  info.has_trusted_rate_controller = true;  // We control the bitrate externally
  return info;
}

int PassthroughVideoEncoder::Release() {
  callback_ = nullptr;
  cached_sps_.clear();
  cached_pps_.clear();
  cached_vps_.clear();
  cached_hevc_sps_.clear();
  cached_hevc_pps_.clear();
  has_parameter_sets_ = false;
  timestamp_offset_us_.reset();
  last_mapped_timestamp_us_ = 0;
  return WEBRTC_VIDEO_CODEC_OK;
}

bool PassthroughVideoEncoder::PushEncodedFrame(const EncodedFrameData& frame) {
  if (!callback_) {
    RTC_LOG(LS_WARNING) << "PassthroughVideoEncoder: no callback registered";
    return false;
  }

  const uint8_t* frame_data = frame.buffer ? frame.buffer->data() : nullptr;
  const size_t frame_size = frame.buffer ? frame.buffer->size() : 0;
  if (!frame_data || frame_size == 0) {
    RTC_LOG(LS_WARNING) << "PassthroughVideoEncoder: empty frame data";
    return false;
  }

  if (codec_type_ == webrtc::kVideoCodecH264 || codec_type_ == webrtc::kVideoCodecH265) {
    if (!StartsWithAnnexBStartCode(frame_data, frame_size)) {
      RTC_LOG(LS_WARNING) << "PassthroughVideoEncoder: encoded frame does not start with Annex B start code; prefix="
                          << HexPrefix(frame_data, frame_size);
    }
  }

  // Update dimensions if they changed
  if (frame.width > 0 && frame.height > 0) {
    width_ = frame.width;
    height_ = frame.height;
  }

  // For keyframes, extract and cache parameter sets
  if (frame.is_keyframe) {
    if (codec_type_ == webrtc::kVideoCodecH264) {
      ExtractH264ParameterSets(frame_data, frame_size);
    } else if (codec_type_ == webrtc::kVideoCodecH265) {
      ExtractHEVCParameterSets(frame_data, frame_size);
    }
  }

  // Prepare the output buffer - prepend parameter sets if needed
  rtc::scoped_refptr<webrtc::EncodedImageBufferInterface> output_buffer = frame.buffer;
  if (frame.is_keyframe && has_parameter_sets_ &&
      !ContainsParameterSets(frame_data, frame_size)) {
    output_buffer = PrependParameterSets(frame_data, frame_size);
  }
  if (!output_buffer) {
    RTC_LOG(LS_WARNING) << "PassthroughVideoEncoder: output buffer is null";
    return false;
  }

  // Build EncodedImage
  webrtc::EncodedImage encoded_image;
  encoded_image._encodedWidth = width_;
  encoded_image._encodedHeight = height_;
  int64_t mapped_timestamp_us = frame.timestamp_us;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!timestamp_offset_us_) {
      timestamp_offset_us_ = rtc::TimeMicros() - frame.timestamp_us;
      last_mapped_timestamp_us_ = 0;
    }
    mapped_timestamp_us = frame.timestamp_us + *timestamp_offset_us_;
    if (last_mapped_timestamp_us_ != 0 && mapped_timestamp_us <= last_mapped_timestamp_us_) {
      mapped_timestamp_us = last_mapped_timestamp_us_ + 1;
    }
    last_mapped_timestamp_us_ = mapped_timestamp_us;
  }
  encoded_image.capture_time_ms_ = mapped_timestamp_us / 1000;

  // Convert timestamp to RTP clock rate (90kHz for video)
  uint32_t rtp_timestamp = static_cast<uint32_t>(mapped_timestamp_us * 90 / 1000);
  encoded_image.SetRtpTimestamp(rtp_timestamp);

  encoded_image._frameType = frame.is_keyframe
      ? webrtc::VideoFrameType::kVideoFrameKey
      : webrtc::VideoFrameType::kVideoFrameDelta;

  encoded_image.SetEncodedData(output_buffer);

  // Build codec-specific info
  webrtc::CodecSpecificInfo codec_info;
  codec_info.codecType = codec_type_;
  if (codec_type_ == webrtc::kVideoCodecH264) {
    codec_info.codecSpecific.H264.packetization_mode =
        webrtc::H264PacketizationMode::NonInterleaved;
    codec_info.codecSpecific.H264.idr_frame = frame.is_keyframe;
  }

  // Send to WebRTC
  auto result = callback_->OnEncodedImage(encoded_image, &codec_info);

  if (result.error != webrtc::EncodedImageCallback::Result::Error::OK) {
    RTC_LOG(LS_WARNING) << "PassthroughVideoEncoder: OnEncodedImage returned error";
    return false;
  }

  frame_count_++;
  return true;
}

void PassthroughVideoEncoder::SetKeyframeRequestCallback(
    KeyframeRequestCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  keyframe_request_cb_ = std::move(callback);
}

void PassthroughVideoEncoder::SetDimensions(int width, int height) {
  width_ = width;
  height_ = height;
}

size_t PassthroughVideoEncoder::FindNextNalStart(
    const uint8_t* data,
    size_t size,
    size_t start_pos) {
  // Look for 0x000001 or 0x00000001
  for (size_t i = start_pos; i < size - 2; ++i) {
    if (data[i] == 0 && data[i + 1] == 0) {
      if (i + 2 < size && data[i + 2] == 1) {
        return i;
      }
      if (i + 3 < size && data[i + 2] == 0 && data[i + 3] == 1) {
        return i;
      }
    }
  }
  return size;  // Not found
}

void PassthroughVideoEncoder::ExtractH264ParameterSets(
    const uint8_t* data,
    size_t size) {
  size_t pos = 0;

  while (pos < size - 4) {
    // Find start code
    size_t nal_start = FindNextNalStart(data, size, pos);
    if (nal_start >= size - 4) {
      break;
    }

    // Determine start code length (3 or 4 bytes)
    size_t start_code_len = 3;
    if (data[nal_start + 2] == 0) {
      start_code_len = 4;
    }

    size_t nal_header_pos = nal_start + start_code_len;
    if (nal_header_pos >= size) {
      break;
    }

    uint8_t nal_type = GetH264NalType(data[nal_header_pos]);

    // Find end of this NAL (next start code or end of data)
    size_t nal_end = FindNextNalStart(data, size, nal_header_pos);

    // Extract the NAL unit including start code
    if (nal_type == kH264NalTypeSps) {
      cached_sps_.assign(data + nal_start, data + nal_end);
      RTC_LOG(LS_INFO) << "PassthroughVideoEncoder: cached H.264 SPS ("
                       << cached_sps_.size() << " bytes)";
    } else if (nal_type == kH264NalTypePps) {
      cached_pps_.assign(data + nal_start, data + nal_end);
      RTC_LOG(LS_INFO) << "PassthroughVideoEncoder: cached H.264 PPS ("
                       << cached_pps_.size() << " bytes)";
    }

    pos = nal_end;
  }

  has_parameter_sets_ = !cached_sps_.empty() && !cached_pps_.empty();
}

void PassthroughVideoEncoder::ExtractHEVCParameterSets(
    const uint8_t* data,
    size_t size) {
  size_t pos = 0;

  while (pos < size - 4) {
    // Find start code
    size_t nal_start = FindNextNalStart(data, size, pos);
    if (nal_start >= size - 4) {
      break;
    }

    // Determine start code length (3 or 4 bytes)
    size_t start_code_len = 3;
    if (data[nal_start + 2] == 0) {
      start_code_len = 4;
    }

    size_t nal_header_pos = nal_start + start_code_len;
    if (nal_header_pos >= size) {
      break;
    }

    uint8_t nal_type = GetHevcNalType(data[nal_header_pos]);

    // Find end of this NAL (next start code or end of data)
    size_t nal_end = FindNextNalStart(data, size, nal_header_pos);

    // Extract the NAL unit including start code
    if (nal_type == kHevcNalTypeVps) {
      cached_vps_.assign(data + nal_start, data + nal_end);
      RTC_LOG(LS_INFO) << "PassthroughVideoEncoder: cached HEVC VPS ("
                       << cached_vps_.size() << " bytes)";
    } else if (nal_type == kHevcNalTypeSps) {
      cached_hevc_sps_.assign(data + nal_start, data + nal_end);
      RTC_LOG(LS_INFO) << "PassthroughVideoEncoder: cached HEVC SPS ("
                       << cached_hevc_sps_.size() << " bytes)";
    } else if (nal_type == kHevcNalTypePps) {
      cached_hevc_pps_.assign(data + nal_start, data + nal_end);
      RTC_LOG(LS_INFO) << "PassthroughVideoEncoder: cached HEVC PPS ("
                       << cached_hevc_pps_.size() << " bytes)";
    }

    pos = nal_end;
  }

  has_parameter_sets_ = !cached_vps_.empty() && !cached_hevc_sps_.empty() &&
                        !cached_hevc_pps_.empty();
}

bool PassthroughVideoEncoder::ContainsParameterSets(
    const uint8_t* data,
    size_t size) {
  size_t pos = 0;

  while (pos < size - 4) {
    size_t nal_start = FindNextNalStart(data, size, pos);
    if (nal_start >= size - 4) {
      break;
    }

    size_t start_code_len = (data[nal_start + 2] == 0) ? 4 : 3;
    size_t nal_header_pos = nal_start + start_code_len;
    if (nal_header_pos >= size) {
      break;
    }

    if (codec_type_ == webrtc::kVideoCodecH264) {
      uint8_t nal_type = GetH264NalType(data[nal_header_pos]);
      if (nal_type == kH264NalTypeSps || nal_type == kH264NalTypePps) {
        return true;
      }
    } else if (codec_type_ == webrtc::kVideoCodecH265) {
      uint8_t nal_type = GetHevcNalType(data[nal_header_pos]);
      if (nal_type == kHevcNalTypeVps || nal_type == kHevcNalTypeSps ||
          nal_type == kHevcNalTypePps) {
        return true;
      }
    }

    size_t nal_end = FindNextNalStart(data, size, nal_header_pos);
    pos = nal_end;
  }

  return false;
}

rtc::scoped_refptr<webrtc::EncodedImageBufferInterface>
PassthroughVideoEncoder::PrependParameterSets(const uint8_t* data, size_t size) {
  if (!data || size == 0) {
    return nullptr;
  }

  auto append = [](uint8_t* dst, size_t& offset, const std::vector<uint8_t>& src) {
    if (src.empty()) {
      return;
    }
    memcpy(dst + offset, src.data(), src.size());
    offset += src.size();
  };

  size_t total_size = 0;
  if (codec_type_ == webrtc::kVideoCodecH264) {
    total_size = cached_sps_.size() + cached_pps_.size() + size;
  } else if (codec_type_ == webrtc::kVideoCodecH265) {
    total_size =
        cached_vps_.size() + cached_hevc_sps_.size() + cached_hevc_pps_.size() + size;
  } else {
    total_size = size;
  }

  auto output = webrtc::EncodedImageBuffer::Create(total_size);
  size_t offset = 0;
  uint8_t* out_data = output->data();

  if (codec_type_ == webrtc::kVideoCodecH264) {
    append(out_data, offset, cached_sps_);
    append(out_data, offset, cached_pps_);
  } else if (codec_type_ == webrtc::kVideoCodecH265) {
    append(out_data, offset, cached_vps_);
    append(out_data, offset, cached_hevc_sps_);
    append(out_data, offset, cached_hevc_pps_);
  }

  memcpy(out_data + offset, data, size);
  return output;
}

// PassthroughVideoEncoderFactory implementation

PassthroughVideoEncoderFactory::PassthroughVideoEncoderFactory() {
  RTC_LOG(LS_INFO) << "PassthroughVideoEncoderFactory created";
}

PassthroughVideoEncoderFactory::~PassthroughVideoEncoderFactory() {
  RTC_LOG(LS_INFO) << "PassthroughVideoEncoderFactory destroyed";
}

std::vector<webrtc::SdpVideoFormat>
PassthroughVideoEncoderFactory::GetSupportedFormats() const {
  std::vector<webrtc::SdpVideoFormat> formats;

  const bool prefer_h264 =
      !preferred_codec_ || *preferred_codec_ == webrtc::kVideoCodecH264;
  const bool prefer_h265 =
      !preferred_codec_ || *preferred_codec_ == webrtc::kVideoCodecH265;
  const bool prefer_av1 =
      !preferred_codec_ || *preferred_codec_ == webrtc::kVideoCodecAV1;

  if (prefer_h264) {
    // H.264 Constrained Baseline
    formats.push_back(webrtc::SdpVideoFormat(
        "H264",
        {{"level-asymmetry-allowed", "1"},
         {"packetization-mode", "1"},
         {"profile-level-id", "42e01f"}}));

    // H.264 Baseline
    formats.push_back(webrtc::SdpVideoFormat(
        "H264",
        {{"level-asymmetry-allowed", "1"},
         {"packetization-mode", "1"},
         {"profile-level-id", "42001f"}}));

    // H.264 Main
    formats.push_back(webrtc::SdpVideoFormat(
        "H264",
        {{"level-asymmetry-allowed", "1"},
         {"packetization-mode", "1"},
         {"profile-level-id", "4d001f"}}));

    // H.264 High
    formats.push_back(webrtc::SdpVideoFormat(
        "H264",
        {{"level-asymmetry-allowed", "1"},
         {"packetization-mode", "1"},
         {"profile-level-id", "64001f"}}));
  }

  if (prefer_h265) {
    // H.265/HEVC
    if (h265_parameters_) {
      formats.push_back(webrtc::SdpVideoFormat("H265", *h265_parameters_));
      formats.push_back(webrtc::SdpVideoFormat("HEVC", *h265_parameters_));
    }
    formats.push_back(webrtc::SdpVideoFormat("H265"));
    formats.push_back(webrtc::SdpVideoFormat("HEVC"));
  }

  if (prefer_av1) {
    if (av1_parameters_) {
      formats.push_back(webrtc::SdpVideoFormat("AV1", *av1_parameters_));
    } else {
      formats.push_back(webrtc::SdpVideoFormat::AV1Profile0());
    }
  }

  return formats;
}

std::unique_ptr<webrtc::VideoEncoder> PassthroughVideoEncoderFactory::Create(
    const webrtc::Environment& /*env*/,
    const webrtc::SdpVideoFormat& format) {
  webrtc::VideoCodecType codec_type = webrtc::kVideoCodecH264;

  const bool is_h264 = format.name == "H264";
  const bool is_h265 = (format.name == "H265" || format.name == "HEVC");
  const bool is_av1 = format.name == "AV1";

  if (preferred_codec_) {
    const bool allowed =
        (*preferred_codec_ == webrtc::kVideoCodecH264 && is_h264) ||
        (*preferred_codec_ == webrtc::kVideoCodecH265 && is_h265) ||
        (*preferred_codec_ == webrtc::kVideoCodecAV1 && is_av1);
    if (!allowed) {
      RTC_LOG(LS_WARNING) << "PassthroughVideoEncoderFactory: codec not allowed "
                          << format.name;
      return nullptr;
    }
  }

  if (is_h264) {
    codec_type = webrtc::kVideoCodecH264;
  } else if (is_h265) {
    codec_type = webrtc::kVideoCodecH265;
  } else if (is_av1) {
    codec_type = webrtc::kVideoCodecAV1;
  } else {
    RTC_LOG(LS_WARNING) << "PassthroughVideoEncoderFactory: unsupported codec "
                        << format.name;
    return nullptr;
  }

  auto encoder = std::make_unique<PassthroughVideoEncoder>(codec_type, this);

  // Store reference to active encoder
  {
    std::lock_guard<std::mutex> lock(mutex_);
    active_encoder_ = encoder.get();

    // Apply pending keyframe callback if any
    if (pending_keyframe_cb_) {
      active_encoder_->SetKeyframeRequestCallback(pending_keyframe_cb_);
    }
  }

  RTC_LOG(LS_INFO) << "PassthroughVideoEncoderFactory: created encoder for "
                   << format.name;
  return encoder;
}

webrtc::VideoEncoderFactory::CodecSupport
PassthroughVideoEncoderFactory::QueryCodecSupport(
    const webrtc::SdpVideoFormat& format,
    absl::optional<std::string> /*scalability_mode*/) const {
  CodecSupport support;
  const bool is_h264 = format.name == "H264";
  const bool is_h265 = (format.name == "H265" || format.name == "HEVC");
  const bool is_av1 = format.name == "AV1";
  if (preferred_codec_) {
    support.is_supported =
        (*preferred_codec_ == webrtc::kVideoCodecH264 && is_h264) ||
        (*preferred_codec_ == webrtc::kVideoCodecH265 && is_h265) ||
        (*preferred_codec_ == webrtc::kVideoCodecAV1 && is_av1);
  } else {
    support.is_supported = is_h264 || is_h265 || is_av1;
  }
  support.is_power_efficient = true;  // Pre-encoded, so very power efficient
  return support;
}

void PassthroughVideoEncoderFactory::SetPreferredCodec(
    webrtc::VideoCodecType codec) {
  preferred_codec_ = codec;
}

void PassthroughVideoEncoderFactory::SetH265Parameters(
    std::optional<webrtc::CodecParameterMap> params) {
  h265_parameters_ = std::move(params);
}

void PassthroughVideoEncoderFactory::SetAv1Parameters(
    std::optional<webrtc::CodecParameterMap> params) {
  av1_parameters_ = std::move(params);
}

PassthroughVideoEncoder* PassthroughVideoEncoderFactory::GetActiveEncoder() {
  std::lock_guard<std::mutex> lock(mutex_);
  return active_encoder_;
}

void PassthroughVideoEncoderFactory::SetKeyframeRequestCallback(
    KeyframeRequestCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  pending_keyframe_cb_ = std::move(callback);

  // Apply immediately if encoder already exists
  if (active_encoder_) {
    active_encoder_->SetKeyframeRequestCallback(pending_keyframe_cb_);
  }
}

void PassthroughVideoEncoderFactory::OnEncoderDestroyed(
    PassthroughVideoEncoder* encoder) {
  std::lock_guard<std::mutex> lock(mutex_);
  // Only clear if this is the active encoder (could be a stale reference)
  if (active_encoder_ == encoder) {
    RTC_LOG(LS_INFO) << "PassthroughVideoEncoderFactory: active encoder destroyed";
    active_encoder_ = nullptr;
  }
}

}  // namespace base
}  // namespace owt
