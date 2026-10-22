#ifndef LIB_WEBRTC_TEST_MEDIA_TYPES_H_
#define LIB_WEBRTC_TEST_MEDIA_TYPES_H_

// Minimal WebRTC enum dependency for the real media_type_utils.h conversion.
namespace cricket {
enum MediaType {
  MEDIA_TYPE_AUDIO,
  MEDIA_TYPE_VIDEO,
  MEDIA_TYPE_DATA,
  MEDIA_TYPE_UNSUPPORTED,
};
}  // namespace cricket

#endif  // LIB_WEBRTC_TEST_MEDIA_TYPES_H_
