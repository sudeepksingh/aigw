## What and why

<!-- What does this change, and which problem does it solve? Link the issue: Fixes #123 -->

## How it was tested

<!-- e.g. ctest, sudo tests/smoke.sh build, platform/openwrt/build-sdk.sh, a real router or adapter -->

## Checklist

- [ ] Builds and runs on both OpenWRT and PC as AP (or only touches one platform's files)
- [ ] Network keeps working if AIGW stops or crashes (fail-open)
- [ ] Anything parsed from the network is bounds-checked and has a test with malformed input
- [ ] Docs updated if behaviour or configuration changed
- [ ] New source files carry the `SPDX-License-Identifier: Apache-2.0` header
