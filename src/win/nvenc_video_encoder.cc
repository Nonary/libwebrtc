// Copyright (C) <2025>
//
// SPDX-License-Identifier: Apache-2.0
#include "src/win/nvenc_video_encoder.h"

#if defined(WEBRTC_WIN)
#include <windows.h>

#include <algorithm>
#include <cstring>

#include "ffnvcodec/nvEncodeAPI.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/logging.h"
#include "src/win/d3d11_texture_buffer.h"
#include "src/win/nvenc_api_compat.h"
#include "src/win/native_encoder_status.h"

namespace {

bool IsKeyframeRequested(
    const std::vector<webrtc::VideoFrameType>* frame_types) {
  if (!frame_types) {
    return false;
  }
  for (auto frame_type : *frame_types) {
    if (frame_type == webrtc::VideoFrameType::kVideoFrameKey) {
      return true;
    }
  }
  return false;
}

NVENCSTATUS NvencStatusForOk() {
  return NV_ENC_SUCCESS;
}

}  // namespace

namespace owt {
namespace base {

struct NvencState {
  NV_ENC_INITIALIZE_PARAMS init_params {};
  NV_ENC_CONFIG config {};
  NV_ENC_PRESET_CONFIG preset_config {};
};

NvencVideoEncoder::NvencVideoEncoder(const webrtc::VideoCodec& codec)
    : codec_settings_(codec),
      codec_type_(codec.codecType) {}

NvencVideoEncoder::~NvencVideoEncoder() {
  Release();
}

std::unique_ptr<NvencVideoEncoder> NvencVideoEncoder::Create(
    webrtc::VideoCodec format) {
  if (format.codecType != webrtc::kVideoCodecH264) {
    return nullptr;
  }
  return std::make_unique<NvencVideoEncoder>(format);
}

int NvencVideoEncoder::InitEncode(const webrtc::VideoCodec* codec_settings,
                                  int /*number_of_cores*/,
                                  size_t /*max_payload_size*/) {
  if (!codec_settings) {
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  codec_settings_ = *codec_settings;
  codec_type_ = codec_settings_.codecType;
  if (!native_active_ && codec_type_ == webrtc::kVideoCodecH264) {
    SetNativeVideoEncoderActive(true);
    native_active_ = true;
  }
  width_ = codec_settings_.width;
  height_ = codec_settings_.height;
  bitrate_bps_ = codec_settings_.startBitrate * 1000;
  frame_rate_ = codec_settings_.maxFramerate > 0
                    ? codec_settings_.maxFramerate
                    : 60;
  pending_reconfigure_ = false;
  inited_.store(false, std::memory_order_release);
  return WEBRTC_VIDEO_CODEC_OK;
}

int NvencVideoEncoder::Encode(
    const webrtc::VideoFrame& input_image,
    const std::vector<webrtc::VideoFrameType>* frame_types) {
  if (!callback_) {
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  if (codec_type_ != webrtc::kVideoCodecH264) {
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  if (!EnsureInitialized(input_image)) {
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  const bool force_idr = IsKeyframeRequested(frame_types);
  if (!EncodeFrame(input_image, force_idr)) {
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  return WEBRTC_VIDEO_CODEC_OK;
}

int NvencVideoEncoder::RegisterEncodeCompleteCallback(
    webrtc::EncodedImageCallback* callback) {
  callback_ = callback;
  return WEBRTC_VIDEO_CODEC_OK;
}

void NvencVideoEncoder::SetRates(const RateControlParameters& parameters) {
  if (!inited_.load(std::memory_order_acquire)) {
    return;
  }
  if (parameters.framerate_fps < 1.0) {
    return;
  }
  const int new_bitrate = parameters.bitrate.get_sum_bps();
  if (new_bitrate > 0) {
    bitrate_bps_ = new_bitrate;
    frame_rate_ = static_cast<uint32_t>(parameters.framerate_fps);
    pending_reconfigure_ = true;
  }
}

void NvencVideoEncoder::OnPacketLossRateUpdate(float /*packet_loss_rate*/) {}

void NvencVideoEncoder::OnRttUpdate(int64_t /*rtt_ms*/) {}

void NvencVideoEncoder::OnLossNotification(
    const LossNotification& /*loss_notification*/) {}

webrtc::VideoEncoder::EncoderInfo NvencVideoEncoder::GetEncoderInfo() const {
  EncoderInfo info;
  info.is_hardware_accelerated = true;
  info.supports_native_handle = true;
  info.scaling_settings = VideoEncoder::ScalingSettings::kOff;
  return info;
}

int NvencVideoEncoder::Release() {
  DestroyEncoder();
  callback_ = nullptr;
  inited_.store(false, std::memory_order_release);
  if (native_active_) {
    SetNativeVideoEncoderActive(false);
    native_active_ = false;
  }
  return WEBRTC_VIDEO_CODEC_OK;
}

bool NvencVideoEncoder::EnsureInitialized(
    const webrtc::VideoFrame& input_image) {
  auto buffer = input_image.video_frame_buffer();
  if (!buffer ||
      buffer->type() != webrtc::VideoFrameBuffer::Type::kNative) {
    return false;
  }
  auto* native_buffer =
      static_cast<owt::base::D3D11TextureBuffer*>(buffer.get());
  ID3D11Texture2D* src_texture = native_buffer->texture();
  if (!src_texture) {
    return false;
  }

  Microsoft::WRL::ComPtr<ID3D11Device> device;
  src_texture->GetDevice(device.GetAddressOf());
  if (!device) {
    return false;
  }

  if (!d3d11_device_ || d3d11_device_ != device ||
      width_ != input_image.width() || height_ != input_image.height()) {
    DestroyEncoder();
    d3d11_context_.Reset();
    device->GetImmediateContext(d3d11_context_.ReleaseAndGetAddressOf());
    if (!InitOnD3D11Device(device.Get(), d3d11_context_.Get())) {
      return false;
    }
  }

  return inited_.load(std::memory_order_acquire);
}

bool NvencVideoEncoder::InitOnD3D11Device(ID3D11Device* device,
                                          ID3D11DeviceContext* context) {
  if (!device || !context) {
    return false;
  }
  d3d11_device_ = device;
  d3d11_context_ = context;

  if (!LoadNvencApi()) {
    return false;
  }

  nvenc_state_ = std::make_unique<NvencState>();
  auto& init_params = nvenc_state_->init_params;
  auto& config = nvenc_state_->config;
  auto& preset_config = nvenc_state_->preset_config;

  init_params = {};
  config = {};
  preset_config = {};

  init_params.version = nvenc_compat::InitializeParamsVersion();
  config.version = nvenc_compat::ConfigVersion();
  preset_config.version = nvenc_compat::PresetConfigVersion();
  preset_config.presetCfg.version = nvenc_compat::ConfigVersion();

  NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open_params = {};
  open_params.version = nvenc_compat::OpenEncodeSessionExParamsVersion();
  open_params.device = device;
  open_params.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
  open_params.apiVersion = nvenc_compat::kNvencApiVersion;

  if (!nvenc_api_ ||
      nvenc_api_->nvEncOpenEncodeSessionEx(&open_params, &encoder_) !=
          NvencStatusForOk()) {
    RTC_LOG(LS_ERROR) << "NVENC: failed to open encode session";
    return false;
  }

  const GUID encode_guid = NV_ENC_CODEC_H264_GUID;
  const GUID preset_guid = NV_ENC_PRESET_P4_GUID;
  const NV_ENC_TUNING_INFO tuning = NV_ENC_TUNING_INFO_LOW_LATENCY;

  if (nvenc_api_->nvEncGetEncodePresetConfigEx(
          encoder_, encode_guid, preset_guid, tuning, &preset_config) !=
      NvencStatusForOk()) {
    RTC_LOG(LS_ERROR) << "NVENC: failed to get preset config";
    return false;
  }

  config = preset_config.presetCfg;
  config.gopLength = NVENC_INFINITE_GOPLENGTH;
  config.frameIntervalP = 1;
  config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
  config.rcParams.averageBitRate = bitrate_bps_;
  config.rcParams.maxBitRate = bitrate_bps_;
  if (frame_rate_ > 0) {
    const int vbv = std::max(bitrate_bps_ / static_cast<int>(frame_rate_) * 2,
                             1);
  config.rcParams.vbvBufferSize = vbv;
  config.rcParams.vbvInitialDelay = vbv;
}
  config.encodeCodecConfig.h264Config.idrPeriod = NVENC_INFINITE_GOPLENGTH;
  config.encodeCodecConfig.h264Config.repeatSPSPPS = 1;
  config.encodeCodecConfig.h264Config.disableDeblockingFilterIDC = 0;
  config.encodeCodecConfig.h264Config.level = NV_ENC_LEVEL_AUTOSELECT;
  config.profileGUID = NV_ENC_H264_PROFILE_BASELINE_GUID;

  init_params.encodeGUID = encode_guid;
  init_params.presetGUID = preset_guid;
  init_params.tuningInfo = tuning;
  init_params.encodeWidth = width_;
  init_params.encodeHeight = height_;
  init_params.darWidth = width_;
  init_params.darHeight = height_;
  init_params.frameRateNum = frame_rate_;
  init_params.frameRateDen = 1;
  init_params.enablePTD = 1;
  init_params.encodeConfig = &config;

  if (nvenc_api_->nvEncInitializeEncoder(encoder_, &init_params) !=
      NvencStatusForOk()) {
    RTC_LOG(LS_ERROR) << "NVENC: failed to initialize encoder";
    return false;
  }

  if (!CreateNvencInputTexture() || !RegisterNvencInput() ||
      !CreateBitstreamBuffer()) {
    RTC_LOG(LS_ERROR) << "NVENC: failed to set up buffers";
    return false;
  }

  SetNativeVideoEncoderActive(true);
  inited_.store(true, std::memory_order_release);
  return true;
}

bool NvencVideoEncoder::CreateNvencInputTexture() {
  if (!d3d11_device_) {
    return false;
  }

  if (nvenc_input_texture_) {
    return true;
  }

  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = width_;
  desc.Height = height_;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_NV12;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_RENDER_TARGET;

  HRESULT hr = d3d11_device_->CreateTexture2D(&desc, nullptr,
                                              &nvenc_input_texture_);
  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "NVENC: failed to create input texture";
    return false;
  }
  return true;
}

bool NvencVideoEncoder::RegisterNvencInput() {
  if (!encoder_ || !nvenc_api_ || !nvenc_input_texture_) {
    return false;
  }

  if (registered_input_) {
    nvenc_api_->nvEncUnregisterResource(encoder_, registered_input_);
    registered_input_ = nullptr;
  }

  NV_ENC_REGISTER_RESOURCE register_resource = {};
  register_resource.version = nvenc_compat::RegisterResourceVersion();
  register_resource.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
  register_resource.resourceToRegister = nvenc_input_texture_.Get();
  register_resource.width = width_;
  register_resource.height = height_;
  register_resource.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12;
  register_resource.bufferUsage = NV_ENC_INPUT_IMAGE;

  if (nvenc_api_->nvEncRegisterResource(encoder_, &register_resource) !=
      NvencStatusForOk()) {
    RTC_LOG(LS_ERROR) << "NVENC: failed to register input resource";
    return false;
  }
  registered_input_ = register_resource.registeredResource;
  return true;
}

bool NvencVideoEncoder::CreateBitstreamBuffer() {
  if (!encoder_ || !nvenc_api_) {
    return false;
  }

  if (bitstream_buffer_) {
    nvenc_api_->nvEncDestroyBitstreamBuffer(encoder_, bitstream_buffer_);
    bitstream_buffer_ = nullptr;
  }

  NV_ENC_CREATE_BITSTREAM_BUFFER create_buffer = {};
  create_buffer.version = nvenc_compat::CreateBitstreamBufferVersion();
  if (nvenc_api_->nvEncCreateBitstreamBuffer(encoder_, &create_buffer) !=
      NvencStatusForOk()) {
    RTC_LOG(LS_ERROR) << "NVENC: failed to create bitstream buffer";
    return false;
  }
  bitstream_buffer_ = create_buffer.bitstreamBuffer;
  return true;
}

bool NvencVideoEncoder::ReconfigureIfNeeded() {
  if (!pending_reconfigure_ || !nvenc_state_ || !nvenc_api_ || !encoder_) {
    return true;
  }

  auto& init_params = nvenc_state_->init_params;
  auto& config = nvenc_state_->config;

  config.rcParams.averageBitRate = bitrate_bps_;
  config.rcParams.maxBitRate = bitrate_bps_;
  if (frame_rate_ > 0) {
    const int vbv = std::max(bitrate_bps_ / static_cast<int>(frame_rate_) * 2,
                             1);
    config.rcParams.vbvBufferSize = vbv;
    config.rcParams.vbvInitialDelay = vbv;
    init_params.frameRateNum = frame_rate_;
  }

  NV_ENC_RECONFIGURE_PARAMS reconfig = {};
  reconfig.version = nvenc_compat::ReconfigureParamsVersion();
  reconfig.reInitEncodeParams = init_params;
  reconfig.resetEncoder = 1;

  if (nvenc_api_->nvEncReconfigureEncoder(encoder_, &reconfig) !=
      NvencStatusForOk()) {
    RTC_LOG(LS_WARNING) << "NVENC: reconfigure failed";
    return false;
  }

  pending_reconfigure_ = false;
  return true;
}

bool NvencVideoEncoder::EncodeFrame(const webrtc::VideoFrame& input_image,
                                    bool force_idr) {
  if (!encoder_ || !nvenc_api_ || !bitstream_buffer_ || !d3d11_context_) {
    return false;
  }

  if (!ReconfigureIfNeeded()) {
    return false;
  }

  auto buffer = input_image.video_frame_buffer();
  auto* native_buffer =
      static_cast<owt::base::D3D11TextureBuffer*>(buffer.get());
  ID3D11Texture2D* src_texture = native_buffer->texture();
  if (!src_texture) {
    return false;
  }

  d3d11_context_->CopyResource(nvenc_input_texture_.Get(), src_texture);

  NV_ENC_MAP_INPUT_RESOURCE map_input = {};
  map_input.version = nvenc_compat::MapInputResourceVersion();
  map_input.registeredResource = registered_input_;
  if (nvenc_api_->nvEncMapInputResource(encoder_, &map_input) !=
      NvencStatusForOk()) {
    RTC_LOG(LS_ERROR) << "NVENC: failed to map input";
    return false;
  }
  mapped_input_ = map_input.mappedResource;

  NV_ENC_PIC_PARAMS pic_params = {};
  pic_params.version = nvenc_compat::PicParamsVersion();
  pic_params.inputBuffer = mapped_input_;
  pic_params.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;
  pic_params.inputWidth = width_;
  pic_params.inputHeight = height_;
  pic_params.outputBitstream = bitstream_buffer_;
  pic_params.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
  pic_params.encodePicFlags = force_idr ? NV_ENC_PIC_FLAG_FORCEIDR : 0;
  pic_params.inputTimeStamp = static_cast<uint64_t>(frame_index_++);

  if (nvenc_api_->nvEncEncodePicture(encoder_, &pic_params) !=
      NvencStatusForOk()) {
    nvenc_api_->nvEncUnmapInputResource(encoder_, mapped_input_);
    mapped_input_ = nullptr;
    RTC_LOG(LS_ERROR) << "NVENC: failed to encode picture";
    return false;
  }

  nvenc_api_->nvEncUnmapInputResource(encoder_, mapped_input_);
  mapped_input_ = nullptr;

  NV_ENC_LOCK_BITSTREAM lock_bitstream = {};
  lock_bitstream.version = nvenc_compat::LockBitstreamVersion();
  lock_bitstream.outputBitstream = bitstream_buffer_;
  lock_bitstream.doNotWait = 0;
  if (nvenc_api_->nvEncLockBitstream(encoder_, &lock_bitstream) !=
      NvencStatusForOk()) {
    RTC_LOG(LS_ERROR) << "NVENC: failed to lock bitstream";
    return false;
  }

  const uint8_t* encoded_data =
      static_cast<const uint8_t*>(lock_bitstream.bitstreamBufferPtr);
  const int encoded_size =
      static_cast<int>(lock_bitstream.bitstreamSizeInBytes);
  if (encoded_size <= 0) {
    nvenc_api_->nvEncUnlockBitstream(encoder_, bitstream_buffer_);
    return true;
  }

  webrtc::EncodedImage encoded_frame;
  encoded_frame._encodedWidth = width_;
  encoded_frame._encodedHeight = height_;
  encoded_frame.capture_time_ms_ = input_image.render_time_ms();
  encoded_frame.SetRtpTimestamp(input_image.timestamp());
  encoded_frame._frameType = force_idr ? webrtc::VideoFrameType::kVideoFrameKey
                                       : webrtc::VideoFrameType::kVideoFrameDelta;
  encoded_frame.SetEncodedData(
      webrtc::EncodedImageBuffer::Create(encoded_data, encoded_size));

  webrtc::CodecSpecificInfo info;
  std::memset(&info, 0, sizeof(info));
  info.codecType = codec_type_;

  const auto result = callback_->OnEncodedImage(encoded_frame, &info);
  nvenc_api_->nvEncUnlockBitstream(encoder_, bitstream_buffer_);

  if (result.error != webrtc::EncodedImageCallback::Result::Error::OK) {
    return false;
  }

  return true;
}

bool NvencVideoEncoder::LoadNvencApi() {
  if (nvenc_api_) {
    return true;
  }

  HMODULE nvenc_dll = LoadLibraryA("nvEncodeAPI64.dll");
  if (!nvenc_dll) {
    RTC_LOG(LS_WARNING) << "NVENC: nvEncodeAPI64.dll not found";
    return false;
  }

  auto create_instance =
      reinterpret_cast<NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*)>(
          GetProcAddress(nvenc_dll, "NvEncodeAPICreateInstance"));
  if (!create_instance) {
    RTC_LOG(LS_ERROR) << "NVENC: NvEncodeAPICreateInstance not found";
    FreeLibrary(nvenc_dll);
    return false;
  }

  auto api = std::make_unique<NV_ENCODE_API_FUNCTION_LIST>();
  api->version = nvenc_compat::FunctionListVersion();
  if (create_instance(api.get()) != NvencStatusForOk()) {
    RTC_LOG(LS_ERROR) << "NVENC: failed to create API instance";
    FreeLibrary(nvenc_dll);
    return false;
  }

  nvenc_dll_ = nvenc_dll;
  nvenc_api_ = std::move(api);
  return true;
}

void NvencVideoEncoder::DestroyEncoder() {
  if (encoder_ && nvenc_api_) {
    if (bitstream_buffer_) {
      nvenc_api_->nvEncDestroyBitstreamBuffer(encoder_, bitstream_buffer_);
      bitstream_buffer_ = nullptr;
    }
    if (registered_input_) {
      nvenc_api_->nvEncUnregisterResource(encoder_, registered_input_);
      registered_input_ = nullptr;
    }
    nvenc_api_->nvEncDestroyEncoder(encoder_);
    encoder_ = nullptr;
  }

  DestroyNvencResources();
  nvenc_state_.reset();
}

void NvencVideoEncoder::DestroyNvencResources() {
  nvenc_input_texture_.Reset();
  if (nvenc_dll_) {
    FreeLibrary(static_cast<HMODULE>(nvenc_dll_));
    nvenc_dll_ = nullptr;
  }
  nvenc_api_.reset();
}

}  // namespace base
}  // namespace owt

#endif  // defined(WEBRTC_WIN)
