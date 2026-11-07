# SPDX-License-Identifier: Apache-2.0
"""Ed25519 keys and signatures in OpenWRT's usign format.

Gateways check model signatures with libag's verifier (core/libag/sign.c),
and `usign -V` accepts the same files. Uses the `cryptography` package when
it is installed, otherwise a small pure-Python Ed25519 (RFC 8032), which is
slow but only ever signs a handful of files.
"""
import base64
import hashlib
import os

try:
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey, Ed25519PublicKey
    HAVE_CRYPTOGRAPHY = True
except ImportError:  # pragma: no cover - depends on the host
    HAVE_CRYPTOGRAPHY = False

# ---------- pure-Python Ed25519 (RFC 8032, section 6) ----------

_p = 2 ** 255 - 19
_q = 2 ** 252 + 27742317777372353535851937790883648493
_d = -121665 * pow(121666, _p - 2, _p) % _p
_sqrt_m1 = pow(2, (_p - 1) // 4, _p)


def _inv(x):
    return pow(x, _p - 2, _p)


def _add(P, Q):
    a = (P[1] - P[0]) * (Q[1] - Q[0]) % _p
    b = (P[1] + P[0]) * (Q[1] + Q[0]) % _p
    c = 2 * P[3] * Q[3] * _d % _p
    d = 2 * P[2] * Q[2] % _p
    e, f, g, h = b - a, d - c, d + c, b + a
    return (e * f % _p, g * h % _p, f * g % _p, e * h % _p)


def _mul(s, P):
    Q = (0, 1, 1, 0)
    while s > 0:
        if s & 1:
            Q = _add(Q, P)
        P = _add(P, P)
        s >>= 1
    return Q


def _recover_x(y, sign):
    if y >= _p:
        return None
    x2 = (y * y - 1) * _inv(_d * y * y + 1)
    if x2 == 0:
        return None if sign else 0
    x = pow(x2, (_p + 3) // 8, _p)
    if (x * x - x2) % _p:
        x = x * _sqrt_m1 % _p
    if (x * x - x2) % _p:
        return None
    if (x & 1) != sign:
        x = _p - x
    return x


_gy = 4 * _inv(5) % _p
_gx = _recover_x(_gy, 0)
_G = (_gx, _gy, 1, _gx * _gy % _p)


def _compress(P):
    zi = _inv(P[2])
    x, y = P[0] * zi % _p, P[1] * zi % _p
    return int.to_bytes(y | ((x & 1) << 255), 32, "little")


def _decompress(s):
    y = int.from_bytes(s, "little")
    sign = y >> 255
    y &= (1 << 255) - 1
    x = _recover_x(y, sign)
    if x is None:
        return None
    return (x, y, 1, x * y % _p)


def _expand(seed):
    h = hashlib.sha512(seed).digest()
    a = int.from_bytes(h[:32], "little")
    a &= (1 << 254) - 8
    a |= 1 << 254
    return a, h[32:]


def _hq(b):
    return int.from_bytes(hashlib.sha512(b).digest(), "little") % _q


def _py_public(seed):
    return _compress(_mul(_expand(seed)[0], _G))


def _py_sign(seed, msg):
    a, prefix = _expand(seed)
    A = _compress(_mul(a, _G))
    r = _hq(prefix + msg)
    R = _compress(_mul(r, _G))
    s = (r + _hq(R + A + msg) * a) % _q
    return R + int.to_bytes(s, 32, "little")


def _py_verify(pk, msg, sig):
    A = _decompress(pk)
    if A is None or len(sig) != 64:
        return False
    s = int.from_bytes(sig[32:], "little")
    if s >= _q:
        return False
    R = _decompress(sig[:32])
    if R is None:
        return False
    h = _hq(sig[:32] + pk + msg)
    lhs = _mul(s, _G)
    rhs = _add(R, _mul(h, A))
    return _compress(lhs) == _compress(rhs)


# ---------- backend choice ----------

def public_from_seed(seed, pure=False):
    if HAVE_CRYPTOGRAPHY and not pure:
        k = Ed25519PrivateKey.from_private_bytes(seed)
        return k.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    return _py_public(seed)


def sign_raw(seed, msg, pure=False):
    if HAVE_CRYPTOGRAPHY and not pure:
        return Ed25519PrivateKey.from_private_bytes(seed).sign(msg)
    return _py_sign(seed, msg)


def verify_raw(pk, msg, sig, pure=False):
    if HAVE_CRYPTOGRAPHY and not pure:
        try:
            Ed25519PublicKey.from_public_bytes(pk).verify(sig, msg)
            return True
        except Exception:
            return False
    return _py_verify(pk, msg, sig)


# ---------- usign files ----------

def _file(comment, raw):
    return f"untrusted comment: {comment}\n{base64.b64encode(raw).decode()}\n"


def _body(text):
    lines = text.splitlines()
    if len(lines) < 2 or not lines[0].startswith("untrusted comment:"):
        raise ValueError("not a usign file")
    return base64.b64decode(lines[1].strip())


def generate(comment="AIGW cortex model signing key"):
    """Returns (secret_key_text, public_key_text, fingerprint_hex)."""
    seed, fp, salt = os.urandom(32), os.urandom(8), os.urandom(16)
    pk = public_from_seed(seed)
    sk64 = seed + pk
    checksum = hashlib.sha512(sk64).digest()[:8]
    sec = b"Ed" + b"BK" + (0).to_bytes(4, "big") + salt + checksum + fp + sk64
    return (_file(f"{comment} (secret key)", sec), _file(f"{comment}", b"Ed" + fp + pk), fp.hex())


def read_secret(text):
    raw = _body(text)
    if len(raw) != 104 or raw[:4] != b"EdBK":
        raise ValueError("not an unencrypted usign secret key")
    if int.from_bytes(raw[4:8], "big"):
        raise ValueError("password-protected usign keys are not supported")
    fp, sk64 = raw[32:40], raw[40:104]
    if hashlib.sha512(sk64).digest()[:8] != raw[24:32]:
        raise ValueError("secret key checksum mismatch")
    return fp, sk64[:32]


def read_public(text):
    raw = _body(text)
    if len(raw) != 42 or raw[:2] != b"Ed":
        raise ValueError("not a usign public key")
    return raw[2:10], raw[10:42]


def sign(secret_text, data):
    fp, seed = read_secret(secret_text)
    return _file(f"signed by key {fp.hex()}", b"Ed" + fp + sign_raw(seed, data))


def verify(public_text, sig_text, data):
    fp, pk = read_public(public_text)
    raw = _body(sig_text)
    if len(raw) != 74 or raw[:2] != b"Ed" or raw[2:10] != fp:
        return False
    return verify_raw(pk, data, raw[10:])
