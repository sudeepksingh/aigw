# SPDX-License-Identifier: Apache-2.0
"""Read and re-stamp .agm model files (format: include/ag/agm.h)."""
import json
import struct

META = 3


def read(data):
    """Returns (header fields, [(type, bytes)])."""
    if len(data) < 52 or data[:4] != b"AGM1":
        raise ValueError("not an .agm file")
    version, kind, nfeat, schema = struct.unpack_from("<HHII", data, 4)
    name = data[16:48].rstrip(b"\0").decode(errors="replace")
    (nsec,) = struct.unpack_from("<I", data, 48)
    off, sections = 52, []
    for _ in range(nsec):
        if off + 8 > len(data):
            raise ValueError("truncated section header")
        t, n = struct.unpack_from("<II", data, off)
        off += 8
        if off + n > len(data):
            raise ValueError("truncated section")
        sections.append((t, data[off:off + n]))
        off += n
    head = {"format": version, "kind": kind, "features": nfeat, "schema": schema, "name": name}
    return head, sections


def meta(data):
    _, sections = read(data)
    for t, b in sections:
        if t == META:
            return json.loads(b.decode())
    return {}


def stamp(data, **fields):
    """Same model with fields merged into its META section (e.g. version=3)."""
    head, sections = read(data)
    m = {}
    out = []
    for t, b in sections:
        if t == META:
            m = json.loads(b.decode())
        else:
            out.append((t, b))
    m.update(fields)
    out.append((META, json.dumps(m, sort_keys=True).encode()))
    blob = data[:48] + struct.pack("<I", len(out))
    blob += b"".join(struct.pack("<II", t, len(b)) + b for t, b in out)
    return blob
