# Focused RTP capability dispatch regression

Run from any working directory with Python 3 and a C++17 compiler:

```sh
python3 test/run_rtp_capabilities_dispatch_test.py
```

`CXX` and `CXXFLAGS` can select a compiler and additional flags. For example:

```sh
CXXFLAGS='-O2 -fsanitize=address,undefined -fno-omit-frame-pointer' \
  python3 test/run_rtp_capabilities_dispatch_test.py
```

The runner extracts both complete capability method definitions verbatim from
`src/rtc_peerconnection_factory_impl.cc` at test time and compiles them as C++.
It does not duplicate the dispatch implementation or assert source strings.
It includes the production media-type conversion and reference-counting headers.
The thread, backend factory, and capability payload adapter are test doubles;
`BlockingCall` runs callbacks on a separate `std::thread`.

Coverage includes:

- Sender and receiver results for audio, video, data, and unsupported media.
- Direct signaling-thread calls, calls from another registered thread, and calls
  from an unregistered thread whose `Current()` is null.
- Exactly one backend call, the correct backend role, signaling-thread affinity,
  no extra dispatch for direct calls, and preserved caller context.
- Distinct, reference-counted results that remain usable after factory teardown,
  with all test wrapper instances destroyed after the final reference is dropped.
- The existing initialized-factory precondition in all three caller contexts.
  A checked test slot reports a missing factory instead of invoking undefined
  behavior. This does **not** assert that native null-factory calls throw, return
  null, or have any defined behavior; the production null handling is unchanged.

To check the regression against an earlier source file, use `--source /path/to/file`.
The pinned `77bfd898` baseline fails with `wrong sender/receiver result`; the
receiver dispatch fix passes 24 normal cases and six missing-factory cases.

This is a focused source-level regression, not a full native WebRTC build, ABI
check, codec-adapter test, or host/browser integration test. Native builds still
require the WebRTC source tree and toolchain described in the repository README.
