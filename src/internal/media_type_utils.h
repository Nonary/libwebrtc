#ifndef LIB_WEBRTC_INTERNAL_MEDIA_TYPE_UTILS_HXX
#define LIB_WEBRTC_INTERNAL_MEDIA_TYPE_UTILS_HXX

#include "api/media_types.h"
#include "rtc_types.h"

namespace libwebrtc {

inline cricket::MediaType ToCricketMediaType(RTCMediaType type) {
  switch (type) {
    case RTCMediaType::AUDIO:
      return cricket::MEDIA_TYPE_AUDIO;
    case RTCMediaType::VIDEO:
      return cricket::MEDIA_TYPE_VIDEO;
    case RTCMediaType::DATA:
      return cricket::MEDIA_TYPE_DATA;
    default:
      return cricket::MEDIA_TYPE_UNSUPPORTED;
  }
}

inline RTCMediaType ToRTCMediaType(cricket::MediaType type) {
  switch (type) {
    case cricket::MEDIA_TYPE_AUDIO:
      return RTCMediaType::AUDIO;
    case cricket::MEDIA_TYPE_VIDEO:
      return RTCMediaType::VIDEO;
    case cricket::MEDIA_TYPE_DATA:
      return RTCMediaType::DATA;
    default:
      return RTCMediaType::UNSUPPORTED;
  }
}

}  // namespace libwebrtc

#endif  // LIB_WEBRTC_INTERNAL_MEDIA_TYPE_UTILS_HXX
