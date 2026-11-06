# Contributing to AIGW

Thanks for helping build an open, AI-native gateway.

## Ground rules

- **Both hosts, always.** Code must build and run on OpenWRT and on a PC acting
  as AP. Use libubox / ubus / UCI rather than host-specific APIs; keep
  host-specific bits in `platform/`.
- **Fail-open.** Nothing in the data path may break the network when AIGW
  stops or crashes.
- **Small footprint.** Fixed-size tables, no unbounded allocation, no new
  runtime dependencies without discussion.
- **Untrusted input is hostile.** Anything parsed from the network gets
  bounds checks and a unit test with malformed input.
- **Explainable.** Decisions carry reasons; see the architecture's data model.

## Workflow

```bash
sudo ./scripts/build-deps.sh                 # once
cmake -S . -B build && cmake --build build
ctest --test-dir build --output-on-failure
sudo tests/smoke.sh build
platform/openwrt/build-sdk.sh                # before touching packaging
```

Style: Linux kernel C style (tabs), `-Wall -Wextra` clean, SPDX header on
every source file. Shell scripts are POSIX `sh` so they run on OpenWRT's
busybox.

Open an issue before large changes so the design stays coherent with
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

By contributing you agree your work is licensed under the Apache License 2.0.
