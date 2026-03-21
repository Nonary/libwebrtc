// Copyright (C) <2025>
//
// SPDX-License-Identifier: Apache-2.0
#ifndef OWT_BASE_WIN_NVENC_API_COMPAT_H_
#define OWT_BASE_WIN_NVENC_API_COMPAT_H_

#include <cstdint>

namespace owt {
namespace base {
namespace nvenc_compat {

constexpr uint32_t MakeNvencVersion(uint32_t major, uint32_t minor) {
  return major | (minor << 24);
}

constexpr uint32_t MakeStructVersion(uint32_t api_version,
                                     uint32_t struct_version,
                                     bool extended = false) {
  return api_version | (struct_version << 16) | (0x7u << 28) |
         (extended ? (1u << 31) : 0u);
}

// WebRTC's native NVENC path is H.264-only, so keep the legacy 11.0 floor for
// maximum driver compatibility while compiling against newer SDK headers.
constexpr uint32_t kNvencApiVersion = MakeNvencVersion(11, 0);

constexpr uint32_t FunctionListVersion() {
  return MakeStructVersion(kNvencApiVersion, 2);
}

constexpr uint32_t OpenEncodeSessionExParamsVersion() {
  return MakeStructVersion(kNvencApiVersion, 1);
}

constexpr uint32_t InitializeParamsVersion() {
  return MakeStructVersion(kNvencApiVersion, 5, true);
}

constexpr uint32_t ConfigVersion() {
  return MakeStructVersion(kNvencApiVersion, 7, true);
}

constexpr uint32_t PresetConfigVersion() {
  return MakeStructVersion(kNvencApiVersion, 4, true);
}

constexpr uint32_t RegisterResourceVersion() {
  return MakeStructVersion(kNvencApiVersion, 3);
}

constexpr uint32_t CreateBitstreamBufferVersion() {
  return MakeStructVersion(kNvencApiVersion, 1);
}

constexpr uint32_t ReconfigureParamsVersion() {
  return MakeStructVersion(kNvencApiVersion, 1, true);
}

constexpr uint32_t MapInputResourceVersion() {
  return MakeStructVersion(kNvencApiVersion, 4);
}

constexpr uint32_t PicParamsVersion() {
  return MakeStructVersion(kNvencApiVersion, 4, true);
}

constexpr uint32_t LockBitstreamVersion() {
  return MakeStructVersion(kNvencApiVersion, 1);
}

}  // namespace nvenc_compat
}  // namespace base
}  // namespace owt

#endif  // OWT_BASE_WIN_NVENC_API_COMPAT_H_
