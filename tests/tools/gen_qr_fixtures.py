#!/usr/bin/env python3
"""Writes the reference matrices of tests/fixtures/qr/ with an encoder that is independent of the app's own: libqrencode
(shared library, called through ctypes), byte mode, error correction level L, the smallest version that fits.

Run it on a development machine that has libqrencode; the tests only read the files:

    python3 tests/tools/gen_qr_fixtures.py tests/fixtures/qr

Each file: comment lines (# tool, # version, # input-hex) and then one row per line of '0' (light) and '1' (dark) modules.
"""
import ctypes
import ctypes.util
import os
import sys


class QRcode(ctypes.Structure):
    _fields_ = [("version", ctypes.c_int), ("width", ctypes.c_int), ("data", ctypes.POINTER(ctypes.c_ubyte))]


def load():
    name = ctypes.util.find_library("qrencode") or "libqrencode.so.4"
    lib = ctypes.CDLL(name)
    lib.QRcode_encodeData.restype = ctypes.POINTER(QRcode)
    lib.QRcode_encodeData.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_int]
    lib.QRcode_free.argtypes = [ctypes.POINTER(QRcode)]
    lib.QRcode_APIVersionString.restype = ctypes.c_char_p
    return lib


def encode(lib, data: bytes):
    q = lib.QRcode_encodeData(len(data), data, 0, 0)   # version 0 = smallest, level 0 = L
    if not q:
        raise SystemExit("libqrencode could not encode %d bytes" % len(data))
    c = q.contents
    rows = []
    for y in range(c.width):
        rows.append("".join("1" if c.data[y * c.width + x] & 1 else "0" for x in range(c.width)))
    version = c.version
    lib.QRcode_free(q)
    return version, rows


def token(n, seed="x"):
    base = "https://cloud.example.org/index.php/login/v2/flow/"
    return (base + (seed * n))[:n]


INPUTS = [
    ("short_url", token(52, "a")),
    ("url_120", token(120, "b7Kq")),
    ("url_200", token(200, "Zm9v")),
    ("len_17_v1_full", "a" * 17),
    ("len_18_v2", "b" * 18),
    ("len_32_v2_full", "c" * 32),
    ("len_33_v3", "d" * 33),
    ("len_106_v5_full", token(106, "e")),
    ("len_107_v6", token(107, "f")),
    ("len_230_v9_full", token(230, "g")),
    ("non_ascii", "https://cloud.example.org/ä€\U0001F600"),
    ("len_367_v12_full", token(367, "h2")),
]


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "tests/fixtures/qr"
    os.makedirs(out, exist_ok=True)
    lib = load()
    tool = "libqrencode %s via ctypes, byte mode, level L, smallest version" % lib.QRcode_APIVersionString().decode()
    for name, text in INPUTS:
        data = text.encode("utf-8")
        version, rows = encode(lib, data)
        with open(os.path.join(out, name + ".txt"), "w") as f:
            f.write("# tool: %s\n# version: %d\n# input-hex: %s\n" % (tool, version, data.hex()))
            f.write("\n".join(rows) + "\n")
        print(name, len(data), "bytes -> version", version)


if __name__ == "__main__":
    main()
