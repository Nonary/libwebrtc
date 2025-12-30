// Copyright (C) <2025>
//
// SPDX-License-Identifier: Apache-2.0
#ifndef PASSTHROUGH_VIDEO_ENCODER_H_
#define PASSTHROUGH_VIDEO_ENCODER_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "api/video/encoded_image.h"
#include "api/video_codecs/video_codec.h"
#include "api/video_codecs/video_encoder.h"
#include "api/video_codecs/video_encoder_factory.h"

namespace owt {
namespace base {

// Pre-encoded frame data structure
struct EncodedFrameData {
  rtc::scoped_refptr<webrtc::EncodedImageBufferInterface> buffer;  // H.264/HEVC NAL units (Annex B) or AV1 OBUs
  int width = 0;
  int height = 0;
  int64_t timestamp_us = 0;
  bool is_keyframe = false;
  webrtc::VideoCodecType codec_type = webrtc::kVideoCodecH264;
};

// Callback type for keyframe (IDR) requests triggered by PLI/FIR
using KeyframeRequestCallback = std::function<void()>;

// Forward declaration
class PassthroughVideoEncoderFactory;

// A "passthrough" video encoder that accepts pre-encoded H.264/HEVC/AV1 frames
// and passes them through to the WebRTC RTP pipeline without re-encoding.
//
// Usage:
// 1. Create via PassthroughVideoEncoderFactory
// 2. Set keyframe request callback via SetKeyframeRequestCallback()
// 3. Push pre-encoded frames via PushEncodedFrame()
// 4. When WebRTC requests a keyframe (PLI/FIR), the callback is invoked
class PassthroughVideoEncoder : public webrtc::VideoEncoder {
 public:
  PassthroughVideoEncoder(webrtc::VideoCodecType codec_type,
                          PassthroughVideoEncoderFactory* factory);
  ~PassthroughVideoEncoder() override;

  // VideoEncoder interface
  int InitEncode(const webrtc::VideoCodec* codec_settings,
                 int number_of_cores,
                 size_t max_payload_size) override;
  int Encode(const webrtc::VideoFrame& input_image,
             const std::vector<webrtc::VideoFrameType>* frame_types) override;
  int RegisterEncodeCompleteCallback(
      webrtc::EncodedImageCallback* callback) override;
  void SetRates(const RateControlParameters& parameters) override;
  void OnPacketLossRateUpdate(float packet_loss_rate) override;
  void OnRttUpdate(int64_t rtt_ms) override;
  void OnLossNotification(const LossNotification& loss_notification) override;
  EncoderInfo GetEncoderInfo() const override;
  int Release() override;

  // Custom methods for passthrough operation

  // Push a pre-encoded frame into the WebRTC pipeline.
  // Returns true on success, false if no callback is registered.
  bool PushEncodedFrame(const EncodedFrameData& frame);

  // Set callback to be invoked when WebRTC requests a keyframe (PLI/FIR).
  // The callback should trigger an IDR request to the upstream encoder.
  void SetKeyframeRequestCallback(KeyframeRequestCallback callback);

  // Update dimensions (call when resolution changes)
  void SetDimensions(int width, int height);

 private:
  // Extract and cache H.264 SPS/PPS from keyframe data
  void ExtractH264ParameterSets(const uint8_t* data, size_t size);

  // Extract and cache HEVC VPS/SPS/PPS from keyframe data
  void ExtractHEVCParameterSets(const uint8_t* data, size_t size);

  // Prepend cached parameter sets to the output buffer
  rtc::scoped_refptr<webrtc::EncodedImageBufferInterface> PrependParameterSets(
      const uint8_t* data,
      size_t size);

  // Find next NAL unit start code position
  size_t FindNextNalStart(const uint8_t* data, size_t size, size_t start_pos);

  // Check if NAL data already contains parameter sets
  bool ContainsParameterSets(const uint8_t* data, size_t size);

  webrtc::EncodedImageCallback* callback_ = nullptr;
  webrtc::VideoCodecType codec_type_;
  PassthroughVideoEncoderFactory* factory_ = nullptr;
  int width_ = 0;
  int height_ = 0;
  uint32_t target_bitrate_bps_ = 0;
  uint32_t frame_rate_ = 30;

  KeyframeRequestCallback keyframe_request_cb_;
  std::mutex mutex_;

  // Cached parameter sets for H.264
  std::vector<uint8_t> cached_sps_;
  std::vector<uint8_t> cached_pps_;

  // Cached parameter sets for HEVC
  std::vector<uint8_t> cached_vps_;
  std::vector<uint8_t> cached_hevc_sps_;
  std::vector<uint8_t> cached_hevc_pps_;

  // Frame counter for RTP timestamps
  uint32_t frame_count_ = 0;

  // Flag to track if we've seen the first keyframe
  bool has_parameter_sets_ = false;

  // Map application timestamps onto WebRTC's TimeMicros() timebase to keep
  // capture_time_ms_ and RTP timestamps consistent with the rest of the stack.
  std::optional<int64_t> timestamp_offset_us_;
  int64_t last_mapped_timestamp_us_ = 0;
};

// Factory for creating PassthroughVideoEncoder instances.
// This factory advertises support for H.264, H.265, and AV1 codecs.
class PassthroughVideoEncoderFactory : public webrtc::VideoEncoderFactory {
 public:
  PassthroughVideoEncoderFactory();
  ~PassthroughVideoEncoderFactory() override;

  std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override;

  std::unique_ptr<webrtc::VideoEncoder> Create(
      const webrtc::Environment& env,
      const webrtc::SdpVideoFormat& format) override;

  CodecSupport QueryCodecSupport(
      const webrtc::SdpVideoFormat& format,
      absl::optional<std::string> scalability_mode) const override;

  void SetPreferredCodec(webrtc::VideoCodecType codec);
  void SetH265Parameters(std::optional<webrtc::CodecParameterMap> params);
  void SetAv1Parameters(std::optional<webrtc::CodecParameterMap> params);

  // Get the currently active encoder instance (for frame injection)
  // Returns nullptr if no encoder has been created yet.
  PassthroughVideoEncoder* GetActiveEncoder();

  // Set the keyframe request callback for the active encoder
  void SetKeyframeRequestCallback(KeyframeRequestCallback callback);

  // Called by encoder when it's being destroyed
  void OnEncoderDestroyed(PassthroughVideoEncoder* encoder);

 private:
  PassthroughVideoEncoder* active_encoder_ = nullptr;
  KeyframeRequestCallback pending_keyframe_cb_;
  std::optional<webrtc::VideoCodecType> preferred_codec_;
  std::optional<webrtc::CodecParameterMap> h265_parameters_;
  std::optional<webrtc::CodecParameterMap> av1_parameters_;
  std::mutex mutex_;
};

}  // namespace base
}  // namespace owt

#endif  // PASSTHROUGH_VIDEO_ENCODER_H_
