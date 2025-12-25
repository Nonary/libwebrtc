// Copyright (C) <2025>
//
// SPDX-License-Identifier: Apache-2.0
#ifndef OWT_BASE_WIN_D3D11_TEXTURE_BUFFER_H_
#define OWT_BASE_WIN_D3D11_TEXTURE_BUFFER_H_

#include <d3d11.h>
#include <dxgi.h>

#include "api/video/video_frame_buffer.h"
#include "rtc_base/ref_counted_object.h"

namespace owt {
namespace base {

class D3D11TextureBuffer : public webrtc::VideoFrameBuffer {
 public:
  static rtc::scoped_refptr<D3D11TextureBuffer> Create(
      ID3D11Texture2D* texture,
      IDXGIKeyedMutex* keyed_mutex,
      int width,
      int height) {
    if (!texture || width <= 0 || height <= 0) {
      return nullptr;
    }
    auto buffer =
        rtc::make_ref_counted<D3D11TextureBuffer>(texture, keyed_mutex, width,
                                                  height);
    if (!buffer->lock_ok()) {
      return nullptr;
    }
    return buffer;
  }

  Type type() const override { return Type::kNative; }
  int width() const override { return width_; }
  int height() const override { return height_; }

  rtc::scoped_refptr<webrtc::I420BufferInterface> ToI420() override {
    return nullptr;
  }

  ID3D11Texture2D* texture() const { return texture_; }

  D3D11TextureBuffer(ID3D11Texture2D* texture,
                     IDXGIKeyedMutex* keyed_mutex,
                     int width,
                     int height)
      : width_(width),
        height_(height),
        texture_(texture),
        keyed_mutex_(keyed_mutex) {
    if (texture_) {
      texture_->AddRef();
    }
    if (keyed_mutex_) {
      keyed_mutex_->AddRef();
      const HRESULT hr = keyed_mutex_->AcquireSync(0, 3000);
      lock_ok_ = (hr == S_OK || hr == WAIT_ABANDONED);
    } else {
      lock_ok_ = true;
    }
  }

  ~D3D11TextureBuffer() override {
    if (keyed_mutex_) {
      keyed_mutex_->ReleaseSync(0);
      keyed_mutex_->Release();
    }
    if (texture_) {
      texture_->Release();
    }
  }

 private:
  bool lock_ok() const { return lock_ok_; }

  const int width_;
  const int height_;
  ID3D11Texture2D* texture_ = nullptr;
  IDXGIKeyedMutex* keyed_mutex_ = nullptr;
  bool lock_ok_ = false;
};

}  // namespace base
}  // namespace owt

#endif  // OWT_BASE_WIN_D3D11_TEXTURE_BUFFER_H_
