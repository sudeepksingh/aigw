#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""The installed flash of AIGW OS's packages, as aigw-selftest counts it (agd, agdns, agctl, aigw-nft,
aigw-remote, aigw-selftest and /usr/share/aigw), from the .ipk files, without a router.

    platform/openwrt/flash-kb.py dist/openwrt [dist/openwrt-small ...]

Each file takes 4 KB blocks. A directory takes one more on ext4 (the x86-64 VM in CI), none on
initramfs (the aarch64 and mipsel VMs): the numbers are the ones CI's QEMU jobs print.
"""
import io
import os
import sys
import tarfile

COUNTED = ('usr/sbin/agd', 'usr/sbin/agdns', 'usr/bin/agctl', 'usr/sbin/aigw-nft', 'usr/sbin/aigw-remote',
           'usr/sbin/aigw-selftest')


def members(ipk):
    outer = tarfile.open(ipk)
    for m in outer.getmembers():
        if m.name.lstrip('./') == 'data.tar.gz':
            yield from tarfile.open(fileobj=io.BytesIO(outer.extractfile(m).read())).getmembers()


def kb(d, arch):
    files, dirs = {}, set()
    for f in os.listdir(d):
        if not f.endswith('.ipk') or f.startswith(('aigw-demo', 'obuspa', 'icwmp')):
            continue
        for m in members(os.path.join(d, f)):
            n = m.name.lstrip('./').rstrip('/')
            if n in COUNTED or n == 'usr/share/aigw' or n.startswith('usr/share/aigw/'):
                if m.isdir():
                    dirs.add(n)
                elif m.isfile():
                    files[n] = m.size
    return sum((s + 4095) // 4096 * 4 for s in files.values()) + (4 * len(dirs) if arch == 'x86_64' else 0)


for root in sys.argv[1:]:
    name = os.path.basename(root.rstrip('/'))
    profile = name.split('-', 1)[1] if '-' in name else 'full'
    for ver in sorted(os.listdir(root)):
        for arch in sorted(os.listdir(os.path.join(root, ver))):
            print('%-8s %-8s %-20s %5d KB' % (profile, ver, arch, kb(os.path.join(root, ver, arch), arch)))
