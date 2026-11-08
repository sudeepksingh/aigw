#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Writes the packets tests/test_sni.c reads, from a real OpenSSL ClientHello:

  sni-tls.bin        TLS records as a TCP stream starts (SNI www.example.com, ALPN h2)
  sni-quic1-a.bin    QUIC v1 client Initial carrying CRYPTO bytes [300:]  (first datagram)
  sni-quic1-b.bin    QUIC v1 client Initial carrying CRYPTO bytes [0:300] (second datagram)
  sni-quic2.bin      QUIC v2 client Initial with the whole ClientHello

    python3 tests/data/make_sni_fixtures.py tests/data

Needs the cryptography package. The packets are protected exactly as RFC 9001
(and RFC 9369 for v2) say, so they also check the C code against an
independent implementation.
"""
import os
import ssl
import struct
import sys

from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from cryptography.hazmat.primitives.kdf.hkdf import HKDFExpand
from cryptography.hazmat.primitives import hmac


def client_hello(host, alpn):
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.set_alpn_protocols(alpn)
    inc, out = ssl.MemoryBIO(), ssl.MemoryBIO()
    obj = ctx.wrap_bio(inc, out, server_hostname=host)
    try:
        obj.do_handshake()
    except ssl.SSLWantReadError:
        pass
    return out.read()


def extract(salt, ikm):
    h = hmac.HMAC(salt, hashes.SHA256())
    h.update(ikm)
    return h.finalize()


def expand_label(secret, label, length):
    full = b"tls13 " + label.encode()
    info = struct.pack(">HB", length, len(full)) + full + b"\x00"
    return HKDFExpand(hashes.SHA256(), length, info).derive(secret)


SALT = {1: bytes.fromhex("38762cf7f55934b34d179ae6a4c80cadccbb7f0a"),
        0x6b3343cf: bytes.fromhex("0dede3def700a6db819381be6e269dcbf9bd2ed9")}


def keys(version, dcid):
    v2 = version != 1
    client = expand_label(extract(SALT[version], dcid), "client in", 32)
    pre = "quicv2 " if v2 else "quic "
    return (expand_label(client, pre + "key", 16), expand_label(client, pre + "iv", 12),
            expand_label(client, pre + "hp", 16))


def varint(v):
    if v < 64:
        return bytes([v])
    if v < 16384:
        return struct.pack(">H", 0x4000 | v)
    return struct.pack(">I", 0x80000000 | v)


def initial(version, dcid, scid, pn, frames):
    key, iv, hp = keys(version, dcid)
    pnlen = 2
    ptype = 1 if version != 1 else 0
    payload = frames + b"\x00" * max(0, 1162 - len(frames))      # pad the datagram to ~1200
    first = 0xc0 | (ptype << 4) | (pnlen - 1)
    length = pnlen + len(payload) + 16
    hdr = bytes([first]) + struct.pack(">I", version) + bytes([len(dcid)]) + dcid + bytes([len(scid)]) + scid
    hdr += varint(0) + varint(length)                             # no token
    pnoff = len(hdr)
    hdr += struct.pack(">H", pn)
    nonce = bytearray(iv)
    for i in range(8):
        nonce[11 - i] ^= (pn >> (8 * i)) & 0xff
    ct = AESGCM(key).encrypt(bytes(nonce), payload, hdr)
    pkt = bytearray(hdr + ct)
    enc = Cipher(algorithms.AES(hp), modes.ECB()).encryptor()
    mask = enc.update(bytes(pkt[pnoff + 4:pnoff + 20])) + enc.finalize()
    pkt[0] ^= mask[0] & 0x0f
    for i in range(pnlen):
        pkt[pnoff + i] ^= mask[1 + i]
    return bytes(pkt)


def crypto(off, data):
    return b"\x06" + varint(off) + varint(len(data)) + data


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else "."
    # RFC 9001 appendix A.1, checked here so a wrong vector can't slip into test_sni.c
    k, iv, hp = keys(1, bytes.fromhex("8394c8f03e515708"))
    assert (k.hex(), iv.hex(), hp.hex()) == ("1f369613dd76d5467730efcbe3b1a22d", "fa044b2f42a3fd3b46fb255c",
                                             "9f50449e04a0e810283a1e9933adedd2"), (k.hex(), iv.hex(), hp.hex())
    rec = client_hello("www.example.com", ["h2", "http/1.1"])
    open(os.path.join(outdir, "sni-tls.bin"), "wb").write(rec)
    hs = rec[5:]                                                  # the handshake message, as QUIC carries it
    qh = client_hello("video.example.net", ["h3"])[5:]
    dcid, scid = bytes.fromhex("0123456789abcdef"), bytes.fromhex("feedf00d")
    open(os.path.join(outdir, "sni-quic1-a.bin"), "wb").write(
        initial(1, dcid, scid, 0, crypto(300, qh[300:])))
    open(os.path.join(outdir, "sni-quic1-b.bin"), "wb").write(
        initial(1, dcid, scid, 1, b"\x01" + crypto(0, qh[:300])))
    open(os.path.join(outdir, "sni-quic2.bin"), "wb").write(
        initial(0x6b3343cf, bytes.fromhex("c0ffee00c0ffee00aa"), scid, 0, crypto(0, hs)))
    print("RFC 9001 A.1 keys match; wrote fixtures (ClientHello %d bytes)" % len(hs))


if __name__ == "__main__":
    main()
