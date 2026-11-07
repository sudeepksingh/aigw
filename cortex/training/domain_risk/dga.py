# SPDX-License-Identifier: Apache-2.0
"""Synthetic examples of algorithmically generated domains (DGAs).

Malware uses DGAs to find its command servers. These generators imitate the
main styles seen in the wild without reproducing any malware code:

  random_letters   uniform letters, 8-20 chars       (e.g. CryptoLocker-like)
  random_alnum     letters and digits, 8-20 chars     (e.g. Necurs-like)
  hex              hex strings, 8-32 chars            (hash-based)
  base32           a-z2-7, 16 or 24 chars             (encoded seeds)
  letters_digits   letters followed by 2-4 digits
  syllables        pronounceable random syllables     (hardest: looks word-like)

Dictionary-based DGAs (concatenated real words) are deliberately not covered:
from the name alone they look like ordinary brands. Catching those needs
behaviour (never-seen domain + no history), which comes from baselines.
"""
import random
import string

TLDS = ["com", "net", "org", "info", "biz", "ru", "top", "xyz", "online", "site", "cc", "pw"]
LETTERS = string.ascii_lowercase
ALNUM = string.ascii_lowercase + string.digits
HEX = "0123456789abcdef"
B32 = string.ascii_lowercase + "234567"
CONS = "bcdfghjklmnprstvwxz"
VOW = "aeiou"


def random_letters(r):
    return "".join(r.choice(LETTERS) for _ in range(r.randint(8, 20)))


def random_alnum(r):
    return "".join(r.choice(ALNUM) for _ in range(r.randint(8, 20)))


def hexstr(r):
    return "".join(r.choice(HEX) for _ in range(r.randint(8, 32)))


def base32(r):
    return "".join(r.choice(B32) for _ in range(r.choice([16, 24])))


def letters_digits(r):
    return "".join(r.choice(LETTERS) for _ in range(r.randint(6, 12))) + \
        "".join(r.choice(string.digits) for _ in range(r.randint(2, 4)))


def syllables(r):
    out = []
    for _ in range(r.randint(3, 6)):
        s = r.choice(CONS) + r.choice(VOW)
        if r.random() < 0.3:
            s += r.choice(CONS)
        out.append(s)
    return "".join(out)


FAMILIES = {
    "random_letters": random_letters,
    "random_alnum": random_alnum,
    "hex": hexstr,
    "base32": base32,
    "letters_digits": letters_digits,
    "syllables": syllables,
}


def generate(n_per_family, seed):
    r = random.Random(seed)
    out = []
    for fam, gen in FAMILIES.items():
        for _ in range(n_per_family):
            label = gen(r)
            out.append((fam, label, label + "." + r.choice(TLDS)))
    return out
