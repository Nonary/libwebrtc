// Copyright (C) <2021> Intel Corporation
//
// SPDX-License-Identifier: Apache-2.0

#ifndef OWT_BASE_WIN_D3D11_MANAGER_H
#define OWT_BASE_WIN_D3D11_MANAGER_H

#include <d3d11_2.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfobjects.h>
#include <wrl/client.h>

#include "rtc_base/logging.h"
#include "rtc_base/ref_count.h"

namespace owt {
namespace base {

class D3D11Manager : public webrtc::RefCountInterface {
 public:
  D3D11Manager()
      : manager_(nullptr),
        device_(nullptr),
        ctx_(nullptr),
        adapter_(nullptr),
        mt_(nullptr),
        reset_token_(0) {}

  ~D3D11Manager() = default;

  bool Init() {
    HRESULT hr = MFCreateDXGIDeviceManager(&reset_token_, &manager_);
    if (FAILED(hr)) {
      RTC_LOG(LS_ERROR) << "MFCreateDxgiDeviceManager failed with error hr:"
                        << hr;
      return false;
    }

    Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
    hr = CreateDXGIFactory1(__uuidof(IDXGIFactory2),
                            reinterpret_cast<void**>(factory.GetAddressOf()));
    if (FAILED(hr)) {
      RTC_LOG(LS_ERROR) << "CreateDxgiFactory failed with error hr:" << hr;
      return false;
    }

    hr = factory->EnumAdapters(0, adapter_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
      RTC_LOG(LS_ERROR) << "EnumAdapters failed with error hr:" << hr;
      return false;
    }

    UINT creation_flags =
        D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;

    D3D_FEATURE_LEVEL feature_levels_in[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0};

    D3D_FEATURE_LEVEL feature_levels_out;
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                           creation_flags, feature_levels_in,
                           ARRAYSIZE(feature_levels_in), D3D11_SDK_VERSION,
                           device_.GetAddressOf(), &feature_levels_out,
                           ctx_.GetAddressOf());
    if (FAILED(hr)) {
      RTC_LOG(LS_ERROR) << "D3D11 CreateDevice failed with error hr:" << hr;
      return false;
    }

    hr = manager_->ResetDevice(device_.Get(), reset_token_);
    if (FAILED(hr)) {
      RTC_LOG(LS_ERROR) << "ResetDevice failed with error hr:" << hr;
      return false;
    }

    hr = device_.As(&mt_);
    if (FAILED(hr)) {
      RTC_LOG(LS_ERROR) << "CreateDxgiFactory failed with error hr:" << hr;
      return false;
    }

    hr = mt_->SetMultithreadProtected(true);
    if (FAILED(hr)) {
      RTC_LOG(LS_ERROR) << "SetMultithread Protection failed with error hr:"
                        << hr;
      return false;
    }

    return true;
  }

  Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> GetManager() {
    return manager_;
  }

  Microsoft::WRL::ComPtr<ID3D11Device> GetDevice() { return device_; }

  Microsoft::WRL::ComPtr<ID3D11DeviceContext> GetDeviceContext() {
    return ctx_;
  }

  Microsoft::WRL::ComPtr<ID3D10Multithread> GetMultiThread() { return mt_; }

 private:
  Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> manager_;
  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx_;
  Microsoft::WRL::ComPtr<IDXGIAdapter> adapter_;
  Microsoft::WRL::ComPtr<ID3D10Multithread> mt_;
  UINT reset_token_;
};
}  // namespace base
}  // namespace owt
#endif  // OWT_BASE_WIN_D3D11_MANAGER_H
