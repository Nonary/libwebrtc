// Copyright (C) <2025>
//
// SPDX-License-Identifier: Apache-2.0
#include "src/win/nvenc_video_encoder_factory.h"

#if defined(WEBRTC_WIN)
#include <windows.h>

#include <algorithm>
#include <cstring>

#include "absl/strings/match.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "media/base/media_constants.h"
#include "d3d11.h"
#include <wrl/client.h>
#include "ffnvcodec/nvEncodeAPI.h"
#include "rtc_base/logging.h"
#include "src/win/codecutils.h"
#include "src/win/nvenc_api_compat.h"
#include "src/win/nvenc_video_encoder.h"

namespace owt {
namespace base {

namespace {

bool CheckNvencAvailability() {
  HMODULE nvenc_dll = LoadLibraryA("nvEncodeAPI64.dll");
  if (!nvenc_dll) {
    return false;
  }

  auto create_instance =
      reinterpret_cast<NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*)>(
          GetProcAddress(nvenc_dll, "NvEncodeAPICreateInstance"));
  if (!create_instance) {
    FreeLibrary(nvenc_dll);
    return false;
  }

  NV_ENCODE_API_FUNCTION_LIST api = {};
  api.version = nvenc_compat::FunctionListVersion();
  if (create_instance(&api) != NV_ENC_SUCCESS) {
    FreeLibrary(nvenc_dll);
    return false;
  }

  Microsoft::WRL::ComPtr<ID3D11Device> device;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
  D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                               nullptr, 0, D3D11_SDK_VERSION,
                               device.ReleaseAndGetAddressOf(), &level,
                               context.ReleaseAndGetAddressOf()))) {
    FreeLibrary(nvenc_dll);
    return false;
  }

  NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open_params = {};
  open_params.version = nvenc_compat::OpenEncodeSessionExParamsVersion();
  open_params.device = device.Get();
  open_params.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
  open_params.apiVersion = nvenc_compat::kNvencApiVersion;

  void* encoder = nullptr;
  const NVENCSTATUS status =
      api.nvEncOpenEncodeSessionEx(&open_params, &encoder);
  if (status != NV_ENC_SUCCESS || !encoder) {
    FreeLibrary(nvenc_dll);
    return false;
  }

  api.nvEncDestroyEncoder(encoder);
  FreeLibrary(nvenc_dll);
  return true;
}

std::vector<webrtc::SdpVideoFormat> MergeFormats(
    const std::vector<webrtc::SdpVideoFormat>& preferred,
    const std::vector<webrtc::SdpVideoFormat>& fallback) {
  std::vector<webrtc::SdpVideoFormat> formats = preferred;
  for (const auto& format : fallback) {
    const bool exists =
        std::any_of(formats.begin(), formats.end(),
                    [&](const webrtc::SdpVideoFormat& existing) {
                      return existing == format;
                    });
    if (!exists) {
      formats.push_back(format);
    }
  }
  return formats;
}

}  // namespace

NvencVideoEncoderFactory::NvencVideoEncoderFactory(
    std::unique_ptr<webrtc::VideoEncoderFactory> fallback)
    : nvenc_available_(CheckNvencAvailability()),
      fallback_(std::move(fallback)) {
  if (!fallback_) {
    fallback_ = webrtc::CreateBuiltinVideoEncoderFactory();
  }
  if (!nvenc_available_) {
    RTC_LOG(LS_WARNING) << "NVENC: unavailable, falling back to builtin encoder";
  }
}

std::unique_ptr<webrtc::VideoEncoder> NvencVideoEncoderFactory::Create(
    const webrtc::Environment& env,
    const webrtc::SdpVideoFormat& format) {
  if (nvenc_available_ &&
      absl::EqualsIgnoreCase(format.name, cricket::kH264CodecName)) {
    webrtc::VideoCodec codec;
    codec.codecType = webrtc::kVideoCodecH264;
    auto encoder = NvencVideoEncoder::Create(codec);
    if (encoder) {
      return encoder;
    }
  }

  return fallback_->Create(env, format);
}

std::vector<webrtc::SdpVideoFormat>
NvencVideoEncoderFactory::GetSupportedFormats() const {
  const auto fallback_formats = fallback_->GetSupportedFormats();
  if (!nvenc_available_) {
    return fallback_formats;
  }

  const auto preferred = CodecUtils::SupportedH264Codecs();
  return MergeFormats(preferred, fallback_formats);
}

webrtc::VideoEncoderFactory::CodecSupport
NvencVideoEncoderFactory::QueryCodecSupport(
    const webrtc::SdpVideoFormat& format,
    absl::optional<std::string> scalability_mode) const {
  auto info = fallback_->QueryCodecSupport(format, scalability_mode);
  if (nvenc_available_ &&
      absl::EqualsIgnoreCase(format.name, cricket::kH264CodecName)) {
    info.is_supported = true;
    info.is_power_efficient = true;
  }
  return info;
}

}  // namespace base
}  // namespace owt

#endif  // defined(WEBRTC_WIN)
