// The runner includes the two current production method definitions below.
// Only their WebRTC dependencies are doubled; dispatch logic is not duplicated.
#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "rtc_types.h"
#include "internal/media_type_utils.h"

namespace {
void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
}  // namespace

namespace rtc {
// Run BlockingCall on a real, separate thread. Refuse self-dispatch so the test
// also detects accidental re-dispatch from the signaling thread.
class Thread {
 public:
  Thread() : worker_([this] { Run(); }) {}
  ~Thread() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    ready_.notify_one();
    worker_.join();
  }

  static Thread* Current() { return current_; }

  template <typename F>
  auto BlockingCall(F callback) -> decltype(callback()) {
    Check(Current() != this, "unexpected self-dispatch");
    ++dispatches;
    auto task = std::make_shared<std::packaged_task<decltype(callback())()>>(
        std::move(callback));
    auto result = task->get_future();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      queue_.push([task] { (*task)(); });
    }
    ready_.notify_one();
    return result.get();
  }

  std::atomic<int> dispatches{0};

 private:
  void Run() {
    current_ = this;
    for (;;) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
        if (queue_.empty() && stopping_) return;
        task = std::move(queue_.front());
        queue_.pop();
      }
      task();
    }
  }

  static thread_local Thread* current_;
  std::mutex mutex_;
  std::condition_variable ready_;
  std::queue<std::function<void()>> queue_;
  bool stopping_ = false;
  std::thread worker_;
};
thread_local Thread* Thread::current_ = nullptr;

}  // namespace rtc

namespace webrtc {
enum class Role { SENDER, RECEIVER };
struct RtpCapabilities {
  Role role;
  cricket::MediaType media_type;
  int sequence;
  std::string codec;
};
}  // namespace webrtc

namespace libwebrtc {
// Keep the repository's real scoped_refptr and RefCountedObject ownership.
class RTCRtpCapabilities : public RefCountInterface {
 public:
  virtual const webrtc::RtpCapabilities& value() const = 0;
};

class RTCRtpCapabilitiesImpl : public RTCRtpCapabilities {
 public:
  explicit RTCRtpCapabilitiesImpl(const webrtc::RtpCapabilities& value)
      : value_(value) {
    ++live_objects;
  }
  ~RTCRtpCapabilitiesImpl() override { --live_objects; }
  const webrtc::RtpCapabilities& value() const override { return value_; }
  static std::atomic<int> live_objects;

 private:
  webrtc::RtpCapabilities value_;
};
std::atomic<int> RTCRtpCapabilitiesImpl::live_objects{0};

struct Backend {
  explicit Backend(rtc::Thread* signaling) : signaling(signaling) {}
  webrtc::RtpCapabilities GetRtpSenderCapabilities(cricket::MediaType type) {
    return Get(webrtc::Role::SENDER, type);
  }
  webrtc::RtpCapabilities GetRtpReceiverCapabilities(cricket::MediaType type) {
    return Get(webrtc::Role::RECEIVER, type);
  }
  webrtc::RtpCapabilities Get(webrtc::Role role, cricket::MediaType type) {
    Check(rtc::Thread::Current() == signaling,
          "backend called off the signaling thread");
    if (role == webrtc::Role::SENDER) ++sender_calls;
    else ++receiver_calls;
    return {role, type, ++sequence, codec};
  }
  rtc::Thread* signaling;
  int sender_calls = 0;
  int receiver_calls = 0;
  int sequence = 0;
  std::string codec = "original-codec";
};

struct MissingFactory : std::runtime_error {
  MissingFactory() : std::runtime_error("factory must be initialized") {}
};

// Instrument the existing initialization precondition without invoking C++
// undefined behavior. This does not model native null-pointer crash behavior.
struct FactorySlot {
  Backend* pointer;
  rtc::Thread* signaling;
  Backend* get() const { return pointer; }
  explicit operator bool() const { return pointer != nullptr; }
  Backend* operator->() const {
    Check(rtc::Thread::Current() == signaling,
          "factory dereferenced off the signaling thread");
    if (!pointer) throw MissingFactory();
    return pointer;
  }
};

class RTCPeerConnectionFactoryImpl {
 public:
  RTCPeerConnectionFactoryImpl(std::shared_ptr<rtc::Thread> signaling,
                               Backend* backend)
      : signaling_thread_(std::move(signaling)),
        rtc_peerconnection_factory_{backend, signaling_thread_.get()} {}
  scoped_refptr<RTCRtpCapabilities> GetRtpSenderCapabilities(RTCMediaType type);
  scoped_refptr<RTCRtpCapabilities> GetRtpReceiverCapabilities(RTCMediaType type);

 private:
  std::shared_ptr<rtc::Thread> signaling_thread_;
  FactorySlot rtc_peerconnection_factory_;
};

#include "production_rtp_capabilities.inc"
}  // namespace libwebrtc

int main() {
  using namespace libwebrtc;
  try {
    auto signaling = std::make_shared<rtc::Thread>();
    rtc::Thread caller;
    auto backend = std::make_unique<Backend>(signaling.get());
    auto factory = std::make_unique<RTCPeerConnectionFactoryImpl>(
        signaling, backend.get());
    scoped_refptr<RTCRtpCapabilities> retained;
    int cases = 0;
    for (auto role : {webrtc::Role::SENDER, webrtc::Role::RECEIVER}) {
      for (auto media : {RTCMediaType::AUDIO, RTCMediaType::VIDEO,
                         RTCMediaType::DATA, RTCMediaType::UNSUPPORTED}) {
        const cricket::MediaType expected_types[] = {
            cricket::MEDIA_TYPE_AUDIO, cricket::MEDIA_TYPE_VIDEO,
            cricket::MEDIA_TYPE_DATA, cricket::MEDIA_TYPE_UNSUPPORTED};
        const auto expected_media = expected_types[static_cast<int>(media)];
        for (int context = 0; context < 3; ++context) {
          auto invoke = [&] {
            auto* original_thread = rtc::Thread::Current();
            const int dispatches = signaling->dispatches;
            const int senders = backend->sender_calls;
            const int receivers = backend->receiver_calls;
            auto result = role == webrtc::Role::SENDER
                              ? factory->GetRtpSenderCapabilities(media)
                              : factory->GetRtpReceiverCapabilities(media);
            Check(result.get() != nullptr, "missing capabilities result");
            Check(result->value().role == role, "wrong sender/receiver result");
            Check(result->value().media_type == expected_media,
                  "wrong media type");
            Check(result->value().codec == "original-codec", "lost payload");
            Check(signaling->dispatches - dispatches == (context == 0 ? 0 : 1),
                  "wrong signaling dispatch count");
            Check(backend->sender_calls - senders ==
                      (role == webrtc::Role::SENDER ? 1 : 0),
                  "wrong sender backend call count");
            Check(backend->receiver_calls - receivers ==
                      (role == webrtc::Role::RECEIVER ? 1 : 0),
                  "wrong receiver backend call count");
            Check(rtc::Thread::Current() == original_thread,
                  "caller context changed");
            Check(RTCRtpCapabilitiesImpl::live_objects == (retained ? 2 : 1),
                  "unexpected wrapper allocation count");
            if (retained) {
              Check(result.get() != retained.get(), "reused a result wrapper");
              Check(result->value().sequence > retained->value().sequence,
                    "returned stale capabilities");
            }
            retained = result;
            auto copy = result;
            result = nullptr;
            Check(copy.get() == retained.get(), "reference ownership lost");
          };
          if (context == 0) signaling->BlockingCall(invoke);
          else if (context == 1) caller.BlockingCall(invoke);
          else invoke();  // Current() == nullptr: unregistered caller thread.
          Check(RTCRtpCapabilitiesImpl::live_objects == 1, "wrapper leaked");
          ++cases;
        }
      }
    }
    backend->codec = "changed-backend";
    factory.reset();
    backend.reset();
    Check(retained->value().codec == "original-codec",
          "result did not own its payload after factory teardown");
    retained = nullptr;
    Check(RTCRtpCapabilitiesImpl::live_objects == 0, "result was not destroyed");

    RTCPeerConnectionFactoryImpl empty(signaling, nullptr);
    for (bool receiver : {false, true}) {
      for (int context = 0; context < 3; ++context) {
        auto invoke = [&] {
          bool missing = false;
          try {
            if (receiver) empty.GetRtpReceiverCapabilities(RTCMediaType::AUDIO);
            else empty.GetRtpSenderCapabilities(RTCMediaType::AUDIO);
          } catch (const MissingFactory&) {
            missing = true;
          }
          Check(missing, "changed missing-factory initialization precondition");
        };
        if (context == 0) signaling->BlockingCall(invoke);
        else if (context == 1) caller.BlockingCall(invoke);
        else invoke();
      }
    }
    std::cout << "PASS: " << cases
              << " dispatch cases, ownership, and 6 missing-factory cases\n";
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
