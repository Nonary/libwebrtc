// Copyright (C) <2025>
//
// SPDX-License-Identifier: Apache-2.0
#ifndef OWT_BASE_WIN_NVENC_VIDEO_ENCODER_H_
#define OWT_BASE_WIN_NVENC_VIDEO_ENCODER_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#if defined(WEBRTC_WIN)
#include <d3d11.h>
#include <wrl/client.h>
#endif

#include "api/video_codecs/video_codec.h"
#include "api/video_codecs/video_encoder.h"

struct _NV_ENCODE_API_FUNCTION_LIST;
typedef struct _NV_ENCODE_API_FUNCTION_LIST NV_ENCODE_API_FUNCTION_LIST;
typedef void* NV_ENC_INPUT_PTR;
typedef void* NV_ENC_OUTPUT_PTR;
typedef void* NV_ENC_REGISTERED_PTR;

namespace owt {
namespace base {

struct NvencState;

class NvencVideoEncoder : public webrtc::VideoEncoder {
 public:
  explicit NvencVideoEncoder(const webrtc::VideoCodec& codec);
  ~NvencVideoEncoder() override;

  static std::unique_ptr<NvencVideoEncoder> Create(webrtc::VideoCodec format);

  int InitEncode(const webrtc::VideoCodec* codec_settings, int number_of_cores,
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

 private:
  bool EnsureInitialized(const webrtc::VideoFrame& input_image);
  bool InitOnD3D11Device(ID3D11Device* device, ID3D11DeviceContext* context);
  bool CreateNvencInputTexture();
  bool RegisterNvencInput();
  bool CreateBitstreamBuffer();
  bool ReconfigureIfNeeded();
  bool EncodeFrame(const webrtc::VideoFrame& input_image, bool force_idr);
  bool LoadNvencApi();
  void DestroyEncoder();
  void DestroyNvencResources();

  webrtc::EncodedImageCallback* callback_ = nullptr;
  webrtc::VideoCodec codec_settings_ {};
  webrtc::VideoCodecType codec_type_ = webrtc::kVideoCodecH264;
  int32_t bitrate_bps_ = 0;
  uint32_t frame_rate_ = 0;
  int width_ = 0;
  int height_ = 0;
  std::atomic<bool> inited_ {false};
  bool pending_reconfigure_ = false;
  bool native_active_ = false;

#if defined(WEBRTC_WIN)
  Microsoft::WRL::ComPtr<ID3D11Device> d3d11_device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3d11_context_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> nvenc_input_texture_;
#endif

  void* nvenc_dll_ = nullptr;
  std::unique_ptr<NV_ENCODE_API_FUNCTION_LIST> nvenc_api_;
  void* encoder_ = nullptr;
  NV_ENC_REGISTERED_PTR registered_input_ = nullptr;
  NV_ENC_INPUT_PTR mapped_input_ = nullptr;
  NV_ENC_OUTPUT_PTR bitstream_buffer_ = nullptr;
  uint64_t frame_index_ = 0;
  std::unique_ptr<NvencState> nvenc_state_;
};

}  // namespace base
}  // namespace owt

#endif  // OWT_BASE_WIN_NVENC_VIDEO_ENCODER_H_
