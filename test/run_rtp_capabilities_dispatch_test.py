#!/usr/bin/env python3
"""Compile the current production capability methods against focused test doubles.

No copied dispatch implementation or source-text behavioral assertions are used.
The full definitions are extracted verbatim on each invocation, with #line
locations pointing compiler diagnostics back to the actual production file.
"""

import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


def extract_methods(source_path):
    source = source_path.read_text()
    definitions = []
    for role in ("Sender", "Receiver"):
        pattern = (
            r"^scoped_refptr<RTCRtpCapabilities>\s*\n"
            r"RTCPeerConnectionFactoryImpl::GetRtp" + role
            + r"Capabilities\s*\([^)]*\)\s*\{.*?^\}"
        )
        matches = list(re.finditer(pattern, source, re.MULTILINE | re.DOTALL))
        if len(matches) != 1:
            raise RuntimeError(f"Expected exactly one complete {role} definition")
        match = matches[0]
        line = source[:match.start()].count("\n") + 1
        escaped_path = str(source_path).replace("\\", "\\\\").replace('"', '\\"')
        definitions.append(f'#line {line} "{escaped_path}"\n{match.group()}\n')
    return "\n".join(definitions)


def main():
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--source", type=Path,
        default=root / "src/rtc_peerconnection_factory_impl.cc",
        help="Production source to compile (can be a saved baseline)",
    )
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="rtp-capabilities-test-") as directory:
        build = Path(directory)
        (build / "production_rtp_capabilities.inc").write_text(
            extract_methods(args.source.resolve())
        )
        executable = build / "rtp_capabilities_dispatch_test"
        command = [
            *shlex.split(os.environ.get("CXX", "c++")),
            "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
            *shlex.split(os.environ.get("CXXFLAGS", "")),
            "-I", str(root / "include"), "-I", str(root / "src"),
            "-I", str(root / "test/support/rtp_capabilities"),
            "-I", str(build),
            str(root / "test/rtp_capabilities_dispatch_test.cc"),
            "-o", str(executable),
        ]
        subprocess.run(command, check=True, timeout=120)
        subprocess.run([str(executable)], check=True, timeout=30)


if __name__ == "__main__":
    main()
