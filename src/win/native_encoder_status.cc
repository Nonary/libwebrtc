// Copyright (C) <2025>
//
// SPDX-License-Identifier: Apache-2.0
#include "src/win/native_encoder_status.h"

#include <atomic>

namespace owt {
namespace base {

namespace {
std::atomic<bool> g_native_video_encoder_active {false};
}  // namespace

void SetNativeVideoEncoderActive(bool active) {
  g_native_video_encoder_active.store(active, std::memory_order_release);
}

bool IsNativeVideoEncoderActive() {
  return g_native_video_encoder_active.load(std::memory_order_acquire);
}

}  // namespace base
}  // namespace owt
